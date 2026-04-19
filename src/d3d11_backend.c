/*
 * DirectGlide - D3D11 backend implementation
 * Handles device creation, LFB blit, and 3D rendering pipeline
 */

#include <initguid.h>
#include "d3d11_backend.h"
#include "d3d11_state.h"
#include "d3d11_texture.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <d3dcompiler.h>

DGState g_dg = {0};

/* ============================================================
 * Window detection
 * ============================================================ */

typedef struct { DWORD pid; HWND result; } FindWindowCtx;

static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
    FindWindowCtx* ctx = (FindWindowCtx*)lParam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && IsWindowVisible(hwnd)) {
        ctx->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND findGameWindow(void) {
    FindWindowCtx ctx;
    ctx.pid = GetCurrentProcessId();
    ctx.result = NULL;
    EnumWindows(enumWindowsProc, (LPARAM)&ctx);
    return ctx.result;
}

/* ============================================================
 * Shader sources
 * ============================================================ */

/* LFB fullscreen blit shader */
static const char* s_lfbShaderSrc =
    "Texture2D    tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(uint id : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    o.uv  = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "float4 PSMain(VSOut i) : SV_TARGET {\n"
    "    float4 c = tex.Sample(smp, i.uv);\n"
    "    if (c.a < 0.01) discard;\n"
    "    return c;\n"
    "}\n";

/* Final blit: offscreen game RT -> swap chain back buffer with aspect preservation */
static const char* s_blitShaderSrc =
    "cbuffer BlitCB : register(b0) {\n"
    "    float4 rect; /* x0, y0, x1, y1 in NDC */\n"
    "    float4 texSize; /* gameW, gameH, 1/gameW, 1/gameH */\n"
    "};\n"
    "Texture2D    tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(uint id : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    /* Generate 4 corners (0,0), (1,0), (0,1), (1,1) for triangle strip quad */\n"
    "    float2 uv = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);\n"
    "    o.uv = uv;\n"
    "    o.pos = float4(lerp(rect.x, rect.z, uv.x), lerp(rect.w, rect.y, uv.y), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "float4 PSMain(VSOut i) : SV_TARGET {\n"
    "    /* Sharp bilinear: flat plateau in middle of each texel (nearest-neighbor),\n"
    "     * blend only within one dest-pixel width of the texel boundary. */\n"
    "    float2 pixel = i.uv * texSize.xy;\n"
    "    float2 f = frac(pixel);\n"
    "    float2 tpp = max(fwidth(pixel), 0.0001);\n"
    "    /* Offset from texel center, in [-0.5, 0.5] */\n"
    "    float2 offset = f - 0.5;\n"
    "    /* snap_range: distance from center where we stay at center (nearest-neighbor region) */\n"
    "    float2 snapRange = 0.5 - tpp * 0.5;\n"
    "    /* Only the excess over snapRange generates a ramp */\n"
    "    float2 excess = sign(offset) * max(abs(offset) - snapRange, 0.0);\n"
    "    /* Scale excess to reach -0.5..0.5 at texel boundaries */\n"
    "    float2 scaled = clamp(excess / (tpp * 0.5), -1.0, 1.0);\n"
    "    float2 fSnapped = 0.5 + scaled * 0.5;\n"
    "    float2 uvSharp = (floor(pixel) + fSnapped) * texSize.zw;\n"
    "    return tex.Sample(smp, uvSharp);\n"
    "}\n";

/* SSAA downsample: linear sample at the center of each dest pixel's source
 * footprint. For SSAA=2, the dest UV's linear sample averages exactly 4
 * source texels (a 2x2 box filter). */
static const char* s_downsampleShaderSrc =
    "cbuffer DSCB : register(b0) {\n"
    "    float4 dsRect;     /* unused — matches blitCB layout */\n"
    "    float4 srcSize;    /* w, h, 1/w, 1/h */\n"
    "};\n"
    "Texture2D    src : register(t0);\n"
    "SamplerState lin : register(s0);\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(uint id : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    float2 uv = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);\n"
    "    o.uv = uv;\n"
    "    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);\n"
    "    return o;\n"
    "}\n"
    "float4 PSMain(VSOut i) : SV_TARGET {\n"
    "    return src.Sample(lin, i.uv);\n"
    "}\n";

/* 3D scene shader with combiner support */
static const char* s_sceneShaderSrc =
    "cbuffer CB : register(b0) {\n"
    "    float4 viewport;    /* w, h, 1/w, 1/h */\n"
    "    float4 constColor;  /* constant color RGBA */\n"
    "    int4 colorCombine;  /* func, factor, local, other */\n"
    "    int4 alphaCombine;  /* func, factor, local, other */\n"
    "    int4 texCombine;    /* func, factor, pad, pad */\n"
    "    float4 fogColor;\n"
    "    int4 fogAlphaChroma; /* fogMode, alphaTestFunc, chromaEnable, pad */\n"
    "    float4 alphaChromaRef; /* alphaTestRef, chromaR, chromaG, chromaB */\n"
    "    int4 invertFlags;   /* colorInv, alphaInv, texRgbInv, texAlphaInv */\n"
    "};\n"
    "\n"
    "Texture2D    texMap : register(t0);\n"
    "SamplerState texSmp : register(s0);\n"
    "\n"
    "cbuffer FogCB : register(b1) {\n"
    "    float4 fogTable[16]; /* 64 floats — fog density per Glide index, 0..1 */\n"
    "};\n"
    "\n"
    "struct VSIn {\n"
    "    float2 pos   : POSITION;\n"
    "    float  depth : TEXCOORD3;\n"
    "    float4 color : COLOR;\n"
    "    float  oow   : OOW;\n"
    "    float3 tmu0  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "struct PSIn {\n"
    "    float4        pos   : SV_POSITION;\n"
    "    sample float4 color : COLOR;\n"
    "    sample float  oow   : OOW;\n"
    "    sample float3 tmu0  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "PSIn VSMain(VSIn v) {\n"
    "    PSIn o;\n"
    "    /* Snap to 1/16 pixel grid — mimics Voodoo's 4-bit subpixel precision. */\n"
    "    float sx = floor(v.pos.x * 16.0 + 0.5) * (1.0/16.0);\n"
    "    float sy = floor(v.pos.y * 16.0 + 0.5) * (1.0/16.0);\n"
    "    /* Apply -0.5 offset to align Glide vertex coords with D3D11 pixel centers */\n"
    "    float px = sx - 0.5;\n"
    "    float py = sy - 0.5;\n"
    "    o.pos.x = px * viewport.z * 2.0 - 1.0;\n"
    "    o.pos.y = 1.0 - py * viewport.w * 2.0;\n"
    "    o.pos.z = v.depth;\n"
    "    o.pos.w = 1.0;\n"
    "    o.color = v.color;\n"
    "    o.oow   = v.oow;\n"
    "    o.tmu0  = v.tmu0;\n"
    "    return o;\n"
    "}\n"
    "\n"
    "/* Evaluate a Glide combine function */\n"
    "float4 combine(int func, int factor, float4 local, float4 other, float4 factorVal) {\n"
    "    float4 result = float4(0,0,0,0);\n"
    "    if (func == 0) result = float4(0,0,0,0);  /* ZERO */\n"
    "    else if (func == 1) result = local;  /* LOCAL */\n"
    "    else if (func == 2) result = local.aaaa;  /* LOCAL_ALPHA */\n"
    "    else if (func == 3) result = other * factorVal;  /* SCALE_OTHER */\n"
    "    else if (func == 4) result = other * factorVal + local;  /* SCALE_OTHER_ADD_LOCAL */\n"
    "    else if (func == 5) result = other * factorVal + local.aaaa;  /* SCALE_OTHER_ADD_LOCAL_ALPHA */\n"
    "    else if (func == 6) result = (other - local) * factorVal;  /* SCALE_OTHER_MINUS_LOCAL */\n"
    "    else if (func == 7) result = (other - local) * factorVal + local;  /* ...ADD_LOCAL */\n"
    "    else if (func == 8) result = (other - local) * factorVal + local.aaaa;  /* ...ADD_LOCAL_ALPHA */\n"
    "    else result = local;  /* fallback */\n"
    "    return saturate(result);\n"
    "}\n"
    "\n"
    "/* Get combine factor value */\n"
    "float4 getFactor(int factor, float4 localC, float4 otherC, float4 texC) {\n"
    "    if (factor == 0) return float4(0,0,0,0);  /* ZERO */\n"
    "    if (factor == 1) return localC;  /* LOCAL */\n"
    "    if (factor == 2) return otherC.aaaa;  /* OTHER_ALPHA */\n"
    "    if (factor == 3) return localC.aaaa;  /* LOCAL_ALPHA */\n"
    "    if (factor == 4) return texC.aaaa;  /* TEXTURE_ALPHA */\n"
    "    if (factor == 5) return texC;  /* TEXTURE_RGB */\n"
    "    if (factor == 8) return float4(1,1,1,1);  /* ONE */\n"
    "    if (factor == 9) return 1.0 - localC;  /* ONE_MINUS_LOCAL */\n"
    "    if (factor == 10) return 1.0 - otherC.aaaa;  /* ONE_MINUS_OTHER_ALPHA */\n"
    "    if (factor == 11) return 1.0 - localC.aaaa;  /* ONE_MINUS_LOCAL_ALPHA */\n"
    "    if (factor == 12) return 1.0 - texC.aaaa;  /* ONE_MINUS_TEXTURE_ALPHA */\n"
    "    return float4(1,1,1,1);\n"
    "}\n"
    "\n"
    "float4 PSMain(PSIn i) : SV_TARGET {\n"
    "    /* Perspective-correct texture sampling */\n"
    "    float4 texColor = float4(1,1,1,1);\n"
    "    if (i.tmu0.z > 0.0000001) {\n"
    "        /* sow/oow gives texel coords in [0,256] range; normalize to [0,1] UV */\n"
    "        float2 uv = (i.tmu0.xy / i.tmu0.z) / 256.0;\n"
    "        /* Sample() lets the sampler state control LOD + anisotropy from UV\n"
    "         * derivatives. This is what actually enables aniso + mipmaps. */\n"
    "        texColor = texMap.Sample(texSmp, uv);\n"
    "    }\n"
    "\n"
    "    float4 iterColor = i.color;\n"
    "\n"
    "    /* Color combine */\n"
    "    float4 cLocal = (colorCombine.z == 0) ? iterColor : constColor;\n"
    "    float4 cOther;\n"
    "    if (colorCombine.w == 1) cOther = texColor;\n"
    "    else if (colorCombine.w == 2) cOther = constColor;\n"
    "    else cOther = iterColor;\n"
    "\n"
    "    /* Get combine factor value */\n"
    "    float4 cFactor = float4(1,1,1,1);\n"
    "    int factorId = colorCombine.y;\n"
    "    if (factorId == 0) cFactor = float4(0,0,0,0);\n"
    "    else if (factorId == 1) cFactor = cLocal;\n"
    "    else if (factorId == 2) cFactor = cOther.aaaa;\n"
    "    else if (factorId == 3) cFactor = cLocal.aaaa;\n"
    "    else if (factorId == 4) cFactor = texColor.aaaa;\n"
    "    else if (factorId == 5) cFactor = texColor;\n"
    "    else if (factorId == 8) cFactor = float4(1,1,1,1);\n"
    "    else if (factorId == 9) cFactor = 1.0 - cLocal;\n"
    "    else if (factorId == 11) cFactor = 1.0 - cLocal.aaaa;\n"
    "    else if (factorId == 12) cFactor = 1.0 - texColor.aaaa;\n"
    "\n"
    "    float4 finalColor;\n"
    "    int cf = colorCombine.x;\n"
    "    if (cf == 0) finalColor = float4(0,0,0,0);\n"
    "    else if (cf == 1) finalColor = cLocal;\n"
    "    else if (cf == 2) finalColor = cLocal.aaaa;\n"
    "    else if (cf == 3) finalColor = cOther * cFactor;\n"
    "    else if (cf == 4) finalColor = cOther * cFactor + cLocal;\n"
    "    else if (cf == 5) finalColor = cOther * cFactor + cLocal.aaaa;\n"
    "    else if (cf == 6) finalColor = (cOther - cLocal) * cFactor;\n"
    "    else if (cf == 7) finalColor = (cOther - cLocal) * cFactor + cLocal;\n"
    "    else if (cf == 8) finalColor = (cOther - cLocal) * cFactor + cLocal.aaaa;\n"
    "    else finalColor = cOther;\n"
    "\n"
    "    /* Alpha combine */\n"
    "    float4 aLocal = (alphaCombine.z == 0) ? iterColor : constColor;\n"
    "    float4 aOther;\n"
    "    if (alphaCombine.w == 1) aOther = texColor;\n"
    "    else if (alphaCombine.w == 2) aOther = constColor;\n"
    "    else aOther = iterColor;\n"
    "\n"
    "    float4 aFactor = float4(1,1,1,1);\n"
    "    int afId = alphaCombine.y;\n"
    "    if (afId == 0) aFactor = float4(0,0,0,0);\n"
    "    else if (afId == 1) aFactor = aLocal;\n"
    "    else if (afId == 3) aFactor = aLocal.aaaa;\n"
    "    else if (afId == 4) aFactor = texColor.aaaa;\n"
    "    else if (afId == 8) aFactor = float4(1,1,1,1);\n"
    "    else if (afId == 11) aFactor = 1.0 - aLocal.aaaa;\n"
    "    else if (afId == 12) aFactor = 1.0 - texColor.aaaa;\n"
    "\n"
    "    int af = alphaCombine.x;\n"
    "    if (af == 0) finalColor.a = 0;\n"
    "    else if (af == 1) finalColor.a = aLocal.a;\n"
    "    else if (af == 2) finalColor.a = aLocal.a;\n"
    "    else if (af == 3) finalColor.a = (aOther * aFactor).a;\n"
    "    else if (af == 4) finalColor.a = (aOther * aFactor + aLocal).a;\n"
    "    else finalColor.a = aOther.a;\n"
    "\n"
    "    /* Alpha test */\n"
    "    int atFunc = fogAlphaChroma.y;\n"
    "    float atRef = alphaChromaRef.x;\n"
    "    if (atFunc == 0) discard;\n"
    "    if (atFunc == 1 && finalColor.a >= atRef) discard;\n"
    "    if (atFunc == 4 && finalColor.a <= atRef) discard;\n"
    "\n"
    "    finalColor = saturate(finalColor);\n"
    "\n"
    "    /* Glide fog: fogMode bits: 1=WITH_TABLE, 2=WITH_ITERATED_ALPHA, 4=MULT2, 8=ADD2.\n"
    "     * Source selects the factor, MULT2/ADD2 change the blend combine. */\n"
    "    int fogMode = fogAlphaChroma.x;\n"
    "    int fogSrc  = fogMode & 3;\n"
    "    if (fogSrc != 0) {\n"
    "        float fogF = 0.0;\n"
    "        if (fogSrc == 1) {\n"
    "            float w = 1.0 / max(i.oow, 1e-6);\n"
    "            float idx = log2(max(w, 1.0)) * 4.0;\n"
    "            idx = clamp(idx, 0.0, 63.0);\n"
    "            int   i0 = (int)floor(idx);\n"
    "            int   i1 = min(i0 + 1, 63);\n"
    "            float frac_= idx - (float)i0;\n"
    "            float a = fogTable[i0 / 4][i0 & 3];\n"
    "            float b = fogTable[i1 / 4][i1 & 3];\n"
    "            fogF = lerp(a, b, frac_);\n"
    "        } else {\n"
    "            fogF = saturate(i.color.a);\n"
    "        }\n"
    "        finalColor.rgb = lerp(finalColor.rgb, fogColor.rgb, fogF);\n"
    "    }\n"
    "    return finalColor;\n"
    "}\n";

/* ============================================================
 * Shader compilation helper
 * ============================================================ */

static int compileShader(const char* src, const char* entry, const char* target,
                         ID3DBlob** outBlob) {
    ID3DBlob* errBlob = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "shader.hlsl", NULL, NULL,
                            entry, target, 0, 0, outBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            dg_log("Shader compile error (%s): %s\n", entry,
                   (char*)ID3D10Blob_GetBufferPointer(errBlob));
            ID3D10Blob_Release(errBlob);
        }
        return 0;
    }
    if (errBlob) ID3D10Blob_Release(errBlob);
    return 1;
}

/* ============================================================
 * D3D11 initialization
 * ============================================================ */

static int createDepthBuffer(void);
static int createLfbResources(void);
static int createScenePipeline(void);
static int createBlitResources(void);
static void blitGameToScreen(void);
static void runAAPass(void);

/* Compute the on-screen rect where the game is rendered (letterbox area) */
static void getGameRect(int* destX, int* destY, int* destW, int* destH) {
    float gameAsp = (float)g_dg.width / (float)g_dg.height;
    float scrAsp = (float)g_dg.screenWidth / (float)g_dg.screenHeight;
    if (scrAsp > gameAsp) {
        *destH = g_dg.screenHeight;
        *destW = (int)((float)g_dg.width * (float)g_dg.screenHeight / (float)g_dg.height);
    } else {
        *destW = g_dg.screenWidth;
        *destH = (int)((float)g_dg.height * (float)g_dg.screenWidth / (float)g_dg.width);
    }
    *destX = (g_dg.screenWidth - *destW) / 2;
    *destY = (g_dg.screenHeight - *destH) / 2;
}

/* Position-dependent 16-bit canary used as LFB "unwritten" marker. Replaces
 * the old fixed magenta (0xF81F) sentinel which collided with KQ8's own use
 * of magenta as a "no save preview" placeholder. Because the canary varies
 * per pixel, the odds that a game-written pixel matches the canary at THAT
 * SPECIFIC position are ~1/65536 — scattered false positives instead of whole
 * blocks of mis-classified pixels. */
static FxU16 lfbCanary(int x, int y) {
    FxU32 h = ((FxU32)x * 73856093u) ^ ((FxU32)y * 19349663u);
    h = (h * 0x9E3779B1u) >> 16;
    return (FxU16)(h | 1); /* avoid 0 so a zeroed buffer reads as "written" */
}

static void lfbFillCanary(FxU8* buf, FxU32 strideBytes, int w, int h) {
    int x, y;
    for (y = 0; y < h; y++) {
        FxU16* row = (FxU16*)(buf + y * strideBytes);
        for (x = 0; x < w; x++) row[x] = lfbCanary(x, y);
    }
}

/* Symmetric-rounded screen->game translation. Round-trip-stable with
 * gameToScreen below — essential for mouse-look, where the game does
 * SetCursorPos(center) → GetCursorPos → delta → SetCursorPos(center)
 * in a tight loop. With plain integer-division (floor), non-integer
 * letterbox scaling produces a 1-pixel round-trip error every cycle,
 * which the camera interprets as continuous rotation input. */
static int screenXToGameX(int winX, int destX, int destW) {
    long num = ((long)winX - destX) * (long)g_dg.width + destW / 2;
    return (int)(num / destW);
}
static int screenYToGameY(int winY, int destY, int destH) {
    long num = ((long)winY - destY) * (long)g_dg.height + destH / 2;
    return (int)(num / destH);
}

/* Translate screen-space mouse (window-client) coords to game coords (640x480) */
static LPARAM translateMouseLParam(LPARAM lParam) {
    int destX, destY, destW, destH;
    short winX = (short)(lParam & 0xFFFF);
    short winY = (short)((lParam >> 16) & 0xFFFF);
    int gameX, gameY;
    getGameRect(&destX, &destY, &destW, &destH);
    if (destW == 0 || destH == 0) return lParam;
    gameX = screenXToGameX(winX, destX, destW);
    gameY = screenYToGameY(winY, destY, destH);
    /* Minimum gameX = 1, not 0. The LFB-flush HUD-detection hack probes the
     * leftmost pixel column (x=0) to identify HUD bar extents; the cursor is
     * the only game element that could reach x=0, so we keep it one pixel in. */
    if (gameX < 1) gameX = 1; else if (gameX >= g_dg.width)  gameX = g_dg.width - 1;
    if (gameY < 0) gameY = 0; else if (gameY >= g_dg.height) gameY = g_dg.height - 1;
    return (LPARAM)((gameY & 0xFFFF) << 16) | (LPARAM)(gameX & 0xFFFF);
}

/* Our wrapper window proc: translate mouse coords, then forward */
static volatile LONG s_videoPlaying; /* fwd decl — defined fully further down */

static LRESULT CALLBACK dgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_dg.isOpen && !s_videoPlaying) {
        switch (msg) {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                lParam = translateMouseLParam(lParam);
                break;
        }
    }
    if (g_dg.origWndProc)
        return CallWindowProc(g_dg.origWndProc, hwnd, msg, wParam, lParam);
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* IAT hook: intercept GetCursorPos/SetCursorPos/ClipCursor */
typedef BOOL (WINAPI *GetCursorPosFn)(LPPOINT);
typedef BOOL (WINAPI *SetCursorPosFn)(int, int);
typedef BOOL (WINAPI *ClipCursorFn)(const RECT*);
typedef BOOL (WINAPI *ScreenToClientFn)(HWND, LPPOINT);
static GetCursorPosFn s_origGetCursorPos = NULL;
static SetCursorPosFn s_origSetCursorPos = NULL;
static ClipCursorFn   s_origClipCursor   = NULL;
static ScreenToClientFn s_origScreenToClient = NULL;

/* Tracks whether the game is currently playing an FMV via Video for Windows.
 * When set, our cursor IAT hooks pass through to the originals without
 * coordinate translation — KQ8's video playback hangs otherwise. Toggled by
 * hooks on AVIFileOpenA / AVIFileRelease.  (Forward-decl'd above.) */

typedef HRESULT (WINAPI *AVIFileOpenAFn)(void*, LPCSTR, UINT, LPVOID);
typedef ULONG   (WINAPI *AVIFileReleaseFn)(void*);
typedef HWND    (WINAPI *CreateWindowExAFn)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef HWND    (WINAPI *MCIWndCreateAFn)(HWND, HINSTANCE, DWORD, LPCSTR);
typedef DWORD   (WINAPI *MciSendCommandAFn)(DWORD, UINT, DWORD_PTR, DWORD_PTR);

static AVIFileOpenAFn    s_origAVIFileOpenA    = NULL;
static AVIFileReleaseFn  s_origAVIFileRelease  = NULL;
static CreateWindowExAFn s_origCreateWindowExA = NULL;
static MCIWndCreateAFn   s_origMCIWndCreateA   = NULL;
static MciSendCommandAFn s_origMciSendCommandA = NULL;

/* DIAGNOSTIC no-op: two resize approaches both crashed the video playback.
 * Leave the window alone and verify the video plays at all at native size.
 * If that works, the proper path is MCIWNDM_PUT_DEST (telling the video
 * player where to draw) rather than SetWindowPos on its window. */
static void stretchVideoWindow(HWND vidHwnd) {
    if (!vidHwnd) return;
    dg_log("VIDEO: 0x%p — no-op stretch (diagnostic pass)\n", (void*)vidHwnd);
}

/* Post-creation video stretch via EnumChildWindows. Instead of hooking
 * MCIWndCreateA (which caused crashes at EIP=0x00400005), observe MCIWnd
 * windows AFTER they've finished initializing by scanning the game HWND's
 * children each frame during Present. Tracks already-stretched windows to
 * avoid flicker/lag from resizing every frame. */
#ifndef MCIWNDM_PUT_DEST
#define MCIWNDM_PUT_DEST (WM_USER + 230)
#endif
#ifndef MCIWNDM_PUT_SOURCE
#define MCIWNDM_PUT_SOURCE (WM_USER + 229)
#endif
#ifndef MCIWNDM_SETZOOM
#define MCIWNDM_SETZOOM (WM_USER + 104)
#endif
#ifndef MCIWNDM_GETDEVICEID
#define MCIWNDM_GETDEVICEID (WM_USER + 116)
#endif

static HWND s_lastStretchedVid = NULL;
static DWORD s_ourPid = 0;
static WNDPROC s_origMciProc = NULL;
static HWND s_subclassedMci = NULL;
static int s_videoSrcW = 640;
static int s_videoSrcH = 480;

/* Subclass proc for MCIWnd. Lets the original paint the video at its native
 * source size into the top-left of the (enlarged) client area, then
 * StretchBlt-s that region to fill the full client. */
static LRESULT CALLBACK mciSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC orig = s_origMciProc;
    if (!orig) return DefWindowProcA(hwnd, msg, wp, lp);

    if (msg == WM_ERASEBKGND) {
        /* Black-fill once on resize (letterbox bars); return 1 to suppress
         * further erase on this cycle. Video frames draw inside this, so no
         * per-frame flicker. */
        HDC dc = (HDC)wp;
        RECT cr;
        if (dc && GetClientRect(hwnd, &cr)) {
            HBRUSH bg = (HBRUSH)GetStockObject(BLACK_BRUSH);
            FillRect(dc, &cr, bg);
        }
        return 1;
    }

    /* Lock window size to parent client rect — MCIWnd snaps itself back to
     * native video size (640x480) after we resize it. Rewrite WINDOWPOS in
     * WM_WINDOWPOSCHANGING so MCIWnd's own resize attempts stay fullscreen. */
    if (msg == WM_WINDOWPOSCHANGING && lp) {
        WINDOWPOS* wp2 = (WINDOWPOS*)lp;
        HWND parent = GetParent(hwnd);
        RECT pc;
        if (parent && GetClientRect(parent, &pc)) {
            int pw = pc.right - pc.left, ph = pc.bottom - pc.top;
            if (pw > 0 && ph > 0) {
                if (!(wp2->flags & SWP_NOSIZE)) {
                    wp2->cx = pw;
                    wp2->cy = ph;
                }
                if (!(wp2->flags & SWP_NOMOVE)) {
                    wp2->x = 0;
                    wp2->y = 0;
                }
            }
        }
    }

    /* WM_PAINT (rarely fires during playback) — stretch if it does. The
     * real video paints happen from MCI's playback thread via DrawDibDraw,
     * which we hook separately at the gdi32-import level in msvfw32. */
    if (msg == WM_PAINT) {
        RECT cr;
        int cw, ch, sw, sh;
        LRESULT ret = CallWindowProcA(orig, hwnd, msg, wp, lp);
        if (!GetClientRect(hwnd, &cr)) return ret;
        cw = cr.right - cr.left;
        ch = cr.bottom - cr.top;
        sw = s_videoSrcW;
        sh = s_videoSrcH;
        if (cw <= sw && ch <= sh) return ret;
        {
            HDC dc = GetDC(hwnd);
            if (dc) {
                HDC memDc = CreateCompatibleDC(dc);
                HBITMAP memBmp = CreateCompatibleBitmap(dc, sw, sh);
                HGDIOBJ oldBmp = SelectObject(memDc, memBmp);
                int dstX, dstY, dstW, dstH;
                if ((long long)cw * sh > (long long)ch * sw) {
                    dstH = ch;
                    dstW = sw * ch / sh;
                } else {
                    dstW = cw;
                    dstH = sh * cw / sw;
                }
                dstX = (cw - dstW) / 2;
                dstY = (ch - dstH) / 2;
                BitBlt(memDc, 0, 0, sw, sh, dc, 0, 0, SRCCOPY);
                {
                    RECT full; full.left = 0; full.top = 0; full.right = cw; full.bottom = ch;
                    HBRUSH bg = (HBRUSH)GetStockObject(BLACK_BRUSH);
                    FillRect(dc, &full, bg);
                }
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, NULL);
                StretchBlt(dc, dstX, dstY, dstW, dstH, memDc, 0, 0, sw, sh, SRCCOPY);
                SelectObject(memDc, oldBmp);
                DeleteObject(memBmp);
                DeleteDC(memDc);
                ReleaseDC(hwnd, dc);
            }
        }
        return ret;
    }

    if (msg == WM_NCDESTROY) {
        /* Restore original WndProc before the window is destroyed. */
        if (s_subclassedMci == hwnd) {
            SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)orig);
            s_origMciProc = NULL;
            s_subclassedMci = NULL;
            s_lastStretchedVid = NULL;
            dg_log("VIDEO: MCIWnd 0x%p destroyed — subclass removed\n", (void*)hwnd);
        }
    }

    return CallWindowProcA(orig, hwnd, msg, wp, lp);
}

