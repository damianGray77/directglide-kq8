/*
 * DirectGlide - Texture management implementation
 */

#include <initguid.h>
#include "d3d11_texture.h"
#include "d3d11_backend.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>  /* sinf for Lanczos mip filter */

/* Chained hash table: each bucket is head of a linked list */
static DGTexCacheEntry* g_texBuckets[DG_TEX_BUCKET_COUNT];
static FxU32 g_palette[256];
static unsigned g_currentFrame = 0;
static int g_entryCount = 0;
#define DG_TEX_MAX_ENTRIES 8192        /* above this, start evicting LRU */
#define DG_TEX_MAX_ENTRY_AGE 1800      /* ~30 seconds at 60fps */
static int g_paletteValid = 0;
static FxU32 g_paletteHash = 0;

/* Fallback texture for cache misses — 1x1 black with alpha 1 */
static ID3D11Texture2D*          g_missTex = NULL;
static ID3D11ShaderResourceView* g_missSrv = NULL;

ID3D11ShaderResourceView* dg_tex_get_miss_srv(void) {
    if (!g_missSrv && g_dg.device) {
        D3D11_TEXTURE2D_DESC td = {0};
        D3D11_SUBRESOURCE_DATA data = {0};
        FxU32 pixel = 0xFF000000; /* opaque black */
        td.Width = 1; td.Height = 1;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        data.pSysMem = &pixel;
        data.SysMemPitch = 4;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dg.device, &td, &data, &g_missTex))) {
            ID3D11Device_CreateShaderResourceView(g_dg.device, (ID3D11Resource*)g_missTex, NULL, &g_missSrv);
        }
    }
    return g_missSrv;
}

/* Hash the current palette — rotate-and-add per OpenGlide's approach */
static FxU32 computePaletteHash(void) {
    FxU32 h = 0;
    int i;
    for (i = 0; i < 256; i++) {
        h = (h << 5) | (h >> 27);
        h += g_palette[i];
    }
    return h;
}

/* Fast hash of texture pixel data */
static FxU32 hashData(const void* data, int size) {
    const FxU8* p = (const FxU8*)data;
    FxU32 h = 2166136261u;
    int i;
    /* Sample every 4th byte for speed — mostly catches content changes */
    for (i = 0; i < size; i += 4) {
        h ^= p[i];
        h *= 16777619u;
    }
    h ^= (FxU32)size;
    return h;
}

void dg_tex_init(void) {
    memset(g_texBuckets, 0, sizeof(g_texBuckets));
    memset(g_palette, 0, sizeof(g_palette));
    g_paletteValid = 0;
}

static void freeEntry(DGTexCacheEntry* e) {
    if (!e) return;
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    if (e->rawData) free(e->rawData);
    free(e);
}

void dg_tex_shutdown(void) {
    int i;
    for (i = 0; i < DG_TEX_BUCKET_COUNT; i++) {
        DGTexCacheEntry* e = g_texBuckets[i];
        while (e) {
            DGTexCacheEntry* next = e->next;
            freeEntry(e);
            e = next;
        }
        g_texBuckets[i] = NULL;
    }
    g_entryCount = 0;
    if (g_missSrv) { ID3D11ShaderResourceView_Release(g_missSrv); g_missSrv = NULL; }
    if (g_missTex) { ID3D11Texture2D_Release(g_missTex); g_missTex = NULL; }
}

/* Evict entries older than DG_TEX_MAX_ENTRY_AGE frames, walking all buckets.
 * When entry count is very high, also evict the oldest 25% even if young. */
