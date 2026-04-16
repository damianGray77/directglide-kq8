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
    "struct VSIn {\n"
    "    float2 pos   : POSITION;\n"
    "    float  depth : TEXCOORD3;\n"
    "    float4 color : COLOR;\n"
    "    float  oow   : OOW;\n"
    "    float3 tmu0  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "struct PSIn {\n"
    "    float4 pos   : SV_POSITION;\n"
    "    float4 color : COLOR;\n"
    "    float  oow   : OOW;\n"
    "    float3 tmu0  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "PSIn VSMain(VSIn v) {\n"
    "    PSIn o;\n"
    "    o.pos.x = v.pos.x * viewport.z * 2.0 - 1.0;\n"
    "    o.pos.y = 1.0 - v.pos.y * viewport.w * 2.0;\n"
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
    "    /* Fog disabled — needs correct fog factor calculation */\n"
    "\n"
    "    finalColor = saturate(finalColor);\n"
    "    /* (placeholder for fog) */\n"
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
    g_dg.width = width;
    g_dg.height = height;

    /* Don't touch the window — let the game manage it */
    dg_log("  Using game window as-is\n");

    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = 1;
    scd.BufferDesc.Width = (UINT)width;
    scd.BufferDesc.Height = (UINT)height;
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

    if (!createDepthBuffer()) return 0;

    ID3D11DeviceContext_OMSetRenderTargets(g_dg.context, 1, &g_dg.rtv, NULL);

    {
        D3D11_VIEWPORT vp = { 0, 0, (float)width, (float)height, 0.0f, 1.0f };
        ID3D11DeviceContext_RSSetViewports(g_dg.context, 1, &vp);
    }

    if (!createLfbResources()) return 0;
    if (!createScenePipeline()) return 0;

    dg_state_init(width, height);
    dg_tex_init();

    g_dg.isOpen = 1;
    dg_log("  D3D11 init complete (with 3D pipeline)\n");
    return 1;
}

/* ============================================================
 * Depth buffer
 * ============================================================ */

static int createDepthBuffer(void) {
    D3D11_TEXTURE2D_DESC td = {0};
    HRESULT hr;

    td.Width = (UINT)g_dg.width;
    td.Height = (UINT)g_dg.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
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

    /* Samplers */
    memset(&sd, 0, sizeof(sd));
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = ID3D11Device_CreateSamplerState(g_dg.device, &sd, &g_dg.samplerPoint);
    if (FAILED(hr)) { dg_log("ERROR: CreateSampler(point) failed\n"); return 0; }

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    hr = ID3D11Device_CreateSamplerState(g_dg.device, &sd, &g_dg.samplerBilinear);
    if (FAILED(hr)) { dg_log("ERROR: CreateSampler(bilinear) failed\n"); return 0; }

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

    if (!compileShader(s_sceneShaderSrc, "VSMain", "vs_4_0", &vsBlob)) return 0;
    if (!compileShader(s_sceneShaderSrc, "PSMain", "ps_4_0", &psBlob)) {
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
     * Glide blend values (from real SDK):
     * 0 = ZERO
     * 1 = DST_COLOR(src) / SRC_COLOR(dst) - context dependent
     * 2 = SRC_COLOR (dst only, rarely used for src)
     * 3 = ONE_MINUS_SRC_COLOR / ONE_MINUS_DST_COLOR
     * 4 = ONE (also SRC_ALPHA — same value in Glide, treat as ONE)
     * 5 = ONE_MINUS_SRC_ALPHA
     * 6 = DST_ALPHA
     * 7 = ONE_MINUS_DST_ALPHA
     * F = ALPHA_SATURATE
     */
    switch (glideBlend) {
        case 0x0: return D3D11_BLEND_ZERO;
        case 0x1: return isSrc ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_SRC_COLOR;
        case 0x2: return D3D11_BLEND_SRC_COLOR;
        case 0x3: return isSrc ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_INV_SRC_COLOR;
        case 0x4: return D3D11_BLEND_ONE;  /* ONE and SRC_ALPHA share value 4; default to ONE */
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
        D3D11_BLEND_DESC bd = {0};
        if (g_dg.currentBlend) { ID3D11BlendState_Release(g_dg.currentBlend); g_dg.currentBlend = NULL; }

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

        hr = ID3D11Device_CreateBlendState(g_dg.device, &bd, &g_dg.currentBlend);
        if (SUCCEEDED(hr)) {
            float blendFactor[4] = {0,0,0,0};
            ID3D11DeviceContext_OMSetBlendState(g_dg.context, g_dg.currentBlend, blendFactor, 0xFFFFFFFF);
        }
        g_rs.blendDirty = 0;
    }

    /* Depth-stencil state */
    if (g_rs.depthDirty) {
        D3D11_DEPTH_STENCIL_DESC dd = {0};
        if (g_dg.currentDepth) { ID3D11DepthStencilState_Release(g_dg.currentDepth); g_dg.currentDepth = NULL; }

        dd.DepthEnable = (g_rs.depthMode != GR_DEPTHBUFFER_DISABLE);
        dd.DepthWriteMask = g_rs.depthMask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = mapCmp(g_rs.depthFunc);
        dd.StencilEnable = FALSE;

        hr = ID3D11Device_CreateDepthStencilState(g_dg.device, &dd, &g_dg.currentDepth);
        if (SUCCEEDED(hr))
            ID3D11DeviceContext_OMSetDepthStencilState(g_dg.context, g_dg.currentDepth, 0);
        g_rs.depthDirty = 0;
    }

    /* Rasterizer state */
    if (g_rs.rasterDirty) {
        D3D11_RASTERIZER_DESC rd = {0};
        if (g_dg.currentRaster) { ID3D11RasterizerState_Release(g_dg.currentRaster); g_dg.currentRaster = NULL; }

        rd.FillMode = D3D11_FILL_SOLID;
        /* Disable culling — Glide winding doesn't match D3D11 reliably */
        rd.CullMode = D3D11_CULL_NONE;
        rd.FrontCounterClockwise = FALSE;
        rd.DepthBias = (INT)g_rs.depthBias;
        rd.ScissorEnable = FALSE;
        rd.DepthClipEnable = TRUE;

        hr = ID3D11Device_CreateRasterizerState(g_dg.device, &rd, &g_dg.currentRaster);
        if (SUCCEEDED(hr))
            ID3D11DeviceContext_RSSetState(g_dg.context, g_dg.currentRaster);
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

void dg_draw_triangle(const GrVertex* a, const GrVertex* b, const GrVertex* c) {
    DGVertex verts[3];
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    UINT stride, offset;

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

        hr = ID3D11DeviceContext_Map(g_dg.context, (ID3D11Resource*)g_dg.vertexBuffer,
                                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) { dg_log("  draw_tri: Map failed\n"); return; }
        memcpy(mapped.pData, verts, sizeof(verts));
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

        {
            ID3D11ShaderResourceView* srv = dg_tex_get_srv(g_rs.texSourceAddr[0]);
            ID3D11SamplerState* smp = (g_rs.texMinFilter[0] == GR_TEXTUREFILTER_BILINEAR)
                                      ? g_dg.samplerBilinear : g_dg.samplerPoint;
            if (logCount < 3 || (drawCount >= 100 && drawCount <= 103)) {
                dg_log("  draw_tri #%d: tex addr=0x%X srv=%p\n", drawCount, g_rs.texSourceAddr[0], srv);
            }
            if (srv == NULL && drawCount > 10 && g_rs.texSourceAddr[0] != 0) {
                static int missCount = 0;
                static FxU32 lastMissAddr = 0;
                if (g_rs.texSourceAddr[0] != lastMissAddr && missCount < 20) {
                    dg_log("  TEX MISS: draw #%d wants addr=0x%X (unique miss #%d)\n",
                           drawCount, g_rs.texSourceAddr[0], missCount);
                    lastMissAddr = g_rs.texSourceAddr[0];
                    missCount++;
                }
            }
            if (srv)
                ID3D11DeviceContext_PSSetShaderResources(g_dg.context, 0, 1, &srv);
            ID3D11DeviceContext_PSSetSamplers(g_dg.context, 0, 1, &smp);
        }

        ID3D11DeviceContext_OMSetRenderTargets(g_dg.context, 1, &g_dg.rtv, g_dg.dsv);

        if (logCount < 3) dg_log("  draw_tri: drawing\n");
        ID3D11DeviceContext_Draw(g_dg.context, 3, 0);
        if (logCount < 3) dg_log("  draw_tri: done\n");

        logCount++;
    }
}

/* ============================================================
 * Present / Clear
 * ============================================================ */

void dg_d3d11_present(int swapInterval) {
    HRESULT hr;
    if (!g_dg.isOpen || !g_dg.swapChain) return;

    /* Blit LFB when written */
    if (g_dg.lfbLockedThisFrame) {
        dg_lfb_flush();
    }

    hr = IDXGISwapChain_Present(g_dg.swapChain, (UINT)(swapInterval > 0 ? 1 : 0), 0);
    if (FAILED(hr)) {
        dg_log("ERROR: Present failed: 0x%08X\n", hr);
        if (hr == 0x887A0005 /*DXGI_ERROR_DEVICE_REMOVED*/) {
            HRESULT reason = ID3D11Device_GetDeviceRemovedReason(g_dg.device);
            dg_log("  Device removed reason: 0x%08X\n", reason);
        }
    }

    /* Reset per-frame tracking — DON'T clear LFB, let it persist like real framebuffer */
    g_dg.lfbLockedThisFrame = 0;
    g_dg.drewThisFrame = 0;
    g_dg.lfbDirty = 0;
}

void dg_d3d11_clear(float r, float g, float b, float a, float depth) {
    if (!g_dg.isOpen) return;
    {
        float color[4] = { r, g, b, a };
        ID3D11DeviceContext_ClearRenderTargetView(g_dg.context, g_dg.rtv, color);
    }
    if (g_dg.dsv)
        ID3D11DeviceContext_ClearDepthStencilView(g_dg.context, g_dg.dsv, D3D11_CLEAR_DEPTH, depth, 0);

    /* Clear LFB on grBufferClear so stale menu content doesn't cover 3D */
    if (g_dg.lfbCpuBuffer)
        memset(g_dg.lfbCpuBuffer, 0, g_dg.width * g_dg.height * 4);
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

void dg_lfb_flush(void) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    int x, y;

    if (!g_dg.isOpen || !g_dg.lfbDirty || !g_dg.lfbCpuBuffer) return;

    dg_log("  lfb_flush: mapping texture\n");
    hr = ID3D11DeviceContext_Map(g_dg.context, (ID3D11Resource*)g_dg.lfbTexture,
                                 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) { dg_log("  lfb_flush: Map failed 0x%08X\n", hr); return; }

    {
        FxU8* dst = (FxU8*)mapped.pData;
        FxU8* src = g_dg.lfbCpuBuffer;
        {
            int is3dFrame = g_dg.drewThisFrame;
            for (y = 0; y < g_dg.height; y++) {
                FxU32* dstRow = (FxU32*)(dst + y * mapped.RowPitch);
                FxU16* srcRow = (FxU16*)(src + y * g_dg.lfbStride);
                for (x = 0; x < g_dg.width; x++) {
                    FxU16 c = srcRow[x];
                    FxU8 r = (FxU8)(((c >> 11) & 0x1F) * 255 / 31);
                    FxU8 g = (FxU8)(((c >> 5)  & 0x3F) * 255 / 63);
                    FxU8 b = (FxU8)(( c        & 0x1F) * 255 / 31);
                    if (is3dFrame && c == 0) {
                        /* During gameplay: unwritten pixels are transparent (3D shows through) */
                        dstRow[x] = 0x00000000;
                    } else {
                        /* Menu/loading OR written pixel: fully opaque */
                        dstRow[x] = 0xFF000000 | ((FxU32)b << 16) | ((FxU32)g << 8) | r;
                    }
                }
            }
        }
    }

    ID3D11DeviceContext_Unmap(g_dg.context, (ID3D11Resource*)g_dg.lfbTexture, 0);
    dg_log("  lfb_flush: drawing quad\n");

    /* Opaque LFB blit — covers entire screen (menu/loading frames) */
    ID3D11DeviceContext_OMSetRenderTargets(g_dg.context, 1, &g_dg.rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_dg.context, NULL, NULL, 0xFFFFFFFF); /* no blending */
    ID3D11DeviceContext_IASetInputLayout(g_dg.context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_dg.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_dg.context, g_dg.lfbVS, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_dg.context, g_dg.lfbPS, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_dg.context, 0, 1, &g_dg.lfbSRV);
    ID3D11DeviceContext_PSSetSamplers(g_dg.context, 0, 1, &g_dg.lfbSampler);
    ID3D11DeviceContext_Draw(g_dg.context, 3, 0);
    dg_log("  lfb_flush: done\n");
    g_dg.lfbDirty = 0;
}

/* ============================================================
 * Shutdown
 * ============================================================ */

void dg_d3d11_shutdown(void) {
    dg_log("dg_d3d11_shutdown\n");
    dg_tex_shutdown();

    if (g_dg.currentBlend)  { ID3D11BlendState_Release(g_dg.currentBlend);  g_dg.currentBlend = NULL; }
    if (g_dg.currentDepth)  { ID3D11DepthStencilState_Release(g_dg.currentDepth);  g_dg.currentDepth = NULL; }
    if (g_dg.currentRaster) { ID3D11RasterizerState_Release(g_dg.currentRaster); g_dg.currentRaster = NULL; }

    if (g_dg.combinerCB)    { ID3D11Buffer_Release(g_dg.combinerCB);     g_dg.combinerCB = NULL; }
    if (g_dg.vertexBuffer)  { ID3D11Buffer_Release(g_dg.vertexBuffer);   g_dg.vertexBuffer = NULL; }
    if (g_dg.sceneLayout)   { ID3D11InputLayout_Release(g_dg.sceneLayout); g_dg.sceneLayout = NULL; }
    if (g_dg.scenePS)       { ID3D11PixelShader_Release(g_dg.scenePS);   g_dg.scenePS = NULL; }
    if (g_dg.sceneVS)       { ID3D11VertexShader_Release(g_dg.sceneVS);  g_dg.sceneVS = NULL; }
    if (g_dg.samplerPoint)  { ID3D11SamplerState_Release(g_dg.samplerPoint);  g_dg.samplerPoint = NULL; }
    if (g_dg.samplerBilinear) { ID3D11SamplerState_Release(g_dg.samplerBilinear); g_dg.samplerBilinear = NULL; }

    if (g_dg.lfbSampler)   { ID3D11SamplerState_Release(g_dg.lfbSampler);  g_dg.lfbSampler = NULL; }
    if (g_dg.lfbPS)         { ID3D11PixelShader_Release(g_dg.lfbPS);       g_dg.lfbPS = NULL; }
    if (g_dg.lfbVS)         { ID3D11VertexShader_Release(g_dg.lfbVS);      g_dg.lfbVS = NULL; }
    if (g_dg.lfbSRV)        { ID3D11ShaderResourceView_Release(g_dg.lfbSRV); g_dg.lfbSRV = NULL; }
    if (g_dg.lfbTexture)    { ID3D11Texture2D_Release(g_dg.lfbTexture);    g_dg.lfbTexture = NULL; }
    if (g_dg.lfbCpuBuffer)  { free(g_dg.lfbCpuBuffer);                     g_dg.lfbCpuBuffer = NULL; }

    if (g_dg.dsv)           { ID3D11DepthStencilView_Release(g_dg.dsv);     g_dg.dsv = NULL; }
    if (g_dg.depthTex)      { ID3D11Texture2D_Release(g_dg.depthTex);      g_dg.depthTex = NULL; }
    if (g_dg.rtv)           { ID3D11RenderTargetView_Release(g_dg.rtv);     g_dg.rtv = NULL; }
    if (g_dg.backBuffer)    { ID3D11Texture2D_Release(g_dg.backBuffer);    g_dg.backBuffer = NULL; }
    if (g_dg.swapChain)     { IDXGISwapChain_Release(g_dg.swapChain);     g_dg.swapChain = NULL; }
    if (g_dg.context)       { ID3D11DeviceContext_Release(g_dg.context);   g_dg.context = NULL; }
    if (g_dg.device)        { ID3D11Device_Release(g_dg.device);           g_dg.device = NULL; }

    g_dg.isOpen = 0;
}