static void stretchMCIWndToTarget(HWND vid, int targetW, int targetH) {
    RECT wr;
    POINT origin;
    HWND parent;
    int ww, wh;
    if (!GetWindowRect(vid, &wr)) return;
    ww = wr.right - wr.left;
    wh = wr.bottom - wr.top;

    /* Wait until the window reaches a reasonable video-native size before
     * subclassing — first observation is 300x0 during init, not useful. */
    if (s_subclassedMci != vid) {
        if (ww < 64 || wh < 48) return; /* not yet configured */
        s_videoSrcW = ww;
        s_videoSrcH = wh;

        {
            WNDPROC prev = (WNDPROC)SetWindowLongPtr(vid, GWLP_WNDPROC, (LONG_PTR)mciSubclassProc);
            if (prev) {
                s_origMciProc = prev;
                s_subclassedMci = vid;
                dg_log("VIDEO: subclassed MCIWnd 0x%p (src %dx%d, orig proc=%p)\n",
                       (void*)vid, s_videoSrcW, s_videoSrcH, (void*)prev);
            }
        }
    }

    /* Move + resize to cover parent client area. */
    parent = GetParent(vid);
    origin.x = 0; origin.y = 0;
    if (!parent) {
        RECT gr;
        if (g_dg.hwnd && GetWindowRect(g_dg.hwnd, &gr)) {
            origin.x = gr.left;
            origin.y = gr.top;
        }
    }

    if (ww == targetW && wh == targetH && wr.left == origin.x && wr.top == origin.y)
        return;

    dg_log("VIDEO: sizing MCIWnd 0x%p from (%d,%d %dx%d) to (%d,%d %dx%d)\n",
           (void*)vid, wr.left, wr.top, ww, wh, origin.x, origin.y, targetW, targetH);
    SetWindowPos(vid, HWND_TOP, origin.x, origin.y, targetW, targetH,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    InvalidateRect(vid, NULL, TRUE);
}

static BOOL CALLBACK enumVideoAnyProc(HWND hwnd, LPARAM lParam) {
    char cls[64];
    DWORD pid = 0;
    RECT pc;
    HWND parent;
    (void)lParam;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s_ourPid) return TRUE;
    if (GetClassNameA(hwnd, cls, (int)sizeof(cls)) <= 0) return TRUE;
    if (strcmp(cls, "MCIWndClass") != 0) return TRUE;

    /* Target size: parent client rect if parented, else our game window client. */
    parent = GetParent(hwnd);
    if (!parent) parent = g_dg.hwnd;
    if (!parent || !GetClientRect(parent, &pc)) return TRUE;
    if (pc.right - pc.left <= 0 || pc.bottom - pc.top <= 0) return TRUE;

    /* Always re-check and re-stretch every poll — MCIWnd may snap window
     * back to video native size once the file loads, so a one-shot stretch
     * doesn't stick. */
    stretchMCIWndToTarget(hwnd, pc.right - pc.left, pc.bottom - pc.top);
    s_lastStretchedVid = hwnd;
    return TRUE;
}