void dg_tex_tick_frame(void) {
    int i;
    int evicted = 0;
    unsigned cutoff;

    g_currentFrame++;

    if (g_entryCount <= DG_TEX_MAX_ENTRIES) {
        /* Only do age-based eviction every 64 frames to save CPU */
        if ((g_currentFrame & 63) != 0) return;
    }

    /* Anything older than this is evictable */
    cutoff = (g_currentFrame > DG_TEX_MAX_ENTRY_AGE)
                ? (g_currentFrame - DG_TEX_MAX_ENTRY_AGE) : 0;

    for (i = 0; i < DG_TEX_BUCKET_COUNT; i++) {
        DGTexCacheEntry** pp = &g_texBuckets[i];
        while (*pp) {
            DGTexCacheEntry* e = *pp;
            if (e->lastUsedFrame < cutoff) {
                *pp = e->next;
                freeEntry(e);
                g_entryCount--;
                evicted++;
            } else {
                pp = &e->next;
            }
        }
    }

    /* If still over the hard cap, do a second pass evicting the oldest. */
    if (g_entryCount > DG_TEX_MAX_ENTRIES) {
        /* Find the median age by sampling; set a stricter cutoff. */
        unsigned target = g_currentFrame > 300 ? (g_currentFrame - 300) : 0;
        for (i = 0; i < DG_TEX_BUCKET_COUNT && g_entryCount > DG_TEX_MAX_ENTRIES; i++) {
            DGTexCacheEntry** pp = &g_texBuckets[i];
            while (*pp && g_entryCount > DG_TEX_MAX_ENTRIES) {
                DGTexCacheEntry* e = *pp;
                if (e->lastUsedFrame < target) {
                    *pp = e->next;
                    freeEntry(e);
                    g_entryCount--;
                    evicted++;
                } else {
                    pp = &e->next;
                }
            }
        }
    }

    if (evicted > 0) {
        dg_log("TEX EVICT: freed %d entries, %d remain (frame=%u)\n",
               evicted, g_entryCount, g_currentFrame);
    }
}

void dg_tex_set_palette(const FxU32* palette256) {
    if (palette256) {
        memcpy(g_palette, palette256, 256 * sizeof(FxU32));
        g_paletteValid = 1;
        g_paletteHash = computePaletteHash();
    }
}

void dg_tex_invalidate_all(void) {
    dg_tex_shutdown();
    dg_tex_init();
}

/* Lanczos-2 downsample by factor of 2: 4-tap separable filter with
 * normalized weights. Substantially reduces mipmap aliasing vs box filter,
 * which directly improves distant-surface moire under anisotropic filtering. */
static void lanczosDownsample2x(const FxU32* src, FxU32* dst, int srcW, int srcH) {
    /* Lanczos-2 weights: sinc(x)*sinc(x/2) at distances 1.5, 0.5, 0.5, 1.5,
     * then normalized so sum = 1.0 exactly. */
    static const float W[4] = { -0.0623f, 0.5623f, 0.5623f, -0.0623f };
    int dstW = srcW / 2;
    int dstH = srcH / 2;
    int x, y, dx, dy;
    if (dstW < 1) dstW = 1;
    if (dstH < 1) dstH = 1;

    for (y = 0; y < dstH; y++) {
        int srcY = 2 * y;  /* center of 2x2 region is srcY+0.5 */
        for (x = 0; x < dstW; x++) {
            int srcX = 2 * x;
            float r = 0, g = 0, b = 0, a = 0;
            for (dy = 0; dy < 4; dy++) {
                int sy = srcY - 1 + dy;
                if (sy < 0) sy = 0; else if (sy >= srcH) sy = srcH - 1;
                for (dx = 0; dx < 4; dx++) {
                    int sx = srcX - 1 + dx;
                    FxU32 px;
                    float w;
                    if (sx < 0) sx = 0; else if (sx >= srcW) sx = srcW - 1;
                    px = src[sy * srcW + sx];
                    w = W[dx] * W[dy];
                    r += w * (float)(px & 0xFF);
                    g += w * (float)((px >> 8) & 0xFF);
                    b += w * (float)((px >> 16) & 0xFF);
                    a += w * (float)((px >> 24) & 0xFF);
                }
            }
            /* Clamp — Lanczos can produce small under/overshoots at high-contrast edges */
            {
                int ri = (int)(r + 0.5f), gi = (int)(g + 0.5f),
                    bi = (int)(b + 0.5f), ai = (int)(a + 0.5f);
                if (ri < 0) ri = 0; else if (ri > 255) ri = 255;
                if (gi < 0) gi = 0; else if (gi > 255) gi = 255;
                if (bi < 0) bi = 0; else if (bi > 255) bi = 255;
                if (ai < 0) ai = 0; else if (ai > 255) ai = 255;
                dst[y * dstW + x] = ((FxU32)ai << 24) | ((FxU32)bi << 16)
                                  | ((FxU32)gi << 8)  |  (FxU32)ri;
            }
        }
    }
}

