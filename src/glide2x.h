/*
 * DirectGlide - Glide 2.x to DirectX 11 Wrapper
 * glide2x.h - Glide 2.x API type definitions, enums, and structures
 *
 * Reconstructed from the Glide 2.x SDK and OpenGlide sources.
 */

#ifndef GLIDE2X_H
#define GLIDE2X_H

#include <windows.h>

/* Basic types */
typedef unsigned long  FxU32;
typedef long           FxI32;
typedef unsigned short FxU16;
typedef short          FxI16;
typedef unsigned char  FxU8;
typedef int            FxBool;
typedef float          FxFloat;

#define FXTRUE  1
#define FXFALSE 0

/* Glide handle types */
typedef FxU32 GrContext_t;
typedef FxU32 GrMipMapId_t;
typedef int   GrChipID_t;
typedef FxU32 GrColor_t;
typedef FxU32 GrAlpha_t;
typedef FxI32 GrLock_t;
typedef FxI32 GrBuffer_t;
typedef FxI32 GrLfbWriteMode_t;

/* Opaque state buffer - large enough for save/restore */
typedef struct { FxU32 data[512]; } GrState;

/* ============================================================
 * Enumerations
 * ============================================================ */

/* Color format */
#define GR_COLORFORMAT_ARGB     0x0
#define GR_COLORFORMAT_ABGR     0x1
#define GR_COLORFORMAT_RGBA     0x2
#define GR_COLORFORMAT_BGRA     0x3

/* Origin location */
#define GR_ORIGIN_UPPER_LEFT    0x0
#define GR_ORIGIN_LOWER_LEFT    0x1
typedef FxI32 GrOriginLocation_t;

/* Screen resolution */
#define GR_RESOLUTION_320x200   0x0
#define GR_RESOLUTION_320x240   0x1
#define GR_RESOLUTION_400x256   0x2
#define GR_RESOLUTION_512x384   0x3
#define GR_RESOLUTION_640x200   0x4
#define GR_RESOLUTION_640x350   0x5
#define GR_RESOLUTION_640x400   0x6
#define GR_RESOLUTION_640x480   0x7
#define GR_RESOLUTION_800x600   0x8
#define GR_RESOLUTION_960x720   0x9
#define GR_RESOLUTION_856x480   0xA
#define GR_RESOLUTION_512x256   0xB
#define GR_RESOLUTION_1024x768  0xC
#define GR_RESOLUTION_1280x1024 0xD
#define GR_RESOLUTION_1600x1200 0xE
typedef FxI32 GrScreenResolution_t;

/* Screen refresh rate */
#define GR_REFRESH_60Hz   0x0
#define GR_REFRESH_70Hz   0x1
#define GR_REFRESH_72Hz   0x2
#define GR_REFRESH_75Hz   0x3
#define GR_REFRESH_80Hz   0x4
#define GR_REFRESH_85Hz   0x5
#define GR_REFRESH_90Hz   0x6
#define GR_REFRESH_100Hz  0x7
#define GR_REFRESH_120Hz  0x8
typedef FxI32 GrScreenRefresh_t;

typedef FxI32 GrColorFormat_t;

/* Combine function */
#define GR_COMBINE_FUNCTION_ZERO              0x0
#define GR_COMBINE_FUNCTION_LOCAL             0x1
#define GR_COMBINE_FUNCTION_LOCAL_ALPHA        0x2
#define GR_COMBINE_FUNCTION_SCALE_OTHER       0x3
#define GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL        0x4
#define GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA  0x5
#define GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL      0x6
#define GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL       0x7
#define GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA 0x8
#define GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL             0x9
#define GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA       0x10
typedef FxI32 GrCombineFunction_t;