static void scanForVideoChildren(HWND parent) {
    (void)parent;
    if (!s_ourPid) s_ourPid = GetCurrentProcessId();
    /* Scan all top-level windows in the process; MCIWnd may be top-level or
     * parented to something other than our game HWND. */
    EnumWindows(enumVideoAnyProc, 0);
    /* Also scan children of the game window in case MCIWnd is nested. */
    if (g_dg.hwnd) EnumChildWindows(g_dg.hwnd, enumVideoAnyProc, 0);
}

/* Forward decl — defined later alongside mouse hook infrastructure. */
static void patchIAT(HMODULE hModule, const char* dllName, const char* funcName, FARPROC newFunc, FARPROC* origFunc);

/* ============================================================
 * msvfw32 gdi IAT hooks for FMV fullscreen stretching
 * ============================================================
 * The real video frame paints happen from MCI's playback thread via
 * msvfw32!DrawDibDraw, which ultimately calls gdi32!SetDIBitsToDevice
 * (no-stretch) or StretchDIBits. By IAT-patching msvfw32's imports from
 * gdi32 we intercept the pixel transfer and redirect it to a stretched
 * destination that fills our resized MCIWnd's client area. */
typedef int (WINAPI *SetDIBitsToDeviceFn)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT,
    const VOID*, const BITMAPINFO*, UINT);
typedef int (WINAPI *StretchDIBitsFn)(HDC, int, int, int, int, int, int, int, int,
    const VOID*, const BITMAPINFO*, UINT, DWORD);
typedef BOOL (WINAPI *BitBltFn)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL (WINAPI *StretchBltFn)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);

static SetDIBitsToDeviceFn s_origSetDIBitsToDevice = NULL;
static StretchDIBitsFn s_origStretchDIBits = NULL;
static BitBltFn s_origBitBlt = NULL;
static StretchBltFn s_origStretchBlt = NULL;
static int s_msvfwHooked = 0;

/* Compute aspect-preserved letterbox dst rect within client size (cw,ch)
 * for a source of size (sw,sh). Writes outputs via pointers. */
static void letterboxFit(int cw, int ch, int sw, int sh,
                          int* dstX, int* dstY, int* dstW, int* dstH) {
    if (sw <= 0 || sh <= 0) { *dstX = 0; *dstY = 0; *dstW = cw; *dstH = ch; return; }
    if ((long long)cw * sh > (long long)ch * sw) {
        *dstH = ch;
        *dstW = (int)((long long)sw * ch / sh);
    } else {
        *dstW = cw;
        *dstH = (int)((long long)sh * cw / sw);
    }
    *dstX = (cw - *dstW) / 2;
    *dstY = (ch - *dstH) / 2;
}

static int WINAPI hookedSetDIBitsToDevice(HDC hdc, int x, int y, DWORD w, DWORD h,
    int xs, int ys, UINT ss, UINT cl, const VOID* bits, const BITMAPINFO* bmi, UINT usage) {
    HWND hwndDC = WindowFromDC(hdc);
    if (hwndDC && hwndDC == s_subclassedMci && s_origStretchDIBits) {
        RECT cr;
        if (GetClientRect(hwndDC, &cr)) {
            int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
            if (cw > (int)w || ch > (int)h) {
                int dx, dy, dw, dh;
                letterboxFit(cw, ch, (int)w, (int)h, &dx, &dy, &dw, &dh);
                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, NULL);
                return s_origStretchDIBits(hdc, dx, dy, dw, dh, xs, ys, (int)w, (int)h,
                                            bits, bmi, usage, SRCCOPY);
            }
        }
    }
    return s_origSetDIBitsToDevice
        ? s_origSetDIBitsToDevice(hdc, x, y, w, h, xs, ys, ss, cl, bits, bmi, usage)
        : 0;
}

static int WINAPI hookedStretchDIBits(HDC hdc, int xd, int yd, int dw, int dh,
    int xs, int ys, int sw, int sh, const VOID* bits, const BITMAPINFO* bmi,
    UINT usage, DWORD rop) {
    HWND hwndDC = WindowFromDC(hdc);
    if (hwndDC && hwndDC == s_subclassedMci) {
        RECT cr;
        if (GetClientRect(hwndDC, &cr)) {
            int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
            if (cw > dw || ch > dh) {
                int ndx, ndy, ndw, ndh;
                letterboxFit(cw, ch, sw, sh, &ndx, &ndy, &ndw, &ndh);
                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, NULL);
                xd = ndx; yd = ndy; dw = ndw; dh = ndh;
            }
        }
    }
    return s_origStretchDIBits
        ? s_origStretchDIBits(hdc, xd, yd, dw, dh, xs, ys, sw, sh, bits, bmi, usage, rop)
        : 0;
}

static BOOL WINAPI hookedBitBlt(HDC hdcDst, int x, int y, int cx, int cy,
                                 HDC hdcSrc, int x1, int y1, DWORD rop) {
    HWND hwndDC = WindowFromDC(hdcDst);
    if (hwndDC && hwndDC == s_subclassedMci && s_origStretchBlt) {
        RECT cr;
        if (GetClientRect(hwndDC, &cr)) {
            int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
            if (cw > cx || ch > cy) {
                int dx, dy, dw, dh;
                letterboxFit(cw, ch, cx, cy, &dx, &dy, &dw, &dh);
                SetStretchBltMode(hdcDst, HALFTONE);
                SetBrushOrgEx(hdcDst, 0, 0, NULL);
                return s_origStretchBlt(hdcDst, dx, dy, dw, dh, hdcSrc, x1, y1, cx, cy, rop);
            }
        }
    }
    return s_origBitBlt ? s_origBitBlt(hdcDst, x, y, cx, cy, hdcSrc, x1, y1, rop) : FALSE;
}

static BOOL WINAPI hookedStretchBlt(HDC hdcDst, int x, int y, int cx, int cy,
                                     HDC hdcSrc, int x1, int y1, int cx1, int cy1, DWORD rop) {
    HWND hwndDC = WindowFromDC(hdcDst);
    if (hwndDC && hwndDC == s_subclassedMci) {
        RECT cr;
        if (GetClientRect(hwndDC, &cr)) {
            int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
            if (cw > cx || ch > cy) {
                int dx, dy, dw, dh;
                letterboxFit(cw, ch, cx1, cy1, &dx, &dy, &dw, &dh);
                SetStretchBltMode(hdcDst, HALFTONE);
                SetBrushOrgEx(hdcDst, 0, 0, NULL);
                x = dx; y = dy; cx = dw; cy = dh;
            }
        }
    }
    return s_origStretchBlt
        ? s_origStretchBlt(hdcDst, x, y, cx, cy, hdcSrc, x1, y1, cx1, cy1, rop)
        : FALSE;
}

static void tryHookMsvfw32(void) {
    HMODULE mod;
    if (s_msvfwHooked) return;
    mod = GetModuleHandleA("msvfw32.dll");
    if (!mod) return;
    patchIAT(mod, "gdi32.dll", "SetDIBitsToDevice",
             (FARPROC)hookedSetDIBitsToDevice, (FARPROC*)&s_origSetDIBitsToDevice);
    patchIAT(mod, "gdi32.dll", "StretchDIBits",
             (FARPROC)hookedStretchDIBits, (FARPROC*)&s_origStretchDIBits);
    patchIAT(mod, "gdi32.dll", "BitBlt",
             (FARPROC)hookedBitBlt, (FARPROC*)&s_origBitBlt);
    patchIAT(mod, "gdi32.dll", "StretchBlt",
             (FARPROC)hookedStretchBlt, (FARPROC*)&s_origStretchBlt);
    s_msvfwHooked = 1;
    dg_log("VIDEO: msvfw32 IAT hooked SetDIB=%p StretchDIB=%p BitBlt=%p StretchBlt=%p\n",
           s_origSetDIBitsToDevice, s_origStretchDIBits, s_origBitBlt, s_origStretchBlt);
}

/* Background watcher thread: polls for MCIWnd windows regardless of whether
 * grBufferSwap is being called. KQ8 stops rendering during FMV playback so
 * the present-loop scan can't see the video window. */
static volatile LONG s_videoWatcherStop = 0;
static HANDLE s_videoWatcherThread = NULL;

static DWORD WINAPI videoWatcherProc(LPVOID arg) {
    (void)arg;
    dg_log("VIDEO: watcher thread started\n");
    while (!s_videoWatcherStop) {
        if (g_dg.isOpen) {
            tryHookMsvfw32();
            scanForVideoChildren(g_dg.hwnd);
            if (s_lastStretchedVid && !IsWindow(s_lastStretchedVid)) {
                s_lastStretchedVid = NULL;
            }
        }
        Sleep(50);
    }
    dg_log("VIDEO: watcher thread exiting\n");
    return 0;
}

static void startVideoWatcher(void) {
    DWORD tid;
    if (s_videoWatcherThread) return;
    s_videoWatcherStop = 0;
    s_videoWatcherThread = CreateThread(NULL, 0, videoWatcherProc, NULL, 0, &tid);
    dg_log("VIDEO: watcher thread create -> %p (tid=%lu)\n",
           (void*)s_videoWatcherThread, tid);
}

static void stopVideoWatcher(void) {
    if (!s_videoWatcherThread) return;
    InterlockedExchange(&s_videoWatcherStop, 1);
    WaitForSingleObject(s_videoWatcherThread, 500);
    CloseHandle(s_videoWatcherThread);
    s_videoWatcherThread = NULL;
}

static HRESULT WINAPI hookedAVIFileOpenA(void* ppFile, LPCSTR name, UINT mode, LPVOID handler) {
    InterlockedIncrement(&s_videoPlaying);
    dg_log("AVI: file open '%s' (videoPlaying=%ld)\n", name ? name : "(null)", s_videoPlaying);
    return s_origAVIFileOpenA ? s_origAVIFileOpenA(ppFile, name, mode, handler) : E_FAIL;
}

static ULONG WINAPI hookedAVIFileRelease(void* pFile) {
    ULONG rc = s_origAVIFileRelease ? s_origAVIFileRelease(pFile) : 0;
    LONG remaining = InterlockedDecrement(&s_videoPlaying);
    if (remaining < 0) InterlockedExchange(&s_videoPlaying, 0);
    dg_log("AVI: file release (videoPlaying=%ld)\n", s_videoPlaying);
    return rc;
}

static HWND WINAPI hookedMCIWndCreateA(HWND parent, HINSTANCE inst, DWORD style, LPCSTR file) {
    /* Pure pass-through. Tried various manipulations (resize, style change,
     * returning NULL) to get KQ8's intro FMV to work on modern Windows —
     * all paths crash or hang at EIP=0x00400005 (inside KQ8's own PE header,
     * an indirect call through a corrupt pointer). The intro-replay path is
     * broken inside KQ8 itself. Documented as a known limitation. */
    dg_log("MCIWnd: create parent=0x%p style=0x%08X file='%s' (passthrough)\n",
           (void*)parent, style, file ? file : "(null)");
    return s_origMCIWndCreateA ? s_origMCIWndCreateA(parent, inst, style, file) : NULL;
}

static DWORD WINAPI hookedMciSendCommandA(DWORD dev, UINT cmd, DWORD_PTR p1, DWORD_PTR p2) {
    dg_log("MCI: sendCommand dev=%u cmd=0x%X p1=0x%p p2=0x%p\n",
           dev, cmd, (void*)p1, (void*)p2);
    return s_origMciSendCommandA ? s_origMciSendCommandA(dev, cmd, p1, p2) : 0;
}

static HWND WINAPI hookedCreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR winName,
                                          DWORD style, int x, int y, int width, int height,
                                          HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND rc = s_origCreateWindowExA ? s_origCreateWindowExA(exStyle, className, winName,
        style, x, y, width, height, parent, menu, inst, param) : NULL;
    if (rc && className) {
        /* className is either a string pointer or a registered class atom
         * (a small integer 0x0000..0xFFFF). Only interpret as string if > 0xFFFF. */
        int isString = (((ULONG_PTR)className) > 0xFFFF);
        const char* cn = isString ? className : "(atom)";
        dg_log("CreateWindowEx: cls='%s' name='%s' style=0x%08X size=%dx%d -> 0x%p\n",
               cn, winName ? winName : "(null)", style, width, height, (void*)rc);
        /* MCIWnd windows use class "MCIWndClass" */
        if (isString && strcmp(className, "MCIWndClass") == 0) {
            stretchVideoWindow(rc);
        }
    }
    return rc;
}

/* Translate game coords -> screen coords. Symmetric rounding pairs
 * with screenXToGameX/screenYToGameY so round-trip is stable. */
static void gameToScreen(int gx, int gy, int* sx, int* sy) {
    int destX, destY, destW, destH;
    getGameRect(&destX, &destY, &destW, &destH);
    *sx = destX + (gx * destW + g_dg.width  / 2) / g_dg.width;
    *sy = destY + (gy * destH + g_dg.height / 2) / g_dg.height;
}

