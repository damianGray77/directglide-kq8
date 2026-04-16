/*
 * DirectGlide - Glide state tracking implementation
 */

#include "d3d11_state.h"
#include <string.h>

DGRenderState g_rs = {0};

void dg_state_init(int width, int height) {
    memset(&g_rs, 0, sizeof(g_rs));

    g_rs.combiner.vpWidth = (float)width;
    g_rs.combiner.vpHeight = (float)height;
    g_rs.combiner.vpInvW = 1.0f / (float)width;
    g_rs.combiner.vpInvH = 1.0f / (float)height;

    g_rs.combiner.constR = 1.0f;
    g_rs.combiner.constG = 1.0f;
    g_rs.combiner.constB = 1.0f;
    g_rs.combiner.constA = 1.0f;

    /* Default combine: vertex color passthrough */
    g_rs.combiner.colorFunc = GR_COMBINE_FUNCTION_LOCAL;
    g_rs.combiner.colorLocal = GR_COMBINE_LOCAL_ITERATED;
    g_rs.combiner.alphaFunc = GR_COMBINE_FUNCTION_LOCAL;
    g_rs.combiner.alphaLocal = GR_COMBINE_LOCAL_ITERATED;

    g_rs.combiner.alphaTestFunc = GR_CMP_ALWAYS;
    g_rs.combiner.alphaTestRef = 0.0f;

    g_rs.blend.srcRGB = GR_BLEND_ONE;
    g_rs.blend.dstRGB = GR_BLEND_ZERO;
    g_rs.blend.srcA = GR_BLEND_ONE;
    g_rs.blend.dstA = GR_BLEND_ZERO;

    g_rs.depthFunc = GR_CMP_LESS;
    g_rs.depthMode = GR_DEPTHBUFFER_DISABLE;
    g_rs.depthMask = 1;

    g_rs.cullMode = GR_CULL_DISABLE;
    g_rs.colorMaskRGB = 1;
    g_rs.colorMaskA = 1;
    g_rs.gamma = 1.0f;

    g_rs.texMinFilter[0] = GR_TEXTUREFILTER_POINT_SAMPLED;
    g_rs.texMagFilter[0] = GR_TEXTUREFILTER_POINT_SAMPLED;

    /* Everything dirty on init */
    g_rs.combinerDirty = 1;
    g_rs.blendDirty = 1;
    g_rs.depthDirty = 1;
    g_rs.rasterDirty = 1;
    g_rs.samplerDirty = 1;
}

void dg_state_set_combiner_dirty(void) {
    g_rs.combinerDirty = 1;
}
