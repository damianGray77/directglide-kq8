/*
 * DirectGlide - D3D11 backend
 * Device, swap chain, render target, 3D rendering pipeline
 */

#ifndef DG_D3D11_BACKEND_H
#define DG_D3D11_BACKEND_H

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "glide2x.h"

/* D3D11 vertex matching our shader input layout */
typedef struct {
    float x, y;         /* screen-space position */
    float depth;        /* normalized depth (0-1) */
    float r, g, b, a;   /* vertex color (0-1) */
    float oow;          /* 1/w for perspective correction */
    float sow0, tow0, oow0;  /* TMU0 texture coords */
} DGVertex;

#define DG_MAX_VERTICES 65536

/* Global D3D11 state */
typedef struct {
    /* D3D11 core */
    ID3D11Device*           device;
    ID3D11DeviceContext*    context;
    IDXGISwapChain*         swapChain;
    ID3D11RenderTargetView* rtv;          /* swap chain back buffer RTV */
    ID3D11Texture2D*        backBuffer;
    int                     screenWidth;
    int                     screenHeight;

    /* Offscreen game render target — Glide renders here at SSAA resolution
     * (ssaaWidth/ssaaHeight = width*SSAA, height*SSAA). A downsample pass then
     * box-filters down to (width, height) which is what the final sharp-bilinear
     * blit samples. This averages out sub-pixel artifacts (tri seams, etc). */
    ID3D11Texture2D*          gameTex;          /* SSAA-resolution color target */
    ID3D11RenderTargetView*   gameRtv;
    ID3D11Texture2D*          gameResolved;     /* legacy — unused when MSAA off */
    ID3D11ShaderResourceView* gameSrv;           /* SRV of gameTex (input to downsample) */
    int                       msaaSamples;
    int                       ssaaFactor;        /* SSAA multiplier, typically 2 */
    int                       ssaaWidth;         /* width * ssaaFactor */
    int                       ssaaHeight;        /* height * ssaaFactor */

    /* Downsample chain: gameTex (ssaaWxssaaH) → downsampledTex (WxH) */
    ID3D11Texture2D*          downsampledTex;
    ID3D11RenderTargetView*   downsampledRtv;
    ID3D11ShaderResourceView* downsampledSrv;
    ID3D11VertexShader*       downsampleVS;
    ID3D11PixelShader*        downsamplePS;
    ID3D11SamplerState*       downsampleSampler;

    /* Window proc hook for mouse coordinate translation */
    WNDPROC                   origWndProc;

    /* Final blit from gameTex to back buffer with letterboxing */
    ID3D11VertexShader*  blitVS;
    ID3D11PixelShader*   blitPS;
    ID3D11SamplerState*  blitSampler;
    ID3D11Buffer*        blitCB;           /* CB with viewport rect (letterbox params) */

    /* Depth buffer */
    ID3D11Texture2D*        depthTex;
    ID3D11DepthStencilView* dsv;

    /* 3D rendering pipeline */
    ID3D11VertexShader*     sceneVS;
    ID3D11PixelShader*      scenePS;
    ID3D11InputLayout*      sceneLayout;
    ID3D11Buffer*           vertexBuffer;
    ID3D11Buffer*           combinerCB;       /* constant buffer for combiner state */
    ID3D11Buffer*           fogTableCB;       /* b1: 64-entry fog density table */
    ID3D11SamplerState*     samplerPoint;
    ID3D11SamplerState*     samplerBilinear;
    ID3D11SamplerState*     samplerWrap;      /* aniso + wrap addressing */
    ID3D11SamplerState*     samplerClamp;     /* aniso + clamp addressing */

    /* Cached D3D11 state objects */
    ID3D11BlendState*           currentBlend;
    ID3D11DepthStencilState*    currentDepth;
    ID3D11RasterizerState*      currentRaster;

    /* State object caches — avoid recreating identical states */
    struct { unsigned key; ID3D11BlendState* state; } blendCache[64];
    int blendCacheCount;
    struct { unsigned key; ID3D11DepthStencilState* state; } depthCache[64];
    int depthCacheCount;
    struct { unsigned key; ID3D11RasterizerState* state; } rasterCache[32];
    int rasterCacheCount;

    /* LFB support */
    ID3D11Texture2D*        lfbTexture;
    ID3D11ShaderResourceView* lfbSRV;
    /* Persistent HUD buffer — left-column probe detects the top/bottom HUD bar
     * extents; non-sentinel pixels inside those rows accumulate here and are
     * displayed in the HUD rows regardless of whether the game redraws them. */
    FxU32*                  lfbStable;      /* width*height RGBA8, zero = transparent */
    FxU8*                   lfbCpuBuffer;
    int                     lfbDirty;
    int                     lfbLockedThisFrame;  /* Was LFB locked since last present? */
    int                     drewThisFrame;       /* Were any 3D draws since last present? */
    GrLfbWriteMode_t        lfbWriteMode;
    FxU32                   lfbStride;
    ID3D11VertexShader*     lfbVS;
    ID3D11PixelShader*      lfbPS;
    ID3D11SamplerState*     lfbSampler;

    /* Window */
    HWND                    hwnd;
    int                     width;
    int                     height;
    GrOriginLocation_t      origin;
    int                     isOpen;
} DGState;

extern DGState g_dg;

/* Init/shutdown */
int  dg_d3d11_init(HWND hwnd, int width, int height);
void dg_d3d11_shutdown(void);

/* Present/clear */
void dg_d3d11_present(int swapInterval);
void dg_d3d11_clear(float r, float g, float b, float a, float depth);

/* 3D drawing */
void dg_draw_triangle(const GrVertex* a, const GrVertex* b, const GrVertex* c);
void dg_draw_begin_frame(void);
void dg_apply_state(void);  /* Apply dirty render state to D3D11 */

/* LFB */
void* dg_lfb_lock(GrLfbWriteMode_t writeMode, FxU32* outStride);
void  dg_lfb_unlock(void);
void  dg_lfb_flush(void);

#endif /* DG_D3D11_BACKEND_H */