/* Lanczos-2 kernel evaluated at distance x (in source-pixel units).
 * sinc(x) * sinc(x/a) for |x| < a, with a = 2, unnormalized. */
static float lanczos2(float x) {
    static const float PI = 3.14159265358979f;
    float absx;
    if (x == 0.0f) return 1.0f;
    absx = x < 0 ? -x : x;
    if (absx >= 2.0f) return 0.0f;
    {
        float pix  = PI * x;
        float pix2 = pix * 0.5f;
        return (sinf(pix) / pix) * (sinf(pix2) / pix2);
    }
}

/* Downsample directly from mip 0 to a target (dstW, dstH) with a filter width
 * scaled to the downsample factor. This is the "gold standard" for mipmap
 * generation — each output level gets a proper low-pass with cutoff at its
 * own Nyquist frequency, eliminating moire that pairwise chains leak through. */
static void lanczosDownsampleFromMip0(const FxU32* src, int srcW, int srcH,
                                       FxU32* dst, int dstW, int dstH) {
    float scaleX = (float)srcW / (float)dstW;
    float scaleY = (float)srcH / (float)dstH;
    /* Filter radius in source-pixel units. Lanczos-2 has support of ±2 in
     * normalized units; we scale by downsample factor so each mip's filter
     * covers ±2 of its own destination-pixel area. */
    float radiusX = 2.0f * scaleX;
    float radiusY = 2.0f * scaleY;
    int tapsX, tapsY;
    int x, y, dx, dy;

    /* Cap tap count to keep CPU cost bounded on deep mips (diminishing return) */
    tapsX = (int)(radiusX * 2.0f) + 1;
    tapsY = (int)(radiusY * 2.0f) + 1;
    if (tapsX > 32) tapsX = 32;
    if (tapsY > 32) tapsY = 32;

    for (y = 0; y < dstH; y++) {
        float centerY = ((float)y + 0.5f) * scaleY - 0.5f;
        int y0 = (int)(centerY - radiusY + 0.5f);
        for (x = 0; x < dstW; x++) {
            float centerX = ((float)x + 0.5f) * scaleX - 0.5f;
            int x0 = (int)(centerX - radiusX + 0.5f);
            float r = 0, g = 0, b = 0, a = 0, wsum = 0;
            for (dy = 0; dy < tapsY; dy++) {
                int sy = y0 + dy;
                float distY = ((float)sy - centerY) / scaleY;
                float wy = lanczos2(distY);
                if (wy == 0.0f) continue;
                if (sy < 0) sy = 0; else if (sy >= srcH) sy = srcH - 1;
                for (dx = 0; dx < tapsX; dx++) {
                    int sx = x0 + dx;
                    float distX = ((float)sx - centerX) / scaleX;
                    float wx = lanczos2(distX);
                    float w;
                    FxU32 px;
                    if (wx == 0.0f) continue;
                    if (sx < 0) sx = 0; else if (sx >= srcW) sx = srcW - 1;
                    w = wx * wy;
                    px = src[sy * srcW + sx];
                    r += w * (float)(px & 0xFF);
                    g += w * (float)((px >> 8) & 0xFF);
                    b += w * (float)((px >> 16) & 0xFF);
                    a += w * (float)((px >> 24) & 0xFF);
                    wsum += w;
                }
            }
            if (wsum != 0.0f) {
                float inv = 1.0f / wsum;
                r *= inv; g *= inv; b *= inv; a *= inv;
            }
            {
                int ri = (int)(r + 0.5f), gi = (int)(g + 0.5f),
                    bi = (int)(b + 0.5f), ai = (int)(a + 0.5f);
                if (ri < 0) ri = 0; else if (ri > 255) ri = 255;
                if (gi < 0) gi = 0; else if (gi > 255) gi = 255;
                if (bi < 0) bi = 0; else if (bi > 255) bi = 255;
                if (ai < 0) ai = 0; else if (ai > 255) ai = 255;
                dst[y * dstW + x] = ((FxU32)ai << 24) | ((FxU32)bi << 16)
                                  | ((FxU32)gi << 8)  |  (FxU32)ri;
            }
        }
    }
}

