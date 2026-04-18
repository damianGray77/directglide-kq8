/*
 * DirectGlide - Glide 2.x API exports
 * Phase 0: All functions stubbed with logging.
 * Functions that the game depends on for init return sensible defaults.
 */

#include "glide2x.h"
#include "d3d11_backend.h"
#include "d3d11_state.h"
#include "d3d11_texture.h"
#include "log.h"
#include <string.h>

/* ============================================================
 * Global state (minimal for Phase 0)
 * ============================================================ */

static int g_initialized = 0;
static int g_sstSelected = 0;
static int g_winOpen = 0;
static int g_width = 640;
static int g_height = 480;
static GrOriginLocation_t g_origin = GR_ORIGIN_UPPER_LEFT;
static GrErrorCallbackFnc_t g_errorCallback = NULL;

/* Simulated texture memory: 4MB, base address 0 */
#define TEX_MEM_SIZE  (4 * 1024 * 1024)
#define TEX_MEM_BASE  0

/* gu* texture management */
#define MAX_MIPMAPS 1024
static GrMipMapInfo g_mipmaps[MAX_MIPMAPS];
static int          g_mipmapCount = 0;
static FxU32        g_texAllocPtr[GLIDE_NUM_TMU]; /* next free address per TMU */

/* ============================================================
 * Init / System functions
 * ============================================================ */

void __stdcall grGlideInit(void) {
    DG_LOG_CALL("grGlideInit");
    g_initialized = 1;
    g_mipmapCount = 0;
    g_texAllocPtr[0] = TEX_MEM_BASE;
    g_texAllocPtr[1] = TEX_MEM_BASE;
}

void __stdcall grGlideShutdown(void) {
    DG_LOG_CALL("grGlideShutdown");
    dg_d3d11_shutdown();
    g_initialized = 0;
    g_winOpen = 0;
}

void __stdcall grGlideGetVersion(char* version) {
    DG_LOG_CALL("grGlideGetVersion");
    if (version) strcpy(version, "2.4");
}

void __stdcall grGlideGetState(GrState* state) {
    DG_LOG_STUB("grGlideGetState");
    if (state) memset(state, 0, sizeof(GrState));
}

void __stdcall grGlideSetState(const GrState* state) {
    DG_LOG_STUB("grGlideSetState");
    (void)state;
}

void __stdcall grGlideShamelessPlug(FxBool on) {
    DG_LOG_STUB("grGlideShamelessPlug");
    (void)on;
}

FxBool __stdcall grSstQueryBoards(GrHwConfiguration* hwConfig) {
    DG_LOG_CALL("grSstQueryBoards");
    if (hwConfig) {
        memset(hwConfig, 0, sizeof(GrHwConfiguration));
        hwConfig->num_sst = 1;
    }
    return FXTRUE;
}

FxBool __stdcall grSstQueryHardware(GrHwConfiguration* hwConfig) {
    DG_LOG_CALL("grSstQueryHardware");
    if (hwConfig) {
        memset(hwConfig, 0, sizeof(GrHwConfiguration));
        hwConfig->num_sst = 1;
        hwConfig->SSTs[0].type = GR_SSTTYPE_VOODOO2;
        hwConfig->SSTs[0].sstBoard.VoodooConfig.fbRam = 4;
        hwConfig->SSTs[0].sstBoard.VoodooConfig.fbiRev = 2;
        hwConfig->SSTs[0].sstBoard.VoodooConfig.nTmu = 1;
        hwConfig->SSTs[0].sstBoard.VoodooConfig.SliDetected = FXFALSE;
        hwConfig->SSTs[0].sstBoard.VoodooConfig.tmuConfig[0].tmuRev = 1;
        hwConfig->SSTs[0].sstBoard.VoodooConfig.tmuConfig[0].tmuRam = 4;
    }
    return FXTRUE;
}

void __stdcall grSstSelect(FxI32 which_sst) {
    DG_LOG_CALL("grSstSelect");
    g_sstSelected = which_sst;
}

GrContext_t __stdcall grSstWinOpen(FxU32 hWnd,
                                    GrScreenResolution_t res,
                                    GrScreenRefresh_t refresh,
                                    GrColorFormat_t colorFmt,
                                    GrOriginLocation_t origin,
                                    int numColorBuf,
                                    int numAuxBuf) {
    dg_log("CALL: grSstWinOpen(hWnd=0x%X, res=%d, refresh=%d, colorFmt=%d, origin=%d, nColor=%d, nAux=%d)\n",
           hWnd, res, refresh, colorFmt, origin, numColorBuf, numAuxBuf);

    GlideResolutionToWH(res, &g_width, &g_height);
    g_origin = origin;
    g_winOpen = 1;

    dg_log("  Resolution: %dx%d\n", g_width, g_height);

    /* Initialize D3D11 */
    if (!dg_d3d11_init((HWND)(UINT_PTR)hWnd, g_width, g_height)) {
        dg_log("ERROR: D3D11 init failed in grSstWinOpen\n");
        return 0;
    }
    g_dg.origin = origin;
    return 1;
}

void __stdcall grSstWinClose(void) {
    DG_LOG_CALL("grSstWinClose");
    dg_d3d11_shutdown();
    g_winOpen = 0;
}

FxBool __stdcall grSstControl(FxU32 code) {
    dg_log("STUB: grSstControl(%d)\n", code);
    return FXTRUE;
}

void __stdcall grSstIdle(void) {
    /* no-op: we're never busy */
}

FxBool __stdcall grSstIsBusy(void) {
    return FXFALSE;
}

FxU32 __stdcall grSstStatus(void) {
    return 0x0FFFF43F; /* Voodoo2-like status bits */
}

