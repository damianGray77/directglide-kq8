/*
 * DirectGlide - Glide state tracking
 * Mirrors all Glide render state, marks dirty flags for D3D11 updates
 */

#ifndef DG_D3D11_STATE_H
#define DG_D3D11_STATE_H

#include "glide2x.h"

/* Combiner state (passed to pixel shader via constant buffer) */
typedef struct {
    /* Viewport (for vertex shader screen-to-NDC) */
    float vpWidth;
    float vpHeight;
    float vpInvW;  /* 1/width */
    float vpInvH;  /* 1/height */

    /* Constant color (grConstantColorValue) */
    float constR, constG, constB, constA;

    /* Color combine */
    int colorFunc;
    int colorFactor;
    int colorLocal;
    int colorOther;

    /* Alpha combine */
    int alphaFunc;
    int alphaFactor;
    int alphaLocal;
    int alphaOther;

    /* Texture combine */
    int texFunc;
    int texFactor;
    int pad0, pad1;

    /* Fog — maps to float4 fogColor in shader */
    float fogR, fogG, fogB, fogA;

    /* Maps to int4 fogAlphaChroma in shader */
    int   fogMode;
    int   alphaTestFunc;
    int   chromakeyEnable;
    int   pad2;

    /* Maps to float4 alphaChromaRef in shader */
    float alphaTestRef;
    float chromaR, chromaG, chromaB;

    /* Maps to int4 invertFlags in shader */
    int colorInvert;
    int alphaInvert;
    int texRgbInvert;
    int texAlphaInvert;
} DGCombinerCB;

/* Blend state params */
typedef struct {
    int srcRGB, dstRGB, srcA, dstA;
} DGBlendParams;

/* Full render state */
typedef struct {
    DGCombinerCB combiner;
    DGBlendParams blend;

    int depthFunc;
    int depthMode;     /* GR_DEPTHBUFFER_* */
    int depthMask;
    float depthBias;

    int cullMode;
    int ditherMode;
    int colorMaskRGB;
    int colorMaskA;

    int renderBuffer;

    float gamma;

    /* Active texture per TMU */
    FxU32 texSourceAddr[2];

    /* Texture filter/clamp per TMU */
    int texMinFilter[2];
    int texMagFilter[2];
    int texClampS[2];
    int texClampT[2];
    int texMipMapMode[2];
    float texLodBias[2];

    /* Dirty flags */
    int combinerDirty;
    int blendDirty;
    int depthDirty;
    int rasterDirty;
    int samplerDirty;
    int textureDirty;
} DGRenderState;

extern DGRenderState g_rs;

void dg_state_init(int width, int height);
void dg_state_set_combiner_dirty(void);

#endif /* DG_D3D11_STATE_H */