static BOOL WINAPI hookedSetCursorPos(int x, int y) {
    if (!g_dg.isOpen || s_videoPlaying)
        return s_origSetCursorPos ? s_origSetCursorPos(x, y) : SetCursorPos(x, y);
    {
        int sx, sy;
        gameToScreen(x, y, &sx, &sy);
        {
            static int s_last_gx = -1, s_last_gy = -1;
            if (x != s_last_gx || y != s_last_gy) {
                dg_log("SetCursorPos: game=(%d,%d) -> screen=(%d,%d)\n", x, y, sx, sy);
                s_last_gx = x; s_last_gy = y;
            }
        }
        if (s_origSetCursorPos) return s_origSetCursorPos(sx, sy);
        return SetCursorPos(sx, sy);
    }
}

static BOOL WINAPI hookedClipCursor(const RECT* r) {
    if (!r || !g_dg.isOpen || s_videoPlaying)
        return s_origClipCursor ? s_origClipCursor(r) : ClipCursor(r);
    {
        RECT sr;
        int x0, y0, x1, y1;
        gameToScreen(r->left, r->top, &x0, &y0);
        gameToScreen(r->right, r->bottom, &x1, &y1);
        sr.left = x0; sr.top = y0; sr.right = x1; sr.bottom = y1;
        {
            static RECT s_lastGame = {0};
            if (r->left != s_lastGame.left || r->top != s_lastGame.top ||
                r->right != s_lastGame.right || r->bottom != s_lastGame.bottom) {
                dg_log("ClipCursor: game=(%d,%d)-(%d,%d) -> screen=(%d,%d)-(%d,%d)\n",
                       r->left, r->top, r->right, r->bottom, x0, y0, x1, y1);
                s_lastGame = *r;
            }
        }
        return s_origClipCursor ? s_origClipCursor(&sr) : ClipCursor(&sr);
    }
}

static BOOL WINAPI hookedGetCursorPos(LPPOINT pt) {
    BOOL result = s_origGetCursorPos ? s_origGetCursorPos(pt) : GetCursorPos(pt);
    if (s_videoPlaying) return result; /* video-playback path wants raw coords */
    if (result && pt && g_dg.hwnd && g_dg.isOpen) {
        int destX, destY, destW, destH;
        POINT winPt = *pt;
        /* screen -> client coords of game window */
        ScreenToClient(g_dg.hwnd, &winPt);
        getGameRect(&destX, &destY, &destW, &destH);
        if (destW > 0 && destH > 0) {
            int gx = screenXToGameX(winPt.x, destX, destW);
            int gy = screenYToGameY(winPt.y, destY, destH);
            /* Clamp minimum x to 1 (not 0) — see translateMouseLParam comment. */
            if (gx < 1) gx = 1; else if (gx >= g_dg.width)  gx = g_dg.width - 1;
            if (gy < 0) gy = 0; else if (gy >= g_dg.height) gy = g_dg.height - 1;
            pt->x = gx;
            pt->y = gy;
        }
    }
    return result;
}

/* Patch the game's IAT to redirect user32 functions */
static void patchIAT(HMODULE hModule, const char* dllName, const char* funcName, FARPROC newFunc, FARPROC* origFunc) {
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hModule;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_IMPORT_DESCRIPTOR imp;
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (PIMAGE_NT_HEADERS)((BYTE*)hModule + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) return;

    imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hModule +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    for (; imp->Name; imp++) {
        const char* name = (const char*)hModule + imp->Name;
        if (_stricmp(name, dllName) != 0) continue;
        {
            PIMAGE_THUNK_DATA oft = (PIMAGE_THUNK_DATA)((BYTE*)hModule + imp->OriginalFirstThunk);
            PIMAGE_THUNK_DATA ft  = (PIMAGE_THUNK_DATA)((BYTE*)hModule + imp->FirstThunk);
            for (; oft->u1.AddressOfData; oft++, ft++) {
                PIMAGE_IMPORT_BY_NAME ibn;
                if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
                ibn = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hModule + oft->u1.AddressOfData);
                if (strcmp((const char*)ibn->Name, funcName) == 0) {
                    DWORD oldProtect;
                    if (origFunc) *origFunc = (FARPROC)ft->u1.Function;
                    VirtualProtect(&ft->u1.Function, sizeof(ft->u1.Function), PAGE_READWRITE, &oldProtect);
                    ft->u1.Function = (ULONG_PTR)newFunc;
                    VirtualProtect(&ft->u1.Function, sizeof(ft->u1.Function), oldProtect, &oldProtect);
                    return;
                }
            }
        }
    }
}

static void hookMouseAPIs(void) {
    HMODULE gameMod = GetModuleHandleA(NULL); /* main EXE */
    patchIAT(gameMod, "user32.dll", "GetCursorPos", (FARPROC)hookedGetCursorPos, (FARPROC*)&s_origGetCursorPos);
    patchIAT(gameMod, "user32.dll", "SetCursorPos", (FARPROC)hookedSetCursorPos, (FARPROC*)&s_origSetCursorPos);
    patchIAT(gameMod, "user32.dll", "ClipCursor",   (FARPROC)hookedClipCursor,   (FARPROC*)&s_origClipCursor);
    /* Hook AVI playback so cursor translation can be bypassed during FMV.
     * Without this, KQ8 hangs when the game's video path queries cursor. */
    patchIAT(gameMod, "avifil32.dll", "AVIFileOpenA",   (FARPROC)hookedAVIFileOpenA,   (FARPROC*)&s_origAVIFileOpenA);
    patchIAT(gameMod, "avifil32.dll", "AVIFileRelease", (FARPROC)hookedAVIFileRelease, (FARPROC*)&s_origAVIFileRelease);
    /* Video-window-stretch hooks DISABLED — adding CreateWindowExA/MCIWndCreateA
     * hooks broke previously-working video playback (intro now crashes or hangs
     * instead of playing at native size). The stretch-to-fullscreen is not
     * worth the regression. Restore video to last-night's working baseline. */
    dg_log("  Mouse IAT hooks: Get=%p Set=%p Clip=%p\n",
           s_origGetCursorPos, s_origSetCursorPos, s_origClipCursor);
    dg_log("  AVI IAT hooks: Open=%p Release=%p\n",
           s_origAVIFileOpenA, s_origAVIFileRelease);
}

int dg_d3d11_init(HWND hwnd, int width, int height) {
    HRESULT hr;
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL featureLevel;

    dg_log("dg_d3d11_init: hwnd=0x%X, %dx%d\n", (unsigned)(UINT_PTR)hwnd, width, height);

    if (!hwnd) {
        hwnd = findGameWindow();
        dg_log("  Found game window: 0x%X\n", (unsigned)(UINT_PTR)hwnd);
    }
    if (!hwnd) {
        dg_log("ERROR: Could not find game window!\n");
        return 0;
    }

    g_dg.hwnd = hwnd;
    g_dg.width = width;     /* native game resolution — 640x480 */
    g_dg.height = height;
#if !defined(DG_DISABLE_HOOKS) && !defined(DG_DISABLE_FULLSCREEN)
    g_dg.screenWidth = GetSystemMetrics(SM_CXSCREEN);
    g_dg.screenHeight = GetSystemMetrics(SM_CYSCREEN);
#else
    /* Windowed build: keep swap chain at game resolution so KQ8's intro
     * mode-switches can work without fighting our borderless-fullscreen window. */
    g_dg.screenWidth = width;
    g_dg.screenHeight = height;
#endif
    dg_log("  Screen: %dx%d, game: %dx%d\n", g_dg.screenWidth, g_dg.screenHeight, width, height);

#if !defined(DG_DISABLE_HOOKS) && !defined(DG_DISABLE_FULLSCREEN)
    /* Resize the game window to cover the screen (borderless fullscreen).
     * Skip this if DG_DISABLE_FULLSCREEN is set — some KQ8 features (like the
     * replay-intro FMV playback) try to change display mode themselves and
     * hang if the game's window is already a borderless-fullscreen popup. */
    SetWindowLongPtr(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, g_dg.screenWidth, g_dg.screenHeight,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
#endif

#if !defined(DG_DISABLE_HOOKS) && !defined(DG_DISABLE_WNDPROC)
    /* Hook the window proc to translate mouse coords from screen to game space */
    g_dg.origWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)dgWndProc);
    dg_log("  Hooked WndProc (orig=%p)\n", g_dg.origWndProc);
#endif
#if !defined(DG_DISABLE_HOOKS) && !defined(DG_DISABLE_IAT_HOOKS)
    /* IAT-patch GetCursorPos in the game EXE to translate screen->game coords */
    hookMouseAPIs();
#endif

    /* Start background watcher for MCIWnd video windows (stretches intro FMV
     * to fullscreen). KQ8 stops calling grBufferSwap during video, so we can't
     * rely on the present loop to scan. */
    startVideoWatcher();

    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = 1;
    scd.BufferDesc.Width = (UINT)g_dg.screenWidth;   /* swap chain matches screen */
    scd.BufferDesc.Height = (UINT)g_dg.screenHeight;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    /* Find NVIDIA adapter explicitly to avoid Intel iGPU */
    {
        IDXGIFactory* factory = NULL;
        IDXGIAdapter* chosenAdapter = NULL;
        hr = CreateDXGIFactory(&IID_IDXGIFactory, (void**)&factory);
        if (SUCCEEDED(hr)) {
            IDXGIAdapter* adapter = NULL;
            UINT i = 0;
            while (IDXGIFactory_EnumAdapters(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND) {
                DXGI_ADAPTER_DESC desc;
                IDXGIAdapter_GetDesc(adapter, &desc);
                dg_log("  Adapter %d: %ls (VRAM: %uMB)\n", i,
                       desc.Description, (unsigned)(desc.DedicatedVideoMemory / (1024*1024)));
                /* Pick first adapter with significant VRAM (discrete GPU) */
                if (!chosenAdapter && desc.DedicatedVideoMemory > 512 * 1024 * 1024) {
                    chosenAdapter = adapter;
                    dg_log("  -> Selected as discrete GPU\n");
                } else {
                    IDXGIAdapter_Release(adapter);
                }
                i++;
            }
        }

        if (chosenAdapter) {
            hr = D3D11CreateDeviceAndSwapChain(chosenAdapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                NULL, 0, D3D11_SDK_VERSION, &scd, &g_dg.swapChain, &g_dg.device,
                &featureLevel, &g_dg.context);
            IDXGIAdapter_Release(chosenAdapter);
            if (SUCCEEDED(hr)) {
                dg_log("  Using NVIDIA HARDWARE renderer\n");
            }
        }

        if (!g_dg.device) {
            /* Fall back to default hardware, then WARP */
            hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                NULL, 0, D3D11_SDK_VERSION, &scd, &g_dg.swapChain, &g_dg.device,
                &featureLevel, &g_dg.context);
            if (SUCCEEDED(hr)) {
                dg_log("  Using default HARDWARE renderer\n");
            } else {
                hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
                    NULL, 0, D3D11_SDK_VERSION, &scd, &g_dg.swapChain, &g_dg.device,
                    &featureLevel, &g_dg.context);
                dg_log("  Using WARP renderer\n");
            }
        }

        if (factory) IDXGIFactory_Release(factory);
    }
    if (FAILED(hr)) {
        dg_log("ERROR: D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", hr);
        return 0;
    }
    dg_log("  D3D11 device created, feature level: 0x%X\n", featureLevel);

    /* Disable Alt+Enter fullscreen toggle */
    {
        IDXGIFactory* factory = NULL;
        IDXGIDevice* dxgiDevice = NULL;
        IDXGIAdapter* adapter = NULL;
        ID3D11Device_QueryInterface(g_dg.device, &IID_IDXGIDevice, (void**)&dxgiDevice);
        if (dxgiDevice) {
            IDXGIDevice_GetAdapter(dxgiDevice, &adapter);
            if (adapter) {
                IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void**)&factory);
                if (factory) {
                    IDXGIFactory_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
                    IDXGIFactory_Release(factory);
                }
                IDXGIAdapter_Release(adapter);
            }
            IDXGIDevice_Release(dxgiDevice);
        }
    }

    hr = IDXGISwapChain_GetBuffer(g_dg.swapChain, 0, &IID_ID3D11Texture2D, (void**)&g_dg.backBuffer);
    if (FAILED(hr)) { dg_log("ERROR: GetBuffer failed\n"); return 0; }

    hr = ID3D11Device_CreateRenderTargetView(g_dg.device, (ID3D11Resource*)g_dg.backBuffer, NULL, &g_dg.rtv);
    if (FAILED(hr)) { dg_log("ERROR: CreateRTV failed\n"); return 0; }

    /* SSAA 2x: render at 2x internal, box-filter downsample to native.
     * Averages out sub-pixel tri-edge seams; with proper aniso + Lanczos mips
     * active, the downsample preserves plenty of detail. */
    g_dg.msaaSamples = 1;
    g_dg.ssaaFactor = 2;
    g_dg.ssaaWidth = width * g_dg.ssaaFactor;
    g_dg.ssaaHeight = height * g_dg.ssaaFactor;
    dg_log("  AA: SSAA %dx (internal RT %dx%d → native %dx%d)\n",
           g_dg.ssaaFactor, g_dg.ssaaWidth, g_dg.ssaaHeight, width, height);

    {
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = (UINT)g_dg.ssaaWidth;
        td.Height = (UINT)g_dg.ssaaHeight;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture2D(g_dg.device, &td, NULL, &g_dg.gameTex);
        if (FAILED(hr)) { dg_log("ERROR: CreateGameTex failed\n"); return 0; }

        hr = ID3D11Device_CreateRenderTargetView(g_dg.device, (ID3D11Resource*)g_dg.gameTex, NULL, &g_dg.gameRtv);
        if (FAILED(hr)) { dg_log("ERROR: CreateGameRTV failed\n"); return 0; }

        hr = ID3D11Device_CreateShaderResourceView(g_dg.device,
            (ID3D11Resource*)g_dg.gameTex, NULL, &g_dg.gameSrv);
        if (FAILED(hr)) { dg_log("ERROR: CreateGameSRV failed\n"); return 0; }
        dg_log("  Offscreen SSAA RT created: %dx%d\n", g_dg.ssaaWidth, g_dg.ssaaHeight);

        /* Downsample target at native resolution */
        td.Width = (UINT)width;
        td.Height = (UINT)height;
        hr = ID3D11Device_CreateTexture2D(g_dg.device, &td, NULL, &g_dg.downsampledTex);
        if (FAILED(hr)) { dg_log("ERROR: CreateDownsampledTex failed\n"); return 0; }

        hr = ID3D11Device_CreateRenderTargetView(g_dg.device,
            (ID3D11Resource*)g_dg.downsampledTex, NULL, &g_dg.downsampledRtv);
        if (FAILED(hr)) { dg_log("ERROR: CreateDownsampledRTV failed\n"); return 0; }

        hr = ID3D11Device_CreateShaderResourceView(g_dg.device,
            (ID3D11Resource*)g_dg.downsampledTex, NULL, &g_dg.downsampledSrv);
        if (FAILED(hr)) { dg_log("ERROR: CreateDownsampledSRV failed\n"); return 0; }
        dg_log("  Downsample RT created: %dx%d\n", width, height);
    }

    if (!createDepthBuffer()) return 0;

    /* All game rendering goes to the offscreen target */
    ID3D11DeviceContext_OMSetRenderTargets(g_dg.context, 1, &g_dg.gameRtv, NULL);

    {
        /* Viewport matches the SSAA RT. The VS still maps game-space 640x480
         * to NDC (-1..1), which the rasterizer then fills across the SSAA viewport. */
        D3D11_VIEWPORT vp = { 0, 0, (float)g_dg.ssaaWidth, (float)g_dg.ssaaHeight, 0.0f, 1.0f };
        ID3D11DeviceContext_RSSetViewports(g_dg.context, 1, &vp);
    }

    if (!createLfbResources()) return 0;
    if (!createScenePipeline()) return 0;
    if (!createBlitResources()) return 0;

    dg_state_init(width, height);
    dg_tex_init();

    g_dg.isOpen = 1;
    dg_log("  D3D11 init complete (with 3D pipeline)\n");
    return 1;
}