FxBool __stdcall grSstVRetraceOn(void) {
    return FXTRUE;
}

FxU32 __stdcall grSstVideoLine(void) {
    return 0;
}

FxU32 __stdcall grSstScreenWidth(void) {
    return (FxU32)g_width;
}

FxU32 __stdcall grSstScreenHeight(void) {
    return (FxU32)g_height;
}

void __stdcall grSstPerfStats(GrSstPerfStats_t* stats) {
    DG_LOG_STUB("grSstPerfStats");
    if (stats) memset(stats, 0, sizeof(GrSstPerfStats_t));
}

void __stdcall grSstResetPerfStats(void) {
    DG_LOG_STUB("grSstResetPerfStats");
}

void __stdcall grSstOrigin(GrOriginLocation_t origin) {
    dg_log("CALL: grSstOrigin(%d)\n", origin);
    g_origin = origin;
}

void __stdcall grSstConfigPipeline(FxU32 a, FxU32 b, FxU32 c) {
    DG_LOG_STUB("grSstConfigPipeline");
    (void)a; (void)b; (void)c;
}

void __stdcall grSstVidMode(FxU32 a, FxU32 b) {
    DG_LOG_STUB("grSstVidMode");
    (void)a; (void)b;
}

void __stdcall grErrorSetCallback(GrErrorCallbackFnc_t cb) {
    DG_LOG_CALL("grErrorSetCallback");
    g_errorCallback = cb;
}

void __stdcall grSplash(float x, float y, float w, float h, FxU32 frame) {
    DG_LOG_STUB("grSplash");
    (void)x; (void)y; (void)w; (void)h; (void)frame;
}

void __stdcall grHints(FxU32 type, FxU32 data) {
    dg_log("STUB: grHints(type=%d, data=0x%X)\n", type, data);
}

/* ============================================================
 * Buffer functions
 * ============================================================ */

void __stdcall grBufferClear(GrColor_t color, GrAlpha_t alpha, FxU32 depth) {
    DG_LOG_ONCE("grBufferClear");
    /* ARGB color to float components */
    float a = (float)((color >> 24) & 0xFF) / 255.0f;
    float r = (float)((color >> 16) & 0xFF) / 255.0f;
    float g = (float)((color >> 8)  & 0xFF) / 255.0f;
    float b = (float)( color        & 0xFF) / 255.0f;
    (void)alpha;
    dg_d3d11_clear(r, g, b, a, (float)depth / 65535.0f);
}

void __stdcall grBufferSwap(FxU32 swapInterval) {
    DG_LOG_ONCE("grBufferSwap");
    dg_d3d11_present((int)swapInterval);
}

int __stdcall grBufferNumPending(void) {
    return 0;
}

void __stdcall grClipWindow(FxU32 minX, FxU32 minY, FxU32 maxX, FxU32 maxY) {
    dg_log("CALL: grClipWindow(%d, %d, %d, %d)\n", minX, minY, maxX, maxY);
}

void __stdcall grRenderBuffer(GrBuffer_t buffer) {
    dg_log("CALL: grRenderBuffer(%d)\n", buffer);
}

/* ============================================================
 * State functions
 * ============================================================ */

void __stdcall grAlphaBlendFunction(GrAlphaBlendFnc_t rgbSrc, GrAlphaBlendFnc_t rgbDst,
                                     GrAlphaBlendFnc_t alphaSrc, GrAlphaBlendFnc_t alphaDst) {
    g_rs.blend.srcRGB = rgbSrc; g_rs.blend.dstRGB = rgbDst;
    g_rs.blend.srcA = alphaSrc; g_rs.blend.dstA = alphaDst;
    g_rs.blendDirty = 1;
}

void __stdcall grAlphaCombine(GrCombineFunction_t func, GrCombineFactor_t factor,
                               GrCombineLocal_t local, GrCombineOther_t other, FxBool invert) {
    g_rs.combiner.alphaFunc = func;
    g_rs.combiner.alphaFactor = factor;
    g_rs.combiner.alphaLocal = local;
    g_rs.combiner.alphaOther = other;
    g_rs.combiner.alphaInvert = invert;
    g_rs.combinerDirty = 1;
}

void __stdcall grAlphaControlsITRGBLighting(FxBool enable) {
    dg_log("STUB: grAlphaControlsITRGBLighting(%d)\n", enable);
}

void __stdcall grAlphaTestFunction(GrCmpFnc_t func) {
    g_rs.combiner.alphaTestFunc = func;
    g_rs.combinerDirty = 1;
}

void __stdcall grAlphaTestReferenceValue(GrAlpha_t value) {
    g_rs.combiner.alphaTestRef = (float)value / 255.0f;
    g_rs.combinerDirty = 1;
}

void __stdcall grColorCombine(GrCombineFunction_t func, GrCombineFactor_t factor,
                               GrCombineLocal_t local, GrCombineOther_t other, FxBool invert) {
    g_rs.combiner.colorFunc = func;
    g_rs.combiner.colorFactor = factor;
    g_rs.combiner.colorLocal = local;
    g_rs.combiner.colorOther = other;
    g_rs.combiner.colorInvert = invert;
    g_rs.combinerDirty = 1;
}

void __stdcall grColorMask(FxBool rgb, FxBool alpha) {
    g_rs.colorMaskRGB = rgb;
    g_rs.colorMaskA = alpha;
    g_rs.blendDirty = 1;
}

