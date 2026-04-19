/*
 * DirectGlide - Texture management
 *
 * Architecture (v2, OpenGlide-inspired):
 *   - Address map: one entry per TMU address (wrapped to 4MB) holding
 *     content hash + raw-data backup + metadata.
 *   - GPU texture pool: one entry per unique content hash, holding the
 *     actual D3D11 texture + SRV. Multiple addresses with identical
 *     content share a single GPU texture.
 *   - 4MB TMU cap: KQ8 addresses are masked into a 4MB window, matching
 *     Voodoo2 hardware.
 *   - LRU eviction on the GPU pool. Addresses are retained (with
 *     rawData), so if a pool entry is evicted, the next bind rebuilds
 *     it from the cached raw data.
 */

#ifndef DG_D3D11_TEXTURE_H
#define DG_D3D11_TEXTURE_H

#include "glide2x.h"
#include <d3d11.h>

/* Virtual TMU memory size — matches Voodoo2 (4MB). Game-supplied
 * addresses are masked into this range. */
#define DG_TMU_SIZE       (4 * 1024 * 1024)
#define DG_TMU_ADDR_MASK  (DG_TMU_SIZE - 1)

/* Hash-bucket counts. Address map is larger because it may have more
 * unique entries (one per live TMU offset); pool stays smaller. */
#define DG_TEX_BUCKET_COUNT  16384
#define DG_POOL_BUCKET_COUNT 4096
#define DG_POOL_MAX_ENTRIES  4096
/* Hard cap on address map entries. KQ8 can generate hundreds of
 * thousands of unique masked addresses within 4MB if its allocator
 * sub-divides finely — letting the addr map grow unbounded consumed
 * enough of the 32-bit process address space to trigger OOM in the
 * game's own allocations. LRU-evicted addr entries return NULL from
 * get_srv until the game re-downloads them. */
#define DG_ADDR_MAX_ENTRIES  16384

/* Per-TMU-address tracking. Holds content hash + raw bytes backup so
 * evicted pool entries can be rebuilt. No D3D11 resources here. */
typedef struct DGTexAddrEntry DGTexAddrEntry;
struct DGTexAddrEntry {
    FxU32 addr;
    FxU32 contentHash;        /* hash of rawData (XOR paletteHash for palettized) */
    int   width, height;
    GrTextureFormat_t fmt;
    int   isPalettized;
    void* rawData;
    int   rawDataSize;
    FxU32 paletteHash;        /* palette snapshot at upload time */
    unsigned lastUsedFrame;
    DGTexAddrEntry* next;
};

/* One per unique content hash in the GPU pool. Holds the D3D11 texture. */
typedef struct DGTexPoolEntry DGTexPoolEntry;
struct DGTexPoolEntry {
    FxU32 contentHash;
    int   width, height;
    GrTextureFormat_t fmt;
    int   isPalettized;
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    int   refCount;
    unsigned lastUsedFrame;
    DGTexPoolEntry* next;
};

/* Bump this each frame; runs pool LRU when count exceeds cap. */
void dg_tex_tick_frame(void);

/* Fallback SRV for cache misses. */
ID3D11ShaderResourceView* dg_tex_get_miss_srv(void);

void dg_tex_init(void);
void dg_tex_shutdown(void);

/* Store palette for palettized texture decoding. */
void dg_tex_set_palette(const FxU32* palette256);

/* Upload a texture to the cache. Address is wrapped into the 4MB TMU
 * window. If an entry already exists with matching content hash, this
 * is a no-op. Otherwise the new content is stored in both the address
 * map (raw bytes) and the pool (D3D11 resources) — with pool dedup so
 * identical content at multiple addresses shares one GPU texture. */
void dg_tex_download(GrChipID_t tmu, FxU32 addr,
                     GrLOD_t smallLod, GrLOD_t largeLod,
                     GrAspectRatio_t aspect, GrTextureFormat_t fmt,
                     const void* data);

/* Get SRV for a cached texture address; returns NULL if not found. */
ID3D11ShaderResourceView* dg_tex_get_srv(FxU32 addr);

/* Diagnostic: find nearest cached address at or before `addr`. */
int dg_tex_find_nearest_before(FxU32 addr, FxU32* outAddr, int* outW, int* outH,
                                int* outBpp, FxU32* outEndAddr);

/* Invalidate all cached textures (called on grSstWinClose and
 * guTexMemReset). */
void dg_tex_invalidate_all(void);

#endif /* DG_D3D11_TEXTURE_H */