/* Combine factor */
#define GR_COMBINE_FACTOR_ZERO               0x0
#define GR_COMBINE_FACTOR_LOCAL              0x1
#define GR_COMBINE_FACTOR_OTHER_ALPHA        0x2
#define GR_COMBINE_FACTOR_LOCAL_ALPHA        0x3
#define GR_COMBINE_FACTOR_TEXTURE_ALPHA      0x4
#define GR_COMBINE_FACTOR_TEXTURE_RGB        0x5
#define GR_COMBINE_FACTOR_DETAIL_FACTOR      0x6
#define GR_COMBINE_FACTOR_LOD_FRACTION       0x7
#define GR_COMBINE_FACTOR_ONE                0x8
#define GR_COMBINE_FACTOR_ONE_MINUS_LOCAL    0x9
#define GR_COMBINE_FACTOR_ONE_MINUS_OTHER_ALPHA   0xA
#define GR_COMBINE_FACTOR_ONE_MINUS_LOCAL_ALPHA    0xB
#define GR_COMBINE_FACTOR_ONE_MINUS_TEXTURE_ALPHA  0xC
#define GR_COMBINE_FACTOR_ONE_MINUS_DETAIL_FACTOR  0xD
#define GR_COMBINE_FACTOR_ONE_MINUS_LOD_FRACTION   0xE
typedef FxI32 GrCombineFactor_t;

/* Combine local */
#define GR_COMBINE_LOCAL_ITERATED   0x0
#define GR_COMBINE_LOCAL_CONSTANT   0x1
#define GR_COMBINE_LOCAL_DEPTH      0x2
typedef FxI32 GrCombineLocal_t;

/* Combine other */
#define GR_COMBINE_OTHER_ITERATED   0x0
#define GR_COMBINE_OTHER_TEXTURE    0x1
#define GR_COMBINE_OTHER_CONSTANT   0x2
#define GR_COMBINE_OTHER_NONE       0x3
typedef FxI32 GrCombineOther_t;

/* Alpha blend — values from the real Glide 2.x SDK */
#define GR_BLEND_ZERO                    0x0
#define GR_BLEND_SRC_ALPHA               0x4
#define GR_BLEND_SRC_COLOR               0x2
#define GR_BLEND_DST_ALPHA               0x6
#define GR_BLEND_DST_COLOR               0x1
#define GR_BLEND_ONE                     0x1
#define GR_BLEND_ONE_MINUS_SRC_ALPHA     0x5
#define GR_BLEND_ONE_MINUS_SRC_COLOR     0x3
#define GR_BLEND_ONE_MINUS_DST_ALPHA     0x7
#define GR_BLEND_PREFOG_COLOR            0xF
#define GR_BLEND_ALPHA_SATURATE          0xF
typedef FxI32 GrAlphaBlendFnc_t;

/* Compare function (depth test, alpha test) */
#define GR_CMP_NEVER     0x0
#define GR_CMP_LESS      0x1
#define GR_CMP_EQUAL     0x2
#define GR_CMP_LEQUAL    0x3
#define GR_CMP_GREATER   0x4
#define GR_CMP_NOTEQUAL  0x5
#define GR_CMP_GEQUAL    0x6
#define GR_CMP_ALWAYS    0x7
typedef FxI32 GrCmpFnc_t;

/* Cull mode */
#define GR_CULL_DISABLE    0x0
#define GR_CULL_NEGATIVE   0x1
#define GR_CULL_POSITIVE   0x2
typedef FxI32 GrCullMode_t;

/* Depth buffer mode */
#define GR_DEPTHBUFFER_DISABLE          0x0
#define GR_DEPTHBUFFER_ZBUFFER          0x1
#define GR_DEPTHBUFFER_WBUFFER          0x2
#define GR_DEPTHBUFFER_ZBUFFER_COMPARE_TO_BIAS  0x3
#define GR_DEPTHBUFFER_WBUFFER_COMPARE_TO_BIAS  0x4
typedef FxI32 GrDepthBufferMode_t;

/* Dither mode */
#define GR_DITHER_DISABLE   0x0
#define GR_DITHER_2x2       0x1
#define GR_DITHER_4x4       0x2
typedef FxI32 GrDitherMode_t;

/* Fog mode */
#define GR_FOG_DISABLE                 0x0
#define GR_FOG_WITH_ITERATED_ALPHA     0x1
#define GR_FOG_WITH_TABLE              0x2
#define GR_FOG_WITH_ITERATED_Z         0x4
#define GR_FOG_MULT2                   0x100
#define GR_FOG_ADD2                    0x200
typedef FxI32 GrFogMode_t;

/* Chromakey mode */
#define GR_CHROMAKEY_DISABLE  0x0
#define GR_CHROMAKEY_ENABLE   0x1
typedef FxI32 GrChromakeyMode_t;