void __stdcall grConstantColorValue(GrColor_t color) {
    g_rs.combiner.constA = (float)((color >> 24) & 0xFF) / 255.0f;
    g_rs.combiner.constR = (float)((color >> 16) & 0xFF) / 255.0f;
    g_rs.combiner.constG = (float)((color >> 8)  & 0xFF) / 255.0f;
    g_rs.combiner.constB = (float)( color        & 0xFF) / 255.0f;
    g_rs.combinerDirty = 1;
}

void __stdcall grConstantColorValue4(float a, float r, float g, float b) {
    g_rs.combiner.constA = a / 255.0f;
    g_rs.combiner.constR = r / 255.0f;
    g_rs.combiner.constG = g / 255.0f;
    g_rs.combiner.constB = b / 255.0f;
    g_rs.combinerDirty = 1;
}

void __stdcall grCullMode(GrCullMode_t mode) {
    g_rs.cullMode = mode;
    g_rs.rasterDirty = 1;
}

void __stdcall grDepthBiasLevel(FxI32 level) {
    g_rs.depthBias = (float)level;
    g_rs.rasterDirty = 1;
}

void __stdcall grDepthBufferFunction(GrCmpFnc_t func) {
    g_rs.depthFunc = func;
    g_rs.depthDirty = 1;
}

void __stdcall grDepthBufferMode(GrDepthBufferMode_t mode) {
    g_rs.depthMode = mode;
    g_rs.depthDirty = 1;
}

void __stdcall grDepthMask(FxBool enable) {
    g_rs.depthMask = enable;
    g_rs.depthDirty = 1;
}

void __stdcall grDisableAllEffects(void) {
    DG_LOG_CALL("grDisableAllEffects");
}

void __stdcall grDitherMode(GrDitherMode_t mode) {
    dg_log("CALL: grDitherMode(%d)\n", mode);
}

void __stdcall grFogColorValue(GrColor_t color) {
    g_rs.combiner.fogR = (float)((color >> 16) & 0xFF) / 255.0f;
    g_rs.combiner.fogG = (float)((color >> 8)  & 0xFF) / 255.0f;
    g_rs.combiner.fogB = (float)( color        & 0xFF) / 255.0f;
    g_rs.combiner.fogA = (float)((color >> 24) & 0xFF) / 255.0f;
    g_rs.combinerDirty = 1;
    {
        static FxU32 lastColor = 0xFFFFFFFF;
        if (color != lastColor) {
            dg_log("grFogColorValue: 0x%08X (R=%.2f G=%.2f B=%.2f)\n",
                   color, g_rs.combiner.fogR, g_rs.combiner.fogG, g_rs.combiner.fogB);
            lastColor = color;
        }
    }
}

void __stdcall grFogMode(GrFogMode_t mode) {
    g_rs.combiner.fogMode = mode;
    g_rs.combinerDirty = 1;
    {
        static int lastMode = -1;
        if ((int)mode != lastMode) {
            dg_log("grFogMode: %d\n", mode);
            lastMode = mode;
        }
    }
}

void __stdcall grFogTable(const GrFog_t* table) {
    if (!table) return;
    memcpy(g_rs.fogTable, table, 64);
    g_rs.fogTableDirty = 1;
    {
        static int logged = 0;
        if (logged < 3) {
            int i;
            dg_log("grFogTable (call #%d):\n", logged + 1);
            for (i = 0; i < 64; i += 8) {
                dg_log("  [%02d..%02d]: %3d %3d %3d %3d %3d %3d %3d %3d\n", i, i+7,
                       table[i], table[i+1], table[i+2], table[i+3],
                       table[i+4], table[i+5], table[i+6], table[i+7]);
            }
            logged++;
        }
    }
}

void __stdcall grGammaCorrectionValue(float value) {
    g_rs.gamma = value;
}

void __stdcall grChromakeyMode(GrChromakeyMode_t mode) {
    g_rs.combiner.chromakeyEnable = mode;
    g_rs.combinerDirty = 1;
}

void __stdcall grChromakeyValue(GrColor_t value) {
    g_rs.combiner.chromaR = (float)((value >> 16) & 0xFF) / 255.0f;
    g_rs.combiner.chromaG = (float)((value >> 8)  & 0xFF) / 255.0f;
    g_rs.combiner.chromaB = (float)( value        & 0xFF) / 255.0f;
    g_rs.combinerDirty = 1;
}

/* ============================================================
 * Drawing functions
 * ============================================================ */

void __stdcall grDrawTriangle(const GrVertex* a, const GrVertex* b, const GrVertex* c) {
    dg_draw_triangle(a, b, c);
}

void __stdcall grDrawLine(const GrVertex* a, const GrVertex* b) {
    /* Draw as a degenerate triangle */
    (void)a; (void)b;
}

void __stdcall grDrawPoint(const GrVertex* a) {
    (void)a;
}

void __stdcall grDrawPolygon(int nVerts, int type, const GrVertex* verts) {
    int i;
    (void)type;
    if (!verts || nVerts < 3) return;
    for (i = 1; i < nVerts - 1; i++)
        dg_draw_triangle(&verts[0], &verts[i], &verts[i + 1]);
}

void __stdcall grDrawPolygonVertexList(int nVerts, const GrVertex* verts) {
    int i;
    if (!verts || nVerts < 3) return;
    for (i = 1; i < nVerts - 1; i++)
        dg_draw_triangle(&verts[0], &verts[i], &verts[i + 1]);
}

void __stdcall grDrawPlanarPolygon(int nVerts, int type, const GrVertex* verts) {
    grDrawPolygon(nVerts, type, verts);
}

void __stdcall grDrawPlanarPolygonVertexList(int nVerts, const GrVertex* verts) {
    grDrawPolygonVertexList(nVerts, verts);
}

