/*
 * DirectGlide - Texture management implementation
 */

#include <initguid.h>
#include "d3d11_texture.h"
#include "d3d11_backend.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

static DGTexCacheEntry g_texCache[DG_TEX_CACHE_SIZE];
static FxU32 g_palette[256];
static int g_paletteValid = 0;

void dg_tex_init(void) {
    memset(g_texCache, 0, sizeof(g_texCache));
    memset(g_palette, 0, sizeof(g_palette));
    g_paletteValid = 0;
}

void dg_tex_shutdown(void) {
    int i;
    for (i = 0; i < DG_TEX_CACHE_SIZE; i++) {
        if (g_texCache[i].srv) { ID3D11ShaderResourceView_Release(g_texCache[i].srv); g_texCache[i].srv = NULL; }
        if (g_texCache[i].tex) { ID3D11Texture2D_Release(g_texCache[i].tex); g_texCache[i].tex = NULL; }
        if (g_texCache[i].rawData) { free(g_texCache[i].rawData); g_texCache[i].rawData = NULL; }
        g_texCache[i].valid = 0;
    }
}

void dg_tex_set_palette(const FxU32* palette256) {
    if (palette256) {
        static int logCount = 0;
        memcpy(g_palette, palette256, 256 * sizeof(FxU32));
        g_paletteValid = 1;
        if (logCount < 5) {
            dg_log("PALETTE SET: first entries: 0x%08X 0x%08X 0x%08X 0x%08X\n",
                   g_palette[0], g_palette[1], g_palette[2], g_palette[3]);
            logCount++;
        }
    }
}

void dg_tex_invalidate_all(void) {
    dg_tex_shutdown();
    dg_tex_init();
}

/* Hash TMU address to cache slot */
static int texCacheSlot(FxU32 addr) {
    FxU32 h = addr;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return (int)(h % DG_TEX_CACHE_SIZE);
}

/* Find slot for addr using linear probing */
static int texFindSlot(FxU32 addr) {
    int slot = texCacheSlot(addr);
    int i;
    for (i = 0; i < 64; i++) { /* probe up to 64 slots */
        int s = (slot + i) % DG_TEX_CACHE_SIZE;
        if (!g_texCache[s].valid || g_texCache[s].addr == addr)
            return s;
    }
    /* All probed slots full — evict the primary slot */
    return slot;
}

