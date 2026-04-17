/*
 * DirectGlide - Glide state tracking implementation
 */

#include "d3d11_state.h"
#include <string.h>
#include <math.h>

/* Canonical Glide fog W lookup (same as g_fogIndexToW in glide_exports.c).
 * Duplicated here so state init can build a default fog table without cross-TU deps. */
static const float k_fogIndexToW[64] = {
    1.000000f, 1.142857f, 1.333333f, 1.600000f,
    2.000000f, 2.285714f, 2.666667f, 3.200000f,
    4.000000f, 4.571429f, 5.333333f, 6.400000f,
    8.000000f, 9.142858f,10.666667f,12.800000f,
   16.000000f,18.285715f,21.333334f,25.600000f,
   32.000000f,36.571430f,42.666668f,51.200001f,
   64.000000f,73.142860f,85.333336f,102.400002f,
  128.000000f,146.285721f,170.666672f,204.800003f,
  256.000000f,292.571442f,341.333344f,409.600006f,
  512.000000f,585.142883f,682.666687f,819.200012f,
 1024.000000f,1170.285767f,1365.333374f,1638.400024f,
 2048.000000f,2340.571533f,2730.666748f,3276.800049f,
 4096.000000f,4681.143066f,5461.333496f,6553.600098f,
 8192.000000f,9362.286133f,10922.666992f,13107.200195f,
16384.000000f,18724.572266f,21845.333984f,26214.400391f,
32768.000000f,37449.144531f,43690.667969f,52428.800781f
};

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

    /* Default fog table — KQ8 never calls grFogTable. Clamped exponential:
     *   W <= nearW  → fog = 0 exactly   (no-fog radius around the player)
     *   W >= farW   → fog = 255 exactly (fully fogged)
     *   between     → smooth exp ramp normalized to hit both endpoints exactly.
     * nearW pushed out enough to leave lantern overlays alone. */
    {
        float nearW = 16000.0f;
        float farW  = 20000.0f;
        float k     = 1.5f;   /* 1 ≈ linear, higher = more front-loaded */
        float norm  = 1.0f - (float)exp(-k);
        int i;
        for (i = 0; i < 64; i++) {
            float W = k_fogIndexToW[i];
            float f;
            if (W <= nearW) {
                f = 0.0f;
            } else if (W >= farW) {
                f = 255.0f;
            } else {
                float t    = (W - nearW) / (farW - nearW);
                float expF = (1.0f - (float)exp(-k * t)) / norm;
                f = expF * 255.0f;
                if (f < 0.0f) f = 0.0f;
                else if (f > 255.0f) f = 255.0f;
            }
            g_rs.fogTable[i] = (FxU8)(f + 0.5f);   /* round */
        }
        g_rs.fogTableDirty = 1;
    }

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