void __stdcall grAADrawTriangle(const GrVertex* a, const GrVertex* b, const GrVertex* c,
                                 FxBool abAA, FxBool bcAA, FxBool caAA) {
    (void)abAA; (void)bcAA; (void)caAA;
    dg_draw_triangle(a, b, c);
}

void __stdcall grAADrawLine(const GrVertex* a, const GrVertex* b) {
    (void)a; (void)b;
}

void __stdcall grAADrawPoint(const GrVertex* a) {
    (void)a;
}

void __stdcall grAADrawPolygon(int nVerts, int type, const GrVertex* verts) {
    grDrawPolygon(nVerts, type, verts);
}

void __stdcall grAADrawPolygonVertexList(int nVerts, const GrVertex* verts) {
    grDrawPolygonVertexList(nVerts, verts);
}

/* ============================================================
 * Texture functions
 * ============================================================ */

FxU32 __stdcall grTexMinAddress(GrChipID_t tmu) {
    (void)tmu;
    return TEX_MEM_BASE;
}

FxU32 __stdcall grTexMaxAddress(GrChipID_t tmu) {
    (void)tmu;
    return TEX_MEM_BASE + TEX_MEM_SIZE - 1;
}

FxU32 __stdcall grTexCalcMemRequired(GrLOD_t smallLod, GrLOD_t largeLod,
                                      GrAspectRatio_t aspect, GrTextureFormat_t fmt) {
    /* Rough size estimate based on format and LOD range */
    int maxDim = 256 >> smallLod; /* smallest mip dimension */
    int topDim = 256 >> largeLod; /* largest mip dimension */
    int bpp = (fmt >= GR_TEXFMT_16BIT) ? 2 : 1;
    FxU32 total = 0;
    int dim;
    for (dim = topDim; dim >= maxDim && dim >= 1; dim >>= 1) {
        total += (FxU32)(dim * dim * bpp);
    }
    if (total == 0) total = 256;
    return total;
}

FxU32 __stdcall grTexTextureMemRequired(FxU32 oddEven, GrTexInfo* info) {
    (void)oddEven;
    if (!info) return 256;
    return grTexCalcMemRequired(info->smallLod, info->largeLod, info->aspectRatio, info->format);
}

void __stdcall grTexClampMode(GrChipID_t tmu, GrTextureClampMode_t sClamp, GrTextureClampMode_t tClamp) {
    if (tmu < GLIDE_NUM_TMU) {
        g_rs.texClampS[tmu] = sClamp;
        g_rs.texClampT[tmu] = tClamp;
        g_rs.samplerDirty = 1;
    }
}

void __stdcall grTexFilterMode(GrChipID_t tmu, GrTextureFilterMode_t minFilter, GrTextureFilterMode_t magFilter) {
    if (tmu < GLIDE_NUM_TMU) {
        g_rs.texMinFilter[tmu] = minFilter;
        g_rs.texMagFilter[tmu] = magFilter;
        g_rs.samplerDirty = 1;
    }
}

void __stdcall grTexMipMapMode(GrChipID_t tmu, GrMipMapMode_t mode, FxBool lodBlend) {
    if (tmu < GLIDE_NUM_TMU) g_rs.texMipMapMode[tmu] = mode;
    (void)lodBlend;
}

void __stdcall grTexLodBiasValue(GrChipID_t tmu, float bias) {
    if (tmu < GLIDE_NUM_TMU) g_rs.texLodBias[tmu] = bias;
}

void __stdcall grTexCombine(GrChipID_t tmu,
                             GrCombineFunction_t rgbFunc, GrCombineFactor_t rgbFactor,
                             GrCombineFunction_t alphaFunc, GrCombineFactor_t alphaFactor,
                             FxBool rgbInvert, FxBool alphaInvert) {
    g_rs.combiner.texFunc = rgbFunc;
    g_rs.combiner.texFactor = rgbFactor;
    g_rs.combiner.texRgbInvert = rgbInvert;
    g_rs.combiner.texAlphaInvert = alphaInvert;
    g_rs.combinerDirty = 1;
    (void)tmu; (void)alphaFunc; (void)alphaFactor;
}

void __stdcall grTexCombineFunction(GrChipID_t tmu, FxI32 func) {
    /* Map simplified combine function to low-level combine */
    (void)tmu;
    switch (func) {
        case GR_TEXTURECOMBINE_ZERO:
            g_rs.combiner.texFunc = GR_COMBINE_FUNCTION_ZERO;
            break;
        case GR_TEXTURECOMBINE_DECAL:
        case GR_TEXTURECOMBINE_ONE:
        default:
            g_rs.combiner.texFunc = GR_COMBINE_FUNCTION_LOCAL;
            break;
    }
    g_rs.combinerDirty = 1;
}

void __stdcall grTexDetailControl(GrChipID_t tmu, int lodBias, FxU8 detailScale, float detailMax) {
    dg_log("STUB: grTexDetailControl(tmu=%d)\n", tmu);
    (void)lodBias; (void)detailScale; (void)detailMax;
}

void __stdcall grTexDownloadMipMap(GrChipID_t tmu, FxU32 startAddr, FxU32 oddEven, GrTexInfo* info) {
    (void)oddEven;
    if (info && info->data) {
        dg_tex_download(tmu, startAddr, info->smallLod, info->largeLod, info->aspectRatio, info->format, info->data);
    }
}