/* Texture format */
#define GR_TEXFMT_8BIT                 0x0
#define GR_TEXFMT_RGB_332              0x0
#define GR_TEXFMT_YIQ_422              0x1
#define GR_TEXFMT_ALPHA_8              0x2
#define GR_TEXFMT_INTENSITY_8          0x3
#define GR_TEXFMT_ALPHA_INTENSITY_44   0x4
#define GR_TEXFMT_P_8                  0x5
#define GR_TEXFMT_RSVD0               0x6
#define GR_TEXFMT_RSVD1               0x7
#define GR_TEXFMT_16BIT               0x8
#define GR_TEXFMT_ARGB_8332           0x8
#define GR_TEXFMT_AYIQ_8422           0x9
#define GR_TEXFMT_RGB_565             0xA
#define GR_TEXFMT_ARGB_1555           0xB
#define GR_TEXFMT_ARGB_4444           0xC
#define GR_TEXFMT_ALPHA_INTENSITY_88  0xD
#define GR_TEXFMT_AP_88               0xE
#define GR_TEXFMT_RSVD2               0xF
typedef FxI32 GrTextureFormat_t;

/* Texture clamp mode */
#define GR_TEXTURECLAMP_WRAP    0x0
#define GR_TEXTURECLAMP_CLAMP   0x1
typedef FxI32 GrTextureClampMode_t;

/* Texture filter mode */
#define GR_TEXTUREFILTER_POINT_SAMPLED   0x0
#define GR_TEXTUREFILTER_BILINEAR        0x1
typedef FxI32 GrTextureFilterMode_t;

/* Texture mipmap mode */
#define GR_MIPMAP_DISABLE        0x0
#define GR_MIPMAP_NEAREST        0x1
#define GR_MIPMAP_NEAREST_DITHER 0x2
typedef FxI32 GrMipMapMode_t;

/* Texture LOD (level of detail) */
#define GR_LOD_256   0x0
#define GR_LOD_128   0x1
#define GR_LOD_64    0x2
#define GR_LOD_32    0x3
#define GR_LOD_16    0x4
#define GR_LOD_8     0x5
#define GR_LOD_4     0x6
#define GR_LOD_2     0x7
#define GR_LOD_1     0x8
typedef FxI32 GrLOD_t;

/* Texture aspect ratio */
#define GR_ASPECT_8x1   0x0
#define GR_ASPECT_4x1   0x1
#define GR_ASPECT_2x1   0x2
#define GR_ASPECT_1x1   0x3
#define GR_ASPECT_1x2   0x4
#define GR_ASPECT_1x4   0x5
#define GR_ASPECT_1x8   0x6
typedef FxI32 GrAspectRatio_t;

/* Buffer types */
#define GR_BUFFER_FRONTBUFFER   0x0
#define GR_BUFFER_BACKBUFFER    0x1
#define GR_BUFFER_AUXBUFFER     0x2
#define GR_BUFFER_DEPTHBUFFER   0x2
#define GR_BUFFER_ALPHABUFFER   0x3
#define GR_BUFFER_TRIPLEBUFFER  0x4

/* LFB lock type */
#define GR_LFB_READ_ONLY   0x0
#define GR_LFB_WRITE_ONLY  0x1
#define GR_LFB_IDLE         0x0
#define GR_LFB_NOIDLE       0x10

/* LFB write mode */
#define GR_LFBWRITEMODE_565        0x0
#define GR_LFBWRITEMODE_555        0x1
#define GR_LFBWRITEMODE_1555       0x2
#define GR_LFBWRITEMODE_RESERVED1  0x3
#define GR_LFBWRITEMODE_888        0x4
#define GR_LFBWRITEMODE_8888       0x5
#define GR_LFBWRITEMODE_RESERVED2  0x6
#define GR_LFBWRITEMODE_RESERVED3  0x7
#define GR_LFBWRITEMODE_RESERVED4  0x8
#define GR_LFBWRITEMODE_RESERVED5  0x9
#define GR_LFBWRITEMODE_RESERVED6  0xA
#define GR_LFBWRITEMODE_RESERVED7  0xB
#define GR_LFBWRITEMODE_565_DEPTH  0xC
#define GR_LFBWRITEMODE_555_DEPTH  0xD
#define GR_LFBWRITEMODE_1555_DEPTH 0xE
#define GR_LFBWRITEMODE_ZA16       0xF
#define GR_LFBWRITEMODE_ANY        0xFF

