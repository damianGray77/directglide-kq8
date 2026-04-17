/*
 * DirectGlide - Texture management
 */

#ifndef DG_D3D11_TEXTURE_H
#define DG_D3D11_TEXTURE_H

#include "glide2x.h"
#include <d3d11.h>

#define DG_TEX_BUCKET_COUNT 16384  /* number of hash buckets (head-of-list each) */

typedef struct DGTexCacheEntry DGTexCacheEntry;
struct DGTexCacheEntry {
    FxU32 addr;           /* TMU memory address (key) */
    int   width, height;
    GrTextureFormat_t fmt;
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    int   valid;
    int   isPalettized;
    void* rawData;
    int   rawDataSize;
    FxU32 paletteHash;
    FxU32 dataHash;
    GrLOD_t storedSmallLod, storedLargeLod;
    GrAspectRatio_t storedAspect;
    DGTexCacheEntry* next;   /* chained hash bucket linked list */
    unsigned lastUsedFrame;  /* for LRU eviction */
};

/* Bump this each frame; texture entries older than MAX_ENTRY_AGE are evictable */
void dg_tex_tick_frame(void);

/* Fallback SRV for cache misses — deterministic visible indicator */
ID3D11ShaderResourceView* dg_tex_get_miss_srv(void);

void dg_tex_init(void);
void dg_tex_shutdown(void);

/* Store palette for palettized texture decoding */
void dg_tex_set_palette(const FxU32* palette256);

/* Upload a texture to the cache */
void dg_tex_download(GrChipID_t tmu, FxU32 addr,
                     GrLOD_t smallLod, GrLOD_t largeLod,
                     GrAspectRatio_t aspect, GrTextureFormat_t fmt,
                     const void* data);

/* Get SRV for a cached texture address; returns NULL if not found */
ID3D11ShaderResourceView* dg_tex_get_srv(FxU32 addr);

/* Invalidate all cached textures */
void dg_tex_invalidate_all(void);

#endif /* DG_D3D11_TEXTURE_H */