/* Upload full mip chain via fast pairwise Lanczos-2 (each level from the prev). */
static void uploadLanczosMips(ID3D11Texture2D* tex, const FxU32* mip0, int w, int h) {
    FxU32* prev;
    FxU32* next;
    int mipW, mipH, mipLevel;

    ID3D11DeviceContext_UpdateSubresource(g_dg.context,
        (ID3D11Resource*)tex, 0, NULL, mip0, (UINT)(w * 4), 0);

    mipW = w;
    mipH = h;
    mipLevel = 1;
    prev = (FxU32*)malloc(w * h * 4);
    if (!prev) return;
    memcpy(prev, mip0, w * h * 4);

    while (mipW > 1 || mipH > 1) {
        int dstW = mipW > 1 ? mipW / 2 : 1;
        int dstH = mipH > 1 ? mipH / 2 : 1;
        next = (FxU32*)malloc(dstW * dstH * 4);
        if (!next) { free(prev); return; }

        if (mipW > 1 && mipH > 1) {
            lanczosDownsample2x(prev, next, mipW, mipH);
        } else {
            /* Degenerate axis: box-average along non-degenerate axis */
            int i, j;
            if (mipW == 1) {
                for (j = 0; j < dstH; j++) {
                    FxU32 a = prev[j*2], b = prev[j*2+1];
                    FxU32 r_ = ((a & 0xFF) + (b & 0xFF)) / 2;
                    FxU32 g_ = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) / 2;
                    FxU32 b_ = (((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) / 2;
                    FxU32 al = (((a >> 24) & 0xFF) + ((b >> 24) & 0xFF)) / 2;
                    next[j] = (al << 24) | (b_ << 16) | (g_ << 8) | r_;
                }
            } else {
                for (i = 0; i < dstW; i++) {
                    FxU32 a = prev[i*2], b = prev[i*2+1];
                    FxU32 r_ = ((a & 0xFF) + (b & 0xFF)) / 2;
                    FxU32 g_ = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) / 2;
                    FxU32 b_ = (((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) / 2;
                    FxU32 al = (((a >> 24) & 0xFF) + ((b >> 24) & 0xFF)) / 2;
                    next[i] = (al << 24) | (b_ << 16) | (g_ << 8) | r_;
                }
            }
        }

        ID3D11DeviceContext_UpdateSubresource(g_dg.context,
            (ID3D11Resource*)tex, (UINT)mipLevel, NULL, next, (UINT)(dstW * 4), 0);

        free(prev);
        prev = next;
        mipW = dstW;
        mipH = dstH;
        mipLevel++;
    }
    if (prev) free(prev);
}

/* Hash TMU address to cache slot */
static int texCacheSlot(FxU32 addr) {
    FxU32 h = addr;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return (int)(h % DG_TEX_BUCKET_COUNT);
}

/* Lookup existing entry in chained hash — touches lastUsedFrame on hit */
static DGTexCacheEntry* texLookup(FxU32 addr) {
    int b = texCacheSlot(addr);
    DGTexCacheEntry* e = g_texBuckets[b];
    while (e) {
        if (e->addr == addr && e->valid) {
            e->lastUsedFrame = g_currentFrame;
            return e;
        }
        e = e->next;
    }
    return NULL;
}

/* Find or create an entry for addr — always succeeds (unbounded chain) */
static DGTexCacheEntry* texFindOrCreate(FxU32 addr) {
    int b = texCacheSlot(addr);
    DGTexCacheEntry* e = texLookup(addr);
    if (e) return e;
    /* Allocate new entry and prepend to bucket */
    e = (DGTexCacheEntry*)calloc(1, sizeof(DGTexCacheEntry));
    if (!e) return NULL;
    e->addr = addr;
    e->lastUsedFrame = g_currentFrame;
    e->next = g_texBuckets[b];
    g_texBuckets[b] = e;
    g_entryCount++;
    return e;
}

/* Get texture dimensions from LOD and aspect ratio */
static void texDimensions(GrLOD_t largeLod, GrAspectRatio_t aspect, int* w, int* h) {
    int baseDim = 256 >> largeLod;
    if (baseDim < 1) baseDim = 1;

    switch (aspect) {
        case GR_ASPECT_8x1: *w = baseDim; *h = baseDim >> 3; break;
        case GR_ASPECT_4x1: *w = baseDim; *h = baseDim >> 2; break;
        case GR_ASPECT_2x1: *w = baseDim; *h = baseDim >> 1; break;
        case GR_ASPECT_1x1: *w = baseDim; *h = baseDim; break;
        case GR_ASPECT_1x2: *w = baseDim >> 1; *h = baseDim; break;
        case GR_ASPECT_1x4: *w = baseDim >> 2; *h = baseDim; break;
        case GR_ASPECT_1x8: *w = baseDim >> 3; *h = baseDim; break;
        default:            *w = baseDim; *h = baseDim; break;
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

/* Bytes per texel for Glide format */
static int texBpp(GrTextureFormat_t fmt) {
    switch (fmt) {
        case GR_TEXFMT_RGB_332:
        case GR_TEXFMT_ALPHA_8:
        case GR_TEXFMT_INTENSITY_8:
        case GR_TEXFMT_ALPHA_INTENSITY_44:
        case GR_TEXFMT_P_8:
        case GR_TEXFMT_YIQ_422:
            return 1;
        case GR_TEXFMT_ARGB_8332:
        case GR_TEXFMT_RGB_565:
        case GR_TEXFMT_ARGB_1555:
        case GR_TEXFMT_ARGB_4444:
        case GR_TEXFMT_ALPHA_INTENSITY_88:
        case GR_TEXFMT_AP_88:
        case GR_TEXFMT_AYIQ_8422:
            return 2;
        default:
            return 1;
    }
}

/* Convert Glide texture data to RGBA8 */
static void convertToRGBA8(const void* src, FxU32* dst, int w, int h, GrTextureFormat_t fmt) {
    int count = w * h;
    int i;
    const FxU8* s8 = (const FxU8*)src;
    const FxU16* s16 = (const FxU16*)src;

    switch (fmt) {
        case GR_TEXFMT_RGB_565:
            for (i = 0; i < count; i++) {
                FxU16 c = s16[i];
                FxU8 r = (FxU8)(((c >> 11) & 0x1F) * 255 / 31);
                FxU8 g = (FxU8)(((c >> 5)  & 0x3F) * 255 / 63);
                FxU8 b = (FxU8)(( c        & 0x1F) * 255 / 31);
                dst[i] = 0xFF000000 | ((FxU32)b << 16) | ((FxU32)g << 8) | r;
            }
            break;

        case GR_TEXFMT_ARGB_1555:
            for (i = 0; i < count; i++) {
                FxU16 c = s16[i];
                FxU8 a = (c & 0x8000) ? 255 : 0;
                FxU8 r = (FxU8)(((c >> 10) & 0x1F) * 255 / 31);
                FxU8 g = (FxU8)(((c >> 5)  & 0x1F) * 255 / 31);
                FxU8 b = (FxU8)(( c        & 0x1F) * 255 / 31);
                dst[i] = ((FxU32)a << 24) | ((FxU32)b << 16) | ((FxU32)g << 8) | r;
            }
            break;

        case GR_TEXFMT_ARGB_4444:
            {
                static int log4444 = 0;
                int nonzero = 0;
                for (i = 0; i < count; i++) {
                    FxU16 c = s16[i];
                    FxU8 a = (FxU8)(((c >> 12) & 0xF) * 17);
                    FxU8 r = (FxU8)(((c >> 8)  & 0xF) * 17);
                    FxU8 g = (FxU8)(((c >> 4)  & 0xF) * 17);
                    FxU8 b = (FxU8)(( c        & 0xF) * 17);
                    dst[i] = ((FxU32)a << 24) | ((FxU32)b << 16) | ((FxU32)g << 8) | r;
                    if (c != 0) nonzero++;
                }
                if (log4444 < 5) {
                    dg_log("4444 TEX %dx%d: %d/%d nonzero, first raw: %04X %04X %04X %04X\n",
                           w, h, nonzero, count,
                           count>0?s16[0]:0, count>1?s16[1]:0, count>2?s16[2]:0, count>3?s16[3]:0);
                    log4444++;
                }
            }
            break;

        case GR_TEXFMT_P_8:
            {
                static int p8log = 0;
                for (i = 0; i < count; i++) {
                    FxU32 c = g_paletteValid ? g_palette[s8[i]] : 0xFFFF00FF;
                    FxU8 r = (FxU8)((c >> 16) & 0xFF);
                    FxU8 g = (FxU8)((c >> 8)  & 0xFF);
                    FxU8 b = (FxU8)( c        & 0xFF);
                    dst[i] = 0xFF000000 | ((FxU32)b << 16) | ((FxU32)g << 8) | r;
                }
                if (p8log < 3) {
                    /* Log first few pixels of this P_8 texture */
                    int nLog = count < 8 ? count : 8;
                    dg_log("P8 TEX %dx%d indices:", w, h);
                    for (i = 0; i < nLog; i++) dg_log(" %d", s8[i]);
                    dg_log(" -> RGBA:");
                    for (i = 0; i < nLog; i++) dg_log(" %08X", dst[i]);
                    dg_log("\n");
                    p8log++;
                }
            }
            break;

        case GR_TEXFMT_AP_88:
            for (i = 0; i < count; i++) {
                FxU16 c = s16[i];
                FxU8 a = (FxU8)((c >> 8) & 0xFF);
                FxU8 idx = (FxU8)(c & 0xFF);
                FxU32 pal = g_paletteValid ? g_palette[idx] : 0xFFFF00FF;
                FxU8 r = (FxU8)((pal >> 16) & 0xFF);
                FxU8 g = (FxU8)((pal >> 8)  & 0xFF);
                FxU8 b = (FxU8)( pal        & 0xFF);
                dst[i] = ((FxU32)a << 24) | ((FxU32)b << 16) | ((FxU32)g << 8) | r;
            }
            break;

        case GR_TEXFMT_RGB_332:
            for (i = 0; i < count; i++) {
                FxU8 c = s8[i];
                FxU8 r = (FxU8)(((c >> 5) & 0x7) * 255 / 7);
                FxU8 g = (FxU8)(((c >> 2) & 0x7) * 255 / 7);
                FxU8 b = (FxU8)(( c       & 0x3) * 255 / 3);
                dst[i] = 0xFF000000 | ((FxU32)b << 16) | ((FxU32)g << 8) | r;
            }
            break;

        case GR_TEXFMT_ARGB_8332:
            for (i = 0; i < count; i++) {
                FxU16 c = s16[i];
                FxU8 a = (FxU8)((c >> 8) & 0xFF);
                FxU8 r = (FxU8)((((c >> 5) & 0x7) * 255) / 7);
                FxU8 g = (FxU8)((((c >> 2) & 0x7) * 255) / 7);
                FxU8 b = (FxU8)((( c       & 0x3) * 255) / 3);
                dst[i] = ((FxU32)a << 24) | ((FxU32)b << 16) | ((FxU32)g << 8) | r;
            }
            break;

        case GR_TEXFMT_ALPHA_8:
            for (i = 0; i < count; i++) {
                dst[i] = ((FxU32)s8[i] << 24) | 0x00FFFFFF;
            }
            break;

        case GR_TEXFMT_INTENSITY_8:
            for (i = 0; i < count; i++) {
                FxU8 v = s8[i];
                dst[i] = 0xFF000000 | ((FxU32)v << 16) | ((FxU32)v << 8) | v;
            }
            break;

        case GR_TEXFMT_ALPHA_INTENSITY_44:
            for (i = 0; i < count; i++) {
                FxU8 a = (FxU8)(((s8[i] >> 4) & 0xF) * 17);
                FxU8 v = (FxU8)(( s8[i]       & 0xF) * 17);
                dst[i] = ((FxU32)a << 24) | ((FxU32)v << 16) | ((FxU32)v << 8) | v;
            }
            break;

        case GR_TEXFMT_ALPHA_INTENSITY_88:
            for (i = 0; i < count; i++) {
                FxU16 c = s16[i];
                FxU8 a = (FxU8)((c >> 8) & 0xFF);
                FxU8 v = (FxU8)( c       & 0xFF);
                dst[i] = ((FxU32)a << 24) | ((FxU32)v << 16) | ((FxU32)v << 8) | v;
            }
            break;

        default:
            /* Unknown format: magenta */
            for (i = 0; i < count; i++)
                dst[i] = 0xFFFF00FF;
            break;
    }
}

void dg_tex_download(GrChipID_t tmu, FxU32 addr,
                     GrLOD_t smallLod, GrLOD_t largeLod,
                     GrAspectRatio_t aspect, GrTextureFormat_t fmt,
                     const void* data) {
    int w, h, dataSize;
    FxU32 hash;
    FxU32* rgba;
    HRESULT hr;
    D3D11_TEXTURE2D_DESC td;
    DGTexCacheEntry* e;
    int isPal;

    (void)tmu;
    if (!data || !g_dg.device) return;

    texDimensions(largeLod, aspect, &w, &h);
    isPal = (fmt == GR_TEXFMT_P_8 || fmt == GR_TEXFMT_AP_88);
    dataSize = w * h * texBpp(fmt);
    hash = hashData(data, dataSize);

    e = texFindOrCreate(addr);
    if (!e) return;

    /* Skip re-upload if same everything */
    if (e->valid && e->fmt == fmt && e->width == w && e->height == h && e->dataHash == hash) {
        return;
    }

    /* In-place update path (same dims/format) */
    if (e->valid && e->fmt == fmt && e->width == w && e->height == h) {
        if (isPal) {
            if (!e->rawData || e->rawDataSize != dataSize) {
                if (e->rawData) free(e->rawData);
                e->rawData = malloc(dataSize);
                e->rawDataSize = dataSize;
            }
            if (e->rawData) memcpy(e->rawData, data, dataSize);
            e->dataHash = hash;
            if (e->tex && e->rawData) {
                FxU32* tmp = (FxU32*)malloc(w * h * 4);
                if (tmp) {
                    convertToRGBA8(e->rawData, tmp, w, h, fmt);
                    uploadLanczosMips(e->tex, tmp, w, h);
                    free(tmp);
                }
            }
            return;
        } else if (e->tex) {
            FxU32* tmp = (FxU32*)malloc(w * h * 4);
            if (tmp) {
                convertToRGBA8(data, tmp, w, h, fmt);
                uploadLanczosMips(e->tex, tmp, w, h);
                free(tmp);
            }
            e->dataHash = hash;
            return;
        }
    }

    /* Free old resources — dimensions or format changed */
    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
    if (e->tex) { ID3D11Texture2D_Release(e->tex); e->tex = NULL; }
    if (e->rawData) { free(e->rawData); e->rawData = NULL; }

    e->width = w;
    e->height = h;
    e->fmt = fmt;
    e->isPalettized = isPal;
    e->dataHash = hash;
    e->storedSmallLod = smallLod;
    e->storedLargeLod = largeLod;
    e->storedAspect = aspect;
    e->valid = 1;

    if (isPal) {
        /* Store raw data for lazy decode */
        e->rawData = malloc(dataSize);
        if (e->rawData) memcpy(e->rawData, data, dataSize);
        e->rawDataSize = dataSize;
        e->paletteHash = 0;
        return;
    }

    /* Non-palettized: decode immediately */
    rgba = (FxU32*)malloc(w * h * 4);
    if (!rgba) return;
    convertToRGBA8(data, rgba, w, h, fmt);

    memset(&td, 0, sizeof(td));
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 0;   /* full chain allocated; we fill each level ourselves */
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;  /* no RT bind — CPU Lanczos mips */
    td.MiscFlags = 0;

    hr = ID3D11Device_CreateTexture2D(g_dg.device, &td, NULL, &e->tex);
    if (FAILED(hr)) { free(rgba); return; }

    /* Generate full Lanczos-2 mip chain on CPU and upload all levels */
    uploadLanczosMips(e->tex, rgba, w, h);

    hr = ID3D11Device_CreateShaderResourceView(g_dg.device,
        (ID3D11Resource*)e->tex, NULL, &e->srv);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(e->tex); e->tex = NULL;
        free(rgba);
        return;
    }

    {
        static int uploadCount = 0;
        static int fmtCount[16] = {0};
        static FxU32 maxAddr = 0;
        uploadCount++;
        if (addr > maxAddr) maxAddr = addr;
        if (fmt >= 0 && fmt < 16) fmtCount[fmt]++;
        if (uploadCount <= 20 || (uploadCount % 2000 == 0)) {
            dg_log("TEX UPLOAD #%d: addr=0x%X %dx%d fmt=%d (fmts: 332=%d P8=%d 565=%d 1555=%d 4444=%d AI88=%d AP88=%d)\n",
                   uploadCount, addr, w, h, fmt,
                   fmtCount[0], fmtCount[5], fmtCount[10], fmtCount[11],
                   fmtCount[12], fmtCount[13], fmtCount[14]);
        }
    }

    free(rgba);
}

ID3D11ShaderResourceView* dg_tex_get_srv(FxU32 addr) {
    DGTexCacheEntry* e = texLookup(addr);
    if (!e) return NULL;
    if (!e->isPalettized) return e->srv;
    if (!g_paletteValid || !e->rawData) return e->srv;
    if (e->srv) return e->srv;  /* already decoded */

    {
        FxU32* rgba;
        HRESULT hr;
        D3D11_TEXTURE2D_DESC td = {0};

        rgba = (FxU32*)malloc(e->width * e->height * 4);
        if (!rgba) return NULL;
        convertToRGBA8(e->rawData, rgba, e->width, e->height, e->fmt);

        td.Width = (UINT)e->width;
        td.Height = (UINT)e->height;
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = 0;

        hr = ID3D11Device_CreateTexture2D(g_dg.device, &td, NULL, &e->tex);
        if (SUCCEEDED(hr)) {
            uploadLanczosMips(e->tex, rgba, e->width, e->height);
            ID3D11Device_CreateShaderResourceView(g_dg.device,
                (ID3D11Resource*)e->tex, NULL, &e->srv);
        }
        free(rgba);
        e->paletteHash = g_paletteHash;
    }
    return e->srv;
}