/* Texture table type */
#define GR_TEXTABLE_NCC0       0x0
#define GR_TEXTABLE_NCC1       0x1
#define GR_TEXTABLE_PALETTE    0x2

/* TMU */
#define GR_TMU0   0x0
#define GR_TMU1   0x1
#define GR_TMU2   0x2

/* Hints */
#define GR_HINT_STWHINT    0x0
#define GR_STWHINT_W_DIFF_FBI   0x01
#define GR_STWHINT_W_DIFF_TMU0  0x02
#define GR_STWHINT_ST_DIFF_TMU0 0x04
#define GR_STWHINT_W_DIFF_TMU1  0x08
#define GR_STWHINT_ST_DIFF_TMU1 0x10
#define GR_STWHINT_W_DIFF_TMU2  0x20
#define GR_STWHINT_ST_DIFF_TMU2 0x40

/* Render buffer */
#define GR_BUFFER_TEXTUREBUFFER  0x5
#define GR_BUFFER_TEXTUREAUXBUFFER 0x6

/* Alpha source (gu helper) */
#define GR_ALPHASOURCE_CC_ALPHA           0x0
#define GR_ALPHASOURCE_ITERATED_ALPHA     0x1
#define GR_ALPHASOURCE_TEXTURE_ALPHA      0x2
#define GR_ALPHASOURCE_TEXTURE_ALPHA_TIMES_ITERATED_ALPHA 0x3

/* Color combine function (gu helper) */
#define GR_COLORCOMBINE_ZERO              0x0
#define GR_COLORCOMBINE_CCRGB             0x1
#define GR_COLORCOMBINE_ITRGB             0x2
#define GR_COLORCOMBINE_ITRGB_DELTA0      0x3
#define GR_COLORCOMBINE_DECAL_TEXTURE     0x4
#define GR_COLORCOMBINE_TEXTURE_TIMES_CCRGB   0x5
#define GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB   0x6
#define GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB_DELTA0   0x7
#define GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB_ADD_ALPHA 0x8
#define GR_COLORCOMBINE_TEXTURE_TIMES_ALPHA   0x9
#define GR_COLORCOMBINE_TEXTURE_TIMES_ALPHA_ADD_ITRGB 0xA
#define GR_COLORCOMBINE_TEXTURE_ADD_ITRGB     0xB
#define GR_COLORCOMBINE_TEXTURE_SUB_ITRGB     0xC
#define GR_COLORCOMBINE_CCRGB_BLEND_ITRGB_ON_TEXALPHA 0xD
#define GR_COLORCOMBINE_DIFF_SPEC_A           0xE
#define GR_COLORCOMBINE_DIFF_SPEC_B           0xF
#define GR_COLORCOMBINE_ONE                   0x10

/* Texture combine function (gu helper) */
#define GR_TEXTURECOMBINE_ZERO             0x0
#define GR_TEXTURECOMBINE_DECAL            0x1
#define GR_TEXTURECOMBINE_OTHER            0x2
#define GR_TEXTURECOMBINE_ADD              0x3
#define GR_TEXTURECOMBINE_MULTIPLY         0x4
#define GR_TEXTURECOMBINE_SUBTRACT         0x5
#define GR_TEXTURECOMBINE_DETAIL           0x6
#define GR_TEXTURECOMBINE_DETAIL_OTHER     0x7
#define GR_TEXTURECOMBINE_TRILINEAR_ODD    0x8
#define GR_TEXTURECOMBINE_TRILINEAR_EVEN   0x9
#define GR_TEXTURECOMBINE_ONE              0xA

/* SST type */
#define GR_SSTTYPE_VOODOO    0
#define GR_SSTTYPE_SST96     1
#define GR_SSTTYPE_AT3D      2
#define GR_SSTTYPE_VOODOO2   3

/* Number of fog table entries */
#define GR_FOG_TABLE_SIZE  64

/* Max TMUs */
#define GLIDE_NUM_TMU  2

/* ============================================================
 * Structures
 * ============================================================ */

/* Texture coordinates per TMU */
typedef struct {
    float sow;
    float tow;
    float oow;
} GrTmuVertex;

/* Glide vertex */
typedef struct {
    float x, y, z;
    float r, g, b;
    float ooz;
    float a;
    float oow;
    GrTmuVertex tmuvtx[GLIDE_NUM_TMU];
} GrVertex;