/* ============================================================
 * Final blit (offscreen game RT → swap chain with letterboxing)
 * ============================================================ */

static int createBlitResources(void) {
    HRESULT hr;
    ID3DBlob* vsBlob = NULL;
    ID3DBlob* psBlob = NULL;
    D3D11_SAMPLER_DESC sd = {0};
    D3D11_BUFFER_DESC bd = {0};

    if (!compileShader(s_blitShaderSrc, "VSMain", "vs_4_0", &vsBlob)) return 0;
    if (!compileShader(s_blitShaderSrc, "PSMain", "ps_4_0", &psBlob)) { ID3D10Blob_Release(vsBlob); return 0; }

    hr = ID3D11Device_CreateVertexShader(g_dg.device,
        ID3D10Blob_GetBufferPointer(vsBlob), ID3D10Blob_GetBufferSize(vsBlob), NULL, &g_dg.blitVS);
    ID3D10Blob_Release(vsBlob);
    if (FAILED(hr)) { ID3D10Blob_Release(psBlob); return 0; }

    hr = ID3D11Device_CreatePixelShader(g_dg.device,
        ID3D10Blob_GetBufferPointer(psBlob), ID3D10Blob_GetBufferSize(psBlob), NULL, &g_dg.blitPS);
    ID3D10Blob_Release(psBlob);
    if (FAILED(hr)) return 0;

    /* Bilinear sampler: the shader snaps UVs but lets bilinear smooth texel edges */
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = ID3D11Device_CreateSamplerState(g_dg.device, &sd, &g_dg.blitSampler);
    if (FAILED(hr)) return 0;

    /* Constant buffer for target rect + texture size */
    bd.ByteWidth = 32; /* float4 rect + float4 texSize = 32 bytes */
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g_dg.device, &bd, NULL, &g_dg.blitCB);
    if (FAILED(hr)) return 0;

    /* Downsample shader + sampler (linear, clamp) */
    {
        ID3DBlob* dvsBlob = NULL;
        ID3DBlob* dpsBlob = NULL;
        D3D11_SAMPLER_DESC dsd = {0};
        if (!compileShader(s_downsampleShaderSrc, "VSMain", "vs_4_0", &dvsBlob)) return 0;
        if (!compileShader(s_downsampleShaderSrc, "PSMain", "ps_4_0", &dpsBlob)) {
            ID3D10Blob_Release(dvsBlob); return 0;
        }
        hr = ID3D11Device_CreateVertexShader(g_dg.device,
            ID3D10Blob_GetBufferPointer(dvsBlob), ID3D10Blob_GetBufferSize(dvsBlob), NULL, &g_dg.downsampleVS);
        ID3D10Blob_Release(dvsBlob);
        if (FAILED(hr)) { ID3D10Blob_Release(dpsBlob); return 0; }
        hr = ID3D11Device_CreatePixelShader(g_dg.device,
            ID3D10Blob_GetBufferPointer(dpsBlob), ID3D10Blob_GetBufferSize(dpsBlob), NULL, &g_dg.downsamplePS);
        ID3D10Blob_Release(dpsBlob);
        if (FAILED(hr)) return 0;

        dsd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        dsd.AddressU = dsd.AddressV = dsd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        dsd.MaxLOD = 0.0f;
        hr = ID3D11Device_CreateSamplerState(g_dg.device, &dsd, &g_dg.downsampleSampler);
        if (FAILED(hr)) return 0;
    }

    dg_log("  Blit resources created\n");
    return 1;
}

/* Post-process AA pass: gameSrv (raw 3D) → downsampledRtv (AA'd, same size).
 * Runs the FXAA shader (or box-downsample if ssaaFactor > 1). The HUD/LFB
 * then blits on top of downsampledRtv so it stays pixel-sharp. */
static void runAAPass(void) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    ID3D11ShaderResourceView* nullSrv = NULL;
    D3D11_VIEWPORT vp = { 0, 0, (float)g_dg.width, (float)g_dg.height, 0.0f, 1.0f };

    /* Update blitCB with SSAA source size (srcSize used by FXAA shader). */
    hr = ID3D11DeviceContext_Map(g_dg.context, (ID3D11Resource*)g_dg.blitCB,
        0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        float* f = (float*)mapped.pData;
        f[0] = 0; f[1] = 0; f[2] = 0; f[3] = 0;
        f[4] = (float)g_dg.ssaaWidth; f[5] = (float)g_dg.ssaaHeight;
        f[6] = 1.0f / (float)g_dg.ssaaWidth; f[7] = 1.0f / (float)g_dg.ssaaHeight;
        ID3D11DeviceContext_Unmap(g_dg.context, (ID3D11Resource*)g_dg.blitCB, 0);
    }

    ID3D11DeviceContext_OMSetRenderTargets(g_dg.context, 1, &g_dg.downsampledRtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_dg.context, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(g_dg.context, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_IASetInputLayout(g_dg.context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_dg.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(g_dg.context, g_dg.downsampleVS, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_dg.context, g_dg.downsamplePS, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_dg.context, 0, 1, &g_dg.blitCB);
    ID3D11DeviceContext_PSSetShaderResources(g_dg.context, 0, 1, &g_dg.gameSrv);
    ID3D11DeviceContext_PSSetSamplers(g_dg.context, 0, 1, &g_dg.downsampleSampler);
    ID3D11DeviceContext_Draw(g_dg.context, 4, 0);
    /* Unbind SRV so next pass can rebind the same texture as output if needed */
    ID3D11DeviceContext_PSSetShaderResources(g_dg.context, 0, 1, &nullSrv);
}

static void blitGameToScreen(void) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    float rectX0, rectY0, rectX1, rectY1;
    float gameAspect, screenAspect, scale;
    float destW, destH;
    D3D11_VIEWPORT vp;

    /* Compute aspect-preserved destination rect in NDC */
    gameAspect = (float)g_dg.width / (float)g_dg.height;
    screenAspect = (float)g_dg.screenWidth / (float)g_dg.screenHeight;

    if (screenAspect > gameAspect) {
        /* Screen wider than game — pillarbox (black bars on sides) */
        scale = (float)g_dg.screenHeight / (float)g_dg.height;
        destW = (float)g_dg.width * scale;
        destH = (float)g_dg.screenHeight;
    } else {
        /* Screen taller than game — letterbox (bars top/bottom) */
        scale = (float)g_dg.screenWidth / (float)g_dg.width;
        destW = (float)g_dg.screenWidth;
        destH = (float)g_dg.height * scale;
    }

    /* Convert destW/H to NDC coords centered on screen */
    rectX0 = -destW / (float)g_dg.screenWidth;
    rectX1 = +destW / (float)g_dg.screenWidth;
    rectY0 = -destH / (float)g_dg.screenHeight;
    rectY1 = +destH / (float)g_dg.screenHeight;

    /* Update blit CB */
    hr = ID3D11DeviceContext_Map(g_dg.context, (ID3D11Resource*)g_dg.blitCB,
        0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        float* f = (float*)mapped.pData;
        f[0] = rectX0; f[1] = rectY0; f[2] = rectX1; f[3] = rectY1;
        f[4] = (float)g_dg.width; f[5] = (float)g_dg.height;
        f[6] = 1.0f / (float)g_dg.width; f[7] = 1.0f / (float)g_dg.height;
        ID3D11DeviceContext_Unmap(g_dg.context, (ID3D11Resource*)g_dg.blitCB, 0);
    }

    /* Clear back buffer to black (for letterbox borders) */
    {
        float black[4] = {0, 0, 0, 1};
        ID3D11DeviceContext_ClearRenderTargetView(g_dg.context, g_dg.rtv, black);
    }

    /* Full-screen viewport */
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)g_dg.screenWidth;
    vp.Height = (float)g_dg.screenHeight;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_dg.context, 1, &vp);

    /* Bind swap chain back buffer + blit shader */
    ID3D11DeviceContext_OMSetRenderTargets(g_dg.context, 1, &g_dg.rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_dg.context, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_IASetInputLayout(g_dg.context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_dg.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(g_dg.context, g_dg.blitVS, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_dg.context, 0, 1, &g_dg.blitCB);
    ID3D11DeviceContext_PSSetShader(g_dg.context, g_dg.blitPS, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_dg.context, 0, 1, &g_dg.blitCB);
    ID3D11DeviceContext_PSSetShaderResources(g_dg.context, 0, 1, &g_dg.downsampledSrv);
    ID3D11DeviceContext_PSSetSamplers(g_dg.context, 0, 1, &g_dg.blitSampler);
    ID3D11DeviceContext_Draw(g_dg.context, 4, 0);

    /* Restore SSAA viewport for subsequent game draws */
    vp.Width = (float)g_dg.ssaaWidth;
    vp.Height = (float)g_dg.ssaaHeight;
    ID3D11DeviceContext_RSSetViewports(g_dg.context, 1, &vp);
}

/* ============================================================
 * Depth buffer
 * ============================================================ */

static int createDepthBuffer(void) {
    D3D11_TEXTURE2D_DESC td = {0};
    HRESULT hr;

    td.Width = (UINT)g_dg.ssaaWidth;
    td.Height = (UINT)g_dg.ssaaHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;  /* SSAA only, no MSAA */
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = ID3D11Device_CreateTexture2D(g_dg.device, &td, NULL, &g_dg.depthTex);
    if (FAILED(hr)) { dg_log("ERROR: CreateDepthTex failed\n"); return 0; }

    hr = ID3D11Device_CreateDepthStencilView(g_dg.device,
        (ID3D11Resource*)g_dg.depthTex, NULL, &g_dg.dsv);
    if (FAILED(hr)) { dg_log("ERROR: CreateDSV failed\n"); return 0; }

    dg_log("  Depth buffer created\n");
    return 1;
}

/* ============================================================
 * 3D Scene pipeline
 * ============================================================ */

static int createScenePipeline(void) {
    HRESULT hr;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;

    /* Shaders compiled lazily on first draw call to avoid D3DCompile conflicts */
    g_dg.sceneVS = NULL;
    g_dg.scenePS = NULL;
    g_dg.sceneLayout = NULL;

    /* Dynamic vertex buffer */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = DG_MAX_VERTICES * sizeof(DGVertex);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g_dg.device, &bd, NULL, &g_dg.vertexBuffer);
    if (FAILED(hr)) { dg_log("ERROR: CreateVB failed\n"); return 0; }

    /* Combiner constant buffer */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = (sizeof(DGCombinerCB) + 15) & ~15;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g_dg.device, &bd, NULL, &g_dg.combinerCB);
    if (FAILED(hr)) { dg_log("ERROR: CreateCB failed\n"); return 0; }

    /* Fog table CB — 64 floats packed as 16 float4s (256 bytes) */
    bd.ByteWidth = 256;
    hr = ID3D11Device_CreateBuffer(g_dg.device, &bd, NULL, &g_dg.fogTableCB);
    if (FAILED(hr)) { dg_log("ERROR: CreateFogCB failed\n"); return 0; }

    /* 8x anisotropic, no LOD bias. Mipmap quality is improved via custom
     * Kaiser-windowed sinc filtering at upload time (see dg_tex_download). */
    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    hr = ID3D11Device_CreateSamplerState(g_dg.device, &sd, &g_dg.samplerWrap);
    if (FAILED(hr)) { dg_log("ERROR: CreateSampler(wrap) failed\n"); return 0; }
    g_dg.samplerPoint = g_dg.samplerWrap;     /* alias */
    g_dg.samplerBilinear = g_dg.samplerWrap;  /* alias */

    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = ID3D11Device_CreateSamplerState(g_dg.device, &sd, &g_dg.samplerClamp);
    if (FAILED(hr)) { dg_log("ERROR: CreateSampler(clamp) failed\n"); return 0; }

    dg_log("  Scene pipeline created (shaders deferred)\n");
    return 1;
}

/* Compile scene shaders lazily on first draw call */
static int ensureSceneShaders(void) {
    HRESULT hr;
    ID3DBlob* vsBlob = NULL;
    ID3DBlob* psBlob = NULL;

    if (g_dg.sceneVS) return 1; /* already compiled */

    dg_log("  Compiling scene shaders...\n");

    /* ps_5_0 required for 'sample' interpolation modifier on PSIn */
    if (!compileShader(s_sceneShaderSrc, "VSMain", "vs_5_0", &vsBlob)) return 0;
    if (!compileShader(s_sceneShaderSrc, "PSMain", "ps_5_0", &psBlob)) {
        ID3D10Blob_Release(vsBlob); return 0;
    }

    hr = ID3D11Device_CreateVertexShader(g_dg.device,
        ID3D10Blob_GetBufferPointer(vsBlob), ID3D10Blob_GetBufferSize(vsBlob),
        NULL, &g_dg.sceneVS);
    if (FAILED(hr)) { dg_log("ERROR: CreateVS failed\n"); goto fail; }

    hr = ID3D11Device_CreatePixelShader(g_dg.device,
        ID3D10Blob_GetBufferPointer(psBlob), ID3D10Blob_GetBufferSize(psBlob),
        NULL, &g_dg.scenePS);
    if (FAILED(hr)) { dg_log("ERROR: CreatePS failed\n"); goto fail; }

    {
        D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32_FLOAT,           0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "OOW",      0, DXGI_FORMAT_R32_FLOAT,           0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        hr = ID3D11Device_CreateInputLayout(g_dg.device, layout, 5,
            ID3D10Blob_GetBufferPointer(vsBlob), ID3D10Blob_GetBufferSize(vsBlob),
            &g_dg.sceneLayout);
        if (FAILED(hr)) { dg_log("ERROR: CreateInputLayout failed\n"); goto fail; }
    }

    ID3D10Blob_Release(vsBlob);
    ID3D10Blob_Release(psBlob);
    dg_log("  Scene shaders compiled\n");
    return 1;

fail:
    if (vsBlob) ID3D10Blob_Release(vsBlob);
    if (psBlob) ID3D10Blob_Release(psBlob);
    return 0;
}