void __stdcall grTexDownloadMipMapLevel(GrChipID_t tmu, FxU32 startAddr, GrLOD_t thisLod,
                                         GrLOD_t largeLod, GrAspectRatio_t aspect,
                                         GrTextureFormat_t fmt, FxU32 oddEven, void* data) {
    static int count = 0;
    (void)oddEven;
    if (count < 5) {
        dg_log("TEX LEVEL: addr=0x%X thisLod=%d largeLod=%d aspect=%d fmt=%d\n",
               startAddr, thisLod, largeLod, aspect, fmt);
        count++;
    }
    if (data) {
        dg_tex_download(tmu, startAddr, thisLod, thisLod, aspect, fmt, data);
    }
}

void __stdcall grTexDownloadMipMapLevelPartial(GrChipID_t tmu, FxU32 startAddr, GrLOD_t thisLod,
                                                GrLOD_t largeLod, GrAspectRatio_t aspect,
                                                GrTextureFormat_t fmt, FxU32 oddEven,
                                                void* data, int start, int end) {
    static int count = 0;
    (void)oddEven; (void)largeLod;
    if (count < 5) {
        dg_log("TEX PARTIAL: addr=0x%X lod=%d fmt=%d rows=%d-%d\n",
               startAddr, thisLod, fmt, start, end);
        count++;
    }
    /* TODO: only upload partial rows. For now upload full level. */
    if (data)
        dg_tex_download(tmu, startAddr, thisLod, thisLod, aspect, fmt, data);
}

void __stdcall grTexDownloadTable(GrChipID_t tmu, FxU32 type, void* data) {
    (void)tmu;
    if (type == GR_TEXTABLE_PALETTE && data)
        dg_tex_set_palette((const FxU32*)data);
}

void __stdcall grTexDownloadTablePartial(GrChipID_t tmu, FxU32 type, void* data, int start, int end) {
    dg_log("CALL: grTexDownloadTablePartial(tmu=%d, type=%d, %d-%d)\n", tmu, type, start, end);
    (void)data;
}

void __stdcall grTexNCCTable(GrChipID_t tmu, FxU32 table) {
    dg_log("STUB: grTexNCCTable(tmu=%d, table=%d)\n", tmu, table);
}

void __stdcall grTexSource(GrChipID_t tmu, FxU32 startAddr, FxU32 oddEven, GrTexInfo* info) {
    (void)oddEven; (void)info;
    if (tmu < GLIDE_NUM_TMU) {
        g_rs.texSourceAddr[tmu] = startAddr;
        g_rs.textureDirty = 1;
    }
}

void __stdcall grTexMultibase(GrChipID_t tmu, FxBool enable) {
    dg_log("STUB: grTexMultibase(tmu=%d, %d)\n", tmu, enable);
}

void __stdcall grTexMultibaseAddress(GrChipID_t tmu, FxU32 range, FxU32 startAddr,
                                      FxU32 oddEven, GrTexInfo* info) {
    dg_log("STUB: grTexMultibaseAddress(tmu=%d)\n", tmu);
    (void)range; (void)startAddr; (void)oddEven; (void)info;
}

FxBool __stdcall grCheckForRoom(FxI32 n) {
    (void)n;
    return FXTRUE;
}

/* ============================================================
 * LFB functions
 * ============================================================ */

FxBool __stdcall grLfbLock(GrLock_t type, GrBuffer_t buffer,
                            GrLfbWriteMode_t writeMode, GrOriginLocation_t origin,
                            FxBool pixelPipeline, GrLfbInfo_t* info) {
    FxU32 stride = 0;
    void* ptr;
    DG_LOG_ONCE("grLfbLock");
    (void)buffer; (void)origin; (void)pixelPipeline;

    ptr = dg_lfb_lock(writeMode, &stride);
    if (!ptr) return FXFALSE;

    /* If game wants to READ the framebuffer (e.g., to capture a save-file
     * thumbnail of the current 3D scene), populate the buffer with current
     * gameTex contents as RGB565. Without this, the game reads our canary
     * pattern and stores garbage in the save file. */
    if ((type & GR_LFB_WRITE_ONLY) == 0) {
        dg_lfb_read_region(0, 0, (FxU32)g_dg.width, (FxU32)g_dg.height,
                            (FxU32)(g_dg.width * 2), ptr);
    }

    if (info) {
        info->size = sizeof(GrLfbInfo_t);
        info->lfbPtr = ptr;
        info->strideInBytes = stride;
        info->writeMode = writeMode;
        info->origin = origin;
    }
    return FXTRUE;
}

FxBool __stdcall grLfbUnlock(GrLock_t type, GrBuffer_t buffer) {
    DG_LOG_ONCE("grLfbUnlock");
    (void)type; (void)buffer;
    dg_lfb_unlock();
    return FXTRUE;
}

FxBool __stdcall grLfbReadRegion(GrBuffer_t buffer, FxU32 srcX, FxU32 srcY,
                                  FxU32 srcW, FxU32 srcH, FxU32 dstStride, void* dst) {
    DG_LOG_ONCE("grLfbReadRegion");
    (void)buffer;
    return dg_lfb_read_region(srcX, srcY, srcW, srcH, dstStride, dst)
        ? FXTRUE : FXFALSE;
}

FxBool __stdcall grLfbWriteRegion(GrBuffer_t buffer, FxU32 dstX, FxU32 dstY,
                                   GrLfbWriteMode_t writeMode, FxU32 srcW, FxU32 srcH,
                                   FxU32 srcStride, void* src) {
    DG_LOG_ONCE("grLfbWriteRegion");
    (void)buffer;
    return dg_lfb_write_region(dstX, dstY, writeMode, srcW, srcH, srcStride, src)
        ? FXTRUE : FXFALSE;
}

void __stdcall grLfbConstantAlpha(GrAlpha_t alpha) {
    DG_LOG_ONCE("grLfbConstantAlpha");
    (void)alpha;
}

