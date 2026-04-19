/*
 * DirectGlide - Texture management implementation (v2 two-layer cache)
 *
 * Layer 1: Address map (g_addrBuckets) — one DGTexAddrEntry per TMU
 *   address (masked to 4MB). Holds the raw texture bytes plus a content
 *   hash. No D3D11 resources.
 * Layer 2: GPU pool (g_poolBuckets) — one DGTexPoolEntry per unique
 *   content hash. Holds the actual ID3D11Texture2D + SRV. Addresses
 *   with identical content share a single pool entry.
 *
 * LRU is applied to the pool only. When a pool entry is evicted, the
 * next dg_tex_get_srv rebuilds it from the raw bytes still kept in the
 * address map.
 */

#include <initguid.h>
#include "d3d11_texture.h"
#include "d3d11_backend.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>

/* Address map: one entry per masked TMU address */
static DGTexAddrEntry* g_addrBuckets[DG_TEX_BUCKET_COUNT];
static int g_addrCount = 0;

/* GPU pool: one entry per unique content hash */
static DGTexPoolEntry* g_poolBuckets[DG_POOL_BUCKET_COUNT];
static int g_poolCount = 0;

static unsigned g_currentFrame = 0;

static FxU32 g_palette[256];
static int   g_paletteValid = 0;
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

/* Lanczos-2 downsample by factor of 2: 4-tap separable filter with
 * normalized weights. Substantially reduces mipmap aliasing vs box filter,
 * which directly improves distant-surface moire under anisotropic filtering. */