/* LFB info returned by grLfbLock */
typedef struct {
    int             size;
    void*           lfbPtr;
    FxU32           strideInBytes;
    GrLfbWriteMode_t writeMode;
    GrOriginLocation_t origin;
} GrLfbInfo_t;

/* Hardware configuration structures */
typedef struct {
    int  fbRam;
    int  fbiRev;
    int  nTmu;
    BOOL SliDetected;
    struct {
        int tmuRev;
        int tmuRam;
    } tmuConfig[GLIDE_NUM_TMU];
} GrVoodooConfig_t;

typedef GrVoodooConfig_t GrVoodoo2Config_t;

typedef struct {
    int fbRam;
    int fbiRev;
    int nTmu;
} GrSst96Config_t;

typedef GrSst96Config_t GrAT3DConfig_t;

typedef struct {
    int num_sst;
    struct {
        int type;
        union {
            GrVoodooConfig_t  VoodooConfig;
            GrSst96Config_t   SST96Config;
            GrAT3DConfig_t    AT3DConfig;
            GrVoodoo2Config_t Voodoo2Config;
        } sstBoard;
    } SSTs[4];
} GrHwConfiguration;

/* Performance stats */
typedef struct {
    float pixelsIn;
    float chromaFail;
    float zFuncFail;
    float aFuncFail;
    float pixelsOut;
} GrSstPerfStats_t;

/* Fog table entry */
typedef FxU8 GrFog_t;

/* NCC table (Narrow Channel Compression) */
typedef struct {
    FxU8  yRGB[16];
    FxI16 iRGB[4][3];
    FxI16 qRGB[4][3];
    FxU32 packed_data[12];
} GuNccTable;

/* Texture table union */
typedef union {
    GuNccTable nccTable;
    FxU32      palette[256];
} GuTexTable;

/* Texture info for gu* functions */
typedef struct {
    GrLOD_t            smallLod;
    GrLOD_t            largeLod;
    GrAspectRatio_t    aspectRatio;
    GrTextureFormat_t  format;
    void*              data;
} GrTexInfo;

/* Mipmap info for gu* texture management */
typedef struct {
    GrChipID_t         tmu;
    FxU32              startAddress;
    FxBool             evenOdd;
    GrTexInfo          info;
    GrMipMapId_t       nextMipMap;
    FxBool             trilinear;
    /* internal tracking */
    FxU32              memRequired;
} GrMipMapInfo;

/* 3df file info (for gu3dfGetInfo/gu3dfLoad) */
typedef struct {
    GrTexInfo   info;
    FxU32       memRequired;
    void*       data;
    GuTexTable* table;
} Gu3dfInfo;

/* Error callback type */
typedef void (*GrErrorCallbackFnc_t)(const char* string, FxBool fatal);

/* ============================================================
 * Resolution helper
 * ============================================================ */

static inline void GlideResolutionToWH(GrScreenResolution_t res, int* w, int* h) {
    switch (res) {
        case GR_RESOLUTION_320x200:   *w = 320;  *h = 200;  break;
        case GR_RESOLUTION_320x240:   *w = 320;  *h = 240;  break;
        case GR_RESOLUTION_400x256:   *w = 400;  *h = 256;  break;
        case GR_RESOLUTION_512x384:   *w = 512;  *h = 384;  break;
        case GR_RESOLUTION_640x200:   *w = 640;  *h = 200;  break;
        case GR_RESOLUTION_640x350:   *w = 640;  *h = 350;  break;
        case GR_RESOLUTION_640x400:   *w = 640;  *h = 400;  break;
        case GR_RESOLUTION_640x480:   *w = 640;  *h = 480;  break;
        case GR_RESOLUTION_800x600:   *w = 800;  *h = 600;  break;
        case GR_RESOLUTION_960x720:   *w = 960;  *h = 720;  break;
        case GR_RESOLUTION_856x480:   *w = 856;  *h = 480;  break;
        case GR_RESOLUTION_512x256:   *w = 512;  *h = 256;  break;
        case GR_RESOLUTION_1024x768:  *w = 1024; *h = 768;  break;
        case GR_RESOLUTION_1280x1024: *w = 1280; *h = 1024; break;
        case GR_RESOLUTION_1600x1200: *w = 1600; *h = 1200; break;
        default:                      *w = 640;  *h = 480;  break;
    }
}

#endif /* GLIDE2X_H */