void __stdcall grLfbConstantDepth(FxU32 depth) {
    DG_LOG_ONCE("grLfbConstantDepth");
    (void)depth;
}

void __stdcall grLfbWriteColorFormat(GrColorFormat_t colorFmt) {
    dg_log("STUB: grLfbWriteColorFormat(%d)\n", colorFmt);
}

void __stdcall grLfbWriteColorSwizzle(FxBool swizzleBytes, FxBool swapWords) {
    dg_log("STUB: grLfbWriteColorSwizzle(%d, %d)\n", swizzleBytes, swapWords);
}

/* ============================================================
 * Stats
 * ============================================================ */

void __stdcall grResetTriStats(void) {
    DG_LOG_STUB("grResetTriStats");
}

void __stdcall grTriStats(FxU32* trisProcessed, FxU32* trisDrawn) {
    if (trisProcessed) *trisProcessed = 0;
    if (trisDrawn) *trisDrawn = 0;
}

/* ============================================================
 * gu* Utility functions
 * ============================================================ */

GrMipMapId_t __stdcall guTexAllocateMemory(GrChipID_t tmu,
    FxU8 oddEven, int width, int height,
    GrTextureFormat_t fmt, GrMipMapMode_t mmMode,
    GrLOD_t smallLod, GrLOD_t largeLod,
    GrAspectRatio_t aspect, GrTextureClampMode_t sClamp,
    GrTextureClampMode_t tClamp, GrTextureFilterMode_t minFilter,
    GrTextureFilterMode_t magFilter, float lodBias, FxBool lodBlend) {
    dg_log("CALL: guTexAllocateMemory(tmu=%d, %dx%d, fmt=%d)\n", tmu, width, height, fmt);
    (void)oddEven; (void)mmMode; (void)sClamp; (void)tClamp;
    (void)minFilter; (void)magFilter; (void)lodBias; (void)lodBlend;

    if (g_mipmapCount >= MAX_MIPMAPS) return (GrMipMapId_t)-1;

    FxU32 memReq = grTexCalcMemRequired(smallLod, largeLod, aspect, fmt);
    FxU32 addr = g_texAllocPtr[tmu];

    if (addr + memReq > TEX_MEM_SIZE) return (GrMipMapId_t)-1;

    GrMipMapId_t id = (GrMipMapId_t)g_mipmapCount;
    g_mipmaps[g_mipmapCount].tmu = tmu;
    g_mipmaps[g_mipmapCount].startAddress = addr;
    g_mipmaps[g_mipmapCount].evenOdd = oddEven;
    g_mipmaps[g_mipmapCount].info.smallLod = smallLod;
    g_mipmaps[g_mipmapCount].info.largeLod = largeLod;
    g_mipmaps[g_mipmapCount].info.aspectRatio = aspect;
    g_mipmaps[g_mipmapCount].info.format = fmt;
    g_mipmaps[g_mipmapCount].info.data = NULL;
    g_mipmaps[g_mipmapCount].memRequired = memReq;
    g_mipmapCount++;

    g_texAllocPtr[tmu] = addr + memReq;
    return id;
}

FxBool __stdcall guTexChangeAttributes(GrMipMapId_t id,
    int width, int height, GrTextureFormat_t fmt,
    GrMipMapMode_t mmMode, GrLOD_t smallLod, GrLOD_t largeLod,
    GrAspectRatio_t aspect, GrTextureClampMode_t sClamp,
    GrTextureClampMode_t tClamp, GrTextureFilterMode_t minFilter,
    GrTextureFilterMode_t magFilter) {
    DG_LOG_STUB("guTexChangeAttributes");
    (void)id; (void)width; (void)height; (void)fmt;
    (void)mmMode; (void)smallLod; (void)largeLod;
    (void)aspect; (void)sClamp; (void)tClamp; (void)minFilter; (void)magFilter;
    return FXTRUE;
}

void __stdcall guTexCombineFunction(GrChipID_t tmu, FxI32 func) {
    dg_log("CALL: guTexCombineFunction(tmu=%d, func=%d)\n", tmu, func);
}

void __stdcall guTexDownloadMipMap(GrMipMapId_t id, const void* src, const void* nccTable) {
    dg_log("CALL: guTexDownloadMipMap(id=%d, src=%p, count=%d)\n", id, src, g_mipmapCount);
    (void)nccTable;
    if (id >= 0 && id < g_mipmapCount && src) {
        GrMipMapInfo* mm = &g_mipmaps[id];
        dg_tex_download(mm->tmu, mm->startAddress,
                        mm->info.smallLod, mm->info.largeLod,
                        mm->info.aspectRatio, mm->info.format, src);
    }
}

void __stdcall guTexDownloadMipMapLevel(GrMipMapId_t id, GrLOD_t lod, const void** src) {
    if (id >= 0 && id < g_mipmapCount && src && *src) {
        GrMipMapInfo* mm = &g_mipmaps[id];
        dg_tex_download(mm->tmu, mm->startAddress,
                        lod, lod, mm->info.aspectRatio, mm->info.format, *src);
    }
}

GrMipMapId_t __stdcall guTexGetCurrentMipMap(GrChipID_t tmu) {
    (void)tmu;
    return 0;
}

GrMipMapInfo* __stdcall guTexGetMipMapInfo(GrMipMapId_t id) {
    if (id >= 0 && id < g_mipmapCount)
        return &g_mipmaps[id];
    return NULL;
}