static void lanczosDownsample2x(const FxU32* src, FxU32* dst, int srcW, int srcH) {
    static const float W[4] = { -0.0623f, 0.5623f, 0.5623f, -0.0623f };
    int dstW = srcW / 2;
    int dstH = srcH / 2;
    int x, y, dx, dy;
    if (dstW < 1) dstW = 1;
    if (dstH < 1) dstH = 1;

    for (y = 0; y < dstH; y++) {
        int srcY = 2 * y;
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

/* Lanczos-2 kernel evaluated at distance x (in source-pixel units). */
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

/* Downsample directly from mip 0 to a target (dstW, dstH). Kept for
 * reference / future use; uploadLanczosMips currently uses pairwise 2x. */
static void lanczosDownsampleFromMip0(const FxU32* src, int srcW, int srcH,
                                       FxU32* dst, int dstW, int dstH) {
    float scaleX = (float)srcW / (float)dstW;
    float scaleY = (float)srcH / (float)dstH;
    float radiusX = 2.0f * scaleX;
    float radiusY = 2.0f * scaleY;
    int tapsX, tapsY;
    int x, y, dx, dy;

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
    (void)lanczosDownsampleFromMip0; /* kept for reference; not currently used */
}

/* ====================================================================
 * Address map
 * ==================================================================== */

static int addrCacheSlot(FxU32 addr) {
    FxU32 h = addr;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return (int)(h % DG_TEX_BUCKET_COUNT);
}

static DGTexAddrEntry* addrLookup(FxU32 addr) {
    int b = addrCacheSlot(addr);
    DGTexAddrEntry* e = g_addrBuckets[b];
    while (e) {
        if (e->addr == addr) return e;
        e = e->next;
    }
    return NULL;
}

static void freeAddrEntry(DGTexAddrEntry* e) {
    if (!e) return;
    if (e->rawData) free(e->rawData);
    free(e);
}

/* Forward decl — addr eviction needs to also drop pool refs of evicted entries */
static void addrReleasePool(DGTexAddrEntry* a);

/* qsort helper: sort by lastUsedFrame ascending (oldest first) */
static int compareAddrByAge(const void* a, const void* b) {
    const DGTexAddrEntry* ea = *(const DGTexAddrEntry* const*)a;
    const DGTexAddrEntry* eb = *(const DGTexAddrEntry* const*)b;
    if (ea->lastUsedFrame < eb->lastUsedFrame) return -1;
    if (ea->lastUsedFrame > eb->lastUsedFrame) return 1;
    return 0;
}

/* Evict oldest address entries until g_addrCount <= targetCount. Drops
 * each evicted entry's pool reference first. */
static void addrEvictTo(int targetCount) {
    DGTexAddrEntry** all;
    int count = 0, i, toEvict;

    if (g_addrCount <= targetCount) return;

    all = (DGTexAddrEntry**)malloc(sizeof(DGTexAddrEntry*) * g_addrCount);
    if (!all) return;

    for (i = 0; i < DG_TEX_BUCKET_COUNT; i++) {
        DGTexAddrEntry* e = g_addrBuckets[i];
        while (e) { all[count++] = e; e = e->next; }
    }
    qsort(all, count, sizeof(DGTexAddrEntry*), compareAddrByAge);

    toEvict = count - targetCount;
    for (i = 0; i < toEvict; i++) {
        DGTexAddrEntry* e = all[i];
        int b = addrCacheSlot(e->addr);
        DGTexAddrEntry** pp = &g_addrBuckets[b];
        while (*pp && *pp != e) pp = &(*pp)->next;
        if (*pp) {
            *pp = e->next;
            addrReleasePool(e);
            freeAddrEntry(e);
            g_addrCount--;
        }
    }
    free(all);
}

static DGTexAddrEntry* addrFindOrCreate(FxU32 addr) {
    int b;
    DGTexAddrEntry* e = addrLookup(addr);
    if (e) return e;
    /* Enforce cap before creating. 10% watermark so eviction doesn't
     * run on every single new addr — amortizes the O(n) sort. */
    if (g_addrCount >= DG_ADDR_MAX_ENTRIES) {
        addrEvictTo(DG_ADDR_MAX_ENTRIES * 9 / 10);
        dg_log("TEX ADDR: evicted to %d (cap %d) at frame %u\n",
               g_addrCount, DG_ADDR_MAX_ENTRIES, g_currentFrame);
    }
    e = (DGTexAddrEntry*)calloc(1, sizeof(DGTexAddrEntry));
    if (!e) return NULL;
    e->addr = addr;
    e->lastUsedFrame = g_currentFrame;
    b = addrCacheSlot(addr);
    e->next = g_addrBuckets[b];
    g_addrBuckets[b] = e;
    g_addrCount++;
    return e;
}

/* ====================================================================
 * GPU pool
 * ==================================================================== */

static int poolCacheSlot(FxU32 contentHash) {
    FxU32 h = contentHash;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return (int)(h % DG_POOL_BUCKET_COUNT);
}

static DGTexPoolEntry* poolLookup(FxU32 contentHash) {
    int b = poolCacheSlot(contentHash);
    DGTexPoolEntry* e = g_poolBuckets[b];
    while (e) {
        if (e->contentHash == contentHash) return e;
        e = e->next;
    }
    return NULL;
}

static void freePoolEntry(DGTexPoolEntry* e) {
    if (!e) return;
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    free(e);
}

static void poolEvictTo(int targetCount);

/* Build a pool entry from an address entry's raw data. If a pool entry
 * already exists for the same contentHash, shares it (bumps refCount).
 * Returns the pool entry or NULL on failure. */
static DGTexPoolEntry* poolCreateFromAddr(DGTexAddrEntry* a) {
    DGTexPoolEntry* p;
    FxU32* rgba;
    HRESULT hr;
    D3D11_TEXTURE2D_DESC td;
    int b;

    if (!a || !a->rawData || !g_dg.device) return NULL;

    /* Existing entry with same content — just share. */
    p = poolLookup(a->contentHash);
    if (p) {
        p->refCount++;
        p->lastUsedFrame = g_currentFrame;
        return p;
    }

    /* Palettized decode needs the palette to be set. */
    if (a->isPalettized && !g_paletteValid) return NULL;

    /* Enforce pool cap before creating. 10% watermark so eviction
     * amortizes over ~400 inserts instead of running per-create. */
    if (g_poolCount >= DG_POOL_MAX_ENTRIES) {
        int before = g_poolCount;
        poolEvictTo(DG_POOL_MAX_ENTRIES * 9 / 10);
        dg_log("TEX POOL: evicted %d, now %d pool / %d addr at frame %u\n",
               before - g_poolCount, g_poolCount, g_addrCount, g_currentFrame);
    }

    rgba = (FxU32*)malloc(a->width * a->height * 4);
    if (!rgba) return NULL;
    convertToRGBA8(a->rawData, rgba, a->width, a->height, a->fmt);

    p = (DGTexPoolEntry*)calloc(1, sizeof(DGTexPoolEntry));
    if (!p) { free(rgba); return NULL; }

    memset(&td, 0, sizeof(td));
    td.Width = (UINT)a->width;
    td.Height = (UINT)a->height;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = ID3D11Device_CreateTexture2D(g_dg.device, &td, NULL, &p->tex);
    if (FAILED(hr)) {
        static int s_texFail = 0;
        if (s_texFail < 10) {
            dg_log("  CreateTexture2D FAILED hr=0x%08X for %dx%d (pool=%d addr=%d)\n",
                   (unsigned)hr, a->width, a->height, g_poolCount, g_addrCount);
            s_texFail++;
        }
        free(rgba); free(p); return NULL;
    }

    uploadLanczosMips(p->tex, rgba, a->width, a->height);

    hr = ID3D11Device_CreateShaderResourceView(g_dg.device,
        (ID3D11Resource*)p->tex, NULL, &p->srv);
    free(rgba);
    if (FAILED(hr)) {
        static int s_srvFail = 0;
        if (s_srvFail < 10) {
            dg_log("  CreateSRV FAILED hr=0x%08X for %dx%d\n",
                   (unsigned)hr, a->width, a->height);
            s_srvFail++;
        }
        ID3D11Texture2D_Release(p->tex);
        free(p);
        return NULL;
    }

    p->contentHash   = a->contentHash;
    p->width         = a->width;
    p->height        = a->height;
    p->fmt           = a->fmt;
    p->isPalettized  = a->isPalettized;
    p->refCount      = 1;
    p->lastUsedFrame = g_currentFrame;

    b = poolCacheSlot(a->contentHash);
    p->next = g_poolBuckets[b];
    g_poolBuckets[b] = p;
    g_poolCount++;
    return p;
}

/* Release the pool reference held by this address entry. Called when
 * the address's content is about to change. Does not evict the pool
 * entry — only decrements refCount. LRU handles actual eviction. */
static void addrReleasePool(DGTexAddrEntry* a) {
    DGTexPoolEntry* p;
    if (!a || !a->rawData) return;  /* never had content */
    p = poolLookup(a->contentHash);
    if (p && p->refCount > 0) p->refCount--;
}

/* qsort helper: sort by lastUsedFrame ascending (oldest first) */
static int comparePoolByAge(const void* a, const void* b) {
    const DGTexPoolEntry* ea = *(const DGTexPoolEntry* const*)a;
    const DGTexPoolEntry* eb = *(const DGTexPoolEntry* const*)b;
    if (ea->lastUsedFrame < eb->lastUsedFrame) return -1;
    if (ea->lastUsedFrame > eb->lastUsedFrame) return 1;
    return 0;
}

/* Evict oldest pool entries until g_poolCount <= targetCount. */
static void poolEvictTo(int targetCount) {
    DGTexPoolEntry** all;
    int count = 0, i, toEvict;

    if (g_poolCount <= targetCount) return;

    all = (DGTexPoolEntry**)malloc(sizeof(DGTexPoolEntry*) * g_poolCount);
    if (!all) return;

    for (i = 0; i < DG_POOL_BUCKET_COUNT; i++) {
        DGTexPoolEntry* e = g_poolBuckets[i];
        while (e) { all[count++] = e; e = e->next; }
    }
    qsort(all, count, sizeof(DGTexPoolEntry*), comparePoolByAge);

    toEvict = count - targetCount;
    for (i = 0; i < toEvict; i++) {
        DGTexPoolEntry* e = all[i];
        int b = poolCacheSlot(e->contentHash);
        DGTexPoolEntry** pp = &g_poolBuckets[b];
        while (*pp && *pp != e) pp = &(*pp)->next;
        if (*pp) {
            *pp = e->next;
            freePoolEntry(e);
            g_poolCount--;
        }
    }
    free(all);
}

/* ====================================================================
 * Public API
 * ==================================================================== */

void dg_tex_init(void) {
    memset(g_addrBuckets, 0, sizeof(g_addrBuckets));
    memset(g_poolBuckets, 0, sizeof(g_poolBuckets));
    memset(g_palette, 0, sizeof(g_palette));
    g_addrCount = 0;
    g_poolCount = 0;
    g_currentFrame = 0;
    g_paletteValid = 0;
    g_paletteHash = 0;
}

static void freeAllPool(void) {
    int i;
    for (i = 0; i < DG_POOL_BUCKET_COUNT; i++) {
        DGTexPoolEntry* e = g_poolBuckets[i];
        while (e) {
            DGTexPoolEntry* next = e->next;
            freePoolEntry(e);
            e = next;
        }
        g_poolBuckets[i] = NULL;
    }
    g_poolCount = 0;
}

static void freeAllAddr(void) {
    int i;
    for (i = 0; i < DG_TEX_BUCKET_COUNT; i++) {
        DGTexAddrEntry* e = g_addrBuckets[i];
        while (e) {
            DGTexAddrEntry* next = e->next;
            freeAddrEntry(e);
            e = next;
        }
        g_addrBuckets[i] = NULL;
    }
    g_addrCount = 0;
}

void dg_tex_shutdown(void) {
    freeAllPool();
    freeAllAddr();
    if (g_missSrv) { ID3D11ShaderResourceView_Release(g_missSrv); g_missSrv = NULL; }
    if (g_missTex) { ID3D11Texture2D_Release(g_missTex); g_missTex = NULL; }
}

void dg_tex_invalidate_all(void) {
    freeAllPool();
    freeAllAddr();
}

void dg_tex_set_palette(const FxU32* palette256) {
    if (palette256) {
        memcpy(g_palette, palette256, 256 * sizeof(FxU32));
        g_paletteValid = 1;
        g_paletteHash = computePaletteHash();
    }
}

void dg_tex_tick_frame(void) {
    g_currentFrame++;
    if ((g_currentFrame & 1023) == 0) {
        dg_log("TEX STATS: %d addr / %d pool at frame %u\n",
               g_addrCount, g_poolCount, g_currentFrame);
    }
}

void dg_tex_download(GrChipID_t tmu, FxU32 addr,
                     GrLOD_t smallLod, GrLOD_t largeLod,
                     GrAspectRatio_t aspect, GrTextureFormat_t fmt,
                     const void* data) {
    int w, h, dataSize, isPal;
    FxU32 rawHash, contentHash;
    DGTexAddrEntry* e;
    DGTexPoolEntry* p;

    (void)tmu; (void)smallLod;
    if (!data || !g_dg.device) return;

    /* Wrap TMU address into the 4MB window — KQ8's ever-advancing
     * allocator aliases onto the same slots once it passes 4MB. */
    addr &= DG_TMU_ADDR_MASK;

    texDimensions(largeLod, aspect, &w, &h);
    isPal = (fmt == GR_TEXFMT_P_8 || fmt == GR_TEXFMT_AP_88);
    dataSize = w * h * texBpp(fmt);
    rawHash = hashData(data, dataSize);
    contentHash = isPal ? (rawHash ^ g_paletteHash) : rawHash;

    e = addrFindOrCreate(addr);
    if (!e) return;
    e->lastUsedFrame = g_currentFrame;

    /* Unchanged content — just touch LRU on the pool entry (if any)
     * and return. */
    if (e->rawData && e->contentHash == contentHash &&
        e->width == w && e->height == h && e->fmt == fmt) {
        p = poolLookup(contentHash);
        if (p) p->lastUsedFrame = g_currentFrame;
        return;
    }

    /* Content changed at this address. Drop reference to the old
     * pool entry (if we had one) before overwriting our metadata. */
    addrReleasePool(e);

    /* (Re)allocate rawData buffer if size changed. */
    if (!e->rawData || e->rawDataSize != dataSize) {
        if (e->rawData) { free(e->rawData); e->rawData = NULL; }
        e->rawData = malloc(dataSize);
        if (!e->rawData) {
            e->rawDataSize = 0;
            return;
        }
        e->rawDataSize = dataSize;
    }
    memcpy(e->rawData, data, dataSize);

    e->contentHash  = contentHash;
    e->width        = w;
    e->height       = h;
    e->fmt          = fmt;
    e->isPalettized = isPal;
    e->paletteHash  = g_paletteHash;

    /* Look up or build the matching pool entry. Palettized entries with
     * no palette yet are deferred to dg_tex_get_srv. */
    p = poolLookup(contentHash);
    if (p) {
        p->refCount++;
        p->lastUsedFrame = g_currentFrame;
    } else if (!isPal || g_paletteValid) {
        poolCreateFromAddr(e);
    }

    {
        static int uploadCount = 0;
        uploadCount++;
        if (uploadCount <= 1000 || (uploadCount & 1023) == 0) {
            dg_log("UL#%d a=0x%X %dx%d f=%d aspect=%d size=%d ch=0x%X %s\n",
                   uploadCount, addr, w, h, fmt, aspect, dataSize, contentHash,
                   p ? "SHARE" : (isPal && !g_paletteValid) ? "DEFER" : "NEW");
        }
    }
}

ID3D11ShaderResourceView* dg_tex_get_srv(FxU32 addr) {
    DGTexAddrEntry* e;
    DGTexPoolEntry* p;

    addr &= DG_TMU_ADDR_MASK;
    e = addrLookup(addr);
    if (!e) {
        static int s_nolookup = 0;
        if (s_nolookup < 5) {
            dg_log("  SRV-MISS: addr=0x%X no addr entry\n", addr);
            s_nolookup++;
        }
        return NULL;
    }
    if (!e->rawData) {
        static int s_noraw = 0;
        if (s_noraw < 5) {
            dg_log("  SRV-MISS: addr=0x%X addr entry has no rawData\n", addr);
            s_noraw++;
        }
        return NULL;
    }

    e->lastUsedFrame = g_currentFrame;

    /* Palettized: if the palette changed since we last hashed this
     * entry, recompute the content hash so we point at (or create) the
     * right pool entry. */
    if (e->isPalettized && g_paletteValid && e->paletteHash != g_paletteHash) {
        FxU32 newRaw = hashData(e->rawData, e->rawDataSize);
        FxU32 newContent = newRaw ^ g_paletteHash;
        if (newContent != e->contentHash) {
            addrReleasePool(e);
            e->contentHash = newContent;
        }
        e->paletteHash = g_paletteHash;
    }

    /* Get — or rebuild — the matching pool entry. */
    p = poolLookup(e->contentHash);
    if (!p) {
        if (e->isPalettized && !g_paletteValid) return NULL;
        p = poolCreateFromAddr(e);
        if (!p) {
            static int s_nobuild = 0;
            if (s_nobuild < 5) {
                dg_log("  SRV-MISS: addr=0x%X pool rebuild failed (%dx%d fmt=%d)\n",
                       addr, e->width, e->height, e->fmt);
                s_nobuild++;
            }
            return NULL;
        }
    }
    p->lastUsedFrame = g_currentFrame;
    return p->srv;
}

int dg_tex_find_nearest_before(FxU32 addr, FxU32* outAddr, int* outW, int* outH,
                                int* outBpp, FxU32* outEndAddr) {
    int i;
    DGTexAddrEntry* best = NULL;
    addr &= DG_TMU_ADDR_MASK;
    for (i = 0; i < DG_TEX_BUCKET_COUNT; i++) {
        DGTexAddrEntry* e = g_addrBuckets[i];
        while (e) {
            if (e->rawData && e->addr <= addr) {
                if (!best || e->addr > best->addr) best = e;
            }
            e = e->next;
        }
    }
    if (!best) return 0;
    *outAddr = best->addr;
    *outW = best->width;
    *outH = best->height;
    *outBpp = texBpp(best->fmt);
    *outEndAddr = best->addr + (FxU32)(best->width * best->height * (*outBpp));
    return 1;
}
