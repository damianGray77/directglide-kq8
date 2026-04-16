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
    ID3D11RenderTargetView* rtv;
    ID3D11Texture2D*        backBuffer;

    /* Depth buffer */
    ID3D11Texture2D*        depthTex;
    ID3D11DepthStencilView* dsv;

    /* 3D rendering pipeline */
    ID3D11VertexShader*     sceneVS;
    ID3D11PixelShader*      scenePS;
    ID3D11InputLayout*      sceneLayout;
    ID3D11Buffer*           vertexBuffer;
    ID3D11Buffer*           combinerCB;       /* constant buffer for combiner state */
    ID3D11SamplerState*     samplerPoint;
    ID3D11SamplerState*     samplerBilinear;

    /* Cached D3D11 state objects */
    ID3D11BlendState*           currentBlend;
    ID3D11DepthStencilState*    currentDepth;
    ID3D11RasterizerState*      currentRaster;

    /* LFB support */
    ID3D11Texture2D*        lfbTexture;
    ID3D11ShaderResourceView* lfbSRV;
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