FxU32 __stdcall guTexMemQueryAvail(GrChipID_t tmu) {
    if (tmu < GLIDE_NUM_TMU)
        return TEX_MEM_SIZE - g_texAllocPtr[tmu];
    return 0;
}

void __stdcall guTexMemReset(void) {
    DG_LOG_CALL("guTexMemReset");
    g_mipmapCount = 0;
    g_texAllocPtr[0] = TEX_MEM_BASE;
    g_texAllocPtr[1] = TEX_MEM_BASE;
}

void __stdcall guTexSource(GrMipMapId_t id) {
    dg_log("CALL: guTexSource(id=%d)\n", id);
    if (id >= 0 && id < g_mipmapCount) {
        GrMipMapInfo* mm = &g_mipmaps[id];
        grTexSource(mm->tmu, mm->startAddress, mm->evenOdd, &mm->info);
    }
}

void __stdcall guAlphaSource(FxI32 mode) {
    dg_log("CALL: guAlphaSource(%d)\n", mode);
}

void __stdcall guColorCombineFunction(FxI32 func) {
    dg_log("CALL: guColorCombineFunction(%d)\n", func);
}

/* ============================================================
 * gu* misc utilities
 * ============================================================ */

FxBool __stdcall gu3dfGetInfo(const char* filename, Gu3dfInfo* info) {
    DG_LOG_STUB("gu3dfGetInfo");
    (void)filename; (void)info;
    return FXFALSE;
}

FxBool __stdcall gu3dfLoad(const char* filename, Gu3dfInfo* info) {
    DG_LOG_STUB("gu3dfLoad");
    (void)filename; (void)info;
    return FXFALSE;
}

void __stdcall guAADrawTriangleWithClip(const GrVertex* a, const GrVertex* b, const GrVertex* c) {
    (void)a; (void)b; (void)c;
}

void __stdcall guDrawPolygonVertexListWithClip(int nVerts, const GrVertex* verts) {
    (void)nVerts; (void)verts;
}

void __stdcall guDrawTriangleWithClip(const GrVertex* a, const GrVertex* b, const GrVertex* c) {
    (void)a; (void)b; (void)c;
}

FxI32 __stdcall guEncodeRLE16(void* dst, void* src, FxU32 w, FxU32 h) {
    (void)dst; (void)src; (void)w; (void)h;
    return 0;
}

FxU32 __stdcall guEndianSwapBytes(FxU32 value) {
    return ((value & 0xFF) << 24) | ((value & 0xFF00) << 8) |
           ((value >> 8) & 0xFF00) | ((value >> 24) & 0xFF);
}

FxU16 __stdcall guEndianSwapWords(FxU16 value) {
    return (FxU16)((value << 8) | (value >> 8));
}

void __stdcall guFbReadRegion(int srcX, int srcY, int w, int h, void* dst, int dstStride) {
    (void)srcX; (void)srcY; (void)w; (void)h; (void)dst; (void)dstStride;
}

void __stdcall guFbWriteRegion(int dstX, int dstY, int w, int h, void* src, int srcStride) {
    (void)dstX; (void)dstY; (void)w; (void)h; (void)src; (void)srcStride;
}

/* Glide fog W lookup table — canonical values from OpenGlide's OGLFogTables.cpp.
 * Maps 64 fog-table indices to the eye-space W value where each entry "sits". */
static const float g_fogIndexToW[64] = {
    1.000000f,   1.142857f,   1.333333f,   1.600000f,
    2.000000f,   2.285714f,   2.666667f,   3.200000f,
    4.000000f,   4.571429f,   5.333333f,   6.400000f,
    8.000000f,   9.142858f,  10.666667f,  12.800000f,
   16.000000f,  18.285715f,  21.333334f,  25.600000f,
   32.000000f,  36.571430f,  42.666668f,  51.200001f,
   64.000000f,  73.142860f,  85.333336f, 102.400002f,
  128.000000f, 146.285721f, 170.666672f, 204.800003f,
  256.000000f, 292.571442f, 341.333344f, 409.600006f,
  512.000000f, 585.142883f, 682.666687f, 819.200012f,
 1024.000000f,1170.285767f,1365.333374f,1638.400024f,
 2048.000000f,2340.571533f,2730.666748f,3276.800049f,
 4096.000000f,4681.143066f,5461.333496f,6553.600098f,
 8192.000000f,9362.286133f,10922.666992f,13107.200195f,
16384.000000f,18724.572266f,21845.333984f,26214.400391f,
32768.000000f,37449.144531f,43690.667969f,52428.800781f
};

void __stdcall guFogGenerateExp(GrFog_t* table, float density) {
    /* Per OpenGlide: f = (1 - e^(-density*W)) * scale where scale normalizes to 255 at W(63) */
    float dpEnd = density * g_fogIndexToW[63];
    float scale = 255.0f / (1.0f - (float)exp(-dpEnd));
    int i;
    if (!table) return;
    for (i = 0; i < 64; i++) {
        float dp = density * g_fogIndexToW[i];
        float f  = (1.0f - (float)exp(-dp)) * scale;
        if (f > 255.0f) f = 255.0f;
        else if (f < 0.0f) f = 0.0f;
        table[i] = (GrFog_t)f;
    }
    dg_log("guFogGenerateExp density=%.4f → table[0]=%d table[32]=%d table[63]=%d\n",
           density, (int)table[0], (int)table[32], (int)table[63]);
}

void __stdcall guFogGenerateExp2(GrFog_t* table, float density) {
    int i;
    if (!table) return;
    for (i = 0; i < 64; i++) {
        float e  = (float)exp(-density * g_fogIndexToW[i]);
        float f  = (1.0f - e * e) * 255.0f;
        if (f > 255.0f) f = 255.0f;
        else if (f < 0.0f) f = 0.0f;
        table[i] = (GrFog_t)f;
    }
    dg_log("guFogGenerateExp2 density=%.4f\n", density);
}