/* ============================================================
 * State application
 * ============================================================ */

static D3D11_BLEND mapBlend(int glideBlend, int isSrc) {
    /*
     * Glide blend values (from sdk2_glide.h):
     *   0 = ZERO
     *   1 = ONE / DST_COLOR (same value — "ONE" for dst factor, "DST_COLOR" for src)
     *   2 = SRC_COLOR (only valid as dst factor)
     *   3 = ONE_MINUS_SRC_COLOR / ONE_MINUS_DST_COLOR (context-dependent)
     *   4 = SRC_ALPHA
     *   5 = ONE_MINUS_SRC_ALPHA
     *   6 = DST_ALPHA
     *   7 = ONE_MINUS_DST_ALPHA
     *   F = ALPHA_SATURATE / PREFOG_COLOR
     */
    switch (glideBlend) {
        case 0x0: return D3D11_BLEND_ZERO;
        case 0x1: return isSrc ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_ONE;
        case 0x2: return D3D11_BLEND_SRC_COLOR;
        case 0x3: return isSrc ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_INV_SRC_COLOR;
        case 0x4: return D3D11_BLEND_ONE;  /* Glide spec says SRC_ALPHA, but our
                                            * alpha combiner pipeline emits low
                                            * alpha for opaque pixels; mapping
                                            * to ONE is an incidental workaround
                                            * that keeps KQ8 looking correct.
                                            * Proper fix requires auditing the
                                            * alpha combiner to match the spec. */
        case 0x5: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x6: return D3D11_BLEND_DEST_ALPHA;
        case 0x7: return D3D11_BLEND_INV_DEST_ALPHA;
        case 0xF: return D3D11_BLEND_SRC_ALPHA_SAT;
        default:  return D3D11_BLEND_ONE;
    }
}

static D3D11_COMPARISON_FUNC mapCmp(int glideCmp) {
    switch (glideCmp) {
        case GR_CMP_NEVER:    return D3D11_COMPARISON_NEVER;
        case GR_CMP_LESS:     return D3D11_COMPARISON_LESS;
        case GR_CMP_EQUAL:    return D3D11_COMPARISON_EQUAL;
        case GR_CMP_LEQUAL:   return D3D11_COMPARISON_LESS_EQUAL;
        case GR_CMP_GREATER:  return D3D11_COMPARISON_GREATER;
        case GR_CMP_NOTEQUAL: return D3D11_COMPARISON_NOT_EQUAL;
        case GR_CMP_GEQUAL:   return D3D11_COMPARISON_GREATER_EQUAL;
        case GR_CMP_ALWAYS:   return D3D11_COMPARISON_ALWAYS;
        default:              return D3D11_COMPARISON_ALWAYS;
    }
}

void dg_apply_state(void) {
    HRESULT hr;

    if (!g_dg.isOpen) return;

    /* Blend state */
    if (g_rs.blendDirty) {
        /* Pack blend state into a single key for caching */
        unsigned key = ((unsigned)(g_rs.blend.srcRGB & 0xF))
                     | ((unsigned)(g_rs.blend.dstRGB & 0xF) << 4)
                     | ((unsigned)(g_rs.blend.srcA   & 0xF) << 8)
                     | ((unsigned)(g_rs.blend.dstA   & 0xF) << 12)
                     | ((unsigned)(g_rs.colorMaskRGB ? 1 : 0) << 16)
                     | ((unsigned)(g_rs.colorMaskA   ? 1 : 0) << 17);
        ID3D11BlendState* bs = NULL;
        int i;
        for (i = 0; i < g_dg.blendCacheCount; i++) {
            if (g_dg.blendCache[i].key == key) { bs = g_dg.blendCache[i].state; break; }
        }
        if (!bs) {
            D3D11_BLEND_DESC bd = {0};
            bd.RenderTarget[0].BlendEnable = !(g_rs.blend.srcRGB == 0x1 && g_rs.blend.dstRGB == 0x0);
            bd.RenderTarget[0].SrcBlend = mapBlend(g_rs.blend.srcRGB, 1);
            bd.RenderTarget[0].DestBlend = mapBlend(g_rs.blend.dstRGB, 0);
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = mapBlend(g_rs.blend.srcA, 1);
            bd.RenderTarget[0].DestBlendAlpha = mapBlend(g_rs.blend.dstA, 0);
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = 0;
            if (g_rs.colorMaskRGB) bd.RenderTarget[0].RenderTargetWriteMask |= 0x07;
            if (g_rs.colorMaskA) bd.RenderTarget[0].RenderTargetWriteMask |= 0x08;
            hr = ID3D11Device_CreateBlendState(g_dg.device, &bd, &bs);
            if (SUCCEEDED(hr) && g_dg.blendCacheCount < 64) {
                g_dg.blendCache[g_dg.blendCacheCount].key = key;
                g_dg.blendCache[g_dg.blendCacheCount].state = bs;
                g_dg.blendCacheCount++;
            }
        }
        if (bs) {
            float blendFactor[4] = {0,0,0,0};
            ID3D11DeviceContext_OMSetBlendState(g_dg.context, bs, blendFactor, 0xFFFFFFFF);
            g_dg.currentBlend = bs;
        }
        g_rs.blendDirty = 0;
    }

    /* Depth-stencil state */
    if (g_rs.depthDirty) {
        unsigned key = ((unsigned)(g_rs.depthFunc & 0xF))
                     | ((unsigned)(g_rs.depthMode & 0xF) << 4)
                     | ((unsigned)(g_rs.depthMask ? 1 : 0) << 8);
        ID3D11DepthStencilState* ds = NULL;
        int i;
        for (i = 0; i < g_dg.depthCacheCount; i++) {
            if (g_dg.depthCache[i].key == key) { ds = g_dg.depthCache[i].state; break; }
        }
        if (!ds) {
            D3D11_DEPTH_STENCIL_DESC dd = {0};
            dd.DepthEnable = (g_rs.depthMode != GR_DEPTHBUFFER_DISABLE);
            dd.DepthWriteMask = g_rs.depthMask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = mapCmp(g_rs.depthFunc);
            dd.StencilEnable = FALSE;
            hr = ID3D11Device_CreateDepthStencilState(g_dg.device, &dd, &ds);
            if (SUCCEEDED(hr) && g_dg.depthCacheCount < 64) {
                g_dg.depthCache[g_dg.depthCacheCount].key = key;
                g_dg.depthCache[g_dg.depthCacheCount].state = ds;
                g_dg.depthCacheCount++;
            }
        }
        if (ds) {
            ID3D11DeviceContext_OMSetDepthStencilState(g_dg.context, ds, 0);
            g_dg.currentDepth = ds;
        }
        g_rs.depthDirty = 0;
    }

    /* Rasterizer state */
    if (g_rs.rasterDirty) {
        unsigned key = (unsigned)((int)g_rs.depthBias & 0xFFFF);
        ID3D11RasterizerState* rs = NULL;
        int i;
        for (i = 0; i < g_dg.rasterCacheCount; i++) {
            if (g_dg.rasterCache[i].key == key) { rs = g_dg.rasterCache[i].state; break; }
        }
        if (!rs) {
            D3D11_RASTERIZER_DESC rd = {0};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.FrontCounterClockwise = FALSE;
            rd.DepthBias = (INT)g_rs.depthBias;
            rd.ScissorEnable = FALSE;
            rd.DepthClipEnable = TRUE;
            rd.MultisampleEnable = FALSE; /* MSAA off — SSAA instead */
            rd.AntialiasedLineEnable = FALSE;
            hr = ID3D11Device_CreateRasterizerState(g_dg.device, &rd, &rs);
            if (SUCCEEDED(hr) && g_dg.rasterCacheCount < 32) {
                g_dg.rasterCache[g_dg.rasterCacheCount].key = key;
                g_dg.rasterCache[g_dg.rasterCacheCount].state = rs;
                g_dg.rasterCacheCount++;
            }
        }
        if (rs) {
            ID3D11DeviceContext_RSSetState(g_dg.context, rs);
            g_dg.currentRaster = rs;
        }
        g_rs.rasterDirty = 0;
    }

    /* Combiner constant buffer */
    if (g_rs.combinerDirty) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = ID3D11DeviceContext_Map(g_dg.context, (ID3D11Resource*)g_dg.combinerCB,
                                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, &g_rs.combiner, sizeof(DGCombinerCB));
            ID3D11DeviceContext_Unmap(g_dg.context, (ID3D11Resource*)g_dg.combinerCB, 0);
        }
        g_rs.combinerDirty = 0;
    }

    /* Fog table CB — pack 64 FxU8 entries as 64 floats in [0,1] across 16 float4s */
    if (g_rs.fogTableDirty) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = ID3D11DeviceContext_Map(g_dg.context, (ID3D11Resource*)g_dg.fogTableCB,
                                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            float* f = (float*)mapped.pData;
            int i;
            for (i = 0; i < 64; i++) f[i] = (float)g_rs.fogTable[i] / 255.0f;
            ID3D11DeviceContext_Unmap(g_dg.context, (ID3D11Resource*)g_dg.fogTableCB, 0);
        }
        g_rs.fogTableDirty = 0;
    }
}

/* ============================================================
 * 3D Drawing
 * ============================================================ */

static void convertVertex(DGVertex* out, const GrVertex* in) {
    float tmuOow;
    out->x = in->x;
    out->y = in->y;

    /* Depth: use oow for W-buffer (mode 2), ooz for Z-buffer */
    if (g_rs.depthMode == GR_DEPTHBUFFER_WBUFFER || g_rs.depthMode == GR_DEPTHBUFFER_WBUFFER_COMPARE_TO_BIAS) {
        /* oow is 1/w: near objects have larger oow, far have smaller.
         * D3D11 expects 0=near, 1=far with LESS comparison.
         * Use 1-oow to flip: near objects get small depth values. */
        out->depth = 1.0f - in->oow;
        if (out->depth < 0.0f) out->depth = 0.0f;
        if (out->depth > 1.0f) out->depth = 1.0f;
    } else {
        out->depth = (in->ooz > 0.0f) ? (in->ooz / 65535.0f) : 0.0f;
        if (out->depth > 1.0f) out->depth = 1.0f;
    }
    out->r = in->r / 255.0f;
    out->g = in->g / 255.0f;
    out->b = in->b / 255.0f;
    out->a = in->a / 255.0f;
    out->oow = in->oow;

    /* Use per-TMU oow; fall back to vertex oow only if TMU oow is exactly 0 */
    tmuOow = in->tmuvtx[0].oow;
    if (tmuOow == 0.0f) tmuOow = in->oow;

    out->sow0 = in->tmuvtx[0].sow;
    out->tow0 = in->tmuvtx[0].tow;
    out->oow0 = tmuOow;
}

/* Ring-buffer vertex position tracking */
static int g_vbWritePos = 0;

void dg_draw_triangle(const GrVertex* a, const GrVertex* b, const GrVertex* c) {
    DGVertex verts[3];
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    UINT stride, offset;
    D3D11_MAP mapType;

    if (!g_dg.isOpen || !a || !b || !c) return;
    if (!ensureSceneShaders()) return;
    g_dg.drewThisFrame = 1;

    {
        static int logCount = 0;
        static int drawCount = 0;
        drawCount++;

        if (logCount < 3) {
            dg_log("  draw_tri #%d: applying state\n", drawCount);
        }

        dg_apply_state();

        convertVertex(&verts[0], a);
        convertVertex(&verts[1], b);
        convertVertex(&verts[2], c);

        /* Log vertex data for early and mid-frame draws */
        if (drawCount == 100 || drawCount == 500 || drawCount == 1000 || drawCount == 5000) {
            dg_log("  Draw #%d: a_alpha=%.4f b_alpha=%.4f fogMode=%d fogColor=(%0.2f,%0.2f,%0.2f) blend=(%d,%d)\n",
                   drawCount, a->a, b->a,
                   g_rs.combiner.fogMode, g_rs.combiner.fogR, g_rs.combiner.fogG, g_rs.combiner.fogB,
                   g_rs.blend.srcRGB, g_rs.blend.dstRGB);
        }
        /* Track if we ever see non-zero alpha */
        {
            static int foundNonZeroAlpha = 0;
            if (!foundNonZeroAlpha && (a->a > 0.0f || b->a > 0.0f || c->a > 0.0f)) {
                foundNonZeroAlpha = 1;
                dg_log("  FIRST non-zero alpha at draw #%d: a=%.4f b=%.4f c=%.4f\n",
                       drawCount, a->a, b->a, c->a);
            }
        }
        if (logCount == 0 || drawCount == 100) {
            dg_log("  Draw #%d verts:\n", drawCount);
            dg_log("    V0: pos=(%.1f,%.1f) d=%.4f col=(%.2f,%.2f,%.2f,%.2f) tex=(%.2f,%.2f,%.4f)\n",
                   verts[0].x, verts[0].y, verts[0].depth,
                   verts[0].r, verts[0].g, verts[0].b, verts[0].a,
                   verts[0].sow0, verts[0].tow0, verts[0].oow0);
            dg_log("    V1: pos=(%.1f,%.1f) d=%.4f col=(%.2f,%.2f,%.2f,%.2f) tex=(%.2f,%.2f,%.4f)\n",
                   verts[1].x, verts[1].y, verts[1].depth,
                   verts[1].r, verts[1].g, verts[1].b, verts[1].a,
                   verts[1].sow0, verts[1].tow0, verts[1].oow0);
            dg_log("    Combiner: cFunc=%d cFact=%d cLocal=%d cOther=%d\n",
                   g_rs.combiner.colorFunc, g_rs.combiner.colorFactor,
                   g_rs.combiner.colorLocal, g_rs.combiner.colorOther);
        }

        if (logCount < 3) dg_log("  draw_tri: uploading verts\n");

        /* Ring buffer: use WRITE_DISCARD when buffer is full (rename), otherwise NO_OVERWRITE */
        if (g_vbWritePos + 3 > DG_MAX_VERTICES) {
            g_vbWritePos = 0;
            mapType = D3D11_MAP_WRITE_DISCARD;
        } else if (g_vbWritePos == 0) {
            mapType = D3D11_MAP_WRITE_DISCARD;
        } else {
            mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
        }

        hr = ID3D11DeviceContext_Map(g_dg.context, (ID3D11Resource*)g_dg.vertexBuffer,
                                     0, mapType, 0, &mapped);
        if (FAILED(hr)) { dg_log("  draw_tri: Map failed\n"); return; }
        memcpy((DGVertex*)mapped.pData + g_vbWritePos, verts, sizeof(verts));
        ID3D11DeviceContext_Unmap(g_dg.context, (ID3D11Resource*)g_dg.vertexBuffer, 0);

        if (logCount < 3) dg_log("  draw_tri: binding pipeline\n");

        stride = sizeof(DGVertex);
        offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_dg.context, 0, 1, &g_dg.vertexBuffer, &stride, &offset);
        ID3D11DeviceContext_IASetInputLayout(g_dg.context, g_dg.sceneLayout);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_dg.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(g_dg.context, g_dg.sceneVS, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_dg.context, g_dg.scenePS, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(g_dg.context, 0, 1, &g_dg.combinerCB);
        ID3D11DeviceContext_PSSetConstantBuffers(g_dg.context, 0, 1, &g_dg.combinerCB);
        ID3D11DeviceContext_PSSetConstantBuffers(g_dg.context, 1, 1, &g_dg.fogTableCB);

        {
            ID3D11ShaderResourceView* srv = dg_tex_get_srv(g_rs.texSourceAddr[0]);
            /* Select sampler based on game's grTexClampMode setting */
            ID3D11SamplerState* smp = (g_rs.texClampS[0] == GR_TEXTURECLAMP_CLAMP)
                                      ? g_dg.samplerClamp : g_dg.samplerWrap;
            if (logCount < 3 || (drawCount >= 100 && drawCount <= 103)) {
                dg_log("  draw_tri #%d: tex addr=0x%X srv=%p\n", drawCount, g_rs.texSourceAddr[0], srv);
            }
            if (srv == NULL && drawCount > 10 && g_rs.texSourceAddr[0] != 0) {
                static int missCount = 0;
                static FxU32 lastMissAddr = 0;
                if (g_rs.texSourceAddr[0] != lastMissAddr && missCount < 30) {
                    FxU32 wantAddr = g_rs.texSourceAddr[0];
                    FxU32 nearAddr = 0, nearEnd = 0;
                    int nearW = 0, nearH = 0, nearBpp = 0;
                    if (dg_tex_find_nearest_before(wantAddr, &nearAddr,
                                                    &nearW, &nearH,
                                                    &nearBpp, &nearEnd)) {
                        int inside = (wantAddr >= nearAddr && wantAddr < nearEnd);
                        dg_log("  TEX MISS #%d draw #%d wants=0x%X | nearest=0x%X %dx%d bpp=%d end=0x%X offset=+%d %s\n",
                               missCount, drawCount, wantAddr,
                               nearAddr, nearW, nearH, nearBpp, nearEnd,
                               (int)(wantAddr - nearAddr),
                               inside ? "INSIDE-RANGE" : "outside");
                    } else {
                        dg_log("  TEX MISS #%d draw #%d wants=0x%X (no prior cache entry)\n",
                               missCount, drawCount, wantAddr);
                    }
                    lastMissAddr = wantAddr;
                    missCount++;
                }
            }
            /* On miss, bind deterministic black texture instead of NULL */
            if (!srv) srv = dg_tex_get_miss_srv();
            ID3D11DeviceContext_PSSetShaderResources(g_dg.context, 0, 1, &srv);
            ID3D11DeviceContext_PSSetSamplers(g_dg.context, 0, 1, &smp);
        }

        ID3D11DeviceContext_OMSetRenderTargets(g_dg.context, 1, &g_dg.gameRtv, g_dg.dsv);

        if (logCount < 3) dg_log("  draw_tri: drawing\n");
        ID3D11DeviceContext_Draw(g_dg.context, 3, (UINT)g_vbWritePos);
        if (logCount < 3) dg_log("  draw_tri: done\n");

        g_vbWritePos += 3;
        logCount++;
    }
}