/* Find existing entry for addr */
static int texLookupSlot(FxU32 addr) {
    int slot = texCacheSlot(addr);
    int i;
    for (i = 0; i < 64; i++) {
        int s = (slot + i) % DG_TEX_CACHE_SIZE;
        if (g_texCache[s].valid && g_texCache[s].addr == addr)
            return s;
        if (!g_texCache[s].valid)
            return -1; /* empty slot = not found */
    }
    return -1;
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
    int slot, w, h;
    FxU32* rgba;
    HRESULT hr;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA initData;

    (void)tmu;
    if (!data || !g_dg.device) return;

    texDimensions(largeLod, aspect, &w, &h);
    slot = texFindSlot(addr);

    /* Evict old entry if slot is occupied */
    if (g_texCache[slot].valid) {
        if (g_texCache[slot].srv) ID3D11ShaderResourceView_Release(g_texCache[slot].srv);
        if (g_texCache[slot].tex) ID3D11Texture2D_Release(g_texCache[slot].tex);
        if (g_texCache[slot].rawData) { free(g_texCache[slot].rawData); g_texCache[slot].rawData = NULL; }
        g_texCache[slot].srv = NULL;
        g_texCache[slot].tex = NULL;
        g_texCache[slot].valid = 0;
    }

    /* If palettized and no palette yet, store raw data for deferred decode */
    if ((fmt == GR_TEXFMT_P_8 || fmt == GR_TEXFMT_AP_88) && !g_paletteValid) {
        int dataSize = w * h * texBpp(fmt);
        g_texCache[slot].rawData = malloc(dataSize);
        if (g_texCache[slot].rawData) {
            memcpy(g_texCache[slot].rawData, data, dataSize);
            g_texCache[slot].rawDataSize = dataSize;
            g_texCache[slot].addr = addr;
            g_texCache[slot].width = w;
            g_texCache[slot].height = h;
            g_texCache[slot].fmt = fmt;
            g_texCache[slot].storedSmallLod = smallLod;
            g_texCache[slot].storedLargeLod = largeLod;
            g_texCache[slot].storedAspect = aspect;
            g_texCache[slot].valid = 1;
            g_texCache[slot].isPalettized = 1;
            g_texCache[slot].srv = NULL;
            g_texCache[slot].tex = NULL;
        }
        {
            static int defCount = 0;
            if (defCount < 5)
                dg_log("TEX DEFERRED: addr=0x%X %dx%d fmt=%d (palette not set)\n", addr, w, h, fmt);
            defCount++;
        }
        return; /* Don't create GPU texture yet */
    }

    /* Convert to RGBA8 */
    rgba = (FxU32*)malloc(w * h * 4);
    if (!rgba) return;
    convertToRGBA8(data, rgba, w, h, fmt);

    /* Create D3D11 texture */
    memset(&td, 0, sizeof(td));
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    memset(&initData, 0, sizeof(initData));
    initData.pSysMem = rgba;
    initData.SysMemPitch = (UINT)(w * 4);

    hr = ID3D11Device_CreateTexture2D(g_dg.device, &td, &initData, &g_texCache[slot].tex);
    if (FAILED(hr)) {
        free(rgba);
        return;
    }

    hr = ID3D11Device_CreateShaderResourceView(g_dg.device,
        (ID3D11Resource*)g_texCache[slot].tex, NULL, &g_texCache[slot].srv);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(g_texCache[slot].tex);
        g_texCache[slot].tex = NULL;
        free(rgba);
        return;
    }

    g_texCache[slot].addr = addr;
    g_texCache[slot].width = w;
    g_texCache[slot].height = h;
    g_texCache[slot].fmt = fmt;
    g_texCache[slot].valid = 1;
    g_texCache[slot].isPalettized = (fmt == GR_TEXFMT_P_8 || fmt == GR_TEXFMT_AP_88);

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
    int slot = texLookupSlot(addr);
    if (slot < 0) return NULL;

    /* Resolve deferred palettized texture */
    if (g_texCache[slot].rawData && !g_texCache[slot].srv) {
        if (!g_paletteValid) return NULL; /* still no palette */
        {
            static int resCount = 0;
            if (resCount < 5)
                dg_log("TEX RESOLVE: addr=0x%X %dx%d fmt=%d\n",
                       g_texCache[slot].addr, g_texCache[slot].width, g_texCache[slot].height, g_texCache[slot].fmt);
            resCount++;
        }
        DGTexCacheEntry* e = &g_texCache[slot];
        FxU32* rgba;
        HRESULT hr;
        D3D11_TEXTURE2D_DESC td = {0};
        D3D11_SUBRESOURCE_DATA initData = {0};

        rgba = (FxU32*)malloc(e->width * e->height * 4);
        if (rgba) {
            convertToRGBA8(e->rawData, rgba, e->width, e->height, e->fmt);

            td.Width = (UINT)e->width;
            td.Height = (UINT)e->height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            initData.pSysMem = rgba;
            initData.SysMemPitch = (UINT)(e->width * 4);

            hr = ID3D11Device_CreateTexture2D(g_dg.device, &td, &initData, &e->tex);
            if (SUCCEEDED(hr)) {
                ID3D11Device_CreateShaderResourceView(g_dg.device,
                    (ID3D11Resource*)e->tex, NULL, &e->srv);
            }
            free(rgba);
        }
        free(e->rawData);
        e->rawData = NULL;
    }

    return g_texCache[slot].srv;
}