void __stdcall guFogGenerateLinear(GrFog_t* table, float nearW, float farW) {
    int start, end, i;
    if (!table) return;
    for (start = 0; start < 64; start++) if (g_fogIndexToW[start] >= nearW) break;
    for (end   = 0; end   < 64; end++)   if (g_fogIndexToW[end]   >= farW)  break;
    memset(table, 0, 64);
    if (end > start) {
        for (i = start; i <= end && i < 64; i++) {
            float f = 255.0f * (float)(i - start) / (float)(end - start);
            if (f > 255.0f) f = 255.0f;
            table[i] = (GrFog_t)f;
        }
    }
    for (i = end; i < 64; i++) table[i] = 255;
    dg_log("guFogGenerateLinear near=%.2f far=%.2f (start=%d end=%d)\n",
           nearW, farW, start, end);
}

float __stdcall guFogTableIndexToW(int i) {
    if (i < 0) i = 0;
    if (i >= 64) i = 63;
    return g_fogIndexToW[i];
}

void __stdcall guMPDrawTriangle(const GrVertex* a, const GrVertex* b, const GrVertex* c) {
    (void)a; (void)b; (void)c;
}

void __stdcall guMPInit(void) {
    DG_LOG_STUB("guMPInit");
}

void __stdcall guMPTexCombineFunction(FxI32 func) {
    (void)func;
}

void __stdcall guMPTexSource(GrChipID_t tmu, GrMipMapId_t id) {
    (void)tmu; (void)id;
}

void __stdcall guMovieSetName(const char* name) {
    DG_LOG_STUB("guMovieSetName");
    (void)name;
}

void __stdcall guMovieStart(void) {
    DG_LOG_STUB("guMovieStart");
}

void __stdcall guMovieStop(void) {
    DG_LOG_STUB("guMovieStop");
}

void __stdcall guTexCreateColorMipMap(void) {
    DG_LOG_STUB("guTexCreateColorMipMap");
}

/* ============================================================
 * ConvertAndDownloadRle
 * ============================================================ */

void __stdcall ConvertAndDownloadRle(GrChipID_t tmu, FxU32 startAddr,
    GrLOD_t thisLod, GrLOD_t largeLod, GrAspectRatio_t aspect,
    GrTextureFormat_t fmt, FxU32 oddEven, FxU32 oddEven2,
    void* data, int start, int end,
    FxU32 offset, void* palette,
    void* ncc0, void* ncc1, void* src) {
    DG_LOG_STUB("ConvertAndDownloadRle");
    (void)tmu; (void)startAddr; (void)thisLod; (void)largeLod;
    (void)aspect; (void)fmt; (void)oddEven; (void)oddEven2;
    (void)data; (void)start; (void)end; (void)offset;
    (void)palette; (void)ncc0; (void)ncc1; (void)src;
}

/* ============================================================
 * PCI functions (all stubs)
 * ============================================================ */

void __stdcall pciClose(void) {}
FxBool __stdcall pciDeviceExists(FxU32 id) { (void)id; return FXFALSE; }
FxBool __stdcall pciFindCard(FxU32 a, FxU32 b, FxU32 c) { (void)a; (void)b; (void)c; return FXFALSE; }
FxBool __stdcall pciFindCardMulti(FxU32 a, FxU32 b, FxU32 c, FxU32 d) { (void)a; (void)b; (void)c; (void)d; return FXFALSE; }
FxBool __stdcall pciFindFreeMTRR(FxU32 a) { (void)a; return FXFALSE; }
FxBool __stdcall pciFindMTRRMatch(FxU32 a, FxU32 b, FxU32 c, FxU32 d) { (void)a; (void)b; (void)c; (void)d; return FXFALSE; }
FxBool __stdcall pciGetConfigData(FxU32 a, FxU32 b, FxU32 c, FxU32 d, void* e) { (void)a; (void)b; (void)c; (void)d; (void)e; return FXFALSE; }
FxU32  __stdcall pciGetErrorCode(void) { return 0; }
const char* __stdcall pciGetErrorString(void) { return "No error"; }
FxBool __stdcall pciMapCard(FxU32 a, FxU32 b, FxU32 c, FxU32 d, FxU32 e) { (void)a; (void)b; (void)c; (void)d; (void)e; return FXFALSE; }
FxBool __stdcall pciMapCardMulti(FxU32 a, FxU32 b, FxU32 c, FxU32 d, FxU32 e, FxU32 f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return FXFALSE; }
FxBool __stdcall pciMapPhysicalToLinear(FxU32 a, FxU32 b, FxU32* c) { (void)a; (void)b; (void)c; return FXFALSE; }
FxBool __stdcall pciOpen(void) { return FXTRUE; }
void   __stdcall pciOutputDebugString(const char* s) { (void)s; }
FxBool __stdcall pciSetConfigData(FxU32 a, FxU32 b, FxU32 c, FxU32 d, void* e) { (void)a; (void)b; (void)c; (void)d; (void)e; return FXFALSE; }
FxBool __stdcall pciSetMTRR(FxU32 a, FxU32 b, FxU32 c, FxU32 d) { (void)a; (void)b; (void)c; (void)d; return FXFALSE; }
FxBool __stdcall pciSetPassThroughBase(FxU32 a) { (void)a; return FXFALSE; }
FxBool __stdcall pciUnmapPhysical(FxU32 a, FxU32 b) { (void)a; (void)b; return FXFALSE; }