/* ============================================================
 * Present / Clear
 * ============================================================ */

void dg_d3d11_present(int swapInterval) {
    HRESULT hr;
    if (!g_dg.isOpen || !g_dg.swapChain) return;

    /* Step 1: AA pass (FXAA or SSAA downsample) writes gameTex → downsampledTex.
     * FXAA runs on the 3D-only image so the HUD stays sharp when overlaid next. */
    runAAPass();

    /* Step 2: Blit LFB/HUD onto the AA'd scene (now in downsampledRtv). */
    if (g_dg.lfbLockedThisFrame) {
        dg_lfb_flush();
    }

    /* Step 3: Sharp-bilinear upscale downsampledTex → swap chain back buffer. */
    blitGameToScreen();

    hr = IDXGISwapChain_Present(g_dg.swapChain, (UINT)(swapInterval > 0 ? 1 : 0), 0);
    if (FAILED(hr)) {
        dg_log("ERROR: Present failed: 0x%08X\n", hr);
        if (hr == 0x887A0005 /*DXGI_ERROR_DEVICE_REMOVED*/) {
            HRESULT reason = ID3D11Device_GetDeviceRemovedReason(g_dg.device);
            dg_log("  Device removed reason: 0x%08X\n", reason);
        }
    }

    /* Restore offscreen game RT for next frame */
    ID3D11DeviceContext_OMSetRenderTargets(g_dg.context, 1, &g_dg.gameRtv, g_dg.dsv);

    /* Wipe LFB only if the game actually wrote to it this frame (locked).
     * Fill with position-dependent canaries so unwritten pixels can be
     * detected without color collisions with game-written content. */
    if (g_dg.drewThisFrame && g_dg.lfbLockedThisFrame && g_dg.lfbCpuBuffer) {
        lfbFillCanary(g_dg.lfbCpuBuffer, (FxU32)(g_dg.width * 2), g_dg.width, g_dg.height);
        g_dg.lfbNeedsPopulate = 1;
    }
    g_dg.lfbLockedThisFrame = 0;
    g_dg.drewThisFrame = 0;
    g_dg.lfbDirty = 0;

    /* Advance texture cache LRU frame counter + opportunistic eviction */
    dg_tex_tick_frame();
}

void dg_d3d11_clear(float r, float g, float b, float a, float depth) {
    if (!g_dg.isOpen) return;
    {
        float color[4] = { r, g, b, a };
        ID3D11DeviceContext_ClearRenderTargetView(g_dg.context, g_dg.gameRtv, color);
    }
    if (g_dg.dsv)
        ID3D11DeviceContext_ClearDepthStencilView(g_dg.context, g_dg.dsv, D3D11_CLEAR_DEPTH, depth, 0);

    /* Clear LFB on grBufferClear — fill with position-dependent canary pattern
     * so unwritten pixels are detectable without conflicting with legitimate
     * game colors (e.g. KQ8 uses magenta 0xF81F as a "no save preview" color). */
    if (g_dg.lfbCpuBuffer) {
        lfbFillCanary(g_dg.lfbCpuBuffer, (FxU32)(g_dg.width * 2), g_dg.width, g_dg.height);
        g_dg.lfbNeedsPopulate = 1;
    }
    /* Intentionally do NOT wipe lfbStable here. KQ8 calls grBufferClear
     * every frame, so wiping stable would defeat cross-frame persistence
     * for HUD elements that are drawn once (XP bar, HP bar, crest). The
     * trade-off: transient content that lands inside the extended HUD
     * zone will leave residue until overwritten by new HUD writes. */
}

/* ============================================================
 * LFB implementation (unchanged from Phase 1)
 * ============================================================ */

static int createLfbResources(void) {
    HRESULT hr;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SAMPLER_DESC sd;
    ID3DBlob* vsBlob = NULL;
    ID3DBlob* psBlob = NULL;

    g_dg.lfbCpuBuffer = (FxU8*)calloc(1, g_dg.width * g_dg.height * 4);
    if (!g_dg.lfbCpuBuffer) return 0;

    /* HUD stable buffer — see dg_lfb_flush for what this is and why it exists. */
    g_dg.lfbStable = (FxU32*)calloc(1, g_dg.width * g_dg.height * 4);
    if (!g_dg.lfbStable) return 0;

    /* Mark LFB as needing population on the first Lock of the session. */
    g_dg.lfbNeedsPopulate = 1;

    memset(&td, 0, sizeof(td));
    td.Width = (UINT)g_dg.width;
    td.Height = (UINT)g_dg.height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateTexture2D(g_dg.device, &td, NULL, &g_dg.lfbTexture);
    if (FAILED(hr)) return 0;

    hr = ID3D11Device_CreateShaderResourceView(g_dg.device, (ID3D11Resource*)g_dg.lfbTexture, NULL, &g_dg.lfbSRV);
    if (FAILED(hr)) return 0;

    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = ID3D11Device_CreateSamplerState(g_dg.device, &sd, &g_dg.lfbSampler);
    if (FAILED(hr)) return 0;

    if (!compileShader(s_lfbShaderSrc, "VSMain", "vs_4_0", &vsBlob)) return 0;
    if (!compileShader(s_lfbShaderSrc, "PSMain", "ps_4_0", &psBlob)) { ID3D10Blob_Release(vsBlob); return 0; }

    hr = ID3D11Device_CreateVertexShader(g_dg.device,
        ID3D10Blob_GetBufferPointer(vsBlob), ID3D10Blob_GetBufferSize(vsBlob), NULL, &g_dg.lfbVS);
    ID3D10Blob_Release(vsBlob);
    if (FAILED(hr)) { ID3D10Blob_Release(psBlob); return 0; }

    hr = ID3D11Device_CreatePixelShader(g_dg.device,
        ID3D10Blob_GetBufferPointer(psBlob), ID3D10Blob_GetBufferSize(psBlob), NULL, &g_dg.lfbPS);
    ID3D10Blob_Release(psBlob);
    if (FAILED(hr)) return 0;

    dg_log("  LFB resources created\n");
    return 1;
}

void* dg_lfb_lock(GrLfbWriteMode_t writeMode, FxU32* outStride) {
    if (!g_dg.lfbCpuBuffer) return NULL;
    g_dg.lfbWriteMode = writeMode;
    g_dg.lfbLockedThisFrame = 1;
    /* Don't clear here — game may lock multiple times per frame */
    switch (writeMode) {
        case GR_LFBWRITEMODE_8888: g_dg.lfbStride = (FxU32)(g_dg.width * 4); break;
        case GR_LFBWRITEMODE_888:  g_dg.lfbStride = (FxU32)(g_dg.width * 3); break;
        default:                   g_dg.lfbStride = (FxU32)(g_dg.width * 2); break;
    }
    if (outStride) *outStride = g_dg.lfbStride;
    g_dg.lfbDirty = 1;
    return g_dg.lfbCpuBuffer;
}

void dg_lfb_unlock(void) { /* flush on present */ }

/* Read a region of the current backbuffer back as RGB565 pixels. Used by
 * KQ8 when saving a game to capture a thumbnail of the current scene. */
int dg_lfb_read_region(FxU32 srcX, FxU32 srcY, FxU32 srcW, FxU32 srcH,
                        FxU32 dstStride, void* dst) {
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11Texture2D* staging = NULL;
    HRESULT hr;
    FxU32 y, x;
    FxU8* dstBytes = (FxU8*)dst;
    int factor;

    if (!g_dg.isOpen || !g_dg.gameTex || !dst) return 0;
    if (srcW == 0 || srcH == 0) return 1;

    factor = g_dg.ssaaFactor > 0 ? g_dg.ssaaFactor : 1;

    /* Create a CPU-readable staging texture matching gameTex dims. */
    memset(&td, 0, sizeof(td));
    td.Width         = (UINT)g_dg.ssaaWidth;
    td.Height        = (UINT)g_dg.ssaaHeight;
    td.MipLevels     = 1;
    td.ArraySize     = 1;
    td.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage         = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(g_dg.device, &td, NULL, &staging);
    if (FAILED(hr)) { dg_log("ReadRegion: CreateTexture2D failed 0x%08X\n", hr); return 0; }

    ID3D11DeviceContext_CopyResource(g_dg.context,
        (ID3D11Resource*)staging, (ID3D11Resource*)g_dg.gameTex);

    hr = ID3D11DeviceContext_Map(g_dg.context, (ID3D11Resource*)staging,
                                  0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(staging);
        dg_log("ReadRegion: Map failed 0x%08X\n", hr);
        return 0;
    }

    for (y = 0; y < srcH; y++) {
        FxU16* dstRow = (FxU16*)(dstBytes + y * dstStride);
        FxU32 ssaaY = (srcY + y) * factor;
        FxU8* srcRow;
        if (ssaaY >= (FxU32)g_dg.ssaaHeight) break;
        srcRow = (FxU8*)mapped.pData + ssaaY * mapped.RowPitch;
        for (x = 0; x < srcW; x++) {
            FxU32 ssaaX = (srcX + x) * factor;
            FxU8* px;
            FxU16 r5, g6, b5;
            if (ssaaX >= (FxU32)g_dg.ssaaWidth) break;
            px = srcRow + ssaaX * 4;
            /* R8G8B8A8_UNORM stores bytes as R,G,B,A in memory. */
            r5 = (FxU16)(px[0] >> 3);
            g6 = (FxU16)(px[1] >> 2);
            b5 = (FxU16)(px[2] >> 3);
            dstRow[x] = (FxU16)((r5 << 11) | (g6 << 5) | b5);
        }
    }

    ID3D11DeviceContext_Unmap(g_dg.context, (ID3D11Resource*)staging, 0);
    ID3D11Texture2D_Release(staging);
    return 1;
}

/* Write a region of pixels into our LFB buffer, to be composited on the
 * next frame. Used by KQ8 to blit saved thumbnails on the save/load screen. */
int dg_lfb_write_region(FxU32 dstX, FxU32 dstY, GrLfbWriteMode_t writeMode,
                         FxU32 srcW, FxU32 srcH, FxU32 srcStride, const void* src) {
    FxU32 y, x;
    const FxU8* srcBytes = (const FxU8*)src;
    FxU32 lfbStride;

    if (!g_dg.isOpen || !g_dg.lfbCpuBuffer || !src) return 0;
    if (srcW == 0 || srcH == 0) return 1;

    /* WriteRegion always targets the native 565 LFB so dg_lfb_flush can
     * composite it correctly. Force stride to match. */
    lfbStride = (FxU32)(g_dg.width * 2);
    g_dg.lfbStride = lfbStride;
    g_dg.lfbWriteMode = GR_LFBWRITEMODE_565;
    g_dg.lfbDirty = 1;
    g_dg.lfbLockedThisFrame = 1;

    for (y = 0; y < srcH; y++) {
        int rowY = (int)(dstY + y);
        FxU16* dstRow;
        const FxU8* srcRow;
        if (rowY < 0 || rowY >= g_dg.height) continue;
        dstRow = (FxU16*)(g_dg.lfbCpuBuffer + rowY * lfbStride);
        srcRow = srcBytes + y * srcStride;

        for (x = 0; x < srcW; x++) {
            int colX = (int)(dstX + x);
            FxU16 pix565 = 0;
            if (colX < 0 || colX >= g_dg.width) continue;

            switch (writeMode) {
                case GR_LFBWRITEMODE_565:
                    pix565 = ((const FxU16*)srcRow)[x];
                    break;
                case GR_LFBWRITEMODE_555: {
                    FxU16 p = ((const FxU16*)srcRow)[x];
                    FxU16 r5 = (FxU16)((p >> 10) & 0x1F);
                    FxU16 g5 = (FxU16)((p >>  5) & 0x1F);
                    FxU16 b5 = (FxU16)( p        & 0x1F);
                    FxU16 g6 = (FxU16)((g5 << 1) | (g5 >> 4));
                    pix565 = (FxU16)((r5 << 11) | (g6 << 5) | b5);
                    break;
                }
                case GR_LFBWRITEMODE_1555: {
                    FxU16 p = ((const FxU16*)srcRow)[x];
                    FxU16 r5 = (FxU16)((p >> 10) & 0x1F);
                    FxU16 g5 = (FxU16)((p >>  5) & 0x1F);
                    FxU16 b5 = (FxU16)( p        & 0x1F);
                    FxU16 g6 = (FxU16)((g5 << 1) | (g5 >> 4));
                    pix565 = (FxU16)((r5 << 11) | (g6 << 5) | b5);
                    break;
                }
                case GR_LFBWRITEMODE_888: {
                    const FxU8* p = srcRow + x * 3;
                    /* 3dfx 888: b,g,r in memory order. */
                    pix565 = (FxU16)(((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[0] >> 3));
                    break;
                }
                case GR_LFBWRITEMODE_8888: {
                    const FxU8* p = srcRow + x * 4;
                    /* 3dfx 8888: b,g,r,a in memory order. */
                    pix565 = (FxU16)(((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[0] >> 3));
                    break;
                }
                default:
                    pix565 = ((const FxU16*)srcRow)[x];
                    break;
            }
            dstRow[colX] = pix565;
        }
    }
    return 1;
}

void dg_lfb_flush(void) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    int x, y;

    if (!g_dg.isOpen || !g_dg.lfbDirty || !g_dg.lfbCpuBuffer) return;

    hr = ID3D11DeviceContext_Map(g_dg.context, (ID3D11Resource*)g_dg.lfbTexture,
                                 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) { dg_log("  lfb_flush: Map failed 0x%08X\n", hr); return; }

    {
        FxU8* dst = (FxU8*)mapped.pData;
        FxU8* src = g_dg.lfbCpuBuffer;
        {
            /* -----------------------------------------------------------------------
             * HUD PERSISTENCE HACK
             * -----------------------------------------------------------------------
             * Problem: KQ8 draws certain static HUD elements (XP bar, decorative
             * face overlay on the KQ icon, etc.) only occasionally — typically once
             * when they first appear, and then NEVER redraws them in subsequent
             * frames. The main inventory UI bar IS redrawn every frame.
             *
             * Our per-frame LFB wipe (which is essential to prevent smearing of
             * dynamic elements like the cursor, dialogue text, subtitles) destroys
             * those static overlays. On frame N the game drew main UI + XP bar.
             * We wipe. On frame N+1 the game redraws only main UI. XP bar is now
             * sentinel → transparent → 3D shows through (often black at night).
             * End result: XP bar flickers out and stays missing.
             *
             * Canary is the sentinel for "game never wrote this pixel" — on
             * composite, canary pixels become transparent so the 3D scene
             * shows through; any other value is output as an opaque written
             * pixel. KQ8 uses 0xF81F magenta itself as a "no save preview"
             * placeholder, which is why the canary is position-dependent
             * rather than a fixed value.
             *
             * (This wrapper previously implemented a dual-edge HUD probe +
             * stable-buffer persistence hack to try to survive HUD elements
             * the game draws only once. It failed for KQ8's XP bar and
             * introduced smearing in enough edge cases that we reverted to
             * the simple one. Tracked as a known limitation.)
             * ----------------------------------------------------------------------- */
            int is3dFrame = g_dg.drewThisFrame;

            for (y = 0; y < g_dg.height; y++) {
                FxU32* dstRow = (FxU32*)(dst + y * mapped.RowPitch);
                FxU16* srcRow = (FxU16*)(src + y * g_dg.lfbStride);
                for (x = 0; x < g_dg.width; x++) {
                    FxU16 c = srcRow[x];
                    if (c == lfbCanary(x, y)) {
                        dstRow[x] = is3dFrame ? 0x00000000 : 0xFF000000;
                    } else {
                        FxU8 r = (FxU8)(((c >> 11) & 0x1F) * 255 / 31);
                        FxU8 g = (FxU8)(((c >> 5)  & 0x3F) * 255 / 63);
                        FxU8 b = (FxU8)(( c        & 0x1F) * 255 / 31);
                        dstRow[x] = 0xFF000000 | ((FxU32)b << 16) | ((FxU32)g << 8) | r;
                    }
                }
            }
        }
    }

    ID3D11DeviceContext_Unmap(g_dg.context, (ID3D11Resource*)g_dg.lfbTexture, 0);

    /* LFB blits onto the AA'd (post-FXAA) buffer so the HUD stays sharp —
     * final sharp-bilinear upscale to screen happens next in blitGameToScreen. */
    ID3D11DeviceContext_OMSetRenderTargets(g_dg.context, 1, &g_dg.downsampledRtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_dg.context, NULL, NULL, 0xFFFFFFFF); /* no blending */
    ID3D11DeviceContext_IASetInputLayout(g_dg.context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_dg.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_dg.context, g_dg.lfbVS, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_dg.context, g_dg.lfbPS, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_dg.context, 0, 1, &g_dg.lfbSRV);
    ID3D11DeviceContext_PSSetSamplers(g_dg.context, 0, 1, &g_dg.lfbSampler);
    ID3D11DeviceContext_Draw(g_dg.context, 3, 0);
    g_dg.lfbDirty = 0;
}

/* ============================================================
 * Shutdown
 * ============================================================ */

void dg_d3d11_shutdown(void) {
    dg_log("dg_d3d11_shutdown: begin\n");

    /* Mark closed FIRST so hooks stop translating */
    g_dg.isOpen = 0;

    /* Stop the video watcher thread before we tear anything else down. */
    stopVideoWatcher();

    /* Restore original WndProc so our proc doesn't get called after DLL unload */
    if (g_dg.origWndProc && g_dg.hwnd) {
        SetWindowLongPtr(g_dg.hwnd, GWLP_WNDPROC, (LONG_PTR)g_dg.origWndProc);
        g_dg.origWndProc = NULL;
    }

    /* Restore IAT hooks. If the game calls grSstWinOpen again after shutdown
     * (KQ8 does this for FMV playback), re-running patchIAT without restoring
     * first would save our own hooks as the "original" and create infinite
     * recursion on the next cursor call. */
    {
        HMODULE gameMod = GetModuleHandleA(NULL);
        if (s_origGetCursorPos) {
            patchIAT(gameMod, "user32.dll", "GetCursorPos", (FARPROC)s_origGetCursorPos, NULL);
            s_origGetCursorPos = NULL;
        }
        if (s_origSetCursorPos) {
            patchIAT(gameMod, "user32.dll", "SetCursorPos", (FARPROC)s_origSetCursorPos, NULL);
            s_origSetCursorPos = NULL;
        }
        if (s_origClipCursor) {
            patchIAT(gameMod, "user32.dll", "ClipCursor", (FARPROC)s_origClipCursor, NULL);
            s_origClipCursor = NULL;
        }
        if (s_origAVIFileOpenA) {
            patchIAT(gameMod, "avifil32.dll", "AVIFileOpenA", (FARPROC)s_origAVIFileOpenA, NULL);
            s_origAVIFileOpenA = NULL;
        }
        if (s_origAVIFileRelease) {
            patchIAT(gameMod, "avifil32.dll", "AVIFileRelease", (FARPROC)s_origAVIFileRelease, NULL);
            s_origAVIFileRelease = NULL;
        }
        /* Video-stretch hooks were disabled — nothing to un-hook here. */
    }

    dg_log("  shutdown: hooks restored\n");

    /* Flush pending GPU work before releasing resources — avoids driver-level hang */
    if (g_dg.context) {
        dg_log("  shutdown: ClearState\n");
        ID3D11DeviceContext_ClearState(g_dg.context);
        dg_log("  shutdown: Flush\n");
        ID3D11DeviceContext_Flush(g_dg.context);
        dg_log("  shutdown: flushed\n");
    }

    dg_log("  shutdown: dg_tex_shutdown\n");
    dg_tex_shutdown();
    dg_log("  shutdown: tex done\n");

    {
        int i;
        for (i = 0; i < g_dg.blendCacheCount; i++)
            if (g_dg.blendCache[i].state) ID3D11BlendState_Release(g_dg.blendCache[i].state);
        for (i = 0; i < g_dg.depthCacheCount; i++)
            if (g_dg.depthCache[i].state) ID3D11DepthStencilState_Release(g_dg.depthCache[i].state);
        for (i = 0; i < g_dg.rasterCacheCount; i++)
            if (g_dg.rasterCache[i].state) ID3D11RasterizerState_Release(g_dg.rasterCache[i].state);
        g_dg.blendCacheCount = 0;
        g_dg.depthCacheCount = 0;
        g_dg.rasterCacheCount = 0;
    }
    g_dg.currentBlend = NULL;
    g_dg.currentDepth = NULL;
    g_dg.currentRaster = NULL;

    if (g_dg.combinerCB)    { ID3D11Buffer_Release(g_dg.combinerCB);     g_dg.combinerCB = NULL; }
    if (g_dg.fogTableCB)    { ID3D11Buffer_Release(g_dg.fogTableCB);     g_dg.fogTableCB = NULL; }
    if (g_dg.vertexBuffer)  { ID3D11Buffer_Release(g_dg.vertexBuffer);   g_dg.vertexBuffer = NULL; }
    if (g_dg.sceneLayout)   { ID3D11InputLayout_Release(g_dg.sceneLayout); g_dg.sceneLayout = NULL; }
    if (g_dg.scenePS)       { ID3D11PixelShader_Release(g_dg.scenePS);   g_dg.scenePS = NULL; }
    if (g_dg.sceneVS)       { ID3D11VertexShader_Release(g_dg.sceneVS);  g_dg.sceneVS = NULL; }
    /* samplerPoint/Bilinear are aliases for samplerWrap — don't double-release */
    g_dg.samplerPoint = NULL;
    g_dg.samplerBilinear = NULL;
    if (g_dg.samplerWrap)  { ID3D11SamplerState_Release(g_dg.samplerWrap);  g_dg.samplerWrap = NULL; }
    if (g_dg.samplerClamp) { ID3D11SamplerState_Release(g_dg.samplerClamp); g_dg.samplerClamp = NULL; }

    if (g_dg.lfbSampler)   { ID3D11SamplerState_Release(g_dg.lfbSampler);  g_dg.lfbSampler = NULL; }
    if (g_dg.lfbPS)         { ID3D11PixelShader_Release(g_dg.lfbPS);       g_dg.lfbPS = NULL; }
    if (g_dg.lfbVS)         { ID3D11VertexShader_Release(g_dg.lfbVS);      g_dg.lfbVS = NULL; }
    if (g_dg.lfbSRV)        { ID3D11ShaderResourceView_Release(g_dg.lfbSRV); g_dg.lfbSRV = NULL; }
    if (g_dg.lfbTexture)    { ID3D11Texture2D_Release(g_dg.lfbTexture);    g_dg.lfbTexture = NULL; }
    if (g_dg.lfbCpuBuffer)  { free(g_dg.lfbCpuBuffer);                     g_dg.lfbCpuBuffer = NULL; }
    if (g_dg.lfbStable)     { free(g_dg.lfbStable);                        g_dg.lfbStable = NULL; }

    /* Blit + downsample resources */
    if (g_dg.blitCB)            { ID3D11Buffer_Release(g_dg.blitCB);           g_dg.blitCB = NULL; }
    if (g_dg.blitSampler)       { ID3D11SamplerState_Release(g_dg.blitSampler); g_dg.blitSampler = NULL; }
    if (g_dg.blitPS)            { ID3D11PixelShader_Release(g_dg.blitPS);       g_dg.blitPS = NULL; }
    if (g_dg.blitVS)            { ID3D11VertexShader_Release(g_dg.blitVS);      g_dg.blitVS = NULL; }
    if (g_dg.downsampleSampler) { ID3D11SamplerState_Release(g_dg.downsampleSampler); g_dg.downsampleSampler = NULL; }
    if (g_dg.downsamplePS)      { ID3D11PixelShader_Release(g_dg.downsamplePS); g_dg.downsamplePS = NULL; }
    if (g_dg.downsampleVS)      { ID3D11VertexShader_Release(g_dg.downsampleVS); g_dg.downsampleVS = NULL; }

    /* Offscreen game RT + downsample chain */
    if (g_dg.downsampledSrv) { ID3D11ShaderResourceView_Release(g_dg.downsampledSrv); g_dg.downsampledSrv = NULL; }
    if (g_dg.downsampledRtv) { ID3D11RenderTargetView_Release(g_dg.downsampledRtv);   g_dg.downsampledRtv = NULL; }
    if (g_dg.downsampledTex) { ID3D11Texture2D_Release(g_dg.downsampledTex);          g_dg.downsampledTex = NULL; }
    if (g_dg.gameSrv)       { ID3D11ShaderResourceView_Release(g_dg.gameSrv); g_dg.gameSrv = NULL; }
    if (g_dg.gameRtv)       { ID3D11RenderTargetView_Release(g_dg.gameRtv);    g_dg.gameRtv = NULL; }
    if (g_dg.gameResolved)  { ID3D11Texture2D_Release(g_dg.gameResolved);      g_dg.gameResolved = NULL; }
    if (g_dg.gameTex)       { ID3D11Texture2D_Release(g_dg.gameTex);           g_dg.gameTex = NULL; }

    dg_log("  shutdown: releasing dsv/depth/rtv\n");
    if (g_dg.dsv)           { ID3D11DepthStencilView_Release(g_dg.dsv);     g_dg.dsv = NULL; }
    if (g_dg.depthTex)      { ID3D11Texture2D_Release(g_dg.depthTex);      g_dg.depthTex = NULL; }
    if (g_dg.rtv)           { ID3D11RenderTargetView_Release(g_dg.rtv);     g_dg.rtv = NULL; }
    if (g_dg.backBuffer)    { ID3D11Texture2D_Release(g_dg.backBuffer);    g_dg.backBuffer = NULL; }
    /* Skip releasing swap chain, context, and device — they can hang on GPU sync.
     * The OS will clean them up on process exit. This is safe because the process
     * is terminating right after grGlideShutdown returns. */
    g_dg.swapChain = NULL;
    g_dg.context = NULL;
    g_dg.device = NULL;

    dg_log("dg_d3d11_shutdown: complete\n");
}
