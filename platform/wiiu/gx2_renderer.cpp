#include "port_heap.h"
#include "runtime.h"
#include <cstdio>
#ifndef SMS_GX2_ASYNC_PEEK
#define SMS_GX2_ASYNC_PEEK 1
#endif
#ifndef SMS_GX2_TEV_UNTEXTURED
#define SMS_GX2_TEV_UNTEXTURED 1
#endif
#ifndef SMS_GX2_VX_BRACKET
#define SMS_GX2_VX_BRACKET 0
#endif
#ifndef SMS_GX2_GPU_XF_DELAY
#define SMS_GX2_GPU_XF_DELAY 0
#endif
#ifndef SMS_GX2_VX_ORACLE
#define SMS_GX2_VX_ORACLE 0
#endif
#ifndef SMS_GX2_DEBUG_DUMP
#define SMS_GX2_DEBUG_DUMP 0
#endif
#ifndef SMS_GX2_DEBUG_PIXEL_TRACE
#define SMS_GX2_DEBUG_PIXEL_TRACE 0
#endif
#ifndef SMS_GX2_DEBUG_DRAW_LIMIT
#define SMS_GX2_DEBUG_DRAW_LIMIT 0
#endif
#include <coreinit/memdefaultheap.h>
/* Small GX2 host for the shared GX register decoder.
 *
 * The decoder still sees the original GameCube BP/CP/XF stream.  This file is
 * the platform seam: it turns the packed HostVertex stream into Latte
 * attributes, applies the XF transform on the CPU, and binds the two shader
 * variants generated with the lus-rebase GX2 packer.  The TEV selection is
 * intentionally conservative in stage 1: the common vertex-colour pass and
 * one texture-modulate pass cover the title and most early menu geometry.
 */
#include "../gx/src/gx_internal.h"
#include "gx2_shader_pack.h"
#include "gx2_shader_inl.h"

#include <gx2/draw.h>
#include <gx2/swap.h>
#include <gx2/clear.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <coreinit/title.h>
#include <coreinit/cache.h>
#include <coreinit/time.h>
#include <whb/gfx.h>
#include <proc_ui/procui.h>
#include <whb/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <malloc.h>
#include <new>
#include <initializer_list>
#include <vector>

namespace gx {

static GX2ColorBuffer* renderColour();
static GX2DepthBuffer* renderDepth();
uint32_t gx2TexLevelBytes(uint32_t fmt, uint32_t w, uint32_t h);
void gx2DecodeTexture(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h,
                      const uint8_t* tlut, uint32_t tlutFmt, uint8_t* out);

uint32_t g_displayFrames = 0;
double g_flushSeconds = 0.0;
double g_presentWaited = 0.0, g_presentDrain = 0.0;
bool g_gxStats = false;
uint64_t g_statHiresBytes = 0;

static bool s_ready;
static bool s_frameActive;
static bool s_gpuXfEnabled = true;
static bool s_vertexSelfCheck;
static uint32_t s_vertexSelfCheckMismatches;
static uint32_t s_vertexSelfCheckSamples;
static bool initTevVertex();
#if SMS_GX2_DEBUG_PIXEL_TRACE
static void debugPixelTrace(uint32_t draw);
#endif

static bool envFlag(const char* name, bool fallback) {
    const char* value = getenv(name);
    if (!value || !*value) return fallback;
    return value[0] != '0' && value[0] != 'n' && value[0] != 'N' &&
           value[0] != 'f' && value[0] != 'F';
}

static bool sdMarkerPresent(const char* name) {
    // Markers are intentionally read-only.  Try the explicit SD devoptab name
    // first (the Cemu portable card exposes it), then the resolved app root
    // used by Cafe's stdio implementation.
    char sdPath[160], appPath[160];
    std::snprintf(sdPath, sizeof(sdPath), "sd:/wiiu/apps/sms/%s", name);
    std::snprintf(appPath, sizeof(appPath), "%s%s", sms_wiiu_app_root(), name);
    const char* paths[] = {sdPath, appPath};
    for (const char* path : paths) {
        FILE* marker = std::fopen(path, "rb");
        if (!marker) continue;
        std::fclose(marker);
        WHBLogPrintf("SMS Wii U: marker %s present (%s)", name, path);
        return true;
    }
    return false;
}

struct FixedShader {
    GX2VertexShader vertex;
    GX2PixelShader pixel;
    GX2FetchShader fetch;
    GX2AttribStream attrs[3];
    uint32_t stride;
    uint32_t nattrs;
    bool textured;
    void* vsProgram;
    void* psProgram;
};

// The TEV shader is generated from the current BP state.  Its vertex side is
// shared by every cached pixel program: all eight texture coordinates and both
// raster channels are carried to the fragment stage so a TEV order can choose
// any map/channel without rebuilding the vertex program.
struct TevKey {
    uint32_t genmode;
    uint32_t stages;
    uint32_t colorEnv[16];
    uint32_t alphaEnv[16];
    uint32_t order[16];
    uint32_t ksel[16];
    uint32_t swap[4];
    uint32_t alphaCompare;
    uint32_t ind[16];  // BP_IND_CMD per stage (zero without indirect stages)
    uint32_t iref;     // BP_RAS1_IREF (zero without indirect stages)
};

struct TevShader {
    TevKey key;
    GX2PixelShader pixel;
    GX2UniformVar uniform;
    GX2SamplerVar samplers[8];
    void* psProgram;
    uint32_t textureMask;
};

struct VertexKey {
    uint32_t layoutVersion;
    uint32_t outputMode;       // 0: TEV (ten parameters), 1: fixed path
    uint32_t fixedCoord;
    uint32_t fixedTextured;
    uint32_t numChans;
    uint32_t colorCtrl[2];
    uint32_t alphaCtrl[2];
    uint32_t numTexGens;
    uint32_t texgen[8];
    uint32_t dualTex;
    uint32_t postTex[8];
    uint32_t projectionOrtho;
};
static constexpr uint32_t kVertexKeyLayoutVersion = 1;

/*
 * GPU-transform ABI.  The raw stream deliberately uses only formats already
 * exercised by the Cafe/Cemu fetch path: three-component float vectors,
 * float4 colours, float2 texcoords, and float4 matrix-index groups.  Matrix
 * indices are exact integral float values; the vertex program converts them
 * to AR with FLT_TO_INT/MOVA_INT before relative constant-file reads.  Keeping
 * the index groups as float4 is a documented portability fallback for builds
 * whose GX2 headers do not expose an integer byte attribute format.
 *
 *   0: position       float3
 *   12: normal        float3
 *   24: binormal      float3
 *   36: tangent       float3
 *   48: colours       float4, float4 (normalised 0..1)
 *   80: texcoords     eight float2 pairs
 *   144: matrix index 0..11 as three float4 groups
 */
// The raw GX2 fetch ABI is HostVertex itself: the decoder writes straight
// into the vertex arena and the fetch unit converts colours (UNORM8) and
// matrix indices (USCALED8), so no per-vertex CPU packing remains.
using RawVertex = HostVertex;
static_assert(sizeof(RawVertex) == 132, "the raw GX2 fetch ABI must stay compact and stable");
static constexpr uint32_t kRawVertexStride = sizeof(RawVertex);
static constexpr uint32_t kRawVertexFloats = sizeof(RawVertex) / sizeof(float);

enum RawAttrib : uint32_t {
    RAW_ATTR_POS = 0, RAW_ATTR_NRM, RAW_ATTR_BIN, RAW_ATTR_TAN,
    RAW_ATTR_CLR0, RAW_ATTR_CLR1, RAW_ATTR_TEX01, RAW_ATTR_TEX23,
    RAW_ATTR_TEX45, RAW_ATTR_TEX67, RAW_ATTR_MTX0, RAW_ATTR_MTX1, RAW_ATTR_MTX2,
    RAW_ATTR_COUNT,
};

struct VertexProgram {
    VertexKey key;
    uint32_t aluCount;
    GX2VertexShader vertex;
    GX2FetchShader fetch;
    GX2AttribStream attrs[RAW_ATTR_COUNT];
    GX2AttribVar attribVars[RAW_ATTR_COUNT];
    void* vsProgram;
    void* fetchProgram;
    uint32_t stride;
    bool warmed;
    bool bracketed;
    bool failed;
};
static const char* s_vertexRejectReason = "unknown";
static uint32_t s_vertexRejected;

static constexpr uint32_t kXfUniformRegs = 192;
static constexpr uint32_t kXfUniformFloats = kXfUniformRegs * 4;
static constexpr uint32_t kVtxLiveBase = kXfUniformRegs;
static constexpr uint32_t kVtxLiveRegs = 21;
static float s_xfUniform[kXfUniformFloats];
static uint32_t s_xfShadow[XF_MEM_WORDS];
static bool s_xfUniformValid;
static float s_vtxLive[kVtxLiveRegs * 4];
static uint32_t s_xfUploadCalls;
static uint32_t s_xfUploadRegs;

static FixedShader s_untextured, s_modulate;
static GX2VertexShader s_tevVertex;
static GX2FetchShader s_tevFetch;
static GX2AttribStream s_tevAttrs[11];
static void* s_tevVertexProgram;
static void* s_tevFetchProgram;
static unsigned char s_tevShaderStorage[sizeof(std::vector<TevShader*>)] __attribute__((aligned(8)));
static std::vector<TevShader*>* s_tevShaderObject;
static std::vector<TevShader*>& tevShaders() {
    if (!s_tevShaderObject) s_tevShaderObject = new (s_tevShaderStorage) std::vector<TevShader*>();
    return *s_tevShaderObject;
}
#define s_tevShaders (tevShaders())
static unsigned char s_vertexShaderStorage[sizeof(std::vector<VertexProgram*>)] __attribute__((aligned(8)));
static std::vector<VertexProgram*>* s_vertexShaderObject;
static std::vector<VertexProgram*>& vertexPrograms() {
    if (!s_vertexShaderObject)
        s_vertexShaderObject = new (s_vertexShaderStorage) std::vector<VertexProgram*>();
    return *s_vertexShaderObject;
}
#define s_vertexPrograms (vertexPrograms())
static GX2AttribVar s_attribVars[] = {
    {"aVtxPos", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 0},
    {"aTexCoord0", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 1},
    {"aTexCoord1", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 2},
    {"aFog", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 3},
    {"aGrayscaleColor", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 4},
    {"aInput1", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 5},
    {"aInput2", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 6},
    {"aInput3", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 7},
    {"aInput4", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 8},
    {"aInput5", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 9},
    {"aInput6", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 10},
    {"aInput7", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 11},
};
static GX2SamplerVar s_samplerVars[] = {
    {"uTex0", GX2_SAMPLER_VAR_TYPE_SAMPLER_2D, 0},
    {"uTex1", GX2_SAMPLER_VAR_TYPE_SAMPLER_2D, 1},
    {"uTexMask0", GX2_SAMPLER_VAR_TYPE_SAMPLER_2D, 2},
    {"uTexMask1", GX2_SAMPLER_VAR_TYPE_SAMPLER_2D, 3},
    {"uTexBlend0", GX2_SAMPLER_VAR_TYPE_SAMPLER_2D, 4},
    {"uTexBlend1", GX2_SAMPLER_VAR_TYPE_SAMPLER_2D, 5},
};
static GX2UniformVar s_pixelUniform = {"window_params", GX2_SHADER_VAR_TYPE_FLOAT2, 1, 0, -1};

struct Batch {
    std::vector<uint8_t> vertices;
    std::vector<uint32_t> indices;
    uint32_t fmt = 0;
    uint32_t stride = 0;
    uint32_t count = 0;
    PrimClass cls = PRIM_TRIS;
};
// These containers must not be constructed by the Wii U image's static
// initializer.  Cemu reaches that initializer before the port has installed
// its MEM2 heap, so construct them on the first renderer call instead.
static unsigned char s_batchStorage[sizeof(Batch)] __attribute__((aligned(8)));
static Batch* s_batchObject;
static Batch& batchObject() {
    if (!s_batchObject) s_batchObject = new (s_batchStorage) Batch();
    return *s_batchObject;
}
#define s_batch (batchObject())

struct TextureEntry {
    const uint8_t* source = nullptr;
    uint32_t fmt = 0, width = 0, height = 0, tlut = 0, tlutFmt = 0, levels = 1;
    bool arbitraryMips = false;
    GX2Texture texture;
    GX2Sampler sampler;
    bool valid = false;
};
static unsigned char s_textureStorage[sizeof(std::vector<TextureEntry>)] __attribute__((aligned(8)));
static std::vector<TextureEntry>* s_textureObject;
static std::vector<TextureEntry>& textureCache() {
    if (!s_textureObject) s_textureObject = new (s_textureStorage) std::vector<TextureEntry>();
    return *s_textureObject;
}
#define s_textures (textureCache())

// GPU buffers come from the system default heap: newlib's memalign draws on
// wut's small sbrk area and crashed in _memalign_r once real textures loaded.
static void* alignedZero(size_t alignment, size_t size) {
    void* p = MEMAllocFromDefaultHeapEx(uint32_t(size), int(alignment));
    if (p) memset(p, 0, size);
    return p;
}

// `stride` is the number of XF words held by one uniform vector.  Normal
// matrices are 3x3 with three-word rows, so they are spread one row per
// vector (w = 0) to keep a matrix index a plain register offset.
struct XfUniformSegment { uint32_t source, destination, words, stride; };
static constexpr uint32_t kXfNormalBase = 64, kXfPostTexBase = 96, kXfLightBase = 160;
static constexpr XfUniformSegment kXfSegments[] = {
    {0x000,            0, 256, 4}, // raw position/texgen matrix words -> C0..C63
    {0x400, kXfNormalBase,  96, 3}, // normal matrix rows -> C64..C95
    {0x500, kXfPostTexBase, 256, 4}, // post-texture matrix words -> C96..C159
    {0x600, kXfLightBase, 128, 4}, // eight light records -> C160..C191
};
static bool s_xfMemoryDirty = true;
// Clears, copies and context-state restores run GX2's own programs and
// replace the vertex constant registers, so the uniform shadow must forget
// what it believes the GPU holds after any of them.
static void invalidateVertexUniforms() {
    s_xfUniformValid = false;
    s_xfMemoryDirty = true;
}

/*
 * GX2 uniform writes are vector-register writes.  Compare the packed source
 * ranges in four-word units and write only coalesced changed ranges.  This is
 * intentionally independent of markXfMemDirty's no-straddle flush: the GX
 * decoder does not pass the changed XF address through that legacy interface,
 * so the renderer owns the shadow and discovers the exact dirty vectors here.
 */
static void uploadXfUniformRanges() {
    if (!s_xfMemoryDirty) return;
    if (!s_xfUniformValid) memset(s_xfShadow, 0xff, sizeof(s_xfShadow));
    for (const XfUniformSegment& segment : kXfSegments) {
        uint32_t first = ~0u, last = 0;
        const uint32_t n = segment.stride;
        for (uint32_t word = 0; word < segment.words; word += n) {
            const uint32_t src = segment.source + word;
            const uint32_t dst = segment.destination + word / n;
            bool changed = !s_xfUniformValid;
            for (uint32_t c = 0; c < n; ++c)
                changed = changed || s_xfShadow[src + c] != g.xfMem[src + c];
            if (!changed) {
                if (first != ~0u) {
                    const uint32_t count = last - first + 1;
                    GX2SetVertexUniformReg(first * 4, count * 4, &s_xfUniform[first * 4]);
                    ++s_xfUploadCalls;
                    s_xfUploadRegs += count;
                    first = ~0u;
                }
                continue;
            }
            if (first == ~0u) first = dst;
            last = dst;
            if (segment.source == 0x600 && (src & 15) == 0) {
                // XF light colours are packed RGBA bytes in word 3.  The
                // vertex ALU has no integer bit-unpack path, so keep the
                // sixteen-word light record's four-register shape but turn
                // its first vector into normalized floats.  The remaining
                // three vectors are already float fields and retain their
                // source representation.
                const uint32_t packed = g.xfMem[src + 3];
                float* dstVector = &s_xfUniform[dst * 4];
                dstVector[0] = float((packed >> 24) & 255) / 255.0f;
                dstVector[1] = float((packed >> 16) & 255) / 255.0f;
                dstVector[2] = float((packed >> 8) & 255) / 255.0f;
                dstVector[3] = float(packed & 255) / 255.0f;
            } else {
                memcpy(&s_xfUniform[dst * 4], &g.xfMem[src], n * sizeof(uint32_t));
                if (n == 3) s_xfUniform[dst * 4 + 3] = 0.0f;
            }
            memcpy(&s_xfShadow[src], &g.xfMem[src], n * sizeof(uint32_t));
        }
        if (first != ~0u) {
            const uint32_t count = last - first + 1;
            GX2SetVertexUniformReg(first * 4, count * 4, &s_xfUniform[first * 4]);
            ++s_xfUploadCalls;
            s_xfUploadRegs += count;
        }
    }
    s_xfUniformValid = true;
    s_xfMemoryDirty = false;
}

static bool validateLatteVertexEncodings() {
    const uint64_t mova = ALU_MOVA_INT(__, _x, _R(15), _x);
    const uint64_t relative = ALU_MOV_REL_CONST(__, _x, _GX2_C(7), _y);
    const uint64_t relativeNext = ALU_MOV_REL_CONST(__, _x, _GX2_C(8), _y);
    const uint32_t movaWord1 = __builtin_bswap32(uint32_t(mova));
    const uint32_t relativeWord0 = __builtin_bswap32(uint32_t(relative >> 32));
    const uint32_t relativeNextWord0 = __builtin_bswap32(uint32_t(relativeNext >> 32));
    const bool movaOk = ((movaWord1 >> 7) & 0xff) == ALU_OP2_INST_MOVA_INT;
    const bool relativeOk = ((relativeWord0 >> 9) & 1) != 0 &&
                            (relativeWord0 & 0x1ff) == (_GX2_C(7) & 0x1ff) &&
                            (relativeNextWord0 & 0x1ff) == (_GX2_C(8) & 0x1ff) &&
                            relativeWord0 != relativeNextWord0;
    if (!movaOk || !relativeOk) {
        WHBLogPrintf("SMS Wii U: Latte relative-address microcheck failed mova=%08x rel=%08x",
                     (unsigned)movaWord1, (unsigned)relativeWord0);
        return false;
    }
    return true;
}

// Vertex streams live in per-frame arenas the GPU reads after the draw call
// returns, so a draw no longer waits for the GPU (GX2DrawDone per draw cost
// most of the frame on hardware). An arena is reused only after the GPU has
// retired the frame that last filled it; an overflowing frame waits for the
// GPU and starts its arena over.
static constexpr uint32_t kVertexArenas = 3;
static constexpr uint32_t kVertexArenaBytes = 32u << 20;  // raw ABI: ~170k verts x 192 B
static uint8_t* s_vertexArena[kVertexArenas];
static OSTime s_vertexArenaFence[kVertexArenas];
static uint32_t s_vertexArenaIndex;
static uint32_t s_vertexArenaUsed;
static uint32_t s_vertexArenaOverflows;
static OSTime s_vertexFenceWait;
static bool s_vertexArenaInvalidated;

// Per-draw GPU cost: the bottom-of-pipe GPU clock is stamped after every draw
// (and once at frame start) into this arena's slots; once the arena's fence
// has retired, each draw's cost is the difference to the previous stamp.
extern "C" void GX2SampleBottomGPUCycle(uint64_t* result);
static constexpr uint32_t kGpuStamps = 1024;
struct GpuDrawInfo {
    uint32_t verts, stages, texMask, blend, zmode, texW, texH, area, drawNo;
};
static uint64_t* s_gpuStamps[3];
static GpuDrawInfo* s_gpuInfo[3];
static uint32_t s_gpuCount[3];
static uint64_t s_gpuTotal;
static uint32_t s_gpuFrames;
struct GpuTop { uint64_t cost; GpuDrawInfo info; };
static GpuTop s_gpuTop[8];

static void gpuStamp(uint32_t arena, const GpuDrawInfo* info) {
    if (!s_gpuStamps[arena]) {
        s_gpuStamps[arena] = static_cast<uint64_t*>(MEMAllocFromDefaultHeapEx(kGpuStamps * 8, 64));
        s_gpuInfo[arena] = static_cast<GpuDrawInfo*>(MEMAllocFromDefaultHeapEx(kGpuStamps * sizeof(GpuDrawInfo), 64));
        if (!s_gpuStamps[arena] || !s_gpuInfo[arena]) return;
        // No dirty CPU lines may later be written back over the GPU's stamps.
        memset(s_gpuStamps[arena], 0, kGpuStamps * 8);
        DCFlushRange(s_gpuStamps[arena], kGpuStamps * 8);
    }
    uint32_t& n = s_gpuCount[arena];
    if (n >= kGpuStamps) return;
    if (info) s_gpuInfo[arena][n] = *info;
    GX2SampleBottomGPUCycle(&s_gpuStamps[arena][n]);
    ++n;
}

static void gpuCollect(uint32_t arena) {
    const uint32_t n = s_gpuCount[arena];
    s_gpuCount[arena] = 0;
    if (n < 2 || !s_gpuStamps[arena]) return;
    DCInvalidateRange(s_gpuStamps[arena], n * 8);
    const uint64_t* st = s_gpuStamps[arena];
    if (st[n - 1] > st[0]) s_gpuTotal += st[n - 1] - st[0];
    ++s_gpuFrames;
    for (uint32_t i = 1; i < n; ++i) {
        if (st[i] <= st[i - 1]) continue;
        const uint64_t cost = st[i] - st[i - 1];
        int slot = -1;
        for (int k = 0; k < 8; ++k)
            if (slot < 0 || s_gpuTop[k].cost < s_gpuTop[slot].cost) slot = k;
        if (cost > s_gpuTop[slot].cost) s_gpuTop[slot] = {cost, s_gpuInfo[arena][i]};
    }
}

static void gpuReport() {
    if (!s_gpuFrames) return;
    WHBLogPrintf("SMS Wii U: [gpu] frames=%u avg-frame-cycles=%u (top single draws over the window follow)",
                 (unsigned)s_gpuFrames, (unsigned)(s_gpuTotal / s_gpuFrames));
    for (int r = 0; r < 8; ++r) {
        int best = -1;
        for (int k = 0; k < 8; ++k)
            if (s_gpuTop[k].cost && (best < 0 || s_gpuTop[k].cost > s_gpuTop[best].cost)) best = k;
        if (best < 0) break;
        const GpuDrawInfo& d = s_gpuTop[best].info;
        WHBLogPrintf("SMS Wii U: [gpu] %d cycles=%u draw#%u verts=%u stages=%u texmask=%02x tex=%ux%u blend=%08x z=%02x bboxPx=%u",
                     r, (unsigned)s_gpuTop[best].cost, (unsigned)d.drawNo, (unsigned)d.verts, (unsigned)d.stages,
                     (unsigned)d.texMask, (unsigned)d.texW, (unsigned)d.texH, (unsigned)d.blend, (unsigned)d.zmode,
                     (unsigned)d.area);
        s_gpuTop[best].cost = 0;
    }
    memset(s_gpuTop, 0, sizeof(s_gpuTop));
    s_gpuTotal = 0;
    s_gpuFrames = 0;
}

// GPU-read memory released mid-frame (linear texture staging) is freed once
// the fence of the frame that queued it has retired.
struct DeferredFree { void* p; uint32_t arena; };
static DeferredFree s_deferredFree[256];
static uint32_t s_deferredFreeCount;

static void deferFree(void* p) {
    if (!p) return;
    if (s_deferredFreeCount == 256) {
        GX2DrawDone();
        for (uint32_t i = 0; i < s_deferredFreeCount; ++i) MEMFreeToDefaultHeap(s_deferredFree[i].p);
        s_deferredFreeCount = 0;
    }
    s_deferredFree[s_deferredFreeCount++] = {p, s_vertexArenaIndex};
}

static void beginVertexArena() {
    s_vertexArenaIndex = (s_vertexArenaIndex + 1) % kVertexArenas;
    s_vertexArenaUsed = 0;
    s_vertexArenaInvalidated = false;
    if (s_vertexArenaFence[s_vertexArenaIndex]) {
        const OSTime t0 = OSGetTime();
        GX2WaitTimeStamp(s_vertexArenaFence[s_vertexArenaIndex]);
        s_vertexFenceWait += OSGetTime() - t0;
    }
    gpuCollect(s_vertexArenaIndex);
    gpuStamp(s_vertexArenaIndex, nullptr);
    // That fence also covers everything queued while this arena was last used.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < s_deferredFreeCount; ++i) {
        if (s_deferredFree[i].arena == s_vertexArenaIndex) MEMFreeToDefaultHeap(s_deferredFree[i].p);
        else s_deferredFree[kept++] = s_deferredFree[i];
    }
    s_deferredFreeCount = kept;
}

static void endVertexArena() {
    s_vertexArenaFence[s_vertexArenaIndex] = GX2GetLastSubmittedTimeStamp();
}

static float* allocVertices(uint32_t bytes) {
    uint8_t*& arena = s_vertexArena[s_vertexArenaIndex];
    if (!arena) arena = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(kVertexArenaBytes, 256));
    if (!arena || bytes > kVertexArenaBytes) return nullptr;
    if (!s_vertexArenaInvalidated) {
        GX2Invalidate(GX2_INVALIDATE_MODE_ATTRIBUTE_BUFFER, arena, kVertexArenaBytes);
        s_vertexArenaInvalidated = true;
    }
    if (s_vertexArenaUsed + bytes > kVertexArenaBytes) {
        GX2DrawDone();
        GX2Invalidate(GX2_INVALIDATE_MODE_ATTRIBUTE_BUFFER, arena, kVertexArenaBytes);
        s_vertexArenaUsed = 0;
        ++s_vertexArenaOverflows;
    }
    float* p = reinterpret_cast<float*>(arena + s_vertexArenaUsed);
    s_vertexArenaUsed = (s_vertexArenaUsed + bytes + 255) & ~255u;
    return p;
}

static void destroyShader(FixedShader& shader) {
    MEMFreeToDefaultHeap(shader.vsProgram);
    MEMFreeToDefaultHeap(shader.psProgram);
    MEMFreeToDefaultHeap(shader.fetch.program);
    memset(&shader, 0, sizeof(shader));
}

static void addAttribute(FixedShader& shader, uint32_t location, uint32_t components, uint32_t offset) {
    GX2AttribStream& a = shader.attrs[shader.nattrs++];
    a.location = location;
    a.buffer = 0;
    a.offset = offset;
    a.format = components == 2 ? GX2_ATTRIB_FORMAT_FLOAT_32_32 : GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32;
    a.type = GX2_ATTRIB_INDEX_PER_VERTEX;
    a.aluDivisor = 0;
    a.mask = GX2_COMP_MAP(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
    a.endianSwap = GX2_ENDIAN_SWAP_DEFAULT;
}

static bool initShader(FixedShader& shader, const unsigned char* vs, size_t vsSize,
                       const uint32_t* vsRegs, const unsigned char* ps, size_t psSize,
                       const uint32_t* psRegs, bool textured) {
    memset(&shader, 0, sizeof(shader));
    shader.textured = textured;
    shader.vsProgram = alignedZero(GX2_SHADER_PROGRAM_ALIGNMENT, vsSize);
    shader.psProgram = alignedZero(GX2_SHADER_PROGRAM_ALIGNMENT, psSize);
    if (!shader.vsProgram || !shader.psProgram) return false;
    memcpy(shader.vsProgram, vs, vsSize);
    memcpy(shader.psProgram, ps, psSize);
    memcpy(&shader.vertex.regs, vsRegs, sizeof(shader.vertex.regs));
    memcpy(&shader.pixel.regs, psRegs, sizeof(shader.pixel.regs));
    // lus-rebase emits slot 0 for fragment position even when position_ena is
    // clear, with the same semantic as the first real parameter. Cafe ignores
    // that slot; Cemu declares semantic 0 twice and the shader fails to build.
    // Removing the slot shifted every input one register down (all-black,
    // alpha-0 output), so keep it and give it a semantic no VS exports.
    if (!(shader.pixel.regs.spi_ps_in_control_0 & (1u << 8)) &&
        shader.pixel.regs.num_spi_ps_input_cntl > 1)
        shader.pixel.regs.spi_ps_input_cntls[0] = (shader.pixel.regs.spi_ps_input_cntls[0] & ~0xffu) | 0x7fu;
    shader.vertex.size = uint32_t(vsSize);
    shader.vertex.program = shader.vsProgram;
    shader.vertex.mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
    shader.vertex.attribVarCount = sizeof(s_attribVars) / sizeof(s_attribVars[0]);
    shader.vertex.attribVars = s_attribVars;
    shader.pixel.size = uint32_t(psSize);
    shader.pixel.program = shader.psProgram;
    shader.pixel.mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
    shader.pixel.uniformVarCount = 1;
    shader.pixel.uniformVars = &s_pixelUniform;
    shader.pixel.samplerVarCount = sizeof(s_samplerVars) / sizeof(s_samplerVars[0]);
    shader.pixel.samplerVars = s_samplerVars;

    addAttribute(shader, 0, 4, 0);
    if (textured) addAttribute(shader, 1, 2, 16);
    addAttribute(shader, 5, 4, textured ? 24 : 16);
    shader.stride = textured ? 40 : 32;
    shader.fetch.size = GX2CalcFetchShaderSizeEx(shader.nattrs,
                                                  GX2_FETCH_SHADER_TESSELLATION_NONE,
                                                  GX2_TESSELLATION_MODE_DISCRETE);
    shader.fetch.program = alignedZero(GX2_SHADER_PROGRAM_ALIGNMENT, shader.fetch.size);
    if (!shader.fetch.program) return false;
    GX2InitFetchShaderEx(&shader.fetch, static_cast<uint8_t*>(shader.fetch.program), shader.nattrs, shader.attrs,
                         GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader.vertex.program, shader.vertex.size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader.pixel.program, shader.pixel.size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader.fetch.program, shader.fetch.size);
    return true;
}

static bool initShaders() {
    if (!validateLatteVertexEncodings()) {
        s_gpuXfEnabled = false;
        WHBLogPrintf("SMS Wii U: disabling GPU vertex transform after Latte encoding check");
    }
    if (!initShader(s_untextured, dkr_untextured_vs, sizeof(dkr_untextured_vs), dkr_untextured_vs_regs,
                    dkr_untextured_ps, sizeof(dkr_untextured_ps), dkr_untextured_ps_regs, false)) return false;
    if (!initShader(s_modulate, dkr_modulate_vs, sizeof(dkr_modulate_vs), dkr_modulate_vs_regs,
                    dkr_modulate_ps, sizeof(dkr_modulate_ps), dkr_modulate_ps_regs, true)) return false;
    if (!initTevVertex()) return false;
    WHBLogPrintf("SMS Wii U: lus-rebase GX2 shaders loaded");
    return true;
}

static bool usedTexture(int* coord, int* map) {
    const uint32_t stages = ((g.bp[BP_GENMODE] >> 10) & 15) + 1;
    for (uint32_t stage = 0; stage < stages; ++stage) {
        const uint32_t tref = (g.bp[BP_TREF + (stage >> 1)] >> ((stage & 1) * 12)) & 0x3ff;
        if ((tref >> 6) & 1) {
            if (coord) *coord = int((tref >> 3) & 7);
            if (map) *map = int(tref & 7);
            return true;
        }
    }
    return false;
}

// GX wrap modes (TX_MODE0 bits 0-1 / 2-3): 0 clamp, 1 repeat, 2 mirror; the
// reserved 3 repeats, as the PC renderer treats it.
static GX2TexClampMode clampMode(uint32_t value) {
    switch (value & 3) {
    case 0: return GX2_TEX_CLAMP_MODE_CLAMP;
    case 2: return GX2_TEX_CLAMP_MODE_MIRROR;
    default: return GX2_TEX_CLAMP_MODE_WRAP;
    }
}

// TX_MODE0: bit 4 is the magnification filter (linear when set); bits 5-7
// the minification filter, whose bit 2 selects linear and whose low bits the
// mip filter (1 nearest, 2 linear); bits 9-16 a signed LOD bias in 1/32.
// TX_MODE1 holds the min and max LOD in 1/16. Textures whose small levels
// are an effect sample the GameCube's level of detail: the bias makes up for
// the TV target's scale over the 640x480 EFB.
static float s_lodScale = 1.5f;
static void initSampler(GX2Sampler* sampler, uint32_t mode0, uint32_t mode1, uint32_t levels, bool arbitraryMips) {
    GX2InitSampler(sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSamplerClamping(sampler, clampMode(mode0 & 3), clampMode((mode0 >> 2) & 3),
                           GX2_TEX_CLAMP_MODE_WRAP);
    const GX2TexXYFilterMode mag = ((mode0 >> 4) & 1) ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT;
    const GX2TexXYFilterMode min = ((mode0 >> 7) & 1) ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT;
    GX2InitSamplerXYFilter(sampler, mag, min, GX2_TEX_ANISO_RATIO_NONE);
    const uint32_t mip = (mode0 >> 5) & 3;
    if (levels <= 1 || mip == 0) {
        GX2InitSamplerZMFilter(sampler, GX2_TEX_Z_FILTER_MODE_NONE, GX2_TEX_MIP_FILTER_MODE_NONE);
        GX2InitSamplerLOD(sampler, 0.0f, 0.0f, 0.0f);
        return;
    }
    GX2InitSamplerZMFilter(sampler, GX2_TEX_Z_FILTER_MODE_NONE,
                           mip == 1 ? GX2_TEX_MIP_FILTER_MODE_POINT : GX2_TEX_MIP_FILTER_MODE_LINEAR);
    float bias = float(int8_t((mode0 >> 9) & 0xff)) / 32.0f;
    if (arbitraryMips) bias += log2f(s_lodScale);
    const float minLod = float(mode1 & 0xff) / 16.0f;
    const float maxLod = std::min(float((mode1 >> 8) & 0xff) / 16.0f, float(levels - 1));
    GX2InitSamplerLOD(sampler, minLod, maxLod, bias);
}

static int tevSext11(uint32_t v) { return int(v << 21) >> 21; }

static uint32_t tevKsel(uint32_t stage) {
    const uint32_t r = g.bp[BP_TEV_KSEL + (stage >> 1)];
    return (r >> ((stage & 1) ? 14 : 4)) & 0x3ff;
}

static uint32_t tevNumInd(const TevKey& k) {
    const uint32_t n = (k.genmode >> 16) & 7;
    return n > 4 ? 4 : n;
}

static TevKey currentTevKey() {
    TevKey k;
    memset(&k, 0, sizeof(k));
    k.genmode = g.bp[BP_GENMODE] & ((15u << 10) | (7u << 16));
    k.stages = ((g.bp[BP_GENMODE] >> 10) & 15) + 1;
    for (uint32_t s = 0; s < k.stages; ++s) {
        k.colorEnv[s] = g.bp[BP_TEV_COLOR_ENV + 2 * s] & 0xffffff;
        k.alphaEnv[s] = g.bp[BP_TEV_COLOR_ENV + 2 * s + 1] & 0xffffff;
        k.order[s] = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3ff;
        k.ksel[s] = tevKsel(s);
    }
    for (uint32_t t = 0; t < 4; ++t)
        k.swap[t] = (g.bp[BP_TEV_KSEL + 2 * t] & 15) |
                    ((g.bp[BP_TEV_KSEL + 2 * t + 1] & 15) << 4);
    k.alphaCompare = g.bp[BP_ALPHACOMPARE];
    const uint32_t numInd = tevNumInd(k);
    if (numInd) {
        for (uint32_t s = 0; s < k.stages; ++s) k.ind[s] = g.bp[BP_IND_CMD + s] & 0x1fffff;
        k.iref = g.bp[BP_RAS1_IREF] & 0xffffff;
    }
    return k;
}

static VertexKey currentVertexKey(bool tevOutput, int fixedCoord, bool fixedTextured) {
    VertexKey k;
    memset(&k, 0, sizeof(k));
    k.layoutVersion = kVertexKeyLayoutVersion;
    k.outputMode = tevOutput ? 0 : 1;
    k.fixedCoord = uint32_t(fixedCoord < 0 ? 0 : fixedCoord);
    k.fixedTextured = fixedTextured ? 1 : 0;
    k.numChans = g.xfReg[XFR_NUMCHANS] & 3;
    for (uint32_t c = 0; c < 2; ++c) {
        k.colorCtrl[c] = g.xfReg[XFR_COLOR0CTRL + c] & 0x7fff;
        k.alphaCtrl[c] = g.xfReg[XFR_ALPHA0CTRL + c] & 0x7fff;
    }
    k.numTexGens = std::min<uint32_t>(g.xfReg[XFR_NUMTEXGENS] & 15, 8);
    for (uint32_t coord = 0; coord < k.numTexGens; ++coord) {
        k.texgen[coord] = g.xfReg[XFR_TEXGEN + coord];
        k.postTex[coord] = g.xfReg[XFR_POSTTEX + coord] & 0x13f;
    }
    k.dualTex = g.xfReg[XFR_DUALTEX] & 1;
    k.projectionOrtho = g.xfReg[XFR_PROJ + 6] & 1;
    return k;
}

static void packRawVertex(const HostVertex& in, RawVertex& out) { out = in; }

static uint32_t tevTextureMask(const TevKey& k) {
    uint32_t mask = 0;
    for (uint32_t s = 0; s < k.stages; ++s)
        if ((k.order[s] >> 6) & 1) mask |= 1u << (k.order[s] & 7);
    for (uint32_t i = 0; i < tevNumInd(k); ++i) mask |= 1u << ((k.iref >> (6 * i)) & 7);
    return mask;
}

static bool tevSupported(const TevKey& k) {
    for (uint32_t s = 0; s < k.stages; ++s) {
        if (((k.order[s] >> 6) & 1) && (k.order[s] & 7) >= 8) return false;
    }
    return true;
}

struct TevSource {
    uint32_t sel;
    uint32_t chan;
};

static TevSource tevFixed(uint32_t value) {
    // Uniform vectors 8..15 hold the fixed TEV constants.  Keeping these in
    // the same constant file as TEVREG/KONST makes every numeric input a
    // uniform and avoids baking colour values into a cached program.
    return {_GX2_C(8 + (value >> 2)), value & 3};
}

static TevSource tevKonst(const TevKey& k, uint32_t sel, uint32_t chan, bool alpha = false) {
    if (sel < 8) return tevFixed(sel);
    if (sel >= 12 && sel < 16) return {_GX2_C(4 + sel - 12), chan};
    if (sel >= 16) return {_GX2_C(4 + (sel & 3)), (sel - 16) >> 2};
    return tevFixed(12);
}

static uint32_t tevSwapChannel(const TevKey& k, uint32_t table, uint32_t channel) {
    return (k.swap[table & 3] >> (channel * 2)) & 3;
}

// The generated TEV works in GX's integer domain, as genFS does: every value
// is an integer held exactly in a float (inputs 0..255, registers -1024..1023).
// A, B and C read only the low 8 bits of a register (an unclamped stage can
// wrap: Bianco's river and the file-select sea sparkle depend on it); D reads
// the whole register. The constant file (setTevShader) holds:
//   C0-3 TEVREG (PREV, REG0-2)  C4-7 KONST  C8-9 the k8 fractions
//   C11 (255, 128, -, 0)  C12 (256, 1/256, 128, 255)  C13 (1/255, 1/128, -1024, 1023)
//   C14 (-128, ...)
//
// GPRs are allocated per shader (TevAlloc): the interpolated inputs the key
// reads come first (the pixel shader imports only those semantics), then the
// sampled textures, the TEV registers in use, the stage result, four
// temporaries, 8-bit views of the registers read as A/B/C, and four GPR
// copies of constant-file operands (the constant file has one read port per
// channel, so one instruction group must not read one channel of two
// different constants).
//
// The colour channels (and alpha, when its equation has the same shape) run
// as one instruction group per operation, one slot per channel.
static const uint32_t kTevUniforms = 47;
struct TevAlloc {
    uint32_t coord[8];     // GPR of texcoord n (0xff: not imported)
    uint32_t ras[2];       // GPR of colour channel 0/1 (0xff: not imported)
    uint32_t tex[16];      // GPR of the texture sampled by stage n
    uint32_t reg[4];       // GPR of PREV, REG0-2
    uint32_t mask[4];      // GPR of the 8-bit view of PREV, REG0-2
    uint32_t stage;        // a stage's staged result
    uint32_t t[4];         // temporaries
    uint32_t k[4];         // constant-file copies
    uint32_t ind[4];       // indirect lookup results
    uint32_t icoord[4];    // indirect lookup coordinates
    uint32_t scoord[16];   // texture coordinate of an indirect stage
    uint32_t bump[16];     // bump alpha after stage n (x: bumpA, y: bumpA | bumpA >> 5)
    uint32_t prevTc;       // the previous indirect stage's texel coordinate
    uint32_t inputs;       // imported semantics, in GPR order
    uint32_t semantic[10];
    uint32_t count;        // GPRs used
};
static TevAlloc s_ra;

struct TevStageCtx {
    const TevKey* key;
    uint32_t stage;
    int texStage;   // the stage whose texture TEXC/TEXA read (-1: none yet)
    int bumpStage;  // the stage that last set the bump alpha (-1: none, 0)
};

// Where a stage reads TEV register r's colour (p = 0) or alpha (p = 1) part:
// as D (the whole register) and as A/B/C (its low 8 bits). A part no earlier
// stage wrote is the uniform (C0-3, its low 8 bits in C43-46); a written part
// known to be 0..255 is its own 8-bit view; otherwise a masked copy is made.
struct TevViews {
    uint32_t full[4][2];
    uint32_t masked[4][2];
};
static TevViews s_view;

static TevSource tevRasSource(const TevStageCtx& cx, uint32_t chan, uint32_t ch) {
    // Raster selectors 5 and 6 read the bump alpha (genFS bumpA).
    if (chan == 5 || chan == 6) {
        if (cx.bumpStage < 0) return {ALU_SRC_0, 0};
        return {_R(s_ra.bump[cx.bumpStage]), chan == 5 ? uint32_t(_x) : uint32_t(_y)};
    }
    if (chan > 1 || s_ra.ras[chan] == 0xffffffffu) return {ALU_SRC_0, 0};
    return {_R(s_ra.ras[chan]), ch};
}

static TevSource tevColorSource(const TevStageCtx& cx, uint32_t sel, uint32_t channel, bool masked) {
    const TevKey& k = *cx.key;
    const uint32_t stage = cx.stage;
    if (sel < 8) {
        const uint32_t r = sel >> 1, part = sel & 1;
        return {masked ? s_view.masked[r][part] : s_view.full[r][part], part ? uint32_t(_w) : channel};
    }
    const uint32_t order = k.order[stage];
    const uint32_t rasTable = k.alphaEnv[stage] & 3;
    if (sel == 8 || sel == 9) {
        // genFS keeps the last sampled texture for a stage without one.
        if (cx.texStage < 0) return tevFixed(12);
        const uint32_t texTable = (k.alphaEnv[cx.texStage] >> 2) & 3;
        const uint32_t ch = tevSwapChannel(k, texTable, sel == 8 ? channel : 3);
        return {_R(s_ra.tex[cx.texStage]), ch};
    }
    if (sel == 10 || sel == 11)
        return tevRasSource(cx, (order >> 7) & 7, tevSwapChannel(k, rasTable, sel == 10 ? channel : 3));
    if (sel == 12) return tevFixed(12);
    if (sel == 13) return tevFixed(13);
    if (sel == 14) return tevKonst(k, k.ksel[stage] & 31, channel);
    return tevFixed(15);
}

static TevSource tevAlphaSource(const TevStageCtx& cx, uint32_t sel, bool masked) {
    const TevKey& k = *cx.key;
    const uint32_t stage = cx.stage;
    if (sel < 4) return {masked ? s_view.masked[sel][1] : s_view.full[sel][1], _w};
    const uint32_t order = k.order[stage];
    const uint32_t rasTable = k.alphaEnv[stage] & 3;
    if (sel == 4) {
        if (cx.texStage < 0) return tevFixed(12);
        const uint32_t texTable = (k.alphaEnv[cx.texStage] >> 2) & 3;
        return {_R(s_ra.tex[cx.texStage]), tevSwapChannel(k, texTable, 3)};
    }
    if (sel == 5) return tevRasSource(cx, (order >> 7) & 7, tevSwapChannel(k, rasTable, 3));
    if (sel == 6) {
        // Alpha konst selectors 8..15 are reserved; genFS reads them as 255.
        const uint32_t ka = (k.ksel[stage] >> 5) & 31;
        if (ka >= 8 && ka < 16) return tevFixed(12);
        return tevKonst(k, ka, 0, true);
    }
    return tevFixed(15);
}

static void tevEmitAlu(std::vector<uint64_t>& program, std::initializer_list<uint64_t> ops) {
    if (ops.size() == 0) return;
    for (uint64_t op : ops) program.push_back(op);
    program.back() |= to_QWORD(to_LE(1ull << 31), 0);
}

// One instruction of a group, with the operands it reads (for the read-port
// check). dstCh is its vector slot.
struct TevIns {
    uint64_t code;
    uint32_t dstCh;
    TevSource s0, s1;
};

static bool tevIsGpr(TevSource s) { return (s.sel & 0x1fff & ~(1u << 12)) < 128; }
static bool tevIsConst(TevSource s) { return (s.sel & ~(1u << 12)) >= ALU_SRC_CONST_FILE_BASE; }
static uint32_t tevSelNoNeg(TevSource s) { return s.sel & ~(1u << 12); }

// Whether ins can join the group: with bank swizzle VEC_012, src0 is read in
// cycle 0 and src1 in cycle 1, one GPR per channel port per cycle; the
// constant file likewise serves one address per channel and at most four
// (address, channel) reads per group.
static bool tevFits(const std::vector<TevIns>& group, const TevIns& ins) {
    for (const TevIns& g : group) {
        if (g.dstCh == ins.dstCh) return false;
        const TevSource gs[2] = {g.s0, g.s1}, is[2] = {ins.s0, ins.s1};
        for (int cyc = 0; cyc < 2; ++cyc) {
            for (int j = 0; j < 2; ++j) {
                const TevSource a = gs[j], b = is[cyc];
                // GPR ports, per cycle
                if (j == cyc && tevIsGpr(a) && tevIsGpr(b) && a.chan == b.chan &&
                    tevSelNoNeg(a) != tevSelNoNeg(b)) return false;
                // constant ports, any cycle
                if (tevIsConst(a) && tevIsConst(b) && a.chan == b.chan && tevSelNoNeg(a) != tevSelNoNeg(b))
                    return false;
            }
        }
    }
    uint32_t consts[8][2]; uint32_t n = 0;
    auto add = [&](TevSource s) {
        if (!tevIsConst(s)) return;
        for (uint32_t i = 0; i < n; ++i)
            if (consts[i][0] == tevSelNoNeg(s) && consts[i][1] == s.chan) return;
        if (n < 8) { consts[n][0] = tevSelNoNeg(s); consts[n][1] = s.chan; ++n; }
    };
    for (const TevIns& g : group) { add(g.s0); add(g.s1); }
    add(ins.s0); add(ins.s1);
    return n <= 4;
}

// Emit the instructions as few groups as the read ports allow. They must be
// independent of one another (each slot reads only its own channel's values).
static void tevEmitGroup(std::vector<uint64_t>& program, const std::vector<TevIns>& ins) {
    std::vector<TevIns> group;
    for (const TevIns& i : ins) {
        if (!tevFits(group, i)) {
            for (const TevIns& g : group) program.push_back(g.code);
            program.back() |= to_QWORD(to_LE(1ull << 31), 0);
            group.clear();
        }
        group.push_back(i);
    }
    if (group.empty()) return;
    for (const TevIns& g : group) program.push_back(g.code);
    program.back() |= to_QWORD(to_LE(1ull << 31), 0);
}

static TevIns tevOp(uint32_t inst, uint32_t dst, uint32_t ch, TevSource s0, TevSource s1, uint32_t omod = ALU_OMOD_OFF) {
    return {ALU_OP2(inst, _R(dst), ch, s0.sel, s0.chan, s1.sel, s1.chan, omod), ch, s0, s1};
}
static TevIns tevOp1(uint32_t inst, uint32_t dst, uint32_t ch, TevSource s0) {
    return {ALU_OP2(inst, _R(dst), ch, s0.sel, s0.chan, ALU_SRC_0, 0, ALU_OMOD_OFF), ch, s0, {ALU_SRC_0, 0}};
}
static TevSource tevNeg(TevSource s) { return {s.sel | (1u << 12), s.chan}; }
static TevSource tevC(uint32_t reg, uint32_t chan) { return {_GX2_C(reg), chan}; }
static TevSource tevG(uint32_t gpr, uint32_t chan) { return {_R(gpr), chan}; }
enum : uint32_t { kOpAdd = 0x0, kOpMul = 0x1, kOpMax = 0x3, kOpMin = 0x4, kOpRndne = 0x13, kOpFloor = 0x14, kOpMov = 0x19 };

// dst.ch = x & 255 for integers x in -1024..1023 (x - 256 * floor(x / 256)),
// for every channel in chs.
static void tevEmitMask8(std::vector<uint64_t>& program, uint32_t dst, const uint32_t* chs, uint32_t n,
                         const TevSource* x) {
    const uint32_t t = s_ra.t[3];
    std::vector<TevIns> g;
    for (uint32_t i = 0; i < n; ++i) g.push_back(tevOp(kOpMul, t, chs[i], x[i], tevC(12, _y)));
    tevEmitGroup(program, g); g.clear();
    for (uint32_t i = 0; i < n; ++i) g.push_back(tevOp1(kOpFloor, t, chs[i], tevG(t, chs[i])));
    tevEmitGroup(program, g); g.clear();
    for (uint32_t i = 0; i < n; ++i) g.push_back(tevOp(kOpMul, t, chs[i], tevG(t, chs[i]), tevC(12, _x)));
    tevEmitGroup(program, g); g.clear();
    for (uint32_t i = 0; i < n; ++i) g.push_back(tevOp(kOpAdd, dst, chs[i], x[i], tevNeg(tevG(t, chs[i]))));
    tevEmitGroup(program, g);
}

// dst.ch = 1.0 or 0.0 for a > b / a == b.
static void tevEmitCompareOp(std::vector<uint64_t>& program, uint32_t dst, uint32_t dstCh, bool equal,
                             TevSource a, TevSource b) {
    if (tevIsConst(a) && tevIsConst(b)) {
        tevEmitAlu(program, {ALU_MOV(_R(s_ra.k[3]), dstCh, b.sel, b.chan)});
        b = tevG(s_ra.k[3], dstCh);
    }
    tevEmitAlu(program, {equal ? ALU_SETE(_R(dst), dstCh, a.sel, a.chan, b.sel, b.chan)
                               : ALU_SETGT(_R(dst), dstCh, a.sel, a.chan, b.sel, b.chan)});
}

// Compare inputs are fetched per component (R, G, B of the colour A/B
// arguments) because a 16/24-bit compare reads several components regardless
// of which output channel is being produced. They are 8-bit inputs.
struct TevCompareSrc {
    const TevStageCtx* cx; uint32_t aSel, bSel; bool alphaArgs;
    TevSource get(bool isB, uint32_t comp) const {
        const uint32_t sel = isB ? bSel : aSel;
        return alphaArgs ? tevAlphaSource(*cx, sel, true) : tevColorSource(*cx, sel, comp, true);
    }
};

// t0.channel = the compare condition (0 or 1) of mode (scale << 1 | op).
static void tevEmitTevCompare(std::vector<uint64_t>& program, uint32_t channel,
                              const TevCompareSrc& cs, const TevCompareSrc& colorCs, uint32_t mode, bool scalar) {
    const uint32_t r16 = s_ra.t[0], r17 = s_ra.t[1], r18 = s_ra.t[2], r19 = s_ra.t[3];
    // Alpha compares of the 16/24-bit kinds read the stage's colour A/B
    // arguments (as genFS does); only the per-component kind reads alpha.
    const TevCompareSrc& src = (scalar && mode < 6) ? colorCs : cs;
    auto A = [&](uint32_t comp) { return src.get(false, comp); };
    auto B = [&](uint32_t comp) { return src.get(true, comp); };
    if (mode < 2 || mode >= 6) {
        // 8-bit forms compare R for every channel; the per-component forms
        // compare the channel itself (alpha for the alpha channel).
        const uint32_t component = mode < 2 ? 0 : channel;
        tevEmitCompareOp(program, r16, channel, (mode & 1) != 0, A(component), B(component));
        return;
    }
    const bool equal = (mode & 1) != 0;
    const uint32_t low = 0, mid = 1, high = mode >= 4 ? 2 : 1;
    if (equal) {
        tevEmitCompareOp(program, r17, _x, true, A(high), B(high));
        tevEmitCompareOp(program, r18, _x, true, A(mode >= 4 ? mid : low), B(mode >= 4 ? mid : low));
        tevEmitAlu(program, {ALU_MIN(_R(r17), _x, _R(r17), _x, _R(r18), _x)});
        if (mode >= 4) {
            tevEmitCompareOp(program, r18, _x, true, A(low), B(low));
            tevEmitAlu(program, {ALU_MIN(_R(r17), _x, _R(r17), _x, _R(r18), _x)});
        }
    } else {
        tevEmitCompareOp(program, r17, _x, false, A(high), B(high));
        tevEmitCompareOp(program, r18, _x, true, A(high), B(high));
        if (mode >= 4)
            tevEmitAlu(program, {ALU_MOV(_R(r16), _x, _R(r18), _x)});
        tevEmitCompareOp(program, r19, _x, false, A(mode >= 4 ? mid : low), B(mode >= 4 ? mid : low));
        tevEmitAlu(program, {ALU_MUL(_R(r19), _x, mode >= 4 ? _R(r16) : _R(r18), _x, _R(r19), _x)});
        tevEmitAlu(program, {ALU_MAX(_R(r17), _x, _R(r17), _x, _R(r19), _x)});
        if (mode >= 4) {
            tevEmitCompareOp(program, r18, _x, true, A(mid), B(mid));
            tevEmitAlu(program, {ALU_MUL(_R(r16), _x, _R(r16), _x, _R(r18), _x)});
            tevEmitCompareOp(program, r19, _x, false, A(low), B(low));
            tevEmitAlu(program, {ALU_MUL(_R(r19), _x, _R(r16), _x, _R(r19), _x)});
            tevEmitAlu(program, {ALU_MAX(_R(r17), _x, _R(r17), _x, _R(r19), _x)});
        }
    }
    tevEmitAlu(program, {ALU_MOV(_R(r16), channel, _R(r17), _x)});
}

// The operands and controls of one channel of a stage.
struct TevChannel {
    uint32_t ch;
    TevSource a, b, c, d;
    uint32_t bias, op, scale;
    bool clamp;
};

static void vertexEmit(std::vector<uint64_t>& ops, std::initializer_list<uint64_t> group) {
    for (uint64_t op : group) ops.push_back(op);
    if (!ops.empty()) ops.back() |= to_QWORD(to_LE(1ull << 31), 0);
}

static void vertexEmitDot4(std::vector<uint64_t>& ops, uint32_t dst, uint32_t dstChan,
                           uint32_t src0, uint32_t src1) {
    // Keep the result in one component.  The generated shaders use this for
    // matrix rows and lighting vectors; scalar multiply-adds avoid DOT4's
    // reduction broadcast clobbering the other components under Cemu's
    // legacy decompiler.
    vertexEmit(ops, {ALU_MUL(_R(dst), dstChan, src0, _x, src1, _x)});
    vertexEmit(ops, {ALU_MULADD(_R(dst), dstChan, src0, _y, src1, _y, _R(dst), dstChan)});
    vertexEmit(ops, {ALU_MULADD(_R(dst), dstChan, src0, _z, src1, _z, _R(dst), dstChan)});
    vertexEmit(ops, {ALU_MULADD(_R(dst), dstChan, src0, _w, src1, _w, _R(dst), dstChan)});
}

static uint32_t vertexInputForRow(uint32_t row) {
    if (row == 0) return 1; // position
    if (row == 1) return 2; // normal
    if (row == 3) return 3; // binormal
    if (row == 4) return 4; // tangent
    if (row >= 5) return RAW_ATTR_TEX01 + (row - 5) / 2 + 1; // packed source texcoord pair
    return 29; // raster colour 0, selected by the caller for row 2
}

static void vertexCopySource(std::vector<uint64_t>& ops, uint32_t source, uint32_t dst, bool colour) {
    vertexEmit(ops, {ALU_MOV(_R(dst), _x, _R(source), _x)});
    vertexEmit(ops, {ALU_MOV(_R(dst), _y, _R(source), _y)});
    // Geometric rows keep z (texgen form ABC1); the caller forces z = 1 for
    // form AB11.  w is 1 for every geometric source.
    vertexEmit(ops, {ALU_MOV(_R(dst), _z, _R(source), _z)});
    vertexEmit(ops, {ALU_MOV(_R(dst), _w, colour ? _R(source) : ALU_SRC_1, colour ? _w : _x)});
}

static void vertexCopyTexSource(std::vector<uint64_t>& ops, uint32_t coord, uint32_t dst) {
    const uint32_t source = RAW_ATTR_TEX01 + coord / 2 + 1;
    const uint32_t chan = (coord & 1) * 2;
    vertexEmit(ops, {ALU_MOV(_R(dst), _x, _R(source), chan)});
    vertexEmit(ops, {ALU_MOV(_R(dst), _y, _R(source), chan + 1)});
    vertexEmit(ops, {ALU_MOV(_R(dst), _z, ALU_SRC_1, _x)});
    vertexEmit(ops, {ALU_MOV(_R(dst), _w, ALU_SRC_1, _x)});
}

static void vertexLoadAbsoluteMatrix(std::vector<uint64_t>& ops, uint32_t dst, uint32_t base,
                                     uint32_t rows = 4);

static void vertexLoadRelativeMatrix(std::vector<uint64_t>& ops, uint32_t indexReg, uint32_t indexChan,
                                     uint32_t dst, uint32_t base, uint32_t rows = 4) {
    // Every XF matrix row occupies one uniform vector, so the GX matrix index
    // is the register offset itself.  Keep the converted index in PV rather
    // than a GPR: Cemu's legacy decompiler rejects a GPR-held MOVA source.
    // FLT_TO_INT runs on the transcendental unit, so its no-GPR result is PS.
    // PV/PS and AR do not survive a clause boundary, and clauses are cut
    // every 128 slots, so pad with scratch moves to keep this run in one.
    const size_t run = 3 + rows * 4;
    while (ops.size() % 128 + run > 128)
        vertexEmit(ops, {ALU_MOV(_R(57), _x, ALU_SRC_0, _x)});
    vertexEmit(ops, {ALU_FLT_TO_INT(__, _x, _R(indexReg), indexChan)});
    vertexEmit(ops, {ALU_MOV(__, _x, ALU_SRC_PS, _x)});
    vertexEmit(ops, {ALU_MOVA_INT(__, _x, ALU_SRC_PV, _x)});
    for (uint32_t row = 0; row < rows; ++row)
        for (uint32_t ch = 0; ch < 4; ++ch)
            vertexEmit(ops, {ALU_MOV_REL_CONST(_R(dst + row), ch, _GX2_C(base + row), ch)});
}

static void vertexLoadAbsoluteMatrix(std::vector<uint64_t>& ops, uint32_t dst, uint32_t base,
                                     uint32_t rows) {
    for (uint32_t row = 0; row < rows; ++row)
        for (uint32_t ch = 0; ch < 4; ++ch)
            vertexEmit(ops, {ALU_MOV(_R(dst + row), ch, _GX2_C(base + row), ch)});
}

static void vertexMatrixTransform(std::vector<uint64_t>& ops, uint32_t source, uint32_t indexReg,
                                  uint32_t indexChan, uint32_t dst, uint32_t base, bool relative) {
    // Rows go to R58..R60, clear of every destination and source the
    // callers use (texgen writes R55, which used to alias matrix row 2).
    if (relative) vertexLoadRelativeMatrix(ops, indexReg, indexChan, 58, base, 3);
    else vertexLoadAbsoluteMatrix(ops, 58, base, 3);
    for (uint32_t ch = 0; ch < 3; ++ch)
        vertexEmitDot4(ops, dst, ch, _R(source), _R(58 + ch));
}

static void vertexNormalTransform(std::vector<uint64_t>& ops) {
    vertexLoadRelativeMatrix(ops, RAW_ATTR_MTX0 + 1, _x, 53, kXfNormalBase, 3);
    for (uint32_t ch = 0; ch < 3; ++ch)
        vertexEmitDot4(ops, 41, ch, _R(2), _R(53 + ch));
    // Three-component length: R41.w is never written.
    vertexEmit(ops, {ALU_MUL(_R(45), _x, _R(41), _x, _R(41), _x)});
    vertexEmit(ops, {ALU_MULADD(_R(45), _x, _R(41), _y, _R(41), _y, _R(45), _x)});
    vertexEmit(ops, {ALU_MULADD(_R(45), _x, _R(41), _z, _R(41), _z, _R(45), _x)});
    vertexEmit(ops, {ALU_SETGT(_R(47), _x, _R(45), _x, ALU_SRC_0, _x)});
    vertexEmit(ops, {ALU_RECIPSQRT_IEEE(_R(46), _x, _R(45), _x)});
    for (uint32_t ch = 0; ch < 3; ++ch) {
        vertexEmit(ops, {ALU_MUL(_R(41), ch, _R(41), ch, _R(46), _x)});
        vertexEmit(ops, {ALU_MUL(_R(41), ch, _R(41), ch, _R(47), _x)});
    }
    vertexEmit(ops, {ALU_CNDGT(_R(41), _z, _R(47), _x, _R(41), _z, ALU_SRC_1, _x)});
}

static void vertexEmitLighting(std::vector<uint64_t>& ops, uint32_t ctrl, uint32_t channel,
                               uint32_t dst, uint32_t first, uint32_t count) {
    const uint32_t input = 5 + channel;
    const uint32_t mat = kVtxLiveBase + 5 + channel * 2;
    const uint32_t amb = mat + 1;
    // R44 is the material, R43 the accumulated ambient/light sum.  The
    // light records are four vectors each: Cbase is the decoded RGBA colour,
    // Cbase+1 is attenuation a.xyz/k.x, Cbase+2 is k.yz/position.xy, and
    // Cbase+3 is position.z/direction.xyz.
    for (uint32_t ch = 0; ch < 4; ++ch) {
        if (ctrl & 1) vertexEmit(ops, {ALU_MOV(_R(44), ch, _R(input), ch)});
        else vertexEmit(ops, {ALU_MOV(_R(44), ch, _GX2_C(mat), ch)});
        if (ctrl & 64) vertexEmit(ops, {ALU_MOV(_R(43), ch, _R(input), ch)});
        else vertexEmit(ops, {ALU_MOV(_R(43), ch, _GX2_C(amb), ch)});
    }
    // Mirrors chanColor.  Light record vectors: C(c) = decoded colour,
    // C(c+1) = (a0, a1, a2, k0), C(c+2) = (k1, k2, pos.x, pos.y),
    // C(c+3) = (pos.z, dir.x, dir.y, dir.z).  Dots are three-component:
    // the w lanes of R41/R42 are not defined.
    // dst.chan = A.xyz . B.(b0, b0+1, b0+2)
    auto dot3 = [&](uint32_t dst, uint32_t dstChan, uint32_t aReg, uint32_t bSel, uint32_t b0) {
        vertexEmit(ops, {ALU_MUL(_R(dst), dstChan, _R(aReg), _x, bSel, b0)});
        vertexEmit(ops, {ALU_MULADD(_R(dst), dstChan, _R(aReg), _y, bSel, b0 + 1, _R(dst), dstChan)});
        vertexEmit(ops, {ALU_MULADD(_R(dst), dstChan, _R(aReg), _z, bSel, b0 + 2, _R(dst), dstChan)});
    };
    if (!(ctrl & 2)) {
        // Lighting off: the material alone (no ambient term).
        for (uint32_t ch = 0; ch < 4; ++ch) vertexEmit(ops, {ALU_MOV(_R(43), ch, ALU_SRC_1, _x)});
    } else {
        const uint32_t mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
        const uint32_t diff = (ctrl >> 7) & 3;
        const bool atten = (ctrl >> 9) & 1;
        const bool spot = (ctrl >> 10) & 1;
        for (uint32_t light = 0; light < 8; ++light) {
            if (!(mask & (1u << light))) continue;
            const uint32_t c = kXfLightBase + light * 4;
            // R42 = light vector (normalised below), R48.x = its length.
            if (!atten || spot) {
                vertexEmit(ops, {ALU_ADD(_R(42), _x, _GX2_C(c + 2), _z, _R(40) _NEG, _x)});
                vertexEmit(ops, {ALU_ADD(_R(42), _y, _GX2_C(c + 2), _w, _R(40) _NEG, _y)});
                vertexEmit(ops, {ALU_ADD(_R(42), _z, _GX2_C(c + 3), _x, _R(40) _NEG, _z)});
            } else {
                vertexEmit(ops, {ALU_MOV(_R(42), _x, _GX2_C(c + 2), _z)});
                vertexEmit(ops, {ALU_MOV(_R(42), _y, _GX2_C(c + 2), _w)});
                vertexEmit(ops, {ALU_MOV(_R(42), _z, _GX2_C(c + 3), _x)});
            }
            dot3(45, _x, 42, _R(42), _x);                                   // len^2
            vertexEmit(ops, {ALU_SETGT(_R(47), _x, _R(45), _x, ALU_SRC_0, _x)});
            vertexEmit(ops, {ALU_RECIPSQRT_IEEE(_R(46), _y, _R(45), _x)});   // 1/len
            vertexEmit(ops, {ALU_MUL(_R(46), _y, _R(46), _y, _R(47), _x)});  // 0 when len == 0
            vertexEmit(ops, {ALU_MUL(_R(48), _x, _R(45), _x, _R(46), _y)});  // len
            for (uint32_t ch = 0; ch < 3; ++ch)
                vertexEmit(ops, {ALU_MUL(_R(42), ch, _R(42), ch, _R(46), _y)});

            if (atten) {
                // R45.y = attenuation variable (cos for spot, t for specular),
                // R48.y = distance variable (len for spot, t for specular).
                if (spot) {
                    dot3(45, _y, 42, _GX2_C(c + 3), _y);
                    vertexEmit(ops, {ALU_MAX(_R(45), _y, _R(45), _y, ALU_SRC_0, _x)});
                    vertexEmit(ops, {ALU_MOV(_R(48), _y, _R(48), _x)});
                } else {
                    dot3(45, _z, 41, _R(42), _x);                           // n . l
                    vertexEmit(ops, {ALU_SETGT(_R(47), _z, _R(45), _z, ALU_SRC_0, _x)});
                    dot3(45, _y, 41, _GX2_C(c + 3), _y);                    // n . dir
                    vertexEmit(ops, {ALU_MAX(_R(45), _y, _R(45), _y, ALU_SRC_0, _x)});
                    vertexEmit(ops, {ALU_MUL(_R(45), _y, _R(45), _y, _R(47), _z)});
                    vertexEmit(ops, {ALU_MOV(_R(48), _y, _R(45), _y)});
                }
                // num = max(a0 + a1 v + a2 v^2, 0)
                vertexEmit(ops, {ALU_MULADD(_R(46), _x, _GX2_C(c + 1), _z, _R(45), _y, _GX2_C(c + 1), _y)});
                vertexEmit(ops, {ALU_MULADD(_R(46), _x, _R(46), _x, _R(45), _y, _GX2_C(c + 1), _x)});
                vertexEmit(ops, {ALU_MAX(_R(46), _x, _R(46), _x, ALU_SRC_0, _x)});
                // den = k0 + k1 d + k2 d^2; a non-positive denominator gives no light
                vertexEmit(ops, {ALU_MULADD(_R(46), _z, _GX2_C(c + 2), _y, _R(48), _y, _GX2_C(c + 2), _x)});
                vertexEmit(ops, {ALU_MULADD(_R(46), _z, _R(46), _z, _R(48), _y, _GX2_C(c + 1), _w)});
                vertexEmit(ops, {ALU_SETGT(_R(47), _y, _R(46), _z, ALU_SRC_0, _x)});
                vertexEmit(ops, {ALU_RECIP_IEEE(_R(46), _z, _R(46), _z)});
                vertexEmit(ops, {ALU_MUL(_R(46), _x, _R(46), _x, _R(46), _z)});
                vertexEmit(ops, {ALU_MUL(_R(46), _x, _R(46), _x, _R(47), _y)});
            } else {
                vertexEmit(ops, {ALU_MOV(_R(46), _x, ALU_SRC_1, _x)});
            }
            if (diff != 0) {
                dot3(45, _x, 41, _R(42), _x);
                if (diff >= 2) vertexEmit(ops, {ALU_MAX(_R(45), _x, _R(45), _x, ALU_SRC_0, _x)});
                vertexEmit(ops, {ALU_MUL(_R(46), _x, _R(46), _x, _R(45), _x)});
            }
            for (uint32_t ch = 0; ch < 4; ++ch)
                vertexEmit(ops, {ALU_MULADD(_R(43), ch, _R(46), _x, _GX2_C(c), ch, _R(43), ch)});
        }
    }
    for (uint32_t ch = first; ch < first + count; ++ch) {
        vertexEmit(ops, {ALU_MAX(_R(43), ch, _R(43), ch, ALU_SRC_0, _x)});
        vertexEmit(ops, {ALU_MIN(_R(43), ch, _R(43), ch, ALU_SRC_1, _x)});
        vertexEmit(ops, {ALU_MUL(_R(dst), ch, _R(44), ch, _R(43), ch)});
    }
}

static void vertexEmitColour(std::vector<uint64_t>& ops, const VertexKey& key, uint32_t channel, uint32_t dst) {
    if (channel >= key.numChans) {
        for (uint32_t ch = 0; ch < 4; ++ch) vertexEmit(ops, {ALU_MOV(_R(dst), ch, ALU_SRC_0, _x)});
        return;
    }
    const uint32_t ctrl = key.colorCtrl[channel];
    const uint32_t alphaCtrl = key.alphaCtrl[channel];
    if (ctrl == alphaCtrl) {
        vertexEmitLighting(ops, ctrl, channel, dst, 0, 4);
    } else {
        vertexEmitLighting(ops, ctrl, channel, dst, 0, 3);
        vertexEmitLighting(ops, alphaCtrl, channel, dst, 3, 1);
    }
}

static bool vertexKeySupported(const VertexKey& key) {
    if (key.numTexGens > 8 || key.fixedCoord > 7 || key.fixedTextured > 1) return false;
    return true;
}

static bool runningUnderCemu();
static bool s_vertexOneClause;

static bool initVertexProgram(VertexProgram& shader) {
    s_vertexRejectReason = "unknown";
    if (!vertexKeySupported(shader.key)) {
        s_vertexRejectReason = "key-range";
        return false;
    }
    std::vector<uint64_t> alu;
    alu.reserve(512);
    const VertexKey& key = shader.key;
    // Cemu uploads only the uniform registers a program names statically,
    // so matrices reached through AR read stale values there.  Name every XF
    // vector once under Cemu; hardware reads the register file directly.
    if (runningUnderCemu())
        for (uint32_t r = 0; r < kXfUniformRegs; ++r)
            vertexEmit(alu, {ALU_MOV(_R(57), _x, _GX2_C(r), _x)});

    // Position: raw position -> the per-vertex model matrix selected through
    // MOVA/AR-indexed constant reads -> live projection and target mapping.
    vertexMatrixTransform(alu, 1, RAW_ATTR_MTX0 + 1, _x, 40, 0, true);
    if (key.projectionOrtho) {
        vertexEmit(alu, {ALU_MULADD(_R(20), _x, _R(40), _x, _GX2_C(kVtxLiveBase), _x,
                                    _GX2_C(kVtxLiveBase), _y)});
        vertexEmit(alu, {ALU_MULADD(_R(20), _y, _R(40), _y, _GX2_C(kVtxLiveBase), _z,
                                    _GX2_C(kVtxLiveBase), _w)});
        vertexEmit(alu, {ALU_MULADD(_R(20), _z, _R(40), _z, _GX2_C(kVtxLiveBase + 1), _x,
                                    _GX2_C(kVtxLiveBase + 1), _y)});
        vertexEmit(alu, {ALU_MOV(_R(20), _w, ALU_SRC_1, _x)});
    } else {
        vertexEmit(alu, {ALU_MUL(_R(20), _x, _R(40), _x, _GX2_C(kVtxLiveBase), _x)});
        vertexEmit(alu, {ALU_MULADD(_R(20), _x, _R(40), _z, _GX2_C(kVtxLiveBase), _y,
                                    _R(20), _x)});
        vertexEmit(alu, {ALU_MUL(_R(20), _y, _R(40), _y, _GX2_C(kVtxLiveBase), _z)});
        vertexEmit(alu, {ALU_MULADD(_R(20), _y, _R(40), _z, _GX2_C(kVtxLiveBase), _w,
                                    _R(20), _y)});
        vertexEmit(alu, {ALU_MULADD(_R(20), _z, _R(40), _z, _GX2_C(kVtxLiveBase + 1), _x,
                                    _GX2_C(kVtxLiveBase + 1), _y)});
        vertexEmit(alu, {ALU_ADD(_R(20), _w, ALU_SRC_0, _x, _R(40) _NEG, _z)});
    }
    // viewport -> TV clip coordinates. C186 is the six-value viewport block;
    // C187.x/y/z/w are game scale, game X, and TV dimensions. Keep this
    // algebra identical to transformPosition: the generated path must land
    // in the same 640x480 letterboxed rectangle as the CPU reference.
    vertexEmit(alu, {ALU_MUL(_R(54), _x, _GX2_C(kVtxLiveBase + 2), _z,
                             _GX2_C(kVtxLiveBase + 3), _x)});
    vertexEmit(alu, {ALU_MUL(_R(54), _x, _R(54), _x, ALU_SRC_0_5, _x)});
    vertexEmit(alu, {ALU_MUL(_R(54), _y, _GX2_C(kVtxLiveBase + 2), _w,
                             _GX2_C(kVtxLiveBase + 3), _x)});
    vertexEmit(alu, {ALU_MUL(_R(54), _y, _R(54), _y, ALU_SRC_0_5, _x)});
    vertexEmit(alu, {ALU_MULADD(_R(55), _x, _GX2_C(kVtxLiveBase + 2), _x,
                                _GX2_C(kVtxLiveBase + 3), _x,
                                _R(54), _x)});
    vertexEmit(alu, {ALU_ADD(_R(55), _x, _R(55), _x, _GX2_C(kVtxLiveBase + 3), _y)});
    vertexEmit(alu, {ALU_MULADD(_R(55), _y, _GX2_C(kVtxLiveBase + 2), _y,
                                _GX2_C(kVtxLiveBase + 3), _x,
                                _R(54), _y)});
    vertexEmit(alu, {ALU_MUL(_R(55), _x, _R(55), _x, _R(20), _w)});
    vertexEmit(alu, {ALU_MULADD(_R(55), _x, _R(54), _x, _R(20), _x, _R(55), _x)});
    vertexEmit(alu, {ALU_MUL(_R(55), _y, _R(55), _y, _R(20), _w)});
    vertexEmit(alu, {ALU_MULADD(_R(55), _y, _R(54), _y, _R(20) _NEG, _y, _R(55), _y)});
    vertexEmit(alu, {ALU_RECIP_IEEE(_R(56), _x, _GX2_C(kVtxLiveBase + 3), _z)});
    vertexEmit(alu, {ALU_MUL_x2(_R(55), _x, _R(55), _x, _R(56), _x)});
    vertexEmit(alu, {ALU_RECIP_IEEE(_R(56), _x, _GX2_C(kVtxLiveBase + 3), _w)});
    vertexEmit(alu, {ALU_MUL_x2(_R(55), _y, _R(55), _y, _R(56), _x)});
    vertexEmit(alu, {ALU_ADD(_R(20), _x, _R(55), _x, _R(20) _NEG, _w)});
    vertexEmit(alu, {ALU_ADD(_R(20), _y, _R(20), _w, _R(55) _NEG, _y)});
    // depth = scale * cz + offset * cw (transformPosition's out[2]).
    vertexEmit(alu, {ALU_MUL(_R(56), _y, _GX2_C(kVtxLiveBase + 4), _y, _R(20), _w)});
    vertexEmit(alu, {ALU_MULADD(_R(20), _z, _GX2_C(kVtxLiveBase + 4), _x, _R(20), _z, _R(56), _y)});

#if SMS_GX2_VX_ORACLE == 1
    // Diagnostic: export the CPU reference clip position, which flushBatch
    // stores in the raw tex[6..7] slot (R10), instead of the generated one.
    for (uint32_t ch = 0; ch < 4; ++ch)
        vertexEmit(alu, {ALU_MOV(_R(20), ch, _R(10), ch)});
#endif

    if (key.outputMode) {
        // The fixed pixel programs consume the old five-parameter contract:
        // PARAM0 is the selected UV and PARAM4 is the vertex colour.  Keep
        // the other three slots defined so their existing register layout is
        // unchanged on both Cafe and Cemu.
        if (key.fixedTextured) vertexCopyTexSource(alu, key.fixedCoord, 21);
        else {
            vertexEmit(alu, {ALU_MOV(_R(21), _x, ALU_SRC_0, _x)});
            vertexEmit(alu, {ALU_MOV(_R(21), _y, ALU_SRC_0, _x)});
            vertexEmit(alu, {ALU_MOV(_R(21), _z, ALU_SRC_1, _x)});
            vertexEmit(alu, {ALU_MOV(_R(21), _w, ALU_SRC_1, _x)});
        }
        for (uint32_t reg = 22; reg <= 24; ++reg) {
            vertexEmit(alu, {ALU_MOV(_R(reg), _x, ALU_SRC_0, _x)});
            vertexEmit(alu, {ALU_MOV(_R(reg), _y, ALU_SRC_0, _x)});
            vertexEmit(alu, {ALU_MOV(_R(reg), _z, ALU_SRC_0, _x)});
            vertexEmit(alu, {ALU_MOV(_R(reg), _w, ALU_SRC_1, _x)});
        }
        vertexCopySource(alu, 5, 29, true);
    } else {
        bool needsNormal = false;
        for (uint32_t c = 0; c < 2; ++c)
            needsNormal = needsNormal || (c < key.numChans &&
                                          ((key.colorCtrl[c] | key.alphaCtrl[c]) & 2));
        if (needsNormal) vertexNormalTransform(alu);
        vertexEmitColour(alu, key, 0, 29);
        vertexEmitColour(alu, key, 1, 30);
        const uint32_t matrixIndexBase = RAW_ATTR_MTX0 + 1;
        for (uint32_t coord = 0; coord < 8; ++coord) {
        const uint32_t dst = 21 + coord;
        if (coord >= key.numTexGens) {
            vertexEmit(alu, {ALU_MOV(_R(dst), _x, ALU_SRC_0, _x)});
            vertexEmit(alu, {ALU_MOV(_R(dst), _y, ALU_SRC_0, _x)});
            vertexEmit(alu, {ALU_MOV(_R(dst), _z, ALU_SRC_0, _x)});
            vertexEmit(alu, {ALU_MOV(_R(dst), _w, ALU_SRC_1, _x)});
            continue;
        }
        const uint32_t tg = key.texgen[coord];
        const uint32_t type = (tg >> 4) & 7;
        const uint32_t row = (tg >> 7) & 31;
        const uint32_t form = (tg >> 2) & 1;
        if (type == 0) {
            if (row >= 5) vertexCopyTexSource(alu, row - 5, 52);
            else {
                const uint32_t source = row == 2 ? 29 : vertexInputForRow(row);
                vertexCopySource(alu, source, 52, row == 2);
            }
            const uint32_t index = matrixIndexBase + (coord + 1) / 4;
            const uint32_t indexChan = (coord + 1) & 3;
            vertexMatrixTransform(alu, 52, index, indexChan, 55, SMS_GX2_VX_ORACLE == 4 ? 60 : 0,
                                  SMS_GX2_VX_ORACLE != 4);
            if ((key.dualTex & 1) != 0) {
                const uint32_t post = key.postTex[coord];
                if ((post >> 8) & 1) {
                    vertexEmit(alu, {ALU_MOV(_R(55), _w, ALU_SRC_0, _x)});
                    vertexEmitDot4(alu, 56, _x, _R(55), _R(55));
                    vertexEmit(alu, {ALU_SETGT(_R(56), _y, _R(56), _x, ALU_SRC_0, _x)});
                    vertexEmit(alu, {ALU_CNDGT(_R(56), _z, _R(56), _y, _R(56), _x, ALU_SRC_1, _x)});
                    vertexEmit(alu, {ALU_RECIPSQRT_IEEE(_R(56), _x, _R(56), _z)});
                    for (uint32_t ch = 0; ch < 3; ++ch) {
                        vertexEmit(alu, {ALU_MUL(_R(55), ch, _R(55), ch, _R(56), _x)});
                    }
                    vertexEmit(alu, {ALU_MOV(_R(55), _w, ALU_SRC_1, _x)});
                }
                vertexEmit(alu, {ALU_MOV(_R(55), _w, ALU_SRC_1, _x)});
                // R55 is both input and output: accumulate into R57 first.
                vertexLoadAbsoluteMatrix(alu, 58, kXfPostTexBase + (post & 63), 3);
                for (uint32_t ch = 0; ch < 3; ++ch)
                    vertexEmitDot4(alu, 57, ch, _R(55), _R(58 + ch));
                for (uint32_t ch = 0; ch < 3; ++ch)
                    vertexEmit(alu, {ALU_MOV(_R(55), ch, _R(57), ch)});
            }
        } else if (type == 1) {
            const uint32_t emboss = (tg >> 12) & 7;
            const uint32_t source = 31 + (emboss < coord ? emboss : 0);
            vertexEmit(alu, {ALU_MOV(_R(55), _x, _R(source), _x)});
            vertexEmit(alu, {ALU_MOV(_R(55), _y, _R(source), _y)});
            vertexEmit(alu, {ALU_MOV(_R(55), _z, _R(source), _z)});
            vertexEmit(alu, {ALU_MOV(_R(55), _w, ALU_SRC_1, _x)});
        } else {
            vertexCopySource(alu, type == 3 ? 30 : 29, 55, true);
        }
        if (form == 0 && row < 5) {
            vertexEmit(alu, {ALU_MOV(_R(55), _z, ALU_SRC_1, _x)});
            vertexEmit(alu, {ALU_MOV(_R(55), _w, ALU_SRC_1, _x)});
        }
        if ((tg >> 1) & 1) {
            // Avoid reciprocal(0), preserving the CPU path's zero divide
            // guard while keeping the common path branch-free.
            vertexEmit(alu, {ALU_SETGT(_R(56), _x, _R(55), _z, ALU_SRC_0, _x)});
            vertexEmit(alu, {ALU_SETGT(_R(56), _y, _R(55) _NEG, _z, ALU_SRC_0, _x)});
            vertexEmit(alu, {ALU_MAX(_R(56), _x, _R(56), _x, _R(56), _y)});
            vertexEmit(alu, {ALU_CNDGT(_R(56), _z, _R(56), _x, _R(55), _z, ALU_SRC_1, _x)});
            vertexEmit(alu, {ALU_RECIP_IEEE(_R(56), _x, _R(56), _z)});
            vertexEmit(alu, {ALU_MUL(_R(55), _x, _R(55), _x, _R(56), _x)});
            vertexEmit(alu, {ALU_MUL(_R(55), _y, _R(55), _y, _R(56), _x)});
        }
        vertexEmit(alu, {ALU_MOV(_R(31 + coord), _x, _R(55), _x)});
        vertexEmit(alu, {ALU_MOV(_R(31 + coord), _y, _R(55), _y)});
        vertexEmit(alu, {ALU_MOV(_R(31 + coord), _z, _R(55), _z)});
        vertexEmit(alu, {ALU_MOV(_R(31 + coord), _w, ALU_SRC_1, _x)});
        // u = copy.z + (u * manualScale.x) * copy.x, as transformTev and
        // adjustCopyUv compute it; the scale pair for coord n sits in
        // live vector 9 + n/2, channels (n&1)*2 and (n&1)*2+1.
        const uint32_t scale = kVtxLiveBase + 9 + coord / 2;
        const uint32_t scaleChan = (coord & 1) * 2;
        const uint32_t copy = kVtxLiveBase + 13 + coord;
        vertexEmit(alu, {ALU_MUL(_R(56), _x, _R(55), _x, _GX2_C(scale), scaleChan)});
        vertexEmit(alu, {ALU_MUL(_R(56), _y, _R(55), _y, _GX2_C(scale), scaleChan + 1)});
        vertexEmit(alu, {ALU_MULADD(_R(dst), _x, _R(56), _x, _GX2_C(copy), _x,
                                    _GX2_C(copy), _z)});
        vertexEmit(alu, {ALU_MULADD(_R(dst), _y, _R(56), _y, _GX2_C(copy), _y,
                                    _GX2_C(copy), _w)});
        vertexEmit(alu, {ALU_MOV(_R(dst), _z, ALU_SRC_0, _x)});
        vertexEmit(alu, {ALU_MOV(_R(dst), _w, ALU_SRC_1, _x)});
        }
    }

#if SMS_GX2_VX_ORACLE == 3
    // Diagnostic: raw texcoord 0 attribute straight through (identity texmtx).
    if (!key.outputMode) {
        vertexEmit(alu, {ALU_MOV(_R(21), _x, _R(7), _x)});
        vertexEmit(alu, {ALU_MOV(_R(21), _y, _R(7), _y)});
    }
#endif
#if SMS_GX2_VX_ORACLE == 6
    // Diagnostic: CPU reference colour channels 0/1 (R9/R10) replace the generated ones.
    if (!key.outputMode)
        for (uint32_t ch = 0; ch < 4; ++ch) {
            vertexEmit(alu, {ALU_MOV(_R(29), ch, _R(9), ch)});
            vertexEmit(alu, {ALU_MOV(_R(30), ch, _R(10), ch)});
        }
#endif
#if SMS_GX2_VX_ORACLE == 2
    // Diagnostic: CPU reference texcoords 0/1 (R10.xy/zw) replace the generated ones.
    if (!key.outputMode) {
        vertexEmit(alu, {ALU_MOV(_R(21), _x, _R(10), _x)});
        vertexEmit(alu, {ALU_MOV(_R(21), _y, _R(10), _y)});
        vertexEmit(alu, {ALU_MOV(_R(22), _x, _R(10), _z)});
        vertexEmit(alu, {ALU_MOV(_R(22), _y, _R(10), _w)});
    }
#endif
    if (alu.size() > 2048) {
        s_vertexRejectReason = "alu-limit";
        return false;
    }
    shader.aluCount = uint32_t(alu.size());
    const size_t clauses = (alu.size() + 127) / 128;
    if (!clauses || clauses > 16) {
        s_vertexRejectReason = "clause-limit";
        return false;
    }
    if (clauses > 1 && s_vertexOneClause) {
        s_vertexRejectReason = "multi-clause (gpuxf1clause marker)";
        return false;
    }
    // Control flow first, ALU code after it: the CF stream must end in the
    // END_OF_PROGRAM export, or execution runs on into the ALU words.
    std::vector<uint64_t> exports;
    exports.push_back(EXP_DONE(POS0, _R(20), _x, _y, _z, _w) NO_BARRIER);
    if (key.outputMode) {
        for (uint32_t i = 0; i < 4; ++i)
            exports.push_back(EXP(PARAM(i), _R(21 + i), _x, _y, _z, _w) NO_BARRIER);
        exports.push_back(EXP_DONE(PARAM(4), _R(29), _x, _y, _z, _w) END_OF_PROGRAM);
    } else {
        for (uint32_t coord = 0; coord < 8; ++coord)
            exports.push_back(EXP(PARAM(coord), _R(21 + coord), _x, _y, _z, _w) NO_BARRIER);
        exports.push_back(EXP(PARAM(8), _R(29), _x, _y, _z, _w) NO_BARRIER);
        exports.push_back(EXP_DONE(PARAM(9), _R(30), _x, _y, _z, _w) END_OF_PROGRAM);
    }
    const size_t cfCount = 1 + clauses + exports.size();
    const size_t aluOffset = (cfCount + 1) & ~size_t(1);
    std::vector<uint64_t> program(aluOffset, 0);
    program[0] = CALL_FS NO_BARRIER;
    for (size_t clause = 0; clause < clauses; ++clause) {
        const size_t at = clause * 128;
        const size_t count = std::min<size_t>(128, alu.size() - at);
        program[1 + clause] = ALU(aluOffset + at, count);
    }
    for (size_t i = 0; i < exports.size(); ++i) program[1 + clauses + i] = exports[i];
    program.insert(program.end(), alu.begin(), alu.end());
    const size_t bytes = program.size() * sizeof(uint64_t);
    shader.vsProgram = alignedZero(GX2_SHADER_PROGRAM_ALIGNMENT, bytes);
    if (!shader.vsProgram) {
        s_vertexRejectReason = "vertex-program-allocation";
        return false;
    }
    memcpy(shader.vsProgram, program.data(), bytes);
    memset(&shader.vertex, 0, sizeof(shader.vertex));
    shader.vertex.regs.sq_pgm_resources_vs = 64 | (1u << 8);
    shader.vertex.regs.spi_vs_out_config = key.outputMode ? (4 << 1) : (9 << 1);
    shader.vertex.regs.num_spi_vs_out_id = 3;
    memset(shader.vertex.regs.spi_vs_out_id, 0xff, sizeof(shader.vertex.regs.spi_vs_out_id));
    shader.vertex.regs.spi_vs_out_id[0] = 0x03020100;
    shader.vertex.regs.spi_vs_out_id[1] = 0x07060504;
    // Keep the fixed path's export map identical to the packed shader that
    // the legacy path binds. Cemu rejects the fixed pixel shader when the
    // unused slots are advertised as absent rather than as its ABI's 0x0a.
    shader.vertex.regs.spi_vs_out_id[2] = key.outputMode ? 0xff0a0908 : 0xffff0908;
    shader.vertex.regs.sq_vtx_semantic_clear = ~((1u << RAW_ATTR_COUNT) - 1);
    shader.vertex.regs.num_sq_vtx_semantic = RAW_ATTR_COUNT;
    for (uint32_t i = 0; i < RAW_ATTR_COUNT; ++i) shader.vertex.regs.sq_vtx_semantic[i] = i;
    shader.vertex.regs.vgt_vertex_reuse_block_cntl = 14;
    shader.vertex.regs.vgt_hos_reuse_depth = 16;
    shader.vertex.size = uint32_t(bytes);
    shader.vertex.program = shader.vsProgram;
    shader.vertex.mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
    shader.vertex.attribVarCount = RAW_ATTR_COUNT;
    shader.vertex.attribVars = shader.attribVars;

    const uint32_t offsets[RAW_ATTR_COUNT] = {
        uint32_t(offsetof(RawVertex, pos)), uint32_t(offsetof(RawVertex, nrm)),
        uint32_t(offsetof(RawVertex, bin)), uint32_t(offsetof(RawVertex, tan)),
        uint32_t(offsetof(RawVertex, clr[0])), uint32_t(offsetof(RawVertex, clr[1])),
        uint32_t(offsetof(RawVertex, tex[0])), uint32_t(offsetof(RawVertex, tex[2])),
        uint32_t(offsetof(RawVertex, tex[4])), uint32_t(offsetof(RawVertex, tex[6])),
        uint32_t(offsetof(RawVertex, mtx[0])), uint32_t(offsetof(RawVertex, mtx[4])),
        uint32_t(offsetof(RawVertex, mtx[8])),
    };
    const uint32_t components[RAW_ATTR_COUNT] = {3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    const bool colourAttr[RAW_ATTR_COUNT] = {false, false, false, false, true, true};
    const bool indexAttr[RAW_ATTR_COUNT] = {false, false, false, false, false, false,
                                            false, false, false, false, true, true, true};
    for (uint32_t i = 0; i < RAW_ATTR_COUNT; ++i) {
        shader.attribVars[i] = {"raw", GX2_SHADER_VAR_TYPE_FLOAT4, 0, i};
        GX2AttribStream& attr = shader.attrs[i];
        attr.location = i;
        attr.buffer = 0;
        attr.offset = offsets[i];
        attr.format = colourAttr[i] ? GX2_ATTRIB_FORMAT_UNORM_8_8_8_8 :
                      indexAttr[i] ? GX2AttribFormat(GX2_ATTRIB_FLAG_SCALED | GX2_ATTRIB_TYPE_8_8_8_8) :
                      components[i] == 3 ? GX2_ATTRIB_FORMAT_FLOAT_32_32_32 : GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32;
        attr.type = GX2_ATTRIB_INDEX_PER_VERTEX;
        attr.aluDivisor = 0;
        // Positions are fetched as three floats; w must read as 1 so the
        // dot4 with a matrix row picks up its translation (a missing
        // component's SEL_W reads 0).  Normals keep w = 0.
        attr.mask = GX2_COMP_MAP(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z,
                                 i == RAW_ATTR_POS ? GX2_SQ_SEL_1 : GX2_SQ_SEL_W);
        attr.endianSwap = GX2_ENDIAN_SWAP_DEFAULT;
    }
    shader.stride = kRawVertexStride;
    shader.fetch.size = GX2CalcFetchShaderSizeEx(RAW_ATTR_COUNT, GX2_FETCH_SHADER_TESSELLATION_NONE,
                                                   GX2_TESSELLATION_MODE_DISCRETE);
    shader.fetchProgram = alignedZero(GX2_SHADER_PROGRAM_ALIGNMENT, shader.fetch.size);
    if (!shader.fetchProgram) {
        s_vertexRejectReason = "fetch-program-allocation";
        return false;
    }
    shader.fetch.program = shader.fetchProgram;
    GX2InitFetchShaderEx(&shader.fetch, static_cast<uint8_t*>(shader.fetchProgram), RAW_ATTR_COUNT, shader.attrs,
                         GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader.vertex.program, shader.vertex.size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader.fetch.program, shader.fetch.size);
    return true;
}

static VertexProgram* vertexShaderForKey(const VertexKey& key) {
    for (VertexProgram* shader : s_vertexPrograms)
        if (!memcmp(&shader->key, &key, sizeof(key))) return shader->failed ? nullptr : shader;
    VertexProgram* shader = new VertexProgram();
    memset(shader, 0, sizeof(*shader));
    shader->key = key;
    shader->failed = !initVertexProgram(*shader);
    s_vertexPrograms.push_back(shader);
    if (shader->failed) {
        ++s_vertexRejected;
        WHBLogPrintf("SMS Wii U: GPU vertex program rejected reason=%s mode=%u fixedCoord=%u fixedTex=%u chans=%u texgens=%u dual=%u cache=%u",
                     s_vertexRejectReason,
                     (unsigned)key.outputMode, (unsigned)key.fixedCoord, (unsigned)key.fixedTextured,
                     (unsigned)key.numChans, (unsigned)key.numTexGens, (unsigned)key.dualTex,
                     (unsigned)s_vertexPrograms.size());
        return nullptr;
    }
    WHBLogPrintf("SMS Wii U: generated GPU vertex program alu=%u texgens=%u bytes=%u cache=%u chans=%u cc=%08x/%08x tg=%08x/%08x/%08x/%08x dual=%u",
                 (unsigned)shader->aluCount, (unsigned)key.numTexGens, (unsigned)shader->vertex.size, (unsigned)s_vertexPrograms.size(),
                 (unsigned)key.numChans, (unsigned)key.colorCtrl[0], (unsigned)key.colorCtrl[1],
                 (unsigned)key.texgen[0], (unsigned)key.texgen[1], (unsigned)key.texgen[2], (unsigned)key.texgen[3],
                 (unsigned)key.dualTex);
    return shader;
}

// ---- Range analysis: the work a stage needs, from the key alone ----
// genFS's A/B/C inputs are 8-bit and its lerp is 0..255; what a stage adds
// beyond that (D, bias, scale) bounds its result, so the clamp and the 8-bit
// masks are emitted only where a value can leave their range.
static const int kTevK8[8] = {255, 223, 191, 159, 127, 95, 63, 31};

// The value of a selector known when the shader is generated, or -1.
static int tevSelValue(const TevKey& k, uint32_t stage, uint32_t sel, bool alpha) {
    if (!alpha) {
        if (sel == 15) return 0;
        if (sel == 12) return 255;
        if (sel == 13) return 128;
        if (sel == 14) {
            const uint32_t ks = k.ksel[stage] & 31;
            if (ks < 8) return kTevK8[ks];
            if (ks < 12) return 255;
        }
        return -1;
    }
    if (sel == 7) return 0;
    if (sel == 6) {
        const uint32_t ka = (k.ksel[stage] >> 5) & 31;
        if (ka < 8) return kTevK8[ka];
        if (ka < 16) return 255;
    }
    return -1;
}

enum : int { kLerpGeneral = 0, kLerpAZero = 1, kLerpA = 2, kLerpB = 3 };
struct TevPart {
    uint32_t a, b, c, d;     // selectors
    uint32_t bias, op, scale;
    bool clamp, compare;
    int lerp;                // kLerp*: (A*(256-C')+B*C'+128)>>8 in general, or a shortcut
    bool lerpZero, dZero;
    bool needMax, needMin;
    int lo, hi;              // the result's range
};
struct TevPlan {
    TevPart part[16][2];     // [stage][colour, alpha]
    bool written[16][4][2];  // before stage s: register r's part p was written
    int lo[16][4][2], hi[16][4][2];
    bool mask[16][4];        // stage s makes the 8-bit copy of register r
    bool regWritten[4], maskUsed[4], anyCompare;
    bool outMask;            // the output needs its low 8 bits taken
};
static TevPlan s_plan;

static void tevAnalyze(const TevKey& key) {
    TevPlan& pl = s_plan;
    memset(&pl, 0, sizeof(pl));
    bool written[4][2] = {};
    int lo[4][2], hi[4][2];
    for (int r = 0; r < 4; ++r) for (int p = 0; p < 2; ++p) { lo[r][p] = -1024; hi[r][p] = 1023; }
    uint32_t lastColor = 0, lastAlpha = 0;
    for (uint32_t s = 0; s < key.stages; ++s) {
        memcpy(pl.written[s], written, sizeof(written));
        memcpy(pl.lo[s], lo, sizeof(lo));
        memcpy(pl.hi[s], hi, sizeof(hi));
        const uint32_t ce = key.colorEnv[s], ae = key.alphaEnv[s];
        const uint32_t env[2] = {ce, ae};
        int rlo[2], rhi[2];
        for (int p = 0; p < 2; ++p) {
            TevPart& t = pl.part[s][p];
            const uint32_t e = env[p];
            const bool alpha = p == 1;
            if (!alpha) { t.a = (e >> 12) & 15; t.b = (e >> 8) & 15; t.c = (e >> 4) & 15; t.d = e & 15; }
            else { t.a = (e >> 13) & 7; t.b = (e >> 10) & 7; t.c = (e >> 7) & 7; t.d = (e >> 4) & 7; }
            t.bias = (e >> 16) & 3; t.op = (e >> 18) & 1; t.clamp = ((e >> 19) & 1) != 0; t.scale = (e >> 20) & 3;
            t.compare = t.bias == 3;
            const int av = tevSelValue(key, s, t.a, alpha), bv = tevSelValue(key, s, t.b, alpha);
            const int cv = tevSelValue(key, s, t.c, alpha), dv = tevSelValue(key, s, t.d, alpha);
            // A register selector: its register and part.
            auto reg = [&](uint32_t sel, uint32_t* r, uint32_t* part) {
                if (!alpha) { if (sel >= 8) return false; *r = sel >> 1; *part = sel & 1; return true; }
                if (sel >= 4) return false;
                *r = sel; *part = 1; return true;
            };
            auto range = [&](uint32_t sel, int v, int* l, int* h) {
                uint32_t r, part;
                if (v >= 0) { *l = *h = v; }
                else if (reg(sel, &r, &part)) { *l = lo[r][part]; *h = hi[r][part]; }
                else { *l = 0; *h = 255; }
            };
            if (t.compare) t.lerp = kLerpGeneral;
            else if (cv == 0 || t.a == t.b) t.lerp = kLerpA;
            else if (cv == 255) t.lerp = kLerpB;
            else if (av == 0) t.lerp = kLerpAZero;
            else t.lerp = kLerpGeneral;
            t.lerpZero = (t.lerp == kLerpA && av == 0) || (t.lerp == kLerpB && bv == 0);
            t.dZero = dv == 0;
            // A/B/C operands read through the 8-bit view.
            const bool useA = t.compare || t.lerp == kLerpGeneral || t.lerp == kLerpA;
            const bool useB = t.compare || t.lerp != kLerpA;
            const bool useC = t.compare || t.lerp == kLerpGeneral || t.lerp == kLerpAZero;
            const uint32_t abc[3] = {t.a, t.b, t.c};
            const bool use[3] = {useA, useB, useC};
            for (int i = 0; i < 3; ++i) {
                uint32_t r, part;
                if (use[i] && reg(abc[i], &r, &part) && written[r][part] && (lo[r][part] < 0 || hi[r][part] > 255))
                    pl.mask[s][r] = true;
            }
            int l, h, dl, dh;
            if (t.lerpZero) { l = h = 0; }
            else if (t.lerp == kLerpA) { if (av >= 0) l = h = av; else { l = 0; h = 255; } }
            else if (t.lerp == kLerpB) { if (bv >= 0) l = h = bv; else { l = 0; h = 255; } }
            else { l = 0; h = 255; }
            range(t.d, dv, &dl, &dh);
            int x, y;
            if (t.compare) { x = dl; y = dh + (cv >= 0 ? cv : 255); }
            else if (t.op) { x = dl - h; y = dh - l; }
            else { x = dl + l; y = dh + h; }
            if (!t.compare) {
                if (t.bias == 1) { x += 128; y += 128; }
                if (t.bias == 2) { x -= 128; y -= 128; }
                if (t.scale == 1) { x *= 2; y *= 2; }
                if (t.scale == 2) { x *= 4; y *= 4; }
                if (t.scale == 3) { x >>= 1; y >>= 1; }
            }
            const int cl = t.clamp ? 0 : -1024, ch = t.clamp ? 255 : 1023;
            t.needMax = x < cl;
            t.needMin = y > ch;
            t.lo = x < cl ? cl : x > ch ? ch : x;
            t.hi = y > ch ? ch : y < cl ? cl : y;
            rlo[p] = t.lo; rhi[p] = t.hi;
            if (t.compare) pl.anyCompare = true;
        }
        // A 16/24-bit alpha compare reads the colour A/B arguments too.
        if (pl.part[s][1].compare) {
            for (uint32_t sel : {pl.part[s][0].a, pl.part[s][0].b})
                if (sel < 8 && written[sel >> 1][sel & 1] &&
                    (lo[sel >> 1][sel & 1] < 0 || hi[sel >> 1][sel & 1] > 255)) pl.mask[s][sel >> 1] = true;
        }
        const uint32_t cd = (ce >> 22) & 3, ad = (ae >> 22) & 3;
        written[cd][0] = true; lo[cd][0] = rlo[0]; hi[cd][0] = rhi[0];
        written[ad][1] = true; lo[ad][1] = rlo[1]; hi[ad][1] = rhi[1];
        pl.regWritten[cd] = pl.regWritten[ad] = true;
        for (int r = 0; r < 4; ++r) if (pl.mask[s][r]) pl.maskUsed[r] = true;
        lastColor = cd; lastAlpha = ad;
    }
    pl.outMask = lo[lastColor][0] < 0 || hi[lastColor][0] > 255 || lo[lastAlpha][1] < 0 || hi[lastAlpha][1] > 255;
}

// The register views of stage s (see TevViews).
static void tevSetViews(uint32_t s) {
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t p = 0; p < 2; ++p) {
            if (!s_plan.written[s][r][p]) {
                s_view.full[r][p] = _GX2_C(r);
                s_view.masked[r][p] = _GX2_C(43 + r);
            } else {
                s_view.full[r][p] = _R(s_ra.reg[r]);
                const bool inRange = s_plan.lo[s][r][p] >= 0 && s_plan.hi[s][r][p] <= 255;
                s_view.masked[r][p] = inRange ? _R(s_ra.reg[r]) : _R(s_ra.mask[r]);
            }
        }
}

// One channel of a regular (non-compare) stage as a list of operations on the
// channel's temporaries t0-t2; the last one writes the destination.
struct TevStep {
    uint32_t inst;
    int dst;          // 0..2: temporary, 3: the destination
    TevSource s0, s1;
    uint32_t omod;
};
static bool tevConstConflict(TevSource a, TevSource b) {
    return tevIsConst(a) && tevIsConst(b) && a.chan == b.chan && tevSelNoNeg(a) != tevSelNoNeg(b);
}
static void tevChannelSteps(std::vector<TevStep>& st, const TevPart& t, uint32_t ch, TevSource a, TevSource b,
                            TevSource c, TevSource d) {
    const uint32_t* tg = s_ra.t;
    auto T = [&](int i) { return tevG(tg[i], ch); };
    auto op = [&](uint32_t inst, int dst, TevSource s0, TevSource s1, uint32_t omod = ALU_OMOD_OFF) {
        // One instruction reads one constant-file address per channel.
        if (tevConstConflict(s0, s1)) {
            st.push_back({kOpMov, dst, s1, {ALU_SRC_0, 0}, ALU_OMOD_OFF});
            s1 = T(dst);
        }
        st.push_back({inst, dst, s0, s1, omod});
    };
    // 128 from whichever constant does not collide with x's channel.
    auto k128 = [&](TevSource x) { return tevIsConst(x) && x.chan == _y ? tevC(12, _z) : tevC(11, _y); };
    TevSource cur;
    if (t.compare) {
        // D + (cond ? C : 0); 8/16/24-bit conditions were computed into t0.x
        // (colour) or t0.w (alpha), per-component ones are made here.
        const uint32_t mode = (t.scale << 1) | t.op;
        TevSource cond;
        if (mode < 6) {
            cond = tevG(tg[0], ch == _w ? uint32_t(_w) : uint32_t(_x));
        } else {
            op((mode & 1) ? ALU_OP2_INST_SETE : ALU_OP2_INST_SETGT, 0, a, b);
            cond = T(0);
        }
        op(kOpMul, 1, cond, c);
        cur = T(1);
        if (!t.dZero) { op(kOpAdd, 1, d, cur); cur = T(1); }
        if (t.needMax) { op(kOpMax, 1, cur, t.clamp ? TevSource{ALU_SRC_0, 0} : tevC(13, _z)); cur = T(1); }
        if (t.needMin) { op(kOpMin, 1, cur, t.clamp ? tevC(12, _w) : tevC(13, _w)); cur = T(1); }
        if (st.empty() || !(st.back().dst == 1 && cur.sel == T(1).sel && cur.chan == ch && tevSelNoNeg(cur) == cur.sel))
            st.push_back({kOpMov, 1, cur, {ALU_SRC_0, 0}, ALU_OMOD_OFF});
        st.back().dst = 3;
        return;
    }
    if (t.lerp == kLerpA) cur = a;
    else if (t.lerp == kLerpB) cur = b;
    else {
        // C' = C + (C >= 128)
        op(ALU_OP2_INST_SETGE, 0, c, k128(c));
        op(kOpAdd, 0, c, T(0));
        if (t.lerp == kLerpAZero) {
            op(kOpMul, 1, b, T(0));
        } else {
            op(kOpAdd, 1, b, tevNeg(a));
            op(kOpMul, 1, T(1), T(0));
            op(kOpMul, 2, a, tevC(12, _x));
            op(kOpAdd, 1, T(1), T(2));
        }
        op(kOpAdd, 1, T(1), tevC(11, _y));
        op(kOpMul, 1, T(1), tevC(12, _y));
        op(kOpFloor, 1, T(1), {ALU_SRC_0, 0});
        cur = T(1);
    }
    if (t.lerpZero) {
        cur = d;
    } else if (t.dZero) {
        if (t.op) op(kOpAdd, 1, {ALU_SRC_0, 0}, tevNeg(cur));
        if (t.op) cur = T(1);
    } else {
        op(kOpAdd, 1, d, t.op ? tevNeg(cur) : cur);
        cur = T(1);
    }
    if (t.bias == 1 || t.bias == 2) { op(kOpAdd, 1, cur, t.bias == 1 ? k128(cur) : tevC(14, _x)); cur = T(1); }
    if (t.scale == 1 || t.scale == 2) {
        op(kOpMul, 1, cur, {ALU_SRC_1, 0}, t.scale == 1 ? ALU_OMOD_M2 : ALU_OMOD_M4);
        cur = T(1);
    } else if (t.scale == 3) {
        op(kOpMul, 1, cur, {ALU_SRC_0_5, 0});
        op(kOpFloor, 1, T(1), {ALU_SRC_0, 0});
        cur = T(1);
    }
    if (t.needMax) { op(kOpMax, 1, cur, t.clamp ? TevSource{ALU_SRC_0, 0} : tevC(13, _z)); cur = T(1); }
    if (t.needMin) { op(kOpMin, 1, cur, t.clamp ? tevC(12, _w) : tevC(13, _w)); cur = T(1); }
    if (st.empty() || !(st.back().dst == 1 && cur.sel == T(1).sel && cur.chan == ch && tevSelNoNeg(cur) == cur.sel))
        st.push_back({kOpMov, 1, cur, {ALU_SRC_0, 0}, ALU_OMOD_OFF});
    st.back().dst = 3;
}

// The channels of a regular stage, end-aligned so that every channel writes
// its destination in the last group (a channel still running may read another
// channel of a register being written).
static void tevEmitSteps(std::vector<uint64_t>& program, std::vector<TevStep>* steps, const uint32_t* chs,
                         uint32_t n, uint32_t dst) {
    size_t longest = 0;
    for (uint32_t i = 0; i < n; ++i) if (steps[i].size() > longest) longest = steps[i].size();
    std::vector<TevIns> g;
    for (size_t k = 0; k < longest; ++k) {
        for (uint32_t i = 0; i < n; ++i) {
            const size_t skip = longest - steps[i].size();
            if (k < skip) continue;
            const TevStep& st = steps[i][k - skip];
            const uint32_t to = st.dst == 3 ? dst : s_ra.t[st.dst];
            g.push_back(tevOp(st.inst, to, chs[i], st.s0, st.s1, st.omod));
        }
        tevEmitGroup(program, g);
        g.clear();
    }
}

static bool initTevVertex() {
    std::vector<uint64_t> program;
    program.reserve(64);
    program.push_back(CALL_FS NO_BARRIER);
    program.push_back(EXP_DONE(POS0, _R(1), _x, _y, _z, _w));
    // The last export of each type must be EXPORT_DONE: as a plain EXPORT the
    // final parameter (colour channel 1) never reached the pixel shader, which
    // read it as 0 (Mario's material takes its alpha from channel 1).
    for (uint32_t i = 0; i < 10; ++i) {
        if (i == 9) program.push_back(EXP_DONE(PARAM(9), _R(2 + i), _x, _y, _z, _w) END_OF_PROGRAM);
        else program.push_back(EXP(PARAM(i), _R(2 + i), _x, _y, _z, _w) NO_BARRIER);
    }
    s_tevVertexProgram = alignedZero(GX2_SHADER_PROGRAM_ALIGNMENT, program.size() * sizeof(uint64_t));
    if (!s_tevVertexProgram) return false;
    memcpy(s_tevVertexProgram, program.data(), program.size() * sizeof(uint64_t));
    memset(&s_tevVertex, 0, sizeof(s_tevVertex));
    s_tevVertex.regs.sq_pgm_resources_vs = 12 | (1u << 8);
    s_tevVertex.regs.spi_vs_out_config = 9 << 1;
    s_tevVertex.regs.num_spi_vs_out_id = 3;
    memset(s_tevVertex.regs.spi_vs_out_id, 0xff, sizeof(s_tevVertex.regs.spi_vs_out_id));
    s_tevVertex.regs.spi_vs_out_id[0] = 0x03020100;
    s_tevVertex.regs.spi_vs_out_id[1] = 0x07060504;
    s_tevVertex.regs.spi_vs_out_id[2] = 0xffff0908;
    s_tevVertex.regs.sq_vtx_semantic_clear = ~((1u << 11) - 1);
    s_tevVertex.regs.num_sq_vtx_semantic = 11;
    for (uint32_t i = 0; i < 11; ++i) s_tevVertex.regs.sq_vtx_semantic[i] = i;
    s_tevVertex.regs.vgt_vertex_reuse_block_cntl = 14;
    s_tevVertex.regs.vgt_hos_reuse_depth = 16;
    s_tevVertex.size = uint32_t(program.size() * sizeof(uint64_t));
    s_tevVertex.program = s_tevVertexProgram;
    s_tevVertex.mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
    s_tevVertex.attribVarCount = sizeof(s_attribVars) / sizeof(s_attribVars[0]);
    s_tevVertex.attribVars = s_attribVars;

    uint32_t offset = 0, n = 0;
    auto attr = [&](uint32_t location, uint32_t components) {
        GX2AttribStream& a = s_tevAttrs[n++];
        a.location = location;
        a.buffer = 0;
        a.offset = offset;
        a.format = components == 2 ? GX2_ATTRIB_FORMAT_FLOAT_32_32 : GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32;
        a.type = GX2_ATTRIB_INDEX_PER_VERTEX;
        a.aluDivisor = 0;
        a.mask = GX2_COMP_MAP(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
        a.endianSwap = GX2_ENDIAN_SWAP_DEFAULT;
        offset += components * sizeof(float);
    };
    attr(0, 4);
    for (uint32_t i = 0; i < 8; ++i) attr(1 + i, 2);
    attr(9, 4);
    attr(10, 4);
    s_tevFetch.size = GX2CalcFetchShaderSizeEx(n, GX2_FETCH_SHADER_TESSELLATION_NONE,
                                                GX2_TESSELLATION_MODE_DISCRETE);
    s_tevFetchProgram = alignedZero(GX2_SHADER_PROGRAM_ALIGNMENT, s_tevFetch.size);
    if (!s_tevFetchProgram) return false;
    s_tevFetch.program = s_tevFetchProgram;
    GX2InitFetchShaderEx(&s_tevFetch, static_cast<uint8_t*>(s_tevFetch.program), n, s_tevAttrs,
                         GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, s_tevVertex.program, s_tevVertex.size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, s_tevFetch.program, s_tevFetch.size);
    return true;
}

static void tevEmitLiteral(std::vector<uint64_t>& program, uint64_t op, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    program.push_back(op | to_QWORD(to_LE(1ull << 31), 0));
    program.push_back(to_QWORD(to_LE(bits), 0));
}

// Write the result of one GX alpha comparison as 0.0 or 1.0, comparing the
// 8-bit output alpha with BP_ALPHACOMPARE's 8-bit references.
static void tevEmitCompare(std::vector<uint64_t>& program, uint32_t dst, uint32_t func,
                           uint32_t ref, uint32_t alphaSel, uint32_t alphaChan) {
    const float threshold = float(ref);  // the TEV output alpha is an 0..255 integer
    switch (func) {
    case 0: // GX_COMPARE_FALSE
        tevEmitAlu(program, {ALU_MOV(_R(dst), _x, ALU_SRC_0, _x)});
        break;
    case 1: // GX_LESS: ref > alpha
        tevEmitLiteral(program, ALU_SETGT(_R(dst), _x, ALU_SRC_LITERAL, _x,
                                          alphaSel, alphaChan), threshold);
        break;
    case 2: // GX_EQUAL
        tevEmitLiteral(program, ALU_SETE(_R(dst), _x, alphaSel, alphaChan,
                                        ALU_SRC_LITERAL, _x), threshold);
        break;
    case 3: // GX_LEQUAL: ref >= alpha
        tevEmitLiteral(program, ALU_SETGE(_R(dst), _x, ALU_SRC_LITERAL, _x,
                                          alphaSel, alphaChan), threshold);
        break;
    case 4: // GX_GREATER: alpha > ref
        tevEmitLiteral(program, ALU_SETGT(_R(dst), _x, alphaSel, alphaChan,
                                          ALU_SRC_LITERAL, _x), threshold);
        break;
    case 5: // GX_NEQUAL
        tevEmitLiteral(program, ALU_SETNE(_R(dst), _x, alphaSel, alphaChan,
                                          ALU_SRC_LITERAL, _x), threshold);
        break;
    case 6: // GX_GEQUAL: alpha >= ref
        tevEmitLiteral(program, ALU_SETGE(_R(dst), _x, alphaSel, alphaChan,
                                          ALU_SRC_LITERAL, _x), threshold);
        break;
    default: // GX_COMPARE_TRUE
        tevEmitAlu(program, {ALU_MOV(_R(dst), _x, ALU_SRC_1, _x)});
        break;
    }
}

// Emit the two BP_ALPHACOMPARE tests, combine them, and discard failing
// pixels.  KILLGT(.5, result) is the normalized 0/1 equivalent of KILLE(0,
// result), and is the floating KILL form emitted by lus-rebase.
static bool tevEmitAlphaCompare(std::vector<uint64_t>& program, uint32_t compare,
                                uint32_t alphaSel, uint32_t alphaChan) {
    const uint32_t ref0 = compare & 0xff;
    const uint32_t ref1 = (compare >> 8) & 0xff;
    const uint32_t func0 = (compare >> 16) & 7;
    const uint32_t func1 = (compare >> 19) & 7;
    const uint32_t op = (compare >> 22) & 3;
    if (func0 == 7 && func1 == 7 && op != 2) return false;

    tevEmitCompare(program, s_ra.t[0], func0, ref0, alphaSel, alphaChan);
    tevEmitCompare(program, s_ra.t[1], func1, ref1, alphaSel, alphaChan);
    switch (op) {
    case 0: // AND
        tevEmitAlu(program, {ALU_MIN(_R(s_ra.t[0]), _x, _R(s_ra.t[0]), _x, _R(s_ra.t[1]), _x)});
        break;
    case 1: // OR
        tevEmitAlu(program, {ALU_MAX(_R(s_ra.t[0]), _x, _R(s_ra.t[0]), _x, _R(s_ra.t[1]), _x)});
        break;
    case 2: // XOR: the sum is one exactly when one input is true.
        tevEmitAlu(program, {ALU_ADD(_R(s_ra.t[2]), _x, _R(s_ra.t[0]), _x, _R(s_ra.t[1]), _x)});
        tevEmitAlu(program, {ALU_SETE(_R(s_ra.t[0]), _x, _R(s_ra.t[2]), _x, ALU_SRC_1, _x)});
        break;
    default: // XNOR
        tevEmitAlu(program, {ALU_ADD(_R(s_ra.t[2]), _x, _R(s_ra.t[0]), _x, _R(s_ra.t[1]), _x)});
        tevEmitAlu(program, {ALU_SETE(_R(s_ra.t[0]), _x, _R(s_ra.t[2]), _x, ALU_SRC_1, _x)});
        tevEmitAlu(program, {ALU_ADD(_R(s_ra.t[0]), _x, ALU_SRC_1, _x, _R(s_ra.t[0]) _NEG, _x)});
        break;
    }
    tevEmitAlu(program, {ALU_KILLGT(__, _x, ALU_SRC_0_5, _x, _R(s_ra.t[0]), _x)});
    return true;
}

// The indirect stage of TEV stage `stage` (genFS, "indirect"): its texture
// coordinate in scoord, the bump alpha in bump, from the lookups in ind.
// Temporaries t0-t3; all values are integers held in floats.
static void tevEmitIndirectStage(std::vector<uint64_t>& program, const TevKey& key, uint32_t stage) {
    const TevAlloc& ra = s_ra;
    const uint32_t ind = key.ind[stage], numInd = tevNumInd(key);
    const uint32_t bt = ind & 3, fmt = (ind >> 2) & 3, bias = (ind >> 4) & 7, bs = (ind >> 7) & 3;
    const uint32_t mid = (ind >> 9) & 15, sw = (ind >> 13) & 7, tw = (ind >> 16) & 7, fb = (ind >> 20) & 1;
    const uint32_t I = ra.ind[bt < numInd ? bt : 0];
    const uint32_t map = key.order[stage] & 7, coord = (key.order[stage] >> 3) & 7;
    const uint32_t V = ra.t[0], T = ra.t[1], U = ra.t[2], B = ra.t[3];
    std::vector<TevIns> g;
    auto flush = [&]() { tevEmitGroup(program, g); g.clear(); };
    // The lookup is (a, b, g): s, t, u.
    const TevSource in[3] = {tevG(I, _w), tevG(I, _z), tevG(I, _y)};
    if (fmt == 0) {
        for (uint32_t c = 0; c < 3; ++c) g.push_back(tevOp1(kOpMov, V, c, in[c]));
        flush();
    } else {
        // & 31, & 15, & 7
        const TevSource size = fmt == 1 ? tevC(39, _w) : fmt == 2 ? tevC(40, _x) : tevC(42, _z);
        const TevSource inv = fmt == 1 ? tevC(41, _x) : fmt == 2 ? tevC(41, _y) : tevC(42, _y);
        for (uint32_t c = 0; c < 3; ++c) g.push_back(tevOp(kOpMul, V, c, in[c], inv));
        flush();
        for (uint32_t c = 0; c < 3; ++c) g.push_back(tevOp1(kOpFloor, V, c, tevG(V, c)));
        flush();
        for (uint32_t c = 0; c < 3; ++c) g.push_back(tevOp(kOpMul, V, c, tevG(V, c), size));
        flush();
        for (uint32_t c = 0; c < 3; ++c) g.push_back(tevOp(kOpAdd, V, c, in[c], tevNeg(tevG(V, c))));
        flush();
    }
    if (bs) {
        // bumpA = (s, s, t, u)[bs] & 0xf8 of the unmasked lookup; y: bumpA | bumpA >> 5
        const uint32_t P = ra.bump[stage];
        g.push_back(tevOp(kOpMul, P, _x, in[bs - 1], tevC(42, _y))); flush();
        g.push_back(tevOp1(kOpFloor, P, _x, tevG(P, _x))); flush();
        g.push_back(tevOp(kOpMul, P, _x, tevG(P, _x), tevC(42, _z))); flush();
        g.push_back(tevOp(kOpMul, P, _y, tevG(P, _x), tevC(41, _x))); flush();
        g.push_back(tevOp1(kOpFloor, P, _y, tevG(P, _y))); flush();
        g.push_back(tevOp(kOpAdd, P, _y, tevG(P, _y), tevG(P, _x))); flush();
    }
    if (bias) {
        const TevSource bv = fmt == 0 ? tevC(14, _x) : TevSource{ALU_SRC_1, _x};
        for (uint32_t c = 0; c < 3; ++c)
            if (bias & (1u << c)) g.push_back(tevOp(kOpAdd, V, c, tevG(V, c), bv));
        flush();
    }
    // The base coordinate in texels of the map: (uv - o) * size / k.
    const TevSource A = tevC(23 + map, 0);
    g.push_back(tevOp(kOpAdd, B, _x, tevG(ra.coord[coord], _x), tevNeg(tevC(23 + map, _x))));
    g.push_back(tevOp(kOpAdd, B, _y, tevG(ra.coord[coord], _y), tevNeg(tevC(23 + map, _y))));
    flush();
    g.push_back(tevOp(kOpMul, B, _x, tevG(B, _x), tevC(23 + map, _z)));
    g.push_back(tevOp(kOpMul, B, _y, tevG(B, _y), tevC(23 + map, _w)));
    flush();
    (void)A;
    // Wrap: 256 >> (n - 1) texels, or 0 for 6.
    const TevSource wrapSize[6] = {{ALU_SRC_0, 0}, tevC(39, _x), tevC(39, _y), tevC(39, _z), tevC(39, _w), tevC(40, _x)};
    const TevSource wrapInv[6] = {{ALU_SRC_0, 0}, tevC(40, _y), tevC(40, _z), tevC(40, _w), tevC(41, _x), tevC(41, _y)};
    const uint32_t wrap[2] = {sw, tw};
    for (uint32_t c = 0; c < 2; ++c) {
        if (wrap[c] == 6 || wrap[c] == 7) {
            g.push_back(tevOp1(kOpMov, B, c, {ALU_SRC_0, _x})); flush();
        } else if (wrap[c]) {
            g.push_back(tevOp(kOpMul, U, c, tevG(B, c), wrapInv[wrap[c]])); flush();
            g.push_back(tevOp1(kOpFloor, U, c, tevG(U, c))); flush();
            g.push_back(tevOp(kOpMul, U, c, tevG(U, c), wrapSize[wrap[c]])); flush();
            g.push_back(tevOp(kOpAdd, B, c, tevG(B, c), tevNeg(tevG(U, c)))); flush();
        }
    }
    if (mid >= 1 && mid <= 3) {
        // The offset: s.10 matrix rows times the lookup, >> 3, shifted by the
        // exponent, kept to 24 bits, in 1/128 texels.
        const uint32_t m = mid - 1;
        const uint32_t row[2] = {17 + 2 * m, 18 + 2 * m};
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpMul, T, r, tevC(row[r], _x), tevG(V, _x)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpMul, U, r, tevC(row[r], _y), tevG(V, _y)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpAdd, T, r, tevG(T, r), tevG(U, r)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpMul, U, r, tevC(row[r], _z), tevG(V, _z)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpAdd, T, r, tevG(T, r), tevG(U, r)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpMul, T, r, tevG(T, r), tevC(42, _y)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp1(kOpFloor, T, r, tevG(T, r)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpMul, T, r, tevG(T, r), tevC(row[0], _w)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp1(kOpFloor, T, r, tevG(T, r)));
        flush();
        // 24-bit wrap: T - 2^24 * floor((T + 2^23) / 2^24)
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpAdd, U, r, tevG(T, r), tevC(42, _x)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpMul, U, r, tevG(U, r), tevC(41, _w)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp1(kOpFloor, U, r, tevG(U, r)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpMul, U, r, tevG(U, r), tevC(41, _z)));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpAdd, T, r, tevG(T, r), tevNeg(tevG(U, r))));
        flush();
        for (uint32_t r = 0; r < 2; ++r) g.push_back(tevOp(kOpMul, T, r, tevG(T, r), tevC(40, _z)));
        flush();
        for (uint32_t c = 0; c < 2; ++c) g.push_back(tevOp(kOpAdd, B, c, tevG(B, c), tevG(T, c)));
        flush();
    }
    if (fb) {
        for (uint32_t c = 0; c < 2; ++c) g.push_back(tevOp(kOpAdd, B, c, tevG(B, c), tevG(ra.prevTc, c)));
        flush();
    }
    for (uint32_t c = 0; c < 2; ++c) g.push_back(tevOp1(kOpMov, ra.prevTc, c, tevG(B, c)));
    flush();
    // Back to the shader coordinate: o + texel * k / size.
    for (uint32_t c = 0; c < 2; ++c) g.push_back(tevOp(kOpMul, B, c, tevG(B, c), tevC(31 + map, c)));
    flush();
    for (uint32_t c = 0; c < 2; ++c) g.push_back(tevOp(kOpAdd, ra.scoord[stage], c, tevG(B, c), tevC(23 + map, c)));
    flush();
}

// ALU clauses of at most 128 slots, split between instruction groups.
static uint32_t s_tevGroups;  // ALU instruction groups of the shader being generated (log only)
static bool tevAluClauses(std::vector<uint64_t>& program, size_t& cf, size_t offset, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (program[offset + i] & to_QWORD(to_LE(1ull << 31), 0)) ++s_tevGroups;
    for (size_t at = 0; at < count;) {
        size_t n = count - at < 128 ? count - at : 128;
        while (n > 0 && at + n < count && !(program[offset + at + n - 1] & to_QWORD(to_LE(1ull << 31), 0))) --n;
        if (n == 0) return false;
        program[cf++] = ALU(offset + at, n);
        at += n;
    }
    return true;
}
// TEX clauses of at most 8 fetches (the CF count field).
static void tevTexClauses(std::vector<uint64_t>& program, size_t& cf, size_t offset, uint32_t count) {
    for (uint32_t at = 0; at < count; at += 8) {
        const uint32_t n = count - at < 8 ? count - at : 8;
        program[cf++] = TEX(offset + 2 * at, n) VALID_PIX;
    }
}

static bool initTevPixel(TevShader& shader) {
    s_tevGroups = 0;
    std::vector<uint64_t> program(40, 0);
    const size_t kCfSlots = 40;
    const TevKey& key = shader.key;
    const uint32_t numInd = tevNumInd(key);
    auto indStage = [&](uint32_t s) { return ((key.order[s] >> 6) & 1) && key.ind[s] != 0; };
    // ---- register allocation ----
    TevAlloc& ra = s_ra;
    memset(&ra, 0xff, sizeof(ra));
    bool useCoord[8] = {}, useRas[2] = {}, anyInd = false, anyFb = false;
    for (uint32_t i = 0; i < numInd; ++i) useCoord[(key.iref >> (6 * i + 3)) & 7] = true;
    for (uint32_t stage = 0; stage < key.stages; ++stage) {
        const uint32_t order = key.order[stage], ce = key.colorEnv[stage], ae = key.alphaEnv[stage];
        if ((order >> 6) & 1) useCoord[(order >> 3) & 7] = true;
        if (indStage(stage)) { anyInd = true; if ((key.ind[stage] >> 20) & 1) anyFb = true; }
        const uint32_t chan = (order >> 7) & 7;
        const uint32_t cs[4] = {(ce >> 12) & 15, (ce >> 8) & 15, (ce >> 4) & 15, ce & 15};
        const uint32_t as[4] = {(ae >> 13) & 7, (ae >> 10) & 7, (ae >> 7) & 7, (ae >> 4) & 7};
        for (int i = 0; i < 4; ++i) {
            if (cs[i] == 10 || cs[i] == 11 || as[i] == 5) { if (chan <= 1) useRas[chan] = true; }
        }
    }
    uint32_t next = 0;
    ra.inputs = 0;
    for (uint32_t c = 0; c < 8; ++c)
        if (useCoord[c]) { ra.coord[c] = next; ra.semantic[next++] = c; }
    for (uint32_t c = 0; c < 2; ++c)
        if (useRas[c]) { ra.ras[c] = next; ra.semantic[next++] = 8 + c; }
    ra.inputs = next;
    if (!ra.inputs) { ra.semantic[0] = 8; ra.inputs = 1; next = 1; }  // a pixel shader imports at least one value
    for (uint32_t i = 0; i < numInd; ++i) { ra.ind[i] = next++; ra.icoord[i] = next++; }
    // A texture fetch may overwrite its coordinate's GPR when no later fetch
    // and no indirect stage (whose ALU runs after the fetches) reads it.
    auto coordDeadAfter = [&](uint32_t stage) {
        const uint32_t coord = (key.order[stage] >> 3) & 7;
        for (uint32_t s = 0; s < key.stages; ++s) {
            if (!((key.order[s] >> 6) & 1) || ((key.order[s] >> 3) & 7) != coord) continue;
            if (indStage(s) || s > stage) return false;
        }
        return true;
    };
    for (uint32_t stage = 0; stage < key.stages; ++stage) {
        if ((key.order[stage] >> 6) & 1) {
            if (!indStage(stage) && coordDeadAfter(stage)) ra.tex[stage] = ra.coord[(key.order[stage] >> 3) & 7];
            else ra.tex[stage] = next++;
        }
        if (indStage(stage)) {
            ra.scoord[stage] = next++;
            if ((key.ind[stage] >> 7) & 3) ra.bump[stage] = next++;
        }
    }
    if (anyInd) ra.prevTc = next++;
    // Registers: only those a stage writes (an unwritten one is read from
    // the uniforms), and 8-bit copies only where a value can exceed 0..255.
    tevAnalyze(key);
    bool anyMask = s_plan.outMask;
    for (uint32_t r = 0; r < 4; ++r) if (s_plan.regWritten[r]) ra.reg[r] = next++;
    for (uint32_t r = 0; r < 4; ++r) if (s_plan.maskUsed[r]) { ra.mask[r] = next++; anyMask = true; }
    ra.stage = next++;
    const uint32_t temps = (anyMask || s_plan.anyCompare || anyInd) ? 4 : 3;
    for (uint32_t i = 0; i < temps; ++i) ra.t[i] = next++;
    if (s_plan.anyCompare) ra.k[3] = next++;  // tevEmitCompareOp's constant copy
    ra.count = next;
    (void)anyFb;
    static const uint32_t kAll[4] = {0, 1, 2, 3};
    std::vector<TevIns> g;
    auto flush = [&]() { tevEmitGroup(program, g); g.clear(); };

    // ---- indirect: lookup coordinates, lookups, offsets, indirect stage fetches ----
    size_t prepOffset = program.size(), prepCount = 0, tex1Offset = 0, tex2Offset = 0, indOffset = 0, indCount = 0;
    uint32_t tex1Count = 0, tex2Count = 0;
    if (anyInd || numInd) {
        for (uint32_t i = 0; i < numInd; ++i) {
            const uint32_t coord = (key.iref >> (6 * i + 3)) & 7;
            const uint32_t sc = 15 + i / 2, base = (i & 1) * 2;
            g.push_back(tevOp(kOpMul, ra.icoord[i], _x, tevG(ra.coord[coord], _x), tevC(sc, base)));
            g.push_back(tevOp(kOpMul, ra.icoord[i], _y, tevG(ra.coord[coord], _y), tevC(sc, base + 1)));
            flush();
        }
        if (anyInd) {
            g.push_back(tevOp1(kOpMov, ra.prevTc, _x, {ALU_SRC_0, _x}));
            g.push_back(tevOp1(kOpMov, ra.prevTc, _y, {ALU_SRC_0, _x}));
            flush();
        }
        prepCount = program.size() - prepOffset;
    }
    tex1Offset = program.size();
    for (uint32_t i = 0; i < numInd; ++i) {
        const uint32_t map = (key.iref >> (6 * i)) & 7;
        uint64_t sample[2] = {TEX_SAMPLE(_R(ra.ind[i]), _x, _y, _z, _w, _R(ra.icoord[i]), _x, _y, _0, _x, map, map)};
        program.push_back(sample[0]); program.push_back(sample[1]);
        ++tex1Count;
    }
    for (uint32_t stage = 0; stage < key.stages; ++stage) {
        const uint32_t order = key.order[stage];
        if (!((order >> 6) & 1) || indStage(stage)) continue;
        const uint32_t map = order & 7, coord = (order >> 3) & 7;
        uint64_t sample[2] = {TEX_SAMPLE(_R(ra.tex[stage]), _x, _y, _z, _w, _R(ra.coord[coord]), _x, _y, _0, _x, map, map)};
        program.push_back(sample[0]); program.push_back(sample[1]);
        ++tex1Count;
    }
    if (anyInd) {
        indOffset = program.size();
        for (uint32_t i = 0; i < numInd; ++i) {
            for (uint32_t ch = 0; ch < 4; ++ch) g.push_back(tevOp(kOpMul, ra.ind[i], ch, tevG(ra.ind[i], ch), tevC(12, _w)));
            flush();
            for (uint32_t ch = 0; ch < 4; ++ch) g.push_back(tevOp1(kOpRndne, ra.ind[i], ch, tevG(ra.ind[i], ch)));
            flush();
        }
        for (uint32_t stage = 0; stage < key.stages; ++stage)
            if (indStage(stage)) tevEmitIndirectStage(program, key, stage);
        indCount = program.size() - indOffset;
        tex2Offset = program.size();
        for (uint32_t stage = 0; stage < key.stages; ++stage) {
            if (!indStage(stage)) continue;
            const uint32_t map = key.order[stage] & 7;
            uint64_t sample[2] = {TEX_SAMPLE(_R(ra.tex[stage]), _x, _y, _z, _w, _R(ra.scoord[stage]), _x, _y, _0, _x, map, map)};
            program.push_back(sample[0]); program.push_back(sample[1]);
            ++tex2Count;
        }
    }

    // ---- TEV ----
    const size_t aluOffset = program.size();
    // Texels and raster colours to 0..255 integers (genFS texi: round(clamp(c) * 255)).
    // The raster colours are interpolated from 0..255 / 255 vertex values, so
    // they are already within 0..1.
    std::vector<uint32_t> convert;
    for (uint32_t stage = 0; stage < key.stages; ++stage)
        if (ra.tex[stage] != 0xffffffffu) convert.push_back(ra.tex[stage]);
    for (uint32_t c = 0; c < 2; ++c)
        if (ra.ras[c] != 0xffffffffu) convert.push_back(ra.ras[c]);
    for (uint32_t r : convert) {
        for (uint32_t ch = 0; ch < 4; ++ch) g.push_back(tevOp(kOpMul, r, ch, tevG(r, ch), tevC(12, _w)));
        flush();
        for (uint32_t ch = 0; ch < 4; ++ch) g.push_back(tevOp1(kOpRndne, r, ch, tevG(r, ch)));
        flush();
    }
    uint32_t lastColor = 0, lastAlpha = 0;
    int texStage = -1, bumpStage = -1;
    for (uint32_t stage = 0; stage < key.stages; ++stage) {
        const uint32_t ce = key.colorEnv[stage], ae = key.alphaEnv[stage];
        if ((key.order[stage] >> 6) & 1) texStage = int(stage);
        if (indStage(stage) && ((key.ind[stage] >> 7) & 3)) bumpStage = int(stage);
        const TevStageCtx cx{&key, stage, texStage, bumpStage};
        const uint32_t colorDest = (ce >> 22) & 3, alphaDest = (ae >> 22) & 3;
        const uint32_t ca = (ce >> 12) & 15, cb = (ce >> 8) & 15, cc = (ce >> 4) & 15, cd = ce & 15;
        const uint32_t aa = (ae >> 13) & 7, ab = (ae >> 10) & 7, ac = (ae >> 7) & 7, ad = (ae >> 4) & 7;
        tevSetViews(stage);
        // 8-bit copies of the registers read as A, B or C that may hold more.
        for (uint32_t r = 0; r < 4; ++r) {
            if (!s_plan.mask[stage][r]) continue;
            TevSource x[4];
            for (uint32_t ch = 0; ch < 4; ++ch) x[ch] = tevG(ra.reg[r], ch);
            tevEmitMask8(program, ra.mask[r], kAll, 4, x);
        }
        const TevPart& pc = s_plan.part[stage][0];
        const TevPart& pa = s_plan.part[stage][1];
        TevChannel chs[4];
        for (uint32_t ch = 0; ch < 3; ++ch)
            chs[ch] = {ch, tevColorSource(cx, ca, ch, true), tevColorSource(cx, cb, ch, true),
                       tevColorSource(cx, cc, ch, true), tevColorSource(cx, cd, ch, false),
                       pc.bias, pc.op, pc.scale, pc.clamp};
        chs[3] = {_w, tevAlphaSource(cx, aa, true), tevAlphaSource(cx, ab, true),
                  tevAlphaSource(cx, ac, true), tevAlphaSource(cx, ad, false),
                  pa.bias, pa.op, pa.scale, pa.clamp};
        // Compare stages: the condition of an 8/16/24-bit compare reads
        // several components and is shared by the channels, so it is
        // computed first (alpha's into t0.w, then colour's into t0.x); the
        // per-component kinds compare in each channel's own steps.
        const TevCompareSrc colorCs{&cx, ca, cb, false};
        const TevCompareSrc alphaCs{&cx, aa, ab, true};
        const uint32_t cmode = (pc.scale << 1) | pc.op, amode = (pa.scale << 1) | pa.op;
        if (pa.compare && amode < 6) tevEmitTevCompare(program, _w, alphaCs, colorCs, amode, true);
        if (pc.compare && cmode < 6) tevEmitTevCompare(program, _x, colorCs, colorCs, cmode, false);
        // Each channel writes its destination register directly, all in the
        // last group.
        std::vector<TevStep> steps[4];
        for (uint32_t ch = 0; ch < 4; ++ch)
            tevChannelSteps(steps[ch], ch < 3 ? pc : pa, ch, chs[ch].a, chs[ch].b, chs[ch].c, chs[ch].d);
        size_t longest = 0;
        for (uint32_t ch = 0; ch < 4; ++ch) if (steps[ch].size() > longest) longest = steps[ch].size();
        for (size_t k = 0; k < longest; ++k) {
            for (uint32_t ch = 0; ch < 4; ++ch) {
                const size_t skip = longest - steps[ch].size();
                if (k < skip) continue;
                const TevStep& st = steps[ch][k - skip];
                const uint32_t to = st.dst == 3 ? ra.reg[ch < 3 ? colorDest : alphaDest] : ra.t[st.dst];
                g.push_back(tevOp(st.inst, to, ch, st.s0, st.s1, st.omod));
            }
            flush();
        }
        lastColor = colorDest;
        lastAlpha = alphaDest;
    }
    // The output keeps the low 8 bits of the last stage's registers (genFS:
    // outc = ivec4(...) & 255).
    uint32_t outColor = ra.reg[lastColor], outAlpha = ra.reg[lastAlpha];
    if (s_plan.outMask) {
        TevSource x[4];
        for (uint32_t ch = 0; ch < 3; ++ch) x[ch] = tevG(ra.reg[lastColor], ch);
        x[3] = tevG(ra.reg[lastAlpha], _w);
        tevEmitMask8(program, ra.stage, kAll, 4, x);
        outColor = outAlpha = ra.stage;
    }
    const size_t aluCount = program.size() - aluOffset;
    const size_t compareOffset = program.size();
    const bool killed = tevEmitAlphaCompare(program, key.alphaCompare, _R(outAlpha), _w);
    for (uint32_t ch = 0; ch < 4; ++ch)
        g.push_back(tevOp(kOpMul, ra.stage, ch, tevG(ch < 3 ? outColor : outAlpha, ch), tevC(13, _x)));
    flush();
    const size_t compareCount = program.size() - compareOffset;

    // ---- control flow ----
    size_t cf = 0;
    if (prepCount && !tevAluClauses(program, cf, prepOffset, prepCount)) return false;
    if (tex1Count) tevTexClauses(program, cf, tex1Offset, tex1Count);
    if (indCount && !tevAluClauses(program, cf, indOffset, indCount)) return false;
    if (tex2Count) tevTexClauses(program, cf, tex2Offset, tex2Count);
    if (!tevAluClauses(program, cf, aluOffset, aluCount)) return false;
    // The alpha compare (with its literal thresholds) is a few slots, one clause.
    if (compareCount > 128) return false;
    if (cf + 2 > kCfSlots) return false;
    program[cf++] = ALU(compareOffset, compareCount);
    uint64_t exportOp = EXP_DONE(PIX0, _R(ra.stage), _x, _y, _z, _w);
    if (killed) exportOp |= to_QWORD(0, to_LE(1u << 22));
    program[cf++] = exportOp END_OF_PROGRAM;
    const size_t programSize = program.size() * sizeof(uint64_t);
    shader.psProgram = alignedZero(GX2_SHADER_PROGRAM_ALIGNMENT, programSize);
    if (!shader.psProgram) return false;
    memcpy(shader.psProgram, program.data(), programSize);
    memset(&shader.pixel, 0, sizeof(shader.pixel));
    shader.pixel.regs.sq_pgm_resources_ps = ra.count;
    shader.pixel.regs.sq_pgm_exports_ps = 2;
    // Import only the interpolated values the key reads, packed from R0.
    shader.pixel.regs.spi_ps_in_control_0 = ra.inputs | (1u << 26) | (1u << 28);
    shader.pixel.regs.num_spi_ps_input_cntl = ra.inputs;
    for (uint32_t i = 0; i < ra.inputs; ++i) shader.pixel.regs.spi_ps_input_cntls[i] = ra.semantic[i] | 0x100;
    shader.pixel.regs.cb_shader_mask = 0xf;
    shader.pixel.regs.cb_shader_control = 1;
    shader.pixel.regs.db_shader_control = (1u << 4) | (1u << 6);
    shader.pixel.size = uint32_t(programSize);
    shader.pixel.program = shader.psProgram;
    shader.pixel.mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
    shader.uniform = {"tev_params", GX2_SHADER_VAR_TYPE_FLOAT4, kTevUniforms, 0, -1};
    shader.pixel.uniformVars = &shader.uniform;
    shader.pixel.uniformVarCount = 1;
    static const char* names[8] = {"uTex0", "uTex1", "uTex2", "uTex3", "uTex4", "uTex5", "uTex6", "uTex7"};
    for (uint32_t map = 0; map < 8; ++map)
        shader.samplers[map] = {names[map], GX2_SAMPLER_VAR_TYPE_SAMPLER_2D, map};
    shader.pixel.samplerVars = shader.samplers;
    shader.pixel.samplerVarCount = 8;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader.pixel.program, shader.pixel.size);
    return true;
}

static TevShader* tevShaderForKey(const TevKey& key) {
    for (TevShader* shader : s_tevShaders)
        if (!memcmp(&shader->key, &key, sizeof(key))) return shader;
    TevShader* shader = new TevShader();
    memset(shader, 0, sizeof(*shader));
    shader->key = key;
    shader->textureMask = tevTextureMask(key);
    if (!initTevPixel(*shader)) {
        delete shader;
        return nullptr;
    }
    s_tevShaders.push_back(shader);
    WHBLogPrintf("SMS Wii U: generated TEV pixel shader stages=%u tex=%02x gprs=%u groups=%u bytes=%u cache=%u",
                 (unsigned)key.stages, (unsigned)shader->textureMask, (unsigned)s_ra.count,
                 (unsigned)s_tevGroups, (unsigned)shader->pixel.size, (unsigned)s_tevShaders.size());
    return shader;
}

static void textureUvTransform(const TextureEntry* texture, float out[4]);
static void setTevShader(TevShader& shader, TextureEntry* const* textures, VertexProgram* vertex = nullptr) {
    if (vertex) {
        GX2SetFetchShader(&vertex->fetch);
        GX2SetVertexShader(&vertex->vertex);
    } else {
        GX2SetFetchShader(&s_tevFetch);
        GX2SetVertexShader(&s_tevVertex);
    }
    GX2SetPixelShader(&shader.pixel);
    // Integer TEV operands (see the constant-file layout above tevColorSource).
    float params[kTevUniforms * 4] = {};
    for (int i = 0; i < 4; ++i) {
        const uint32_t ra = g.bp[BP_TEV_REG + 2 * i], bg = g.bp[BP_TEV_REG + 2 * i + 1];
        params[4 * i + 0] = float(tevSext11(ra & 0x7ff));
        params[4 * i + 1] = float(tevSext11((bg >> 12) & 0x7ff));
        params[4 * i + 2] = float(tevSext11(bg & 0x7ff));
        params[4 * i + 3] = float(tevSext11((ra >> 12) & 0x7ff));
    }
    for (int i = 0; i < 4; ++i) {
        const uint32_t ra = g.kreg[2 * i], bg = g.kreg[2 * i + 1];
        params[16 + 4 * i + 0] = float(ra & 0xff);
        params[16 + 4 * i + 1] = float((bg >> 12) & 0xff);
        params[16 + 4 * i + 2] = float(bg & 0xff);
        params[16 + 4 * i + 3] = float((ra >> 12) & 0xff);
    }
    static const int k8[8] = {255, 223, 191, 159, 127, 95, 63, 31};
    for (int i = 0; i < 8; ++i) params[32 + i] = float(k8[i]);
    params[44] = 255.0f;   // C11: ONE, HALF, -, ZERO
    params[45] = 128.0f;
    params[47] = 0.0f;
    params[48] = 256.0f;   // C12
    params[49] = 1.0f / 256.0f;
    params[50] = 128.0f;
    params[51] = 255.0f;
    params[52] = 1.0f / 255.0f;  // C13
    params[53] = 1.0f / 128.0f;
    params[54] = -1024.0f;
    params[55] = 1023.0f;
    params[56] = -128.0f;  // C14
    // Indirect texturing (see tevEmitIndirect).
    // C15/C16: the indirect lookups' coordinate scales (1 / 2^ss), s,t per stage.
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t ss = (g.bp[i < 2 ? BP_RAS1_SS0 : BP_RAS1_SS1] >> ((i & 1) * 8)) & 0xff;
        params[60 + 2 * i] = 1.0f / float(1u << (ss & 15));
        params[61 + 2 * i] = 1.0f / float(1u << ((ss >> 4) & 15));
    }
    // C17-C22: the three matrices, rows of s.10 integers; row 0's w is the
    // exponent's power of two (0 once a shift of 24 or more leaves nothing of
    // the 24-bit offset); the shader rounds the product down, which is the
    // shift right of a negative exponent and exact for the others.
    for (int m = 0; m < 3; ++m) {
        const uint32_t r0 = g.bp[BP_IND_MTX + 3 * m], r1 = g.bp[BP_IND_MTX + 3 * m + 1], r2 = g.bp[BP_IND_MTX + 3 * m + 2];
        const int sc = int(((r0 >> 22) & 3) | ((r1 >> 22) & 3) << 2 | ((r2 >> 22) & 3) << 4) - 17;
        float* row0 = &params[68 + 8 * m];
        float* row1 = &params[72 + 8 * m];
        row0[0] = float(tevSext11(r0 & 0x7ff));
        row1[0] = float(tevSext11((r0 >> 11) & 0x7ff));
        row0[1] = float(tevSext11(r1 & 0x7ff));
        row1[1] = float(tevSext11((r1 >> 11) & 0x7ff));
        row0[2] = float(tevSext11(r2 & 0x7ff));
        row1[2] = float(tevSext11((r2 >> 11) & 0x7ff));
        row0[3] = sc >= 24 ? 0.0f : ldexpf(1.0f, sc);
        row1[3] = 0.0f;
    }
    // C23-C30: per texture map, (ox, oy, w / kx, h / ky) and C31-C38
    // (kx / w, ky / h, 0, 0): the shader's coordinate is ox + kx * u for the
    // map's normalized u (EFB copies sample a rectangle of a larger texture),
    // and w x h is the map's GX size.
    for (uint32_t map = 0; map < 8; ++map) {
        float tr[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        float w = 1.0f, h = 1.0f;
        if (textures[map]) {
            textureUvTransform(textures[map], tr);
            w = float(textures[map]->width ? textures[map]->width : 1);
            h = float(textures[map]->height ? textures[map]->height : 1);
        }
        params[92 + 4 * map + 0] = tr[0];
        params[92 + 4 * map + 1] = tr[1];
        params[92 + 4 * map + 2] = w / tr[2];
        params[92 + 4 * map + 3] = h / tr[3];
        params[124 + 4 * map + 0] = tr[2] / w;
        params[124 + 4 * map + 1] = tr[3] / h;
    }
    // C39-C42: wrap sizes and other indirect constants.
    static const float kIndConst[16] = {256, 128, 64, 32,  16, 1.0f / 256, 1.0f / 128, 1.0f / 64,
                                        1.0f / 32, 1.0f / 16, 16777216.0f, 1.0f / 16777216.0f,
                                        8388608.0f, 1.0f / 8, 8, 0};
    memcpy(&params[156], kIndConst, sizeof(kIndConst));
    // C43-C46: the TEV registers' low 8 bits (the A/B/C view of a register no
    // stage has written yet).
    for (int i = 0; i < 16; ++i) params[172 + i] = float(int(params[i]) & 255);
    GX2SetPixelUniformReg(0, kTevUniforms * 4, params);
    for (uint32_t map = 0; map < 8; ++map) {
        if (!(shader.textureMask & (1u << map)) || !textures[map]) continue;
        GX2SetPixelTexture(&textures[map]->texture, map);
        GX2SetPixelSampler(&textures[map]->sampler, map);
    }
}

static float s_tvW = 1280.0f, s_tvH = 720.0f, s_gameScale = 1.5f, s_gameX = 160.0f;

// EFB copies to textures (GXCopyTex): the game renders a pass, copies the
// frame into a texture and draws it back (water, heat haze, shadows). The
// copy lives in a GX2 texture keyed by its destination address; lookups of
// that address use it instead of decoding RAM the copy never wrote.
struct CopyEntry {
    const void* dest = nullptr;
    TextureEntry tex;
    uint32_t sourceX = 0, sourceY = 0, sourceW = 0, sourceH = 0;
};
static unsigned char s_copyStorage[sizeof(std::vector<CopyEntry>)] __attribute__((aligned(8)));
static std::vector<CopyEntry>* s_copyObject;
static std::vector<CopyEntry>& copyCache() {
    if (!s_copyObject) s_copyObject = new (s_copyStorage) std::vector<CopyEntry>();
    return *s_copyObject;
}
static TextureEntry* copyTexture(const void* source) {
    for (CopyEntry& c : copyCache())
        if (c.dest == source && c.tex.valid) return &c.tex;
    return nullptr;
}
static const CopyEntry* copyEntryForTexture(const TextureEntry* texture) {
    for (const CopyEntry& c : copyCache())
        if (&c.tex == texture && c.tex.valid) return &c;
    return nullptr;
}

static FILE* debugFile() {
    static FILE* f;
    if (!SMS_GX2_DEBUG_DUMP) return nullptr;
    if (!f) f = fopen("fs:/vol/external01/gx2dbg.txt", "w");
    return f;
}

static uint64_t debugXxh64(const void* data, size_t len) {
    static const uint64_t p1 = 0x9E3779B185EBCA87ull, p2 = 0xC2B2AE3D27D4EB4Full,
                          p3 = 0x165667B19E3779F9ull, p4 = 0x85EBCA77C2B2AE63ull,
                          p5 = 0x27D4EB2F165667C5ull;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;
    auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
    auto rd64 = [](const uint8_t* q) {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | q[i];
        return v;
    };
    auto rd32 = [](const uint8_t* q) {
        return uint32_t(q[0]) | (uint32_t(q[1]) << 8) | (uint32_t(q[2]) << 16) | (uint32_t(q[3]) << 24);
    };
    auto round = [&](uint64_t acc, uint64_t in) { return rotl(acc + in * p2, 31) * p1; };
    auto merge = [&](uint64_t acc, uint64_t v) { return (acc ^ round(0, v)) * p1 + p4; };
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = p1 + p2, v2 = p2, v3 = 0, v4 = -p1;
        do {
            v1 = round(v1, rd64(p)); v2 = round(v2, rd64(p + 8));
            v3 = round(v3, rd64(p + 16)); v4 = round(v4, rd64(p + 24)); p += 32;
        } while (end - p >= 32);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
    } else h = p5;
    h += uint64_t(len);
    while (end - p >= 8) { h ^= round(0, rd64(p)); h = rotl(h, 27) * p1 + p4; p += 8; }
    if (end - p >= 4) { h ^= uint64_t(rd32(p)) * p1; h = rotl(h, 23) * p2 + p3; p += 4; }
    while (p < end) { h ^= uint64_t(*p++) * p5; h = rotl(h, 11) * p1; }
    h ^= h >> 33; h *= p2; h ^= h >> 29; h *= p3; h ^= h >> 32;
    return h;
}

static uint64_t debugDecodedHash(const TextureEntry* texture) {
    if (!texture || !texture->valid || !texture->texture.surface.image) return 0;
    const uint32_t w = texture->width, h = texture->height;
    const uint32_t pitch = texture->texture.surface.pitch ? texture->texture.surface.pitch : w;
    const uint32_t n = std::min<uint32_t>(64, w * h);
    uint8_t first[64 * 4];
    const uint8_t* image = static_cast<const uint8_t*>(texture->texture.surface.image);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t y = i / w, x = i % w;
        const uint8_t* p = image + (size_t(y) * pitch + x) * 4;
        memcpy(first + i * 4, p, 4);
    }
    return debugXxh64(first, size_t(n) * 4);
}

static void debugTexture(FILE* f, int map, const TextureEntry* texture) {
    if (!texture) {
        fprintf(f, "m%d:none", map);
        return;
    }
    const CopyEntry* copy = copyEntryForTexture(texture);
    const uint8_t* source = texture->source;
    const uint32_t bytes = gx2TexLevelBytes(texture->fmt, texture->width, texture->height);
    const uint32_t addr = reinterpret_cast<uint32_t>(source);
    const bool readable = source && bytes && addr + bytes > addr &&
        ((addr >= 0x10000000u && addr + bytes <= 0x50000000u) ||
         (addr >= 0xF4000000u && addr + bytes <= 0xF6000000u));
    fprintf(f, "m%d:f%u:%ux%u:src=%p:h=%016llx:tlut=%u/%u:%s", map, (unsigned)texture->fmt,
            (unsigned)texture->width, (unsigned)texture->height, (const void*)source,
            (unsigned long long)debugDecodedHash(texture), (unsigned)texture->tlut, (unsigned)texture->tlutFmt,
            copy ? "efb" : readable ? "ram" : "fallback");
    if (copy)
        fprintf(f, ":copydst=%p:%u,%u:%ux%u", copy->dest, (unsigned)copy->sourceX, (unsigned)copy->sourceY,
                (unsigned)copy->sourceW, (unsigned)copy->sourceH);
}

static bool evalTev(const HostVertex& in, float* rgba, bool* usesTex);

static void debugDraw(const TevKey& key, uint32_t draw, bool tevPath, int textureMap, TextureEntry* texture,
                      TextureEntry* const* tevTextures, uint32_t nverts, const HostVertex* first,
                      const float* firstOut) {
    FILE* f = debugFile();
    if (!f || draw >= 5000) return;
#ifdef SMS_GX2_DEBUG_DUMP_EVERY
    if (g_displayFrames % SMS_GX2_DEBUG_DUMP_EVERY) return;
#endif
    fprintf(f, "DIFF renderer=gx2 frame=%u draw=%u tex=", (unsigned)g_displayFrames, (unsigned)draw);
    const uint32_t mask = tevTextureMask(key);
    bool firstTex = true;
    for (int map = 0; map < 8; ++map) {
        if (!(mask & (1u << map))) continue;
        if (!tevPath && !tevTextures[map] && map != textureMap) continue;
        if (!firstTex) fputc(';', f);
        firstTex = false;
        debugTexture(f, map, tevTextures[map] ? tevTextures[map] : texture);
    }
    if (firstTex) fputs("none", f);
    fprintf(f, " tev=gen%08x/st%u", (unsigned)g.bp[BP_GENMODE], (unsigned)key.stages);
    for (uint32_t s = 0; s < key.stages; ++s)
        fprintf(f, "/%03x:%06x:%06x", (unsigned)key.order[s], (unsigned)key.colorEnv[s], (unsigned)key.alphaEnv[s]);
    fprintf(f, " blend=%08x ac=%08x z=%08x verts=%u shader=%s", (unsigned)g.bp[BP_CMODE0],
            (unsigned)g.bp[BP_ALPHACOMPARE], (unsigned)g.bp[BP_ZMODE], (unsigned)nverts,
            tevPath ? "per-tev" : "evalTev");
    if (first) {
        float eval[4] = {};
        evalTev(*first, eval, nullptr);
        const float* m = reinterpret_cast<const float*>(&g.xfMem[(first->mtx[0] & 63) * 4]);
        const float* p = reinterpret_cast<const float*>(&g.xfReg[XFR_PROJ]);
        fprintf(f, " first=pos(%.6g,%.6g,%.6g):mtx=%u:m(%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g):proj(%.6g,%.6g,%.6g,%.6g,%.6g,%.6g):col(%u,%u,%u,%u):uv(%.6g,%.6g):eval(%.6g,%.6g,%.6g,%.6g):outuv(%.6g,%.6g):clip(%.6g,%.6g,%.6g,%.6g)",
                first->pos[0], first->pos[1], first->pos[2],
                (unsigned)(first->mtx[0] & 63), m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11],
                p[0],p[1],p[2],p[3],p[4],p[5], first->clr[0][0], first->clr[0][1], first->clr[0][2],
                first->clr[0][3], first->tex[0][0], first->tex[0][1], eval[0], eval[1], eval[2], eval[3],
                tevPath ? firstOut[4] : firstOut[4], tevPath ? firstOut[5] : firstOut[5], firstOut[0], firstOut[1], firstOut[2], firstOut[3]);
    }
    fprintf(f, " efb=none\n");
    fflush(f);
}
static void debugCopy(bool disp, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* dest, uint32_t ctrl) {
    FILE* f = debugFile();
    if (!f) return;
    fprintf(f, "DIFF renderer=gx2 copy=%s src=%u,%u:%ux%u dst=%p fmt=%u ctrl=%08x\n", disp ? "display" : "texture",
            (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h, dest,
            unsigned(((ctrl >> 3) & 1) << 3 | ((ctrl >> 4) & 7)), (unsigned)ctrl);
    fflush(f);
}
// The shader coordinate of texture's normalized (u, v) is (ox + kx u, oy + ky v).
static void textureUvTransform(const TextureEntry* texture, float out[4]) {
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 1.0f; out[3] = 1.0f;
    const CopyEntry* e = copyEntryForTexture(texture);
    if (!e || !e->sourceW || !e->sourceH) return;
    out[0] = float(e->sourceX) / s_tvW;
    out[1] = float(e->sourceY) / s_tvH;
    out[2] = float(e->sourceW) / s_tvW;
    out[3] = float(e->sourceH) / s_tvH;
}
static void adjustCopyUv(const TextureEntry* texture, float* u, float* v) {
    const CopyEntry* e = copyEntryForTexture(texture);
    if (!e || !e->sourceW || !e->sourceH) return;
    *u = (float(e->sourceX) + *u * float(e->sourceW)) / s_tvW;
    *v = (float(e->sourceY) + *v * float(e->sourceH)) / s_tvH;
}
extern "C" const int sms_wiiu_efb_full_copy;  // gx_stub.cpp: 1 = full-surface copies on hardware too
static bool runningUnderCemu() {
    static int result = -1;
    if (result < 0) result = sms_wiiu_efb_full_copy || (OSGetTitleID() >> 32) == 0xffffffffu;
    return result != 0;
}
static uint32_t s_perfCopies, s_perfPeeks, s_perfClears;
static void copyToTexture(const void* dest, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool half, uint32_t fmt) {
    ++s_perfCopies;
    GX2ColorBuffer* cb = renderColour();
    if (!dest || !cb || !w || !h) return;
    const uint32_t tx = uint32_t(s_gameX + float(x) * s_gameScale), ty = uint32_t(float(y) * s_gameScale);
    uint32_t tw = uint32_t(float(w) * s_gameScale), th = uint32_t(float(h) * s_gameScale);
    if (tx + tw > cb->surface.width) tw = cb->surface.width - tx;
    if (ty + th > cb->surface.height) th = cb->surface.height - ty;
    if (!tw || !th) return;
    CopyEntry* e = nullptr;
    for (CopyEntry& c : copyCache())
        if (c.dest == dest) { e = &c; break; }
    const bool cemu = runningUnderCemu();
    // Cemu's GX2CopySurfaceEx implementation only handles a full-surface
    // copy. Keep the compatibility path's backing texture the size of the
    // TV target, copy that surface in one operation, and apply the source
    // rectangle in the texture coordinates sent to the shader below.
    const uint32_t fullW = cb->surface.width, fullH = cb->surface.height;
    const uint32_t logicalW = half ? (w + 1) / 2 : w, logicalH = half ? (h + 1) / 2 : h;
    const uint32_t copyW = cemu ? fullW : tw, copyH = cemu ? fullH : th;
    if (e && (e->tex.texture.surface.width != copyW || e->tex.texture.surface.height != copyH || e->tex.fmt != fmt)) {
        GX2DrawDone(); // earlier draws this frame may still sample it
        MEMFreeToDefaultHeap(e->tex.texture.surface.image);
        e->tex.valid = false;
    }
    if (!e) { copyCache().push_back(CopyEntry()); e = &copyCache().back(); e->dest = dest; }
    e->tex.source = static_cast<const uint8_t*>(dest);
    e->tex.fmt = fmt;
    e->tex.width = cemu ? logicalW : copyW;
    e->tex.height = cemu ? logicalH : copyH;
    e->sourceX = cemu ? tx : 0; e->sourceY = cemu ? ty : 0;
    e->sourceW = cemu ? tw : 0; e->sourceH = cemu ? th : 0;
    if (!e->tex.valid) {
        TextureEntry& t = e->tex;
        memset(&t.texture, 0, sizeof(t.texture));
        t.width = logicalW; t.height = logicalH;
        t.texture.surface.use = GX2_SURFACE_USE_TEXTURE;
        t.texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        t.texture.surface.width = copyW;
        t.texture.surface.height = copyH;
        t.texture.surface.depth = 1;
        t.texture.surface.mipLevels = 1;
        // GXCopyTex converts the EFB to the requested GX texture format. In
        // particular, RGB565 has an implicit alpha of one; retaining the TV
        // RGBA8 surface here leaks the EFB's alpha into the title's screen
        // blend and washes out the logo.
        t.texture.surface.format = fmt == 4 ? GX2_SURFACE_FORMAT_UNORM_R5_G6_B5 : cb->surface.format;
        t.texture.surface.aa = GX2_AA_MODE1X;
        // LINEAR_SPECIAL makes Cemu complete the color-buffer copy through
        // its synchronous CPU path; the texture coordinates below still
        // select the logical EFB rectangle from this full-size backing store.
        t.texture.surface.tileMode = GX2_TILE_MODE_DEFAULT;
        t.texture.viewNumMips = 1;
        t.texture.viewNumSlices = 1;
        t.texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
        GX2CalcSurfaceSizeAndAlignment(&t.texture.surface);
        t.texture.surface.image = alignedZero(t.texture.surface.alignment, t.texture.surface.imageSize);
        if (!t.texture.surface.image) return;
        GX2InitTextureRegs(&t.texture);
        t.valid = true;
        static unsigned s_copyEntryLogs;
        if (s_copyEntryLogs++ < 24)
            WHBLogPrintf("[efb] copy entry %s src=%u,%u %ux%u dst=%ux%u fmt=%u tile=%u pitch=%u bytes=%u img=%p",
                         cemu ? "full" : "rect", unsigned(tx), unsigned(ty), unsigned(tw), unsigned(th),
                         unsigned(copyW), unsigned(copyH), unsigned(t.texture.surface.format),
                         unsigned(t.texture.surface.tileMode), unsigned(t.texture.surface.pitch),
                         unsigned(t.texture.surface.imageSize), t.texture.surface.image);
    }
    // The copy's texture format picks the EFB channels the texture holds
    // (genFS's copy shader on the PC): R8 is an intensity of red, RA8 red and
    // alpha, A8 alpha, ... The copy keeps the EFB's RGBA and the texture's
    // component selectors read it as the format would; an EFB without alpha
    // reads alpha as one.
    const uint32_t a = (g.bp[BP_PE_CONTROL] & 7) == 1 ? GX2_SQ_SEL_A : GX2_SQ_SEL_1;
    const uint32_t R = GX2_SQ_SEL_R, G = GX2_SQ_SEL_G, B = GX2_SQ_SEL_B;
    uint32_t compMap;
    switch (fmt) {
    case 0: case 8: compMap = GX2_COMP_MAP(R, R, R, R); break;                          // R4, R8
    case 2: case 3: compMap = GX2_COMP_MAP(R, R, R, a); break;                          // RA4, RA8
    case 4: compMap = GX2_COMP_MAP(R, G, B, GX2_SQ_SEL_1); break;                       // RGB565
    case 7: compMap = GX2_COMP_MAP(a, a, a, a); break;                                  // A8
    case 9: compMap = GX2_COMP_MAP(G, G, G, G); break;                                  // G8
    case 10: compMap = GX2_COMP_MAP(B, B, B, B); break;                                 // B8
    case 11: compMap = GX2_COMP_MAP(G, G, G, R); break;                                 // RG8 (IA8 layout)
    case 12: compMap = GX2_COMP_MAP(B, B, B, G); break;                                 // GB8 (IA8 layout)
    default: compMap = GX2_COMP_MAP(R, G, B, a); break;                                 // RGBA8, RGB5A3
    }
    if (e->tex.texture.compMap != compMap) {
        e->tex.texture.compMap = compMap;
        GX2InitTextureRegs(&e->tex.texture);
    }
    if (cemu) {
        GX2CopySurface(&cb->surface, 0, 0, &e->tex.texture.surface, 0, 0); invalidateVertexUniforms();
    } else {
        GX2Rect source = {int32_t(tx), int32_t(ty), int32_t(tx + tw), int32_t(ty + th)};
        GX2Point destination = {0, 0};
        GX2CopySurfaceEx(&cb->surface, 0, 0, &e->tex.texture.surface, 0, 0, 1,
                        &source, &destination);
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, e->tex.texture.surface.image,
                  e->tex.texture.surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, e->tex.texture.surface.image,
                  e->tex.texture.surface.imageSize);
}

// Mip levels present: GX has no explicit count, so it follows the max LOD of
// a mipmapping filter, as the PC renderer derives it.
static uint32_t textureLevels(uint32_t mode0, uint32_t mode1, uint32_t w, uint32_t h) {
    uint32_t levels = 1;
    if (((mode0 >> 5) & 3) != 0) {
        const uint32_t maxLod = ((mode1 >> 8) & 0xff) / 16;
        const uint32_t m = w > h ? w : h;
        while (levels <= maxLod && (m >> levels) >= 1) levels++;
        if (levels > 11) levels = 11;
    }
    return levels;
}

// How far a decoded level is from a 2x2 box reduction of the level above it
// (mean difference per channel, 0..255). Textures whose smaller levels are an
// effect rather than reductions (the sea's sparkle map: a dark level 0 with
// sparse sparkles, brighter pictures below) must sample the GameCube's level
// of detail, which at the TV's resolution means a bias of log2(scale); the
// PC renderer detects them the same way (kArbitraryMipThreshold).
static float mipLevelDiff(const uint8_t* prev, uint32_t pw, uint32_t ph, const uint8_t* cur, uint32_t w, uint32_t h) {
    uint64_t sum = 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t y0 = std::min(2 * y, ph - 1), y1 = std::min(2 * y + 1, ph - 1);
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t x0 = std::min(2 * x, pw - 1), x1 = std::min(2 * x + 1, pw - 1);
            const uint8_t* a = prev + (size_t(y0) * pw + x0) * 4;
            const uint8_t* b = prev + (size_t(y0) * pw + x1) * 4;
            const uint8_t* c = prev + (size_t(y1) * pw + x0) * 4;
            const uint8_t* d = prev + (size_t(y1) * pw + x1) * 4;
            const uint8_t* o = cur + (size_t(y) * w + x) * 4;
            for (int i = 0; i < 4; i++) {
                const int diff = (a[i] + b[i] + c[i] + d[i] + 2) / 4 - int(o[i]);
                sum += uint32_t(diff < 0 ? -diff : diff);
            }
        }
    }
    return float(double(sum) / (double(w) * h * 4));
}
static const float kArbitraryMipThreshold = 14.0f;

// Sampling LINEAR_ALIGNED textures is slow on the Wii U GPU (file select spent
// ~175 ms a frame waiting for it). The decoded linear surface becomes the
// staging copy: GX2CopySurface retiles every level into a GX2_TILE_MODE_DEFAULT
// texture, and the staging memory is freed after this frame's fence. Only runs
// inside a frame (flushBatch), before applyState/setTevShader re-apply state.
static void rebindTvTarget();
static bool s_retileTextures = true;
static void retileTexture(GX2Texture& texture) {
    if (!s_retileTextures || runningUnderCemu()) return;
    GX2Surface& linear = texture.surface;
    GX2Texture tiled = texture;
    GX2Surface& t = tiled.surface;
    t.tileMode = GX2_TILE_MODE_DEFAULT;
    t.swizzle = 0;
    t.image = nullptr;
    t.mipmaps = nullptr;
    GX2CalcSurfaceSizeAndAlignment(&t);
    t.image = alignedZero(t.alignment, t.imageSize);
    if (!t.image) return;
    if (t.mipLevels > 1) {
        t.mipmaps = alignedZero(t.alignment, t.mipmapSize);
        if (!t.mipmaps) { MEMFreeToDefaultHeap(t.image); return; }
    }
    // The zero fill must not be written back over the GPU's copy later.
    DCFlushRange(t.image, t.imageSize);
    if (t.mipmaps) DCFlushRange(t.mipmaps, t.mipmapSize);
    GX2InitTextureRegs(&tiled);
    for (uint32_t level = 0; level < t.mipLevels; ++level)
        GX2CopySurface(&linear, level, 0, &t, level, 0); invalidateVertexUniforms();
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, t.image, t.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, t.image, t.imageSize);
    if (t.mipmaps) {
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, t.mipmaps, t.mipmapSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, t.mipmaps, t.mipmapSize);
    }
    rebindTvTarget();  // GX2CopySurface leaves its destination bound
    deferFree(linear.image);
    deferFree(linear.mipmaps);
    texture = tiled;
}

static TextureEntry* getTexture(int map) {
    SMS_PORT_HEAP_SCOPE();
    const uint32_t mode0 = g.bp[bpTexReg(BP_TX_MODE0, map)];
    const uint32_t mode1 = g.bp[bpTexReg(BP_TX_MODE1, map)];
    const uint32_t image0 = g.bp[bpTexReg(BP_TX_IMAGE0, map)];
    const uint32_t tlutReg = g.bp[bpTexReg(BP_TX_TLUT, map)];
    const uint32_t w = (image0 & 0x3ff) + 1;
    const uint32_t h = ((image0 >> 10) & 0x3ff) + 1;
    const uint32_t fmt = (image0 >> 20) & 15;
    const bool indexed = fmt >= 8 && fmt <= 10;
    const uint32_t tlut = indexed ? (tlutReg & 0x3ff) : 0;
    const uint32_t tlutFmt = indexed ? ((tlutReg >> 10) & 3) : 0;
    const uint8_t* source = g.texImage[map];
    if (TextureEntry* c = copyTexture(source)) {
        initSampler(&c->sampler, mode0, mode1, 1, false);
        return c;
    }
    const uint32_t levels = textureLevels(mode0, mode1, w, h);
    for (size_t i = 0; i < s_textures.size(); ++i) {
        TextureEntry& e = s_textures[i];
        if (e.valid && e.source == source && e.fmt == fmt && e.width == w && e.height == h &&
            e.tlut == tlut && e.tlutFmt == tlutFmt && e.levels == levels) {
            // The sampler state belongs to the texmap, not the image: the same
            // texture may be bound with other wrap or filter modes.
            initSampler(&e.sampler, mode0, mode1, e.texture.surface.mipLevels, e.arbitraryMips);
            return &e;
        }
    }
    s_textures.push_back(TextureEntry());
    TextureEntry& e = s_textures.back();
    e.source = source; e.fmt = fmt; e.width = w; e.height = h; e.tlut = tlut; e.tlutFmt = tlutFmt;
    e.levels = levels;
    e.texture.surface.use = GX2_SURFACE_USE_TEXTURE;
    e.texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    e.texture.surface.width = w;
    e.texture.surface.height = h;
    e.texture.surface.depth = 1;
    e.texture.surface.mipLevels = levels;
    e.texture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    e.texture.surface.aa = GX2_AA_MODE1X;
    e.texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&e.texture.surface);
    // Level l (l >= 1) is at mipPtr + offset, offset 0 for level 1 and
    // mipLevelOffset[l - 1] after it; a LINEAR_ALIGNED 32-bit level's pitch is
    // its width aligned to 64 texels. Keep only level 0 if GX2's layout does
    // not agree with that.
    GX2Surface& surf = e.texture.surface;
    uint32_t mipOffset[12] = {};
    uint32_t mipPitch[12] = {};
    mipPitch[0] = surf.pitch;
    if (surf.mipLevels > 1) {
        bool agrees = surf.pitch == ((w + 63) & ~63u);
        uint32_t end = 0;
        for (uint32_t l = 1; l < surf.mipLevels && agrees; ++l) {
            const uint32_t lw = std::max(w >> l, 1u), lh = std::max(h >> l, 1u);
            mipPitch[l] = (lw + 63) & ~63u;
            mipOffset[l] = l == 1 ? 0 : surf.mipLevelOffset[l - 1];
            if (mipOffset[l] < end) agrees = false;
            end = mipOffset[l] + mipPitch[l] * lh * 4;
        }
        if (agrees && end > surf.mipmapSize) agrees = false;
        if (!agrees) {
            static bool logged;
            if (!logged) {
                logged = true;
                WHBLogPrintf("SMS Wii U: unexpected GX2 mip layout %ux%u levels=%u pitch=%u mipSize=%u off1=%u",
                             (unsigned)w, (unsigned)h, (unsigned)surf.mipLevels, (unsigned)surf.pitch,
                             (unsigned)surf.mipmapSize, (unsigned)surf.mipLevelOffset[1]);
            }
            surf.mipLevels = 1;
            GX2CalcSurfaceSizeAndAlignment(&surf);
        }
    }
    e.texture.viewNumMips = surf.mipLevels;
    e.texture.viewNumSlices = 1;
    e.texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
    surf.image = alignedZero(surf.alignment, surf.imageSize);
    if (!surf.image) return nullptr;
    if (surf.mipLevels > 1) {
        surf.mipmaps = alignedZero(surf.alignment, surf.mipmapSize);
        if (!surf.mipmaps) {
            MEMFreeToDefaultHeap(surf.image);
            surf.image = nullptr;
            return nullptr;
        }
    }
    GX2InitTextureRegs(&e.texture);

    // Decode through explicitly checked buffers: a failed allocation returns
    // null here (no exceptions), and the decoder then wrote megabytes from
    // address 0 and corrupted newlib's heap.
    uint32_t srcBytes = 0;
    for (uint32_t l = 0; l < surf.mipLevels; ++l)
        srcBytes += gx2TexLevelBytes(fmt, std::max(w >> l, 1u), std::max(h >> l, 1u));
    // A texture address that did not survive the 30-bit GX physical address
    // (outside the main heap) must not be read: in Cemu that kills the title.
    // OSIsAddressValid rejects ordinary heap addresses under Cemu (every
    // texture came out white), so check the app's MEM2/MEM1 ranges directly.
    const uint32_t a0 = reinterpret_cast<uint32_t>(source), a1 = a0 + srcBytes;
    const bool readable = source && srcBytes && a1 > a0 &&
        ((a0 >= 0x10000000u && a1 <= 0x50000000u) || (a0 >= 0xF4000000u && a1 <= 0xF6000000u));
    uint8_t* rgba = readable ? static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(w * h * 4, 64)) : nullptr;
    uint8_t* prev = (readable && surf.mipLevels > 1)
        ? static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(w * h * 4, 64)) : nullptr;
    if (!rgba || (surf.mipLevels > 1 && !prev)) {
        memset(surf.image, 0xff, surf.imageSize);
        if (surf.mipmaps) memset(surf.mipmaps, 0xff, surf.mipmapSize);
    } else {
        const uint8_t* tlutPtr = indexed ? g.tlutMem + (tlut << 9) : nullptr;
        const uint8_t* src = source;
        float mipDiff = 0.0f;
        uint32_t pw = w, ph = h;
        for (uint32_t l = 0; l < surf.mipLevels; ++l) {
            const uint32_t lw = std::max(w >> l, 1u), lh = std::max(h >> l, 1u);
            gx2DecodeTexture(src, fmt, lw, lh, tlutPtr, tlutFmt, rgba);
            uint8_t* dst = l == 0 ? static_cast<uint8_t*>(surf.image)
                                  : static_cast<uint8_t*>(surf.mipmaps) + mipOffset[l];
            for (uint32_t y = 0; y < lh; ++y)
                memcpy(dst + size_t(y) * mipPitch[l] * 4, rgba + size_t(y) * lw * 4, size_t(lw) * 4);
            if (l > 0) mipDiff += mipLevelDiff(prev, pw, ph, rgba, lw, lh);
            if (prev) std::swap(prev, rgba);
            pw = lw; ph = lh;
            src += gx2TexLevelBytes(fmt, lw, lh);
        }
        e.arbitraryMips = surf.mipLevels > 1 && mipDiff / float(surf.mipLevels - 1) > kArbitraryMipThreshold;
    }
    if (rgba) MEMFreeToDefaultHeap(rgba);
    if (prev) MEMFreeToDefaultHeap(prev);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, surf.image, surf.imageSize);
    if (surf.mipmaps) GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, surf.mipmaps, surf.mipmapSize);
    retileTexture(e.texture);
    initSampler(&e.sampler, mode0, mode1, surf.mipLevels, e.arbitraryMips);
    e.valid = true;
    return &e;
}

static void setShader(FixedShader& shader, TextureEntry* texture, VertexProgram* vertex = nullptr) {
    if (vertex) {
        GX2SetFetchShader(&vertex->fetch);
        GX2SetVertexShader(&vertex->vertex);
    } else {
        GX2SetFetchShader(&shader.fetch);
        GX2SetVertexShader(&shader.vertex);
    }
    GX2SetPixelShader(&shader.pixel);
    static const float windowParams[4] = {1, 0, 0, 0};
    GX2SetPixelUniformReg(shader.pixel.uniformVars[0].offset, 4, windowParams);
    if (shader.textured && texture) {
        GX2SetPixelTexture(&texture->texture, 0);
        GX2SetPixelSampler(&texture->sampler, 0);
    }
}

// The game renders into an 854x480 target (the GamePad's size; the 640x480
// EFB at 1:1) instead of WHBGfx's 1080p TV buffer: the generated TEV shaders
// made file select fill-bound (175 ms a frame at 1080p, 37 ms at 640x480).
// The surfaces alias WHBGfx's TV colour/depth memory (MEM1), which nothing
// else draws into any more; present copies (and scales) the target to the TV
// and GamePad scan buffers.
static constexpr bool kNativeRender = true;
// Render size is a setting: SD marker res1080 -> 1920x1080, res720 ->
// 1280x720, otherwise 854x480.  The game's 4:3 picture is scaled to the
// target height and centred.
static uint32_t kRenderW = 854, kRenderH = 480;
static GX2ColorBuffer s_renderColour;
static GX2DepthBuffer s_renderDepth;
static bool s_renderOwn, s_renderTried;

static void initRenderTarget() {
    if (s_renderTried) return;
    s_renderTried = true;
    if (!kNativeRender) return;
    if (sdMarkerPresent("res1080")) { kRenderW = 1920; kRenderH = 1080; }
    else if (sdMarkerPresent("res720")) { kRenderW = 1280; kRenderH = 720; }
    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    GX2DepthBuffer* td = WHBGfxGetTVDepthBuffer();
    if (!tv || !td || !tv->surface.image || !td->surface.image) return;
    s_renderColour = *tv;
    GX2Surface& c = s_renderColour.surface;
    c.width = kRenderW; c.height = kRenderH;
    c.image = nullptr; c.mipmaps = nullptr; c.swizzle = 0;
    GX2CalcSurfaceSizeAndAlignment(&c);
    s_renderDepth = *td;
    GX2Surface& d = s_renderDepth.surface;
    d.width = kRenderW; d.height = kRenderH;
    d.image = nullptr; d.mipmaps = nullptr; d.swizzle = 0;
    GX2CalcSurfaceSizeAndAlignment(&d);
    const bool fits = c.imageSize <= tv->surface.imageSize && d.imageSize <= td->surface.imageSize &&
                      (reinterpret_cast<uintptr_t>(tv->surface.image) % c.alignment) == 0 &&
                      (reinterpret_cast<uintptr_t>(td->surface.image) % d.alignment) == 0;
    WHBLogPrintf("SMS Wii U: native render target %ux%u colour=%u/%u depth=%u/%u -> %s", (unsigned)kRenderW,
                 (unsigned)kRenderH, (unsigned)c.imageSize, (unsigned)tv->surface.imageSize, (unsigned)d.imageSize,
                 (unsigned)td->surface.imageSize, fits ? "on" : "OFF (does not fit)");
    if (!fits) return;
    c.image = tv->surface.image;
    d.image = td->surface.image;
    s_renderDepth.hiZPtr = nullptr;
    GX2InitColorBufferRegs(&s_renderColour);
    GX2InitDepthBufferRegs(&s_renderDepth);
    s_renderOwn = true;
}

static GX2ColorBuffer* renderColour() {
    initRenderTarget();
    return s_renderOwn ? &s_renderColour : WHBGfxGetTVColourBuffer();
}
static GX2DepthBuffer* renderDepth() {
    initRenderTarget();
    return s_renderOwn ? &s_renderDepth : WHBGfxGetTVDepthBuffer();
}

// Frame start (instead of WHBGfxBeginRenderTV + clear) and present.
extern "C" void sms_wiiu_gx2_target_begin(void) {
    initRenderTarget();
    if (!s_renderOwn) {
        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.02f, 0.18f, 0.03f, 1.0f);
        return;
    }
    GX2SetContextState(WHBGfxGetTVContextState()); invalidateVertexUniforms();
    GX2ClearColor(&s_renderColour, 0.0f, 0.0f, 0.0f, 1.0f); invalidateVertexUniforms();
    GX2SetContextState(WHBGfxGetTVContextState()); invalidateVertexUniforms();
}
extern "C" void sms_wiiu_gx2_target_present(void) {
    GX2CopyColorBufferToScanBuffer(renderColour(), GX2_SCAN_TARGET_TV); invalidateVertexUniforms();
    GX2CopyColorBufferToScanBuffer(renderColour(), GX2_SCAN_TARGET_DRC); invalidateVertexUniforms();
}

// TV target size (WHBGfx picks 720p or 1080p) and the 4:3 game rectangle in it:
// the 640x480 EFB scaled to the target's height and centred.
static void updateTarget() {
    const GX2ColorBuffer* cb = renderColour();
    if (cb && cb->surface.width && cb->surface.height) {
        s_tvW = float(cb->surface.width);
        s_tvH = float(cb->surface.height);
    }
    s_gameScale = s_tvH / 480.0f;
    s_lodScale = s_gameScale;
    s_gameX = (s_tvW - 640.0f * s_gameScale) * 0.5f;
}

static void applyState() {
    const uint32_t zmode = g.bp[BP_ZMODE];
    const bool ztest = (zmode & 1) != 0;
    const bool zwrite = (zmode & 16) != 0;
    // GX_CLIP_ENABLE is zero in XF register 5.  The PC backend uses this
    // same per-draw bit to select near/far clipping or depth clamping.  XF
    // writes flush the queued batch before changing the register, so this is
    // safe to apply once for every submitted draw.
    const bool zclip = g.xfReg[0x05] == 0;
    GX2SetRasterizerClipControlHalfZ(TRUE, zclip, TRUE);
    // GX blend state (BP_CMODE0), as the PC renderer decodes it. Opaque 3D
    // materials have blending off and often alpha 0: always blending by source
    // alpha made the whole stage invisible. The EFB has no alpha channel, so
    // destination alpha reads as 1.
    const uint32_t cm = g.bp[BP_CMODE0];
    // Pixel format 1 (RGBA6_Z24) keeps destination alpha: Sunshine's shadow
    // volumes count into it with alpha-only passes and blend by it later.
    // The other formats read destination alpha as 1.
    const bool hasAlpha = (g.bp[BP_PE_CONTROL] & 7) == 1;
    static const GX2BlendMode kSrc[8] = {GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_DST_COLOR,
                                         GX2_BLEND_MODE_INV_DST_COLOR, GX2_BLEND_MODE_SRC_ALPHA,
                                         GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_DST_ALPHA,
                                         GX2_BLEND_MODE_INV_DST_ALPHA};
    static const GX2BlendMode kDst[8] = {GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_SRC_COLOR,
                                         GX2_BLEND_MODE_INV_SRC_COLOR, GX2_BLEND_MODE_SRC_ALPHA,
                                         GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_DST_ALPHA,
                                         GX2_BLEND_MODE_INV_DST_ALPHA};
    auto noDstAlpha = [&](GX2BlendMode m) {
        if (hasAlpha) return m;
        return m == GX2_BLEND_MODE_DST_ALPHA ? GX2_BLEND_MODE_ONE
             : m == GX2_BLEND_MODE_INV_DST_ALPHA ? GX2_BLEND_MODE_ZERO : m;
    };
    const bool blend = (cm & 1) != 0;
    // CMODE1 bit 8 (GX_SetDstAlpha): the alpha written to the target is the
    // constant in its low byte instead of the fragment's alpha.
    const uint32_t cm1 = g.bp[BP_CMODE1];
    const bool dstAlphaConst = ((cm1 >> 8) & 1) != 0;
    GX2BlendMode csrc = GX2_BLEND_MODE_ONE, cdst = GX2_BLEND_MODE_ZERO;
    GX2BlendCombineMode ccomb = GX2_BLEND_COMBINE_MODE_ADD;
    if (blend && ((cm >> 11) & 1)) {
        csrc = GX2_BLEND_MODE_ONE; cdst = GX2_BLEND_MODE_ONE; ccomb = GX2_BLEND_COMBINE_MODE_REV_SUB;
    } else if (blend) {
        csrc = noDstAlpha(kSrc[(cm >> 8) & 7]); cdst = noDstAlpha(kDst[(cm >> 5) & 7]);
    }
    if (dstAlphaConst) {
        GX2SetBlendConstantColor(0.0f, 0.0f, 0.0f, float(cm1 & 0xff) / 255.0f);
        GX2SetBlendControl(GX2_RENDER_TARGET_0, csrc, cdst, ccomb, TRUE, GX2_BLEND_MODE_CONSTANT_ALPHA,
                           GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
    } else {
        GX2SetBlendControl(GX2_RENDER_TARGET_0, csrc, cdst, ccomb, FALSE, csrc, cdst, ccomb);
    }
    GX2SetDepthOnlyControl(ztest, zwrite, ztest ? GX2CompareFunction((zmode >> 1) & 7) : GX2_COMPARE_FUNC_ALWAYS);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetPolygonOffset(0, 0, 0, 0, 0);
    // GX front faces are clockwise on screen (the PC path's GL_CCW in its
    // y-down window space); cull modes 1/2/3 drop back/front/all faces.
    const uint32_t cull = (g.bp[BP_GENMODE] >> 14) & 3;
    GX2SetPolygonControl(GX2_FRONT_FACE_CW, cull == 2 || cull == 3, cull == 1 || cull == 3, FALSE,
                         GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
    const float scaleX = s_gameScale, scaleY = s_gameScale;
    const int vx = int(s_gameX + g.viewport[0] * scaleX);
    const int vy = int(g.viewport[1] * scaleY);
    const int vw = int(g.viewport[2] * scaleX);
    const int vh = int(g.viewport[3] * scaleY);
    // transform() already applies the GX viewport (into the TV target),
    // so GX2's own viewport must be the identity mapping.
    (void)vx; (void)vy; (void)vw; (void)vh;
    GX2SetViewport(0.0f, 0.0f, s_tvW, s_tvH, 0.0f, 1.0f);
    const int sx = int(s_gameX + g.scissor[0] * scaleX);
    const int sy = int(g.scissor[1] * scaleY);
    const int sw = int(g.scissor[2] * scaleX);
    const int sh = int(g.scissor[3] * scaleY);
    GX2SetScissor(sx, sy, sw, sh);
    // Color update is independent of blending in CMODE0.  The title's
    // blend-off cloud and light-ray passes still have cupd set, so use that
    // bit for GX2's color mask rather than treating GX_BM_NONE as write-off.
    const bool colourUpd = ((cm >> 3) & 1) != 0;
    const bool alphaUpd = hasAlpha && ((cm >> 4) & 1) != 0;
    GX2SetColorControl(GX2_LOGIC_OP_COPY, (blend || dstAlphaConst) ? 0xff : 0x00, FALSE, TRUE);
    GX2SetTargetChannelMasks(GX2ChannelMask((colourUpd ? GX2_CHANNEL_MASK_RGB : 0) | (alphaUpd ? GX2_CHANNEL_MASK_A : 0)),
                             (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                             (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
}

// Stage 1 colour: run the TEV chain per vertex on the CPU with the texture
// taken as white, and let the fixed shader multiply the real texture in.
// Exact for the common tex*X materials, approximate for the rest.
static inline int sext11(uint32_t v) { return int(v << 21) >> 21; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int lerpTev(int a, int b, int c) { return (a * (255 - c) + b * c + 127) / 255; }

static void packedColor(uint32_t value, float out[4]) {
    out[0] = float((value >> 24) & 255) / 255.0f;
    out[1] = float((value >> 16) & 255) / 255.0f;
    out[2] = float((value >> 8) & 255) / 255.0f;
    out[3] = float(value & 255) / 255.0f;
}

static void modelPosition(const HostVertex& in, float out[3]) {
    const float* m = reinterpret_cast<const float*>(&g.xfMem[(in.mtx[0] & 63) * 4]);
    for (int i = 0; i < 3; ++i)
        out[i] = m[i * 4 + 0] * in.pos[0] + m[i * 4 + 1] * in.pos[1] + m[i * 4 + 2] * in.pos[2] + m[i * 4 + 3];
}

static void chanColor(const HostVertex& in, int c, int out[4]) {
    const uint32_t numChans = g.xfReg[XFR_NUMCHANS] & 3;
    if (uint32_t(c) >= numChans) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    const uint32_t colorCtrl = g.xfReg[XFR_COLOR0CTRL + c];
    const uint32_t alphaCtrl = g.xfReg[XFR_ALPHA0CTRL + c];

    float normal[3] = {0.0f, 0.0f, 1.0f}, position[3];
    const uint32_t matrix = in.mtx[0] & 63;
    const float* nm = reinterpret_cast<const float*>(&g.xfMem[0x400 + matrix * 3]);
    normal[0] = nm[0] * in.nrm[0] + nm[1] * in.nrm[1] + nm[2] * in.nrm[2];
    normal[1] = nm[3] * in.nrm[0] + nm[4] * in.nrm[1] + nm[5] * in.nrm[2];
    normal[2] = nm[6] * in.nrm[0] + nm[7] * in.nrm[1] + nm[8] * in.nrm[2];
    const float nl = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (nl > 0.0f) for (float& v : normal) v /= nl;
    else normal[2] = 1.0f;
    modelPosition(in, position);

    // The light loop is the same for every component, so one pass yields
    // all four (the colour control uses r/g/b, the alpha control a).
    auto litAll = [&](uint32_t ctrl, float result[4]) {
        float materialColor[4];
        if (ctrl & 1) for (int k = 0; k < 4; ++k) materialColor[k] = float(in.clr[c][k]) / 255.0f;
        else packedColor(g.xfReg[XFR_MAT0 + c], materialColor);
        if (!(ctrl & 2)) { for (int k = 0; k < 4; ++k) result[k] = materialColor[k]; return; }
        float ambient[4];
        if (ctrl & 64) for (int k = 0; k < 4; ++k) ambient[k] = float(in.clr[c][k]) / 255.0f;
        else packedColor(g.xfReg[XFR_AMB0 + c], ambient);
        float sum[4] = {ambient[0], ambient[1], ambient[2], ambient[3]};
        const uint32_t mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
        const uint32_t diff = (ctrl >> 7) & 3;
        for (uint32_t light = 0; light < 8; ++light) {
            if (!(mask & (1u << light))) continue;
            // g.xfMem is the raw XF address space; light records begin at
            // 0x600 (the PC shader packs that block down to word 608).
            const uint32_t base = 0x600 + light * 16;
            float lightCol[4]; packedColor(g.xfMem[base + 3], lightCol);
            const float* a = reinterpret_cast<const float*>(&g.xfMem[base + 4]);
            const float* k = reinterpret_cast<const float*>(&g.xfMem[base + 7]);
            const float* lp = reinterpret_cast<const float*>(&g.xfMem[base + 10]);
            const float* ld = reinterpret_cast<const float*>(&g.xfMem[base + 13]);
            float l[3], factor = 1.0f;
            const bool atten = (ctrl >> 9) & 1, spot = (ctrl >> 10) & 1;
            if (!atten || spot) {
                l[0] = lp[0] - position[0]; l[1] = lp[1] - position[1]; l[2] = lp[2] - position[2];
                if (!spot) { /* positional, unattenuated */ }
            } else {
                l[0] = lp[0]; l[1] = lp[1]; l[2] = lp[2];
            }
            const float len = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            if (len > 0.0f) { l[0] /= len; l[1] /= len; l[2] /= len; }
            if (atten) {
                if (spot) {
                    const float cs = std::max(l[0] * ld[0] + l[1] * ld[1] + l[2] * ld[2], 0.0f);
                    const float d = len, d2 = d * d;
                    factor = std::max(a[0] + a[1] * cs + a[2] * cs * cs, 0.0f) /
                             std::max(k[0] + k[1] * d + k[2] * d2, 1e-30f);
                } else {
                    const float nd = normal[0] * l[0] + normal[1] * l[1] + normal[2] * l[2];
                    const float t = nd > 0.0f ? std::max(normal[0] * ld[0] + normal[1] * ld[1] + normal[2] * ld[2], 0.0f) : 0.0f;
                    factor = std::max(a[0] + a[1] * t + a[2] * t * t, 0.0f) /
                             std::max(k[0] + k[1] * t + k[2] * t * t, 1e-30f);
                }
            }
            float diffuse = 1.0f;
            if (diff == 1) diffuse = normal[0] * l[0] + normal[1] * l[1] + normal[2] * l[2];
            else if (diff >= 2) diffuse = std::max(normal[0] * l[0] + normal[1] * l[1] + normal[2] * l[2], 0.0f);
            const float w = factor * diffuse;
            for (int ch = 0; ch < 4; ++ch) sum[ch] += w * lightCol[ch];
        }
        for (int ch = 0; ch < 4; ++ch) result[ch] = materialColor[ch] * std::max(0.0f, std::min(sum[ch], 1.0f));
    };
    float colorLit[4], alphaLit[4];
    litAll(colorCtrl, colorLit);
    if (alphaCtrl == colorCtrl) alphaLit[3] = colorLit[3];
    else litAll(alphaCtrl, alphaLit);
    for (int channel = 0; channel < 3; ++channel)
        out[channel] = clampi(int(colorLit[channel] * 255.0f + 0.5f), 0, 255);
    out[3] = clampi(int(alphaLit[3] * 255.0f + 0.5f), 0, 255);
}

static bool evalTev(const HostVertex& in, float* rgba, bool* usesTex) {
    int ras0[4], ras1[4];
    chanColor(in, 0, ras0);
    chanColor(in, 1, ras1);
    int reg[4][4];
    for (int i = 0; i < 4; i++) {
        const uint32_t ra = g.bp[BP_TEV_REG + 2 * i], bg = g.bp[BP_TEV_REG + 2 * i + 1];
        reg[i][0] = sext11(ra & 0x7ff); reg[i][1] = sext11((bg >> 12) & 0x7ff);
        reg[i][2] = sext11(bg & 0x7ff); reg[i][3] = sext11((ra >> 12) & 0x7ff);
    }
    int kon[4][4];
    for (int i = 0; i < 4; i++) {
        const uint32_t ra = g.kreg[2 * i], bg = g.kreg[2 * i + 1];
        kon[i][0] = ra & 255; kon[i][1] = (bg >> 12) & 255; kon[i][2] = bg & 255; kon[i][3] = (ra >> 12) & 255;
    }
    static const int k8[8] = {255, 223, 191, 159, 127, 95, 63, 31};
    const uint32_t stages = ((g.bp[BP_GENMODE] >> 10) & 15) + 1;
    int lastC = 0, lastA = 0;
    bool tex = false;
    for (uint32_t st = 0; st < stages; st++) {
        const uint32_t ord = (g.bp[BP_TREF + (st >> 1)] >> ((st & 1) * 12)) & 0x3ff;
        const uint32_t en = (ord >> 6) & 1, chan = (ord >> 7) & 7;
        const uint32_t ce = g.bp[BP_TEV_COLOR_ENV + 2 * st], ae = g.bp[BP_TEV_COLOR_ENV + 2 * st + 1];
        const uint32_t ks = (g.bp[BP_TEV_KSEL + (st >> 1)] >> ((st & 1) * 10)) & 0x3ff;
        const uint32_t kc = ks & 31, ka = (ks >> 5) & 31;
        int t[4] = {255, 255, 255, 255};
        int ras[4] = {0, 0, 0, 0};
        if (chan == 0) memcpy(ras, ras0, sizeof ras); else if (chan == 1) memcpy(ras, ras1, sizeof ras);
        int k[4];
        for (int i = 0; i < 3; i++)
            k[i] = kc < 8 ? k8[kc] : (kc >= 12 && kc < 16) ? kon[kc - 12][i] : kc >= 16 ? kon[kc & 3][(kc - 16) >> 2] : 255;
        k[3] = ka < 8 ? k8[ka] : ka >= 16 ? kon[ka & 3][(ka - 16) >> 2] : 255;
        auto carg = [&](uint32_t sel, int ch) -> int {
            switch (sel) {
            case 0: return reg[0][ch]; case 1: return reg[0][3]; case 2: return reg[1][ch]; case 3: return reg[1][3];
            case 4: return reg[2][ch]; case 5: return reg[2][3]; case 6: return reg[3][ch]; case 7: return reg[3][3];
            case 8: if (en) tex = true; return t[ch]; case 9: if (en) tex = true; return t[3];
            case 10: return ras[ch]; case 11: return ras[3]; case 12: return 255; case 13: return 128;
            case 14: return k[ch]; default: return 0;
            }
        };
        auto aarg = [&](uint32_t sel) -> int {
            switch (sel) {
            case 0: return reg[0][3]; case 1: return reg[1][3]; case 2: return reg[2][3]; case 3: return reg[3][3];
            case 4: if (en) tex = true; return t[3]; case 5: return ras[3]; case 6: return k[3]; default: return 0;
            }
        };
        int res[3];
        {
            const uint32_t d = ce & 15, c = (ce >> 4) & 15, b = (ce >> 8) & 15, a = (ce >> 12) & 15;
            const uint32_t bias = (ce >> 16) & 3, op = (ce >> 18) & 1, cl = (ce >> 19) & 1, sc = (ce >> 20) & 3;
            const uint32_t dest = (ce >> 22) & 3;
            for (int ch = 0; ch < 3; ch++) {
                const int A = carg(a, ch) & 255, B = carg(b, ch) & 255, C = carg(c, ch) & 255, D = carg(d, ch);
                int r;
                if (bias != 3) {
                    r = op ? D - lerpTev(A, B, C) : D + lerpTev(A, B, C);
                    r += bias == 1 ? 128 : bias == 2 ? -128 : 0;
                    r = sc == 1 ? r * 2 : sc == 2 ? r * 4 : sc == 3 ? r >> 1 : r;
                } else {
                    r = D + (A > B ? C : 0);
                }
                res[ch] = cl ? clampi(r, 0, 255) : clampi(r, -1024, 1023);
            }
            for (int ch = 0; ch < 3; ch++) reg[dest][ch] = res[ch];
            lastC = int(dest);
        }
        {
            const uint32_t d = (ae >> 4) & 7, c = (ae >> 7) & 7, b = (ae >> 10) & 7, a = (ae >> 13) & 7;
            const uint32_t bias = (ae >> 16) & 3, op = (ae >> 18) & 1, cl = (ae >> 19) & 1, sc = (ae >> 20) & 3;
            const uint32_t dest = (ae >> 22) & 3;
            const int A = aarg(a) & 255, B = aarg(b) & 255, C = aarg(c) & 255, D = aarg(d);
            int r;
            if (bias != 3) {
                r = op ? D - lerpTev(A, B, C) : D + lerpTev(A, B, C);
                r += bias == 1 ? 128 : bias == 2 ? -128 : 0;
                r = sc == 1 ? r * 2 : sc == 2 ? r * 4 : sc == 3 ? r >> 1 : r;
            } else {
                r = D + (A > B ? C : 0);
            }
            reg[dest][3] = cl ? clampi(r, 0, 255) : clampi(r, -1024, 1023);
            lastA = int(dest);
        }
    }
    for (int ch = 0; ch < 3; ch++) rgba[ch] = float(reg[lastC][ch] & 255) / 255.0f;
    rgba[3] = float(reg[lastA][3] & 255) / 255.0f;
    if (usesTex) *usesTex = tex;
    return true;
}

static void transformPosition(const HostVertex& in, float* out) {
    const uint32_t matrix = in.mtx[0] & 63;
    const uint32_t addr = matrix * 4;
    const float* m = reinterpret_cast<const float*>(&g.xfMem[addr]);
    const float x = m[0] * in.pos[0] + m[1] * in.pos[1] + m[2] * in.pos[2] + m[3];
    const float y = m[4] * in.pos[0] + m[5] * in.pos[1] + m[6] * in.pos[2] + m[7];
    const float z = m[8] * in.pos[0] + m[9] * in.pos[1] + m[10] * in.pos[2] + m[11];
    // The game can update projection through display-list XF writes rather
    // than GXSetProjection(), so g.projection may lag the live registers.
    // The PC shader reads XF_PROJ directly for this reason.
    const float* xfProj = reinterpret_cast<const float*>(&g.xfReg[XFR_PROJ]);
    const float p1 = xfProj[0], p2 = xfProj[1], p3 = xfProj[2], p4 = xfProj[3],
                p5 = xfProj[4], p6 = xfProj[5];
    const bool ortho = (g.xfReg[XFR_PROJ + 6] & 1) != 0;
    float cx, cy, cz, cw;
    if (ortho) {  // GX_ORTHOGRAPHIC: {sx, tx, sy, ty, sz, tz, 1}
        cx = p1 * x + p2;
        cy = p3 * y + p4;
        cz = p5 * z + p6;
        cw = 1.0f;
    } else {  // GX_PERSPECTIVE == 0
        cx = p1 * x + p2 * z;
        cy = p3 * y + p4 * z;
        cz = p5 * z + p6;
        cw = -z;
    }
    if (fabsf(cw) < 0.00001f) cw = 0.00001f;
    // Match the PC renderer's XF viewport conversion, then map the original
    // 640x480 game rectangle into the TV target (4:3 letterboxed).
    const float halfW = g.viewport[2] * 0.5f * s_gameScale;
    const float halfH = g.viewport[3] * 0.5f * s_gameScale;
    const float centerX = s_gameX + g.viewport[0] * s_gameScale + halfW;
    const float centerY = g.viewport[1] * s_gameScale + halfH;
    out[0] = ((centerX * cw + halfW * cx) * (2.0f / s_tvW)) - cw;
    // GX screen y grows downwards; GX2 clip-space y grows upwards.
    out[1] = cw - ((centerY * cw - halfH * cy) * (2.0f / s_tvH));
    // GX's XF viewport writes a 24-bit depth range. GX2's clip-space depth
    // uses the same positive-w convention as the lus/DKR shader path.
    float* vp = reinterpret_cast<float*>(&g.xfReg[XFR_VIEWPORT]);
    // XF viewport: depth * w = (zoffset * w + zscale * z) / 2^24-1, already in
    // GX2's 0..w clip range (the PC GL path maps the same value to -w..w).
    out[2] = (vp[5] * cw + vp[2] * cz) / 16777215.0f;
    out[3] = cw;
}

static void transform(const HostVertex& in, float* out, int texCoord, bool textured, TextureEntry* texture) {
    transformPosition(in, out);
    // Layout as the fetch shader declares it: pos(4) [uv(2)] colour(4).
    evalTev(in, out + (textured ? 6 : 4), nullptr);
    if (textured) {
        float u = in.tex[texCoord][0], v = in.tex[texCoord][1];
        if (g.tcManual[texCoord]) {
            const uint32_t ss = g.bp[BP_SU_SSIZE + texCoord * 2] & 0xffff;
            const uint32_t ts = g.bp[BP_SU_SSIZE + texCoord * 2 + 1] & 0xffff;
            if (texture && texture->width && texture->height) {
                u *= float(ss + 1) / float(texture->width);
                v *= float(ts + 1) / float(texture->height);
            }
        }
        adjustCopyUv(texture, &u, &v);
        out[4] = u; out[5] = v;
    }
}

static TextureEntry* textureForCoord(const TevKey& key, TextureEntry* const* textures, int coord) {
    for (uint32_t stage = 0; stage < key.stages; ++stage) {
        const uint32_t order = key.order[stage];
        if (((order >> 6) & 1) && ((order >> 3) & 7) == uint32_t(coord)) {
            const uint32_t map = order & 7;
            if (textures[map]) return textures[map];
        }
    }
    // A coordinate read only by an indirect lookup belongs to its map.
    for (uint32_t i = 0; i < tevNumInd(key); ++i) {
        if (((key.iref >> (6 * i + 3)) & 7) != uint32_t(coord)) continue;
        const uint32_t map = (key.iref >> (6 * i)) & 7;
        if (textures[map]) return textures[map];
    }
    return nullptr;
}

static void packVertexLiveUniforms(const TevKey& key, TextureEntry* const* textures) {
    memset(s_vtxLive, 0, sizeof(s_vtxLive));
    const float* proj = reinterpret_cast<const float*>(&g.xfReg[XFR_PROJ]);
    const float* xfViewport = reinterpret_cast<const float*>(&g.xfReg[XFR_VIEWPORT]);
    memcpy(&s_vtxLive[0], proj, 6 * sizeof(float));
    s_vtxLive[6] = float(g.xfReg[XFR_PROJ + 6] & 1);
    memcpy(&s_vtxLive[8], g.viewport, 4 * sizeof(float));
    // C187 is the viewport mapping block consumed by the generated vertex
    // program: game scale/X followed by the target dimensions.  Do not leave
    // the XF viewport's depth pair in front of it; that made the generated
    // path use z-offset/scale as its 2D scale and sent every vertex offscreen.
    s_vtxLive[12] = s_gameScale;
    s_vtxLive[13] = s_gameX;
    s_vtxLive[14] = s_tvW;
    s_vtxLive[15] = s_tvH;
    // C188.xy are depth scale/offset, already divided by the XF 24-bit
    // depth range.  This keeps the live block self-contained without
    // spending a constant register on 1 / 16777215.  The generated shader
    // evaluates C188.x * z + C188.y * w, matching transformPosition.
    s_vtxLive[16] = xfViewport[2] / 16777215.0f;
    s_vtxLive[17] = xfViewport[5] / 16777215.0f;
    auto colour = [](uint32_t value, float* out) {
        out[0] = float((value >> 24) & 255) / 255.0f;
        out[1] = float((value >> 16) & 255) / 255.0f;
        out[2] = float((value >> 8) & 255) / 255.0f;
        out[3] = float(value & 255) / 255.0f;
    };
    colour(g.xfReg[XFR_MAT0 + 0], &s_vtxLive[20]);
    colour(g.xfReg[XFR_AMB0 + 0], &s_vtxLive[24]);
    colour(g.xfReg[XFR_MAT0 + 1], &s_vtxLive[28]);
    colour(g.xfReg[XFR_AMB0 + 1], &s_vtxLive[32]);
    // C193..C196: manual scale followed by the copy rectangle transform.
    for (int coord = 0; coord < 8; ++coord) {
        float* scale = &s_vtxLive[36 + coord * 2];
        scale[0] = scale[1] = 1.0f;
        TextureEntry* texture = textureForCoord(key, textures, coord);
        if (g.tcManual[coord] && texture && texture->width && texture->height) {
            scale[0] = float((g.bp[BP_SU_SSIZE + coord * 2] & 0xffff) + 1) / float(texture->width);
            scale[1] = float((g.bp[BP_SU_SSIZE + coord * 2 + 1] & 0xffff) + 1) / float(texture->height);
        }
        float* copy = &s_vtxLive[52 + coord * 4];
        copy[0] = copy[1] = 1.0f;
        copy[2] = copy[3] = 0.0f;
        const CopyEntry* entry = copyEntryForTexture(texture);
        if (entry && entry->sourceW && entry->sourceH) {
            copy[0] = float(entry->sourceW) / s_tvW;
            copy[1] = float(entry->sourceH) / s_tvH;
            copy[2] = float(entry->sourceX) / s_tvW;
            copy[3] = float(entry->sourceY) / s_tvH;
        }
    }
    GX2SetVertexUniformReg(kVtxLiveBase * 4, kVtxLiveRegs * 4, s_vtxLive);
}

static void transformTev(const HostVertex& in, float* out, const TevKey& key, TextureEntry* const* textures) {
    transformPosition(in, out);
    int ras0[4], ras1[4];
    chanColor(in, 0, ras0);
    chanColor(in, 1, ras1);
    float texgen[8][3] = {};
    // Only the enabled texgens produce coordinates; the rest are never sampled.
    const int numTexGens = int(std::min<uint32_t>(g.xfReg[XFR_NUMTEXGENS] & 15, 8));
    for (int coord = numTexGens; coord < 8; ++coord) {
        out[4 + coord * 2] = 0.0f;
        out[5 + coord * 2] = 0.0f;
    }
    for (int coord = 0; coord < numTexGens; ++coord) {
        const uint32_t tg = g.xfReg[XFR_TEXGEN + coord];
        const uint32_t proj = (tg >> 1) & 1, form = (tg >> 2) & 1;
        const uint32_t type = (tg >> 4) & 7, row = (tg >> 7) & 31;
        const uint32_t emboss = (tg >> 12) & 7;
        float source[4] = {0, 0, 1, 1};
        if (row == 0) {
            source[0] = in.pos[0]; source[1] = in.pos[1]; source[2] = in.pos[2];
        } else if (row == 1) {
            source[0] = in.nrm[0]; source[1] = in.nrm[1]; source[2] = in.nrm[2];
        } else if (row == 2) {
            for (int i = 0; i < 4; ++i) source[i] = float(ras0[i]) / 255.0f;
        } else if (row == 3) {
            source[0] = in.bin[0]; source[1] = in.bin[1]; source[2] = in.bin[2];
        } else if (row == 4) {
            source[0] = in.tan[0]; source[1] = in.tan[1]; source[2] = in.tan[2];
        } else {
            const int input = int((row - 5) & 7);
            source[0] = in.tex[input][0]; source[1] = in.tex[input][1];
        }
        if (form == 0 && row < 5) {
            source[2] = 1.0f;
            source[3] = 1.0f;
        }
        float t[3] = {0, 0, 1};
        if (type == 0) {
            const uint32_t matrix = in.mtx[coord + 1] & 63;
            // GX_TEXMTX0..GX_IDENTITY (30..60) share the position-matrix
            // block.  Only GX_PTTEXMTX entries live at XF 0x500; the
            // six-bit vertex matrix index cannot select those entries.
            const float* m = reinterpret_cast<const float*>(&g.xfMem[matrix * 4]);
            for (int r = 0; r < 3; ++r)
                t[r] = m[r * 4 + 0] * source[0] + m[r * 4 + 1] * source[1] +
                       m[r * 4 + 2] * source[2] + m[r * 4 + 3] * source[3];
            if (!proj) t[2] = 1.0f;
        } else if (type == 1) {
            const uint32_t sourceCoord = emboss < uint32_t(coord) ? emboss : 0;
            t[0] = texgen[sourceCoord][0]; t[1] = texgen[sourceCoord][1]; t[2] = texgen[sourceCoord][2];
        } else {
            const int* color = type == 3 ? ras1 : ras0;
            t[0] = float(color[0]) / 255.0f;
            t[1] = float(color[1]) / 255.0f;
            t[2] = 1.0f;
        }
        if ((g.xfReg[XFR_DUALTEX] & 1) && type == 0) {
            const uint32_t post = g.xfReg[XFR_POSTTEX + coord];
            const uint32_t matrix = post & 63;
            if ((post >> 8) & 1) {
                const float len = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                if (len > 0.0f) { t[0] /= len; t[1] /= len; t[2] /= len; }
            }
            const float pv[4] = {t[0], t[1], t[2], 1.0f};
            const float* m = reinterpret_cast<const float*>(&g.xfMem[0x500 + matrix * 4]);
            for (int r = 0; r < 3; ++r)
                t[r] = m[r * 4 + 0] * pv[0] + m[r * 4 + 1] * pv[1] +
                       m[r * 4 + 2] * pv[2] + m[r * 4 + 3] * pv[3];
        }
        if (proj && fabsf(t[2]) > 0.00001f) {
            t[0] /= t[2]; t[1] /= t[2];
        }
        texgen[coord][0] = t[0]; texgen[coord][1] = t[1]; texgen[coord][2] = t[2];
        float u = t[0], v = t[1];
        TextureEntry* texture = textureForCoord(key, textures, coord);
        if (g.tcManual[coord]) {
            const uint32_t ss = g.bp[BP_SU_SSIZE + coord * 2] & 0xffff;
            const uint32_t ts = g.bp[BP_SU_SSIZE + coord * 2 + 1] & 0xffff;
            if (texture && texture->width && texture->height) {
                u *= float(ss + 1) / float(texture->width);
                v *= float(ts + 1) / float(texture->height);
            }
        }
        adjustCopyUv(texture, &u, &v);
        out[4 + coord * 2] = u;
        out[5 + coord * 2] = v;
    }
    for (int i = 0; i < 4; ++i) {
        out[20 + i] = float(ras0[i]) / 255.0f;
        out[24 + i] = float(ras1[i]) / 255.0f;
    }
}

static void vertexSelfCheckDraw(const VertexKey& key, const TevKey& tevKey, bool tevPath,
                                int coord, bool textured, TextureEntry* texture,
                                TextureEntry* const* tevTextures,
                                const HostVertex& vertex, uint32_t draw) {
    if (!s_vertexSelfCheck) return;
    // There is no vertex-stream readback, so this models the generated
    // program on the CPU from the exact uniform arrays sent to the GPU
    // (s_xfUniform, s_vtxLive) and the packed raw vertex, then compares the
    // position and the type-0 texcoords with the CPU reference.  It checks
    // packing, indexing and algebra; Latte translation is checked on screen.
    if (!tevPath) return;
    RawVertex raw;
    packRawVertex(vertex, raw);
    float reference[28] = {};
    transformTev(vertex, reference, tevKey, tevTextures);
    const float* live = s_vtxLive;
    auto row = [](const float* m, const float v[4]) { return m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3] * v[3]; };
    const float pos[4] = {raw.pos[0], raw.pos[1], raw.pos[2], 1.0f};
    const float* pm = &s_xfUniform[uint32_t(raw.mtx[0]) * 4];
    const float mx = row(pm, pos), my = row(pm + 4, pos), mz = row(pm + 8, pos);
    float cx, cy, cz, cw;
    if (key.projectionOrtho) {
        cx = mx * live[0] + live[1]; cy = my * live[2] + live[3]; cz = mz * live[4] + live[5]; cw = 1.0f;
    } else {
        cx = mx * live[0] + mz * live[1]; cy = my * live[2] + mz * live[3]; cz = mz * live[4] + live[5]; cw = -mz;
    }
    const float halfW = live[10] * live[12] * 0.5f, halfH = live[11] * live[12] * 0.5f;
    const float centerX = live[8] * live[12] + halfW + live[13], centerY = live[9] * live[12] + halfH;
    float model[4 + 16];
    model[0] = (centerX * cw + halfW * cx) * (2.0f / live[14]) - cw;
    model[1] = cw - (centerY * cw - halfH * cy) * (2.0f / live[15]);
    model[2] = live[16] * cz + live[17] * cw;
    model[3] = cw;
    float maxError = 0.0f;
    int worst = -1;
    auto check = [&](int i, float got) {
        const float err = fabsf(got - reference[i]) / std::max(1.0f, fabsf(reference[i]));
        if (err > maxError) { maxError = err; worst = i; }
        model[i < 4 ? i : 4 + (i - 4)] = got;
    };
    for (int i = 0; i < 4; ++i) check(i, model[i]);
    for (uint32_t c = 0; c < key.numTexGens; ++c) {
        const uint32_t tg = key.texgen[c];
        const uint32_t type = (tg >> 4) & 7, srcRow = (tg >> 7) & 31;
        if (type != 0 || srcRow == 2) continue;
        const bool form = (tg >> 2) & 1, proj = (tg >> 1) & 1;
        float src[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        const float* rowSrc = srcRow == 0 ? raw.pos : srcRow == 1 ? raw.nrm : srcRow == 3 ? raw.bin :
                              srcRow == 4 ? raw.tan : nullptr;
        if (rowSrc) {
            src[0] = rowSrc[0]; src[1] = rowSrc[1]; src[2] = rowSrc[2];
            if (!form) src[2] = 1.0f;
        } else {
            const uint32_t in = (srcRow - 5) & 7;
            src[0] = raw.tex[in][0]; src[1] = raw.tex[in][1];
        }
        const float* tm = &s_xfUniform[uint32_t(raw.mtx[c + 1]) * 4];
        float t[4] = {row(tm, src), row(tm + 4, src), row(tm + 8, src), 1.0f};
        if (!proj) t[2] = 1.0f;
        if (key.dualTex & 1) {
            const uint32_t post = key.postTex[c];
            if ((post >> 8) & 1) {
                const float len = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                if (len > 0.0f) for (int k = 0; k < 3; ++k) t[k] /= len;
            }
            const float* qm = &s_xfUniform[(kXfPostTexBase + (post & 63)) * 4];
            const float q[3] = {row(qm, t), row(qm + 4, t), row(qm + 8, t)};
            t[0] = q[0]; t[1] = q[1]; t[2] = q[2];
        }
        if (proj && t[2] != 0.0f) { t[0] /= t[2]; t[1] /= t[2]; }
        const float* sc = &live[36 + c * 2];
        const float* cp = &live[52 + c * 4];
        check(4 + int(c) * 2, cp[2] + t[0] * sc[0] * cp[0]);
        check(5 + int(c) * 2, cp[3] + t[1] * sc[1] * cp[1]);
    }
    ++s_vertexSelfCheckSamples;
    const bool mismatch = maxError > 0.001f;
    if (mismatch) ++s_vertexSelfCheckMismatches;
    if (s_vertexSelfCheckSamples <= 8 || (mismatch && s_vertexSelfCheckMismatches <= 40)) {
        WHBLogPrintf("SMS Wii U: [vxcheck] draw=%u texgens=%u tg0=%08x tg1=%08x dual=%u post0=%03x mtx=%u/%u/%u worst=%d ref=%.5f got=%.5f relErr=%.5f mismatches=%u",
                     (unsigned)draw, (unsigned)key.numTexGens, (unsigned)key.texgen[0], (unsigned)key.texgen[1],
                     (unsigned)key.dualTex, (unsigned)key.postTex[0], (unsigned)raw.mtx[0], (unsigned)raw.mtx[1],
                     (unsigned)raw.mtx[2],
                     worst, worst >= 0 ? reference[worst] : 0.0f, worst >= 0 ? model[worst] : 0.0f, maxError,
                     (unsigned)s_vertexSelfCheckMismatches);
    }
}

void rendererInit(float) {
    SMS_PORT_HEAP_SCOPE();
    if (s_ready) return;
    // sd:/wiiu/apps/sms/cpuxf forces the CPU transform for A/B runs.
    s_gpuXfEnabled = envFlag("SMS_GX2_GPU_XF", true) && !sdMarkerPresent("cpuxf");
    s_vertexOneClause = sdMarkerPresent("gpuxf1clause");
    s_vertexSelfCheck = sdMarkerPresent("vxcheck") || envFlag("SMS_GX2_VERTEX_SELF_CHECK", false);
    WHBLogPrintf("SMS Wii U: GPU vertex transform=%s self-check=%s",
                 s_gpuXfEnabled ? "on" : "off", s_vertexSelfCheck ? "on" : "off");
    s_ready = initShaders();
    if (!s_ready) WHBLogPrintf("SMS Wii U: GX2 shader setup failed");
}

float efbScale() { return 1.0f; }
bool rendererReady() { return s_ready; }
void releaseGlThread() {}

void onStateChange() {
    SMS_PORT_HEAP_SCOPE();
    if (!s_batch.indices.empty()) flushBatch();
}

PrimClass primClass(uint8_t op) { return op >= 0xb8 ? PRIM_POINTS : op >= 0xa8 ? PRIM_LINES : PRIM_TRIS; }

static uint32_t primitiveIndicesLocal(uint8_t op, uint32_t n, uint32_t base, uint32_t* out) {
    uint32_t* start = out;
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { *out++ = base + a; *out++ = base + b; *out++ = base + c; };
    switch (op & 0xf8) {
    case 0x80: case 0x88:
        for (uint32_t i = 0; i + 3 < n; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); }
        break;
    case 0x90: for (uint32_t i = 0; i + 2 < n; i += 3) tri(i, i + 1, i + 2); break;
    case 0x98: for (uint32_t i = 2; i < n; ++i) (i & 1) ? tri(i - 1, i - 2, i) : tri(i - 2, i - 1, i); break;
    case 0xa0: for (uint32_t i = 2; i < n; ++i) tri(0, i - 1, i); break;
    case 0xa8: for (uint32_t i = 0; i + 1 < n; i += 2) { *out++ = base + i; *out++ = base + i + 1; } break;
    case 0xb0: for (uint32_t i = 1; i < n; ++i) { *out++ = base + i - 1; *out++ = base + i; } break;
    default: for (uint32_t i = 0; i < n; ++i) *out++ = base + i; break;
    }
    return uint32_t(out - start);
}

uint32_t primitiveIndices(uint8_t op, uint32_t n, uint32_t base, uint32_t* out) {
    return primitiveIndicesLocal(op, n, base, out);
}

uint8_t* primitiveBegin(uint8_t op, uint32_t n, uint32_t fmt, uint32_t stride) {
    SMS_PORT_HEAP_SCOPE();
    const PrimClass cls = primClass(op);
    if (!s_batch.indices.empty() && (cls != s_batch.cls || fmt != s_batch.fmt)) flushBatch();
    s_batch.cls = cls; s_batch.fmt = fmt; s_batch.stride = stride;
    const size_t old = s_batch.vertices.size();
    s_batch.vertices.resize(old + size_t(n) * stride);
    return s_batch.vertices.data() + old;
}

void primitiveEnd(uint8_t op, uint32_t n) {
    SMS_PORT_HEAP_SCOPE();
    if (!n) return;
    const uint32_t base = s_batch.count;
    s_batch.count += n;
    const size_t old = s_batch.indices.size();
    s_batch.indices.resize(old + size_t(n) * 3);
    const uint32_t used = primitiveIndicesLocal(op, n, base, s_batch.indices.data() + old);
    s_batch.indices.resize(old + used);
}

void appendDecoded(PrimClass cls, uint32_t fmt, uint32_t stride, const uint8_t* verts, uint32_t n,
                   const uint32_t* idx, uint32_t nidx) {
    SMS_PORT_HEAP_SCOPE();
    if (!s_batch.indices.empty() && (cls != s_batch.cls || fmt != s_batch.fmt)) flushBatch();
    s_batch.cls = cls; s_batch.fmt = fmt; s_batch.stride = stride;
    const uint32_t base = s_batch.count;
    s_batch.vertices.insert(s_batch.vertices.end(), verts, verts + size_t(n) * stride);
    s_batch.count += n;
    for (uint32_t i = 0; i < nidx; ++i) s_batch.indices.push_back(idx[i] + base);
}

bool arenaEnabled() { return false; }
uint32_t arenaGeneration() { return 0; }
bool arenaUpload(const uint8_t*, uint32_t, uint32_t, const uint32_t*, uint32_t, ArenaRef&) { return false; }
void appendArena(PrimClass, uint32_t, uint32_t, const ArenaRef&, uint32_t, uint32_t, const uint8_t*) {}
bool batchUsesArena() { return false; }

static uint32_t s_drawsThisFrame;
static uint32_t s_debugDrawNo;
// Per-frame CPU cost of the renderer, logged with the [perf] line.
static OSTime s_perfFlush, s_perfTex, s_perfXform, s_perfLastEnd, s_perfWall;
static uint32_t s_perfDraws, s_perfVerts, s_perfUnique, s_perfFrames;
static uint32_t s_perfGpuDraws, s_perfCpuDraws, s_perfGpuVerts, s_perfCpuVerts;
struct PerfScope {
    OSTime& acc; OSTime t0;
    explicit PerfScope(OSTime& a) : acc(a), t0(OSGetTime()) {}
    ~PerfScope() { acc += OSGetTime() - t0; }
};

void flushBatch() {
    SMS_PORT_HEAP_SCOPE();
    if (s_batch.indices.empty()) return;
    if (!s_ready || !s_frameActive || !ProcUIInForeground()) {
        s_batch.vertices.clear(); s_batch.indices.clear(); s_batch.count = 0;
        return;
    }
    PerfScope perfFlush(s_perfFlush);
    OSTime perfTex0 = OSGetTime();
    const TevKey tevKey = currentTevKey();
    const bool textured = tevTextureMask(tevKey) != 0;
    TextureEntry* tevTextures[8] = {};
    TevShader* tevShader = nullptr;
    if (tevSupported(tevKey)) {
        const uint32_t mask = tevTextureMask(tevKey);
        // getTexture() may grow its cache, so resolve all entries once more
        // after the insertions have settled and pointers cannot be invalidated.
        for (int map = 0; map < 8; ++map)
            if (mask & (1u << map)) getTexture(map);
        bool allTextures = true;
        for (int map = 0; map < 8; ++map) {
            if (!(mask & (1u << map))) continue;
            tevTextures[map] = getTexture(map);
            if (!tevTextures[map]) allTextures = false;
        }
        // The packed untextured pixel shader is the stable path for raster
        // colour-only geometry. Keep the generated TEV path for every draw
        // that actually samples a texture; no-texture draws still use the
        // CPU TEV evaluator when they are priming a generated fixed program.
        if (allTextures && (mask || SMS_GX2_TEV_UNTEXTURED)) tevShader = tevShaderForKey(tevKey);
    }
    const bool tevPath = tevShader != nullptr;
    if (SMS_GX2_DEBUG_DUMP && !tevPath) {
        const uint32_t mask = tevTextureMask(tevKey);
        for (int debugMap = 0; debugMap < 8; ++debugMap)
            if (mask & (1u << debugMap)) tevTextures[debugMap] = getTexture(debugMap);
    }
    int coord = 0, map = 0;
    TextureEntry* texture = textured ? getTexture(usedTexture(&coord, &map) ? map : 0) : nullptr;
    // The vertex layout must follow the shader actually bound: a material that
    // samples a texture we could not load still draws through the stage-1
    // fallback (8 or 10 floats), while the TEV path uses 28 floats.
    s_perfTex += OSGetTime() - perfTex0;
    const bool useTex = !tevPath && textured && texture;
    FixedShader& shader = useTex ? s_modulate : s_untextured;
    const VertexKey vertexKey = currentVertexKey(tevPath, coord, useTex);
    // Fixed-path draws take their colour from the CPU TEV evaluator (and
    // their UVs from the manual/copy adjustments), which the generated
    // program does not reproduce, so only TEV draws move to the GPU.
    VertexProgram* vertex = s_gpuXfEnabled && tevPath && g_displayFrames >= SMS_GX2_GPU_XF_DELAY
                                ? vertexShaderForKey(vertexKey) : nullptr;
    // Let the CPU reference present one complete draw before Cemu/Cafe first
    // compiles a new generated pipeline.  Compilation is synchronous on Cemu;
    // without this priming draw the old render target remains black during
    // startup even though the CPU path already has the correct file-select
    // frame.  Each key is primed once, so steady-state draws stay GPU-only.
    const bool gpuPath = vertex != nullptr && vertex->warmed;
    applyState();
    if (tevPath) setTevShader(*tevShader, tevTextures, gpuPath ? vertex : nullptr);
    else setShader(shader, texture, gpuPath ? vertex : nullptr);
    if (gpuPath) {
        uploadXfUniformRanges();
        packVertexLiveUniforms(tevKey, tevTextures);
    }
    // Indexed draw over the batch's unique vertices: each is transformed once,
    // straight into the arena, and the GPU reuses it per index (expanding the
    // index list copied ~112 bytes per index and was the top CPU cost).  The
    // vertices, one zeroed slot for out-of-range indices and the index list
    // share ONE arena block: an overflow between two allocations would recycle
    // the vertices before their draw is submitted.
    const size_t vertexFloats = gpuPath ? kRawVertexFloats : (tevPath ? 28 : (useTex ? 10 : 8));
    const uint32_t vertexStride = gpuPath ? kRawVertexStride : uint32_t(vertexFloats * sizeof(float));
    const size_t vertexCount = s_batch.stride ? s_batch.vertices.size() / s_batch.stride : 0;
    const uint32_t indexCount = uint32_t(s_batch.indices.size());
    const uint32_t vboBytes = uint32_t((vertexCount + 1) * vertexStride);
    const uint32_t iboOffset = (vboBytes + 255) & ~255u;
    const uint32_t blockBytes = iboOffset + indexCount * uint32_t(sizeof(uint32_t));
    uint8_t* block = reinterpret_cast<uint8_t*>(allocVertices(blockBytes));
    if (!block) {
        s_batch.vertices.clear(); s_batch.indices.clear(); s_batch.count = 0;
        return;
    }
    float* vbo = reinterpret_cast<float*>(block);
    uint32_t* ibo = reinterpret_cast<uint32_t*>(block + iboOffset);
    if (gpuPath) {
        // Zero the vertex lines in cache without reading MEM2: the decoder
        // writes every vertex here, and cold write-allocate fills were the
        // top CPU cost.  Covers the sentinel slot too.  block is 256-aligned.
        for (uint32_t off = 0; off < vboBytes; off += 32)
            asm volatile("dcbz 0,%0" : : "r"(block + off) : "memory");
    } else {
        memset(block + vertexCount * vertexStride, 0, vertexStride);
    }
    OSTime perfXform0 = OSGetTime();
    HostVertex firstHv;
    float firstOut[28] = {};
    bool haveFirst = false;
    static std::vector<uint8_t> s_uniqueDone;
    s_uniqueDone.assign(vertexCount, 0);
    // Vertices without per-vertex indices use the current matrix registers.
    const uint32_t ma = g.xfReg[XFR_MATIDX_A], mb = g.xfReg[XFR_MATIDX_B];
    const uint8_t defaults[9] = {uint8_t(ma & 63), uint8_t((ma >> 6) & 63), uint8_t((ma >> 12) & 63),
                                 uint8_t((ma >> 18) & 63), uint8_t((ma >> 24) & 63), uint8_t(mb & 63),
                                 uint8_t((mb >> 6) & 63), uint8_t((mb >> 12) & 63), uint8_t((mb >> 18) & 63)};
    for (uint32_t i = 0; i < indexCount; ++i) {
        const uint32_t index = s_batch.indices[i];
        if (index >= vertexCount) {
            ibo[i] = uint32_t(vertexCount);
            continue;
        }
        ibo[i] = index;
        if (!s_uniqueDone[index]) {
            HostVertex hv;
            const uint8_t* packed = s_batch.vertices.data() + size_t(index) * s_batch.stride;
            float* out = reinterpret_cast<float*>(block + size_t(index) * vertexStride);
            HostVertex& dec = gpuPath ? *reinterpret_cast<HostVertex*>(out) : hv;
            unpackVertex(s_batch.fmt, packed, defaults, dec, gpuPath);
            if (gpuPath && s_vertexSelfCheck)
                vertexSelfCheckDraw(vertexKey, tevKey, tevPath, coord, useTex, texture, tevTextures,
                                    dec, s_debugDrawNo);
            if (gpuPath) {
                // dec already sits in the arena in the fetch layout.
#if SMS_GX2_VX_ORACLE
                const HostVertex orig = dec;
                float reference[28] = {};
                if (tevPath) transformTev(orig, reference, tevKey, tevTextures);
                else transform(orig, reference, coord, useTex, texture);
                if (SMS_GX2_VX_ORACLE != 8)
                memcpy(reinterpret_cast<RawVertex*>(out)->tex[6],
                       reference + (SMS_GX2_VX_ORACLE == 2 ? 4 : SMS_GX2_VX_ORACLE == 6 ? 24 : 0), 4 * sizeof(float));
                if (SMS_GX2_VX_ORACLE == 6)
                    memcpy(reinterpret_cast<RawVertex*>(out)->tex[4], reference + 20, 4 * sizeof(float));
#endif
            } else if (tevPath) {
                transformTev(hv, out, tevKey, tevTextures);
            } else {
                transform(hv, out, coord, useTex, texture);
            }
            s_uniqueDone[index] = 1;
            ++s_perfUnique;
            if (!haveFirst) {
                firstHv = dec;
                if (!gpuPath) memcpy(firstOut, out, vertexFloats * sizeof(float));
                haveFirst = true;
            }
        }
    }
    s_perfXform += OSGetTime() - perfXform0;
    ++s_perfDraws;
    s_perfVerts += uint32_t(s_batch.indices.size());
    if (gpuPath) {
        ++s_perfGpuDraws;
        s_perfGpuVerts += uint32_t(s_batch.indices.size());
    } else {
        ++s_perfCpuDraws;
        s_perfCpuVerts += uint32_t(s_batch.indices.size());
    }
    if (vertex && !vertex->warmed) vertex->warmed = true;
    const uint32_t debugDrawNo = s_debugDrawNo++;
#if SMS_GX2_DEBUG_PIXEL_TRACE
    if ((g_displayFrames % SMS_GX2_DEBUG_PIXEL_TRACE) == SMS_GX2_DEBUG_PIXEL_TRACE - 1 && g_displayFrames >= 1000 && tevPath && !gpuPath) {
        float lo[5] = {9, 9, 9, 1e30f, 9}, hi[5] = {-9, -9, -9, -1e30f, -9};
        uint32_t neg = 0;
        for (size_t v = 0; v < vertexCount; ++v) {
            if (!s_uniqueDone[v]) continue;
            const size_t at = v * 28;
            for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], vbo[at + 20 + c]); hi[c] = std::max(hi[c], vbo[at + 20 + c]); }
            lo[3] = std::min(lo[3], vbo[at + 3]); hi[3] = std::max(hi[3], vbo[at + 3]);
            if (vbo[at + 3] <= 0.0f) ++neg;
        }
        WHBLogPrintf("SMS Wii U: DRAWSTAT frame=%u draw=%u st=%u ce0=%06x ae0=%06x ord0=%03x cm=%08x z=%02x xf5=%u n=%u ras0=(%.3f-%.3f,%.3f-%.3f,%.3f-%.3f) w=%.1f..%.1f neg=%u",
                     (unsigned)g_displayFrames, (unsigned)debugDrawNo, (unsigned)tevKey.stages,
                     (unsigned)tevKey.colorEnv[0], (unsigned)tevKey.alphaEnv[0], (unsigned)tevKey.order[0],
                     (unsigned)g.bp[BP_CMODE0], (unsigned)(g.bp[BP_ZMODE] & 0xff), (unsigned)g.xfReg[0x05],
                     (unsigned)indexCount, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3], (unsigned)neg);
    }
#endif
    debugDraw(tevKey, debugDrawNo, tevPath, map, texture, tevTextures,
              s_batch.count, haveFirst ? &firstHv : nullptr, firstOut);
    if (SMS_GX2_DEBUG_DRAW_LIMIT && debugDrawNo >= SMS_GX2_DEBUG_DRAW_LIMIT) {
        s_batch.vertices.clear(); s_batch.indices.clear(); s_batch.count = 0;
        return;
    }
    // Only the CPU cache per draw: the GPU's vertex cache was invalidated for
    // the whole arena when the frame began (a GPU invalidate per draw stalls
    // the pipeline).
    DCFlushRange(block, blockBytes);
    GX2SetAttribBuffer(0, vboBytes, gpuPath ? vertex->stride : (tevPath ? vertexStride : shader.stride), vbo);
    const GX2PrimitiveMode mode = s_batch.cls == PRIM_LINES ? GX2_PRIMITIVE_MODE_LINES :
                                  s_batch.cls == PRIM_POINTS ? GX2_PRIMITIVE_MODE_POINTS : GX2_PRIMITIVE_MODE_TRIANGLES;
    const bool bracket = SMS_GX2_VX_BRACKET && gpuPath && !vertex->bracketed;
    if (bracket) {
        // Debug: name each generated program before its first draw and wait
        // for the GPU after it, so a hang leaves that program as the last line.
        vertex->bracketed = true;
        const VertexKey& k = vertex->key;
        WHBLogPrintf("SMS Wii U: [vxbracket] first draw alu=%u bytes=%u chans=%u cc=%08x/%08x ac=%08x/%08x tg=%08x/%08x/%08x/%08x n=%u dual=%u post=%03x ortho=%u",
                     (unsigned)vertex->aluCount, (unsigned)vertex->vertex.size, (unsigned)k.numChans,
                     (unsigned)k.colorCtrl[0], (unsigned)k.colorCtrl[1], (unsigned)k.alphaCtrl[0], (unsigned)k.alphaCtrl[1],
                     (unsigned)k.texgen[0], (unsigned)k.texgen[1], (unsigned)k.texgen[2], (unsigned)k.texgen[3],
                     (unsigned)k.numTexGens, (unsigned)k.dualTex, (unsigned)k.postTex[0], (unsigned)k.projectionOrtho);
    }
    GX2DrawIndexedEx(mode, indexCount, GX2_INDEX_TYPE_U32, ibo, 0, 1);
    if (bracket) {
        GX2DrawDone();
        WHBLogPrintf("SMS Wii U: [vxbracket] done");
    }
    ++s_drawsThisFrame;
    {
        GpuDrawInfo info = {};
        info.verts = uint32_t(s_batch.indices.size());
        info.stages = tevPath ? tevKey.stages : 0;
        info.texMask = tevTextureMask(tevKey);
        info.blend = g.bp[BP_CMODE0];
        info.zmode = g.bp[BP_ZMODE] & 0xff;
        info.drawNo = debugDrawNo;
        TextureEntry* t0 = texture;
        for (int m = 0; m < 8 && !t0; ++m) t0 = tevTextures[m];
        if (t0) { info.texW = t0->width; info.texH = t0->height; }
        // Screen-space bounding box of the draw (clip -> TV pixels).
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (size_t v = 0; v < vertexCount && !gpuPath; ++v) {
            if (!s_uniqueDone[v]) continue;
            const size_t at = v * vertexFloats;
            const float w = vbo[at + 3];
            if (w <= 0.00001f) continue;
            const float x = vbo[at] / w, y = vbo[at + 1] / w;
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
        if (x1 > x0 && y1 > y0) {
            const float cx0 = std::max(x0, -1.0f), cx1 = std::min(x1, 1.0f);
            const float cy0 = std::max(y0, -1.0f), cy1 = std::min(y1, 1.0f);
            if (cx1 > cx0 && cy1 > cy0)
                info.area = uint32_t((cx1 - cx0) * 0.5f * s_tvW * (cy1 - cy0) * 0.5f * s_tvH);
        }
        gpuStamp(s_vertexArenaIndex, &info);
    }
#if SMS_GX2_DEBUG_PIXEL_TRACE
    debugPixelTrace(debugDrawNo);
#endif
    s_batch.vertices.clear(); s_batch.indices.clear(); s_batch.count = 0;
}

// GX clears the EFB as part of a copy. Without it the depth of earlier passes
// (shadow and reflection renders) rejected the whole main view. A display copy
// is presented from the TV target, so its clear is deferred until next frame
// start rather than being applied to the image that is about to be presented.
static float s_frameClear[4];
static bool s_frameClearPending = false;
static void rebindTvTarget() {
    GX2SetContextState(WHBGfxGetTVContextState()); invalidateVertexUniforms();
    GX2SetColorBuffer(renderColour(), GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(renderDepth());
}
void executeCopy(uint32_t ctrl) {
    SMS_PORT_HEAP_SCOPE();
    flushBatch();
    const bool clear = (ctrl >> 11) & 1, disp = (ctrl >> 14) & 1;
    const uint32_t src = g.bp[BP_COPY_SRC_TL], size = g.bp[BP_COPY_SRC_WH];
    const uint32_t x = src & 0x3ff, y = (src >> 10) & 0x3ff;
    const uint32_t w = (size & 0x3ff) + 1, h = ((size >> 10) & 0x3ff) + 1;
    debugCopy(disp, x, y, w, h, g.copyDest, ctrl);
    if (disp) {
        // A display copy with clear resets the EFB for the next frame.  The
        // colour buffer is the one presented, so the clear is deferred to the
        // start of the next frame instead of being applied here.
        if (clear) {
            const uint32_t ar = g.bp[BP_CLEAR_AR], gb = g.bp[BP_CLEAR_GB];
            s_frameClear[0] = float(ar & 255) / 255.0f;
            s_frameClear[1] = float((gb >> 8) & 255) / 255.0f;
            s_frameClear[2] = float(gb & 255) / 255.0f;
            s_frameClear[3] = ((g.bp[BP_PE_CONTROL] & 7) == 1) ? float((ar >> 8) & 255) / 255.0f : 1.0f;
            s_frameClearPending = ((g.bp[BP_CMODE0] >> 3) & 1) != 0;
        }
#if SMS_GX2_DEBUG_PIXEL_TRACE
        if ((g_displayFrames % SMS_GX2_DEBUG_PIXEL_TRACE) >= SMS_GX2_DEBUG_PIXEL_TRACE - 2 && g_displayFrames >= 1000)
            WHBLogPrintf("SMS Wii U: DISPCOPY frame=%u ctrl=%08x clear=%d cm=%08x pe=%08x ar=%08x gb=%08x z=%08x zmode=%08x pending=%d",
                         (unsigned)g_displayFrames, (unsigned)ctrl, (int)clear, (unsigned)g.bp[BP_CMODE0],
                         (unsigned)g.bp[BP_PE_CONTROL], (unsigned)g.bp[BP_CLEAR_AR], (unsigned)g.bp[BP_CLEAR_GB],
                         (unsigned)g.bp[BP_CLEAR_Z], (unsigned)g.bp[BP_ZMODE], (int)s_frameClearPending);
#endif
        return;
    }
    if (!s_frameActive || !ProcUIInForeground()) return;
    {
        copyToTexture(g.copyDest, x, y, w, h, (ctrl >> 9) & 1,
                      ((ctrl >> 3) & 1) << 3 | ((ctrl >> 4) & 7));
        rebindTvTarget();  // GX2CopySurface may leave its destination bound
    }
    if (!clear) return;
    const uint32_t ar = g.bp[BP_CLEAR_AR], gb = g.bp[BP_CLEAR_GB], z = g.bp[BP_CLEAR_Z];
    if ((g.bp[BP_CMODE0] >> 3) & 1)
        ++s_perfClears;
        GX2ClearColor(renderColour(), float(ar & 255) / 255.0f, float((gb >> 8) & 255) / 255.0f,
                      float(gb & 255) / 255.0f,
                      ((g.bp[BP_PE_CONTROL] & 7) == 1) ? float((ar >> 8) & 255) / 255.0f : 1.0f);
    if ((g.bp[BP_ZMODE] >> 4) & 1)
        GX2ClearDepthStencilEx(renderDepth(), float(z & 0xffffff) / 16777215.0f, 0,
                               GX2_CLEAR_FLAGS_DEPTH);
    rebindTvTarget();
}
void markXfMemDirty() {
    SMS_PORT_HEAP_SCOPE();
    if (!s_batch.indices.empty()) flushBatch();
    s_xfMemoryDirty = true;
}
// GXPeekARGB: the game reads the EFB back through the CPU. Sunshine's Mario
// occlusion probe draws a cube that writes destination alpha 0x10 and peeks
// Mario's screen position from a draw-sync callback; a stub answer made the
// game believe Mario was always hidden and draw his silhouette every frame.
//
// The TV target is tiled, so the GPU copies it into a LINEAR_ALIGNED surface
// of the same size, and the texel is read after the GPU has finished; only
// the cache lines read are invalidated.
//
// Under Cemu that copy never reaches guest memory (the texel read 0). Cemu
// performs a LINEAR_SPECIAL copy (which GX2 runs on the CPU) synchronously
// from its texture cache, so the emulator reads through one more copy into
// such a surface, issued after the GPU copy (alone it also read 0). On the
// console the special copy would make the CPU detile the whole target for
// every peek, so it is not used there.
static GX2Surface s_peekSurface, s_peekSpecial;
static bool s_peekSurfaceReady;
static bool readTvPixels(const int (*points)[2], uint32_t count, uint32_t* out) {
    ++s_perfPeeks;
    GX2ColorBuffer* cb = renderColour();
    if (!s_ready || !s_frameActive || !ProcUIInForeground() || !cb) return false;
    if (cb->surface.format != GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8 || cb->surface.aa != GX2_AA_MODE1X) return false;
    if (s_peekSurfaceReady && (s_peekSurface.width != cb->surface.width ||
                               s_peekSurface.height != cb->surface.height)) {
        MEMFreeToDefaultHeap(s_peekSurface.image);
        if (s_peekSpecial.image) MEMFreeToDefaultHeap(s_peekSpecial.image);
        s_peekSpecial.image = nullptr;
        s_peekSurfaceReady = false;
    }
    const bool cemu = runningUnderCemu();
    if (!s_peekSurfaceReady) {
        memset(&s_peekSurface, 0, sizeof(s_peekSurface));
        s_peekSurface.use = GX2SurfaceUse(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER);
        s_peekSurface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        s_peekSurface.width = cb->surface.width;
        s_peekSurface.height = cb->surface.height;
        s_peekSurface.depth = 1;
        s_peekSurface.mipLevels = 1;
        s_peekSurface.format = cb->surface.format;
        s_peekSurface.aa = GX2_AA_MODE1X;
        s_peekSurface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
        GX2CalcSurfaceSizeAndAlignment(&s_peekSurface);
        s_peekSurface.image = alignedZero(s_peekSurface.alignment, s_peekSurface.imageSize);
        if (!s_peekSurface.image) return false;
        // The zero fill must not be written back over the GPU's copy later.
        DCFlushRange(s_peekSurface.image, s_peekSurface.imageSize);
        if (cemu) {
            s_peekSpecial = s_peekSurface;
            s_peekSpecial.image = nullptr;
            s_peekSpecial.tileMode = GX2_TILE_MODE_LINEAR_SPECIAL;
            GX2CalcSurfaceSizeAndAlignment(&s_peekSpecial);
            s_peekSpecial.image = alignedZero(s_peekSpecial.alignment, s_peekSpecial.imageSize);
            if (!s_peekSpecial.image) { MEMFreeToDefaultHeap(s_peekSurface.image); return false; }
        }
        s_peekSurfaceReady = true;
    }
    if (!cemu && SMS_GX2_ASYNC_PEEK) {
        // Console: answer from the copy queued by the previous peek (one
        // frame old, finished long ago) and queue this frame's copy without
        // draining the GPU.  The synchronous version stalled the CPU on the
        // whole frame's GPU work twice per peek.
        static uint64_t s_peekStamp;
        static bool s_peekQueued;
        const bool have = s_peekQueued;
        if (have) {
            GX2WaitTimeStamp(s_peekStamp);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t px = uint32_t(s_gameX + (float(points[i][0]) + 0.5f) * s_gameScale);
                uint32_t py = uint32_t((float(points[i][1]) + 0.5f) * s_gameScale);
                if (px >= s_peekSurface.width) px = s_peekSurface.width - 1;
                if (py >= s_peekSurface.height) py = s_peekSurface.height - 1;
                uint8_t* texel = static_cast<uint8_t*>(s_peekSurface.image) + (size_t(py) * s_peekSurface.pitch + px) * 4;
                DCInvalidateRange(texel, 4);
                out[i] = uint32_t(texel[3]) << 24 | uint32_t(texel[0]) << 16 | uint32_t(texel[1]) << 8 | texel[2];
            }
        }
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb->surface.image, cb->surface.imageSize);
        GX2CopySurface(&cb->surface, 0, 0, &s_peekSurface, 0, 0); invalidateVertexUniforms();
        rebindTvTarget();
        GX2Flush();
        s_peekStamp = GX2GetLastSubmittedTimeStamp();
        s_peekQueued = true;
        return have;
    }
    // Make every queued draw land in memory before the copy reads the target.
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb->surface.image, cb->surface.imageSize);
    GX2DrawDone();
    GX2CopySurface(&cb->surface, 0, 0, &s_peekSurface, 0, 0); invalidateVertexUniforms();
    GX2DrawDone();
    if (cemu) GX2CopySurface(&cb->surface, 0, 0, &s_peekSpecial, 0, 0); invalidateVertexUniforms();
    rebindTvTarget();
    const GX2Surface& linear = cemu ? s_peekSpecial : s_peekSurface;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t px = uint32_t(s_gameX + (float(points[i][0]) + 0.5f) * s_gameScale);
        uint32_t py = uint32_t((float(points[i][1]) + 0.5f) * s_gameScale);
        if (px >= linear.width) px = linear.width - 1;
        if (py >= linear.height) py = linear.height - 1;
        uint8_t* texel = static_cast<uint8_t*>(linear.image) + (size_t(py) * linear.pitch + px) * 4;
        DCInvalidateRange(texel, 4);
        out[i] = uint32_t(texel[3]) << 24 | uint32_t(texel[0]) << 16 | uint32_t(texel[1]) << 8 | texel[2];
    }
    return true;
}

uint32_t peekColor(int x, int y) {
    SMS_PORT_HEAP_SCOPE();
    flushBatch();
    if (x < 0 || y < 0) return 0;
    const int point[1][2] = {{x, y}};
    uint32_t c = 0;
    if (!readTvPixels(point, 1, &c)) return 0;
#if SMS_GX2_DEBUG_PIXEL_TRACE
    static uint32_t n;
    if (n++ < 20 || (g_displayFrames % 50) == 0)
        WHBLogPrintf("SMS Wii U: PEEK frame=%u (%d,%d) = %08x", (unsigned)g_displayFrames, x, y, (unsigned)c);
#endif
    return c;
}

#if SMS_GX2_DEBUG_PIXEL_TRACE
// Debug: the TV target's ARGB at a few game-space points after every draw of
// one frame in SMS_GX2_DEBUG_PIXEL_TRACE, logged when a value changes.
static void debugPixelTrace(uint32_t draw) {
    if ((g_displayFrames % SMS_GX2_DEBUG_PIXEL_TRACE) != 0 || g_displayFrames < 1000) return;
    static const int points[5][2] = {{330, 250}, {320, 313}, {100, 330}, {320, 30}, {185, 355}};
    static uint32_t last[5];
    uint32_t v[5];
    if (draw == 0) memset(last, 0xff, sizeof(last));
    if (!readTvPixels(points, 5, v)) return;
    if (!memcmp(v, last, sizeof(v))) return;
    memcpy(last, v, sizeof(v));
    WHBLogPrintf("SMS Wii U: PIXTRACE frame=%u draw=%u hz=%08x seaA=%08x seaC=%08x top=%08x mario=%08x",
                 (unsigned)g_displayFrames, (unsigned)draw, (unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3],
                 (unsigned)v[4]);
}
#endif
uint32_t peekZ(int, int) { return 0xffffff; }
void pixMetricClear() {}
uint32_t pixMetricRead() { return 0; }

void textureInvalidateAll() {}
void textureInvalidateRange(const void*, uint32_t) {}
void textureCpuWrote(const void*, uint32_t) {}

void* physToPtr(uint32_t);
uint32_t ptrToPhys(const void*);

FILE* traceFile() { return nullptr; }
void traceNoteBegin() {}
void traceNoteProjection(const float*) {}
uint32_t g_traceLastVat = 0;

extern "C" int sms_wiiu_gx2_renderer_init(void) {
    rendererInit(1.0f);
    return s_ready ? 1 : 0;
}
extern "C" void sms_wiiu_gx2_renderer_begin(void) {
    SMS_PORT_HEAP_SCOPE();
    if (!s_ready || !ProcUIInForeground()) return;
    beginVertexArena();
    GX2SetContextState(WHBGfxGetTVContextState()); invalidateVertexUniforms();
    // The generated vertex programs use up to R60; the default split gives
    // vertex shaders 48 GPRs and asking for more hangs the GPU on hardware
    // (Cemu does not enforce it).  64 + 184 keeps the default 248 total; the
    // TEV pixel shaders need 44.  Set inside the TV context so it is shadowed.
    GX2SetShaderModeEx(GX2_SHADER_MODE_UNIFORM_REGISTER, 64, 64, 0, 0, 184, 192);
    GX2SetColorBuffer(renderColour(), GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(renderDepth());
    GX2ClearDepthStencilEx(renderDepth(), 1.0f, 0, GX2_CLEAR_FLAGS_DEPTH); invalidateVertexUniforms();
    GX2SetContextState(WHBGfxGetTVContextState()); invalidateVertexUniforms();
    GX2SetColorBuffer(renderColour(), GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(renderDepth());
    updateTarget();
    if (s_frameClearPending) {
        GX2ClearColor(renderColour(), s_frameClear[0], s_frameClear[1], s_frameClear[2], s_frameClear[3]); invalidateVertexUniforms();
        s_frameClearPending = false;
        GX2SetContextState(WHBGfxGetTVContextState()); invalidateVertexUniforms();
        GX2SetColorBuffer(renderColour(), GX2_RENDER_TARGET_0);
        GX2SetDepthBuffer(renderDepth());
    }
    s_debugDrawNo = 0;
    GX2SetViewport(0.0f, 0.0f, s_tvW, s_tvH, 0.0f, 1.0f);
    GX2SetScissor(0, 0, uint32_t(s_tvW), uint32_t(s_tvH));
    s_frameActive = true;
#if SMS_GX2_DEBUG_PIXEL_TRACE
    if ((g_displayFrames % SMS_GX2_DEBUG_PIXEL_TRACE) >= SMS_GX2_DEBUG_PIXEL_TRACE - 2 && g_displayFrames >= 1000) {
        static const int points[4][2] = {{330, 250}, {320, 313}, {100, 330}, {320, 30}};
        uint32_t v[4] = {};
        readTvPixels(points, 4, v);
        WHBLogPrintf("SMS Wii U: BEGIN frame=%u hz=%08x seaA=%08x seaC=%08x top=%08x", (unsigned)g_displayFrames,
                     (unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3]);
    }
#endif
    GX2SetRasterizerClipControlHalfZ(TRUE, g.xfReg[0x05] == 0, TRUE);
}
// Sunshine runs at 30 fps and waits two retraces per game frame; presenting at
// the second one showed a freshly cleared, empty frame half the time.
extern "C" int sms_wiiu_gx2_renderer_has_draws(void) {
    if (!s_batch.indices.empty()) return 1;
    return s_drawsThisFrame != 0;
}
extern "C" void sms_wiiu_gx2_renderer_end(void) {
    SMS_PORT_HEAP_SCOPE();
    if (!s_frameActive) return;
    flushBatch();
    GX2Flush();
    endVertexArena();
    static uint32_t s_arenaPeak;
    s_arenaPeak = std::max(s_arenaPeak, s_vertexArenaUsed);
    const OSTime perfNow = OSGetTime();
    if (s_perfLastEnd) s_perfWall += perfNow - s_perfLastEnd;
    s_perfLastEnd = perfNow;
    if (++s_perfFrames == 30) {
        const auto ms = [](OSTime t) { return unsigned(OSTicksToMicroseconds(t) / 30 / 100); };
        WHBLogPrintf("SMS Wii U: [perf] frame=%u per-frame x0.1ms wall=%u fence=%u flush=%u tex=%u xform=%u draws=%u verts=%u unique=%u gpuDraws=%u cpuDraws=%u gpuVerts=%u cpuVerts=%u rejected=%u vxSamples=%u vxMismatches=%u arenaPeak=%u overflows=%u copies=%u peeks=%u clears=%u",
                     (unsigned)g_displayFrames, ms(s_perfWall), ms(s_vertexFenceWait), ms(s_perfFlush), ms(s_perfTex), ms(s_perfXform),
                     (unsigned)(s_perfDraws / 30), (unsigned)(s_perfVerts / 30), (unsigned)(s_perfUnique / 30),
                     (unsigned)(s_perfGpuDraws / 30), (unsigned)(s_perfCpuDraws / 30),
                     (unsigned)(s_perfGpuVerts / 30), (unsigned)(s_perfCpuVerts / 30),
                     (unsigned)s_vertexRejected,
                     (unsigned)s_vertexSelfCheckSamples, (unsigned)s_vertexSelfCheckMismatches,
                     (unsigned)s_arenaPeak,
                     (unsigned)s_vertexArenaOverflows,
                     (unsigned)s_perfCopies, (unsigned)s_perfPeeks, (unsigned)s_perfClears);
        s_perfCopies = s_perfPeeks = s_perfClears = 0;
        static uint32_t s_gpuReportEvery;
        if (++s_gpuReportEvery >= 4) { s_gpuReportEvery = 0; gpuReport(); }
        s_vertexFenceWait = 0;
        s_perfWall = s_perfFlush = s_perfTex = s_perfXform = 0;
        s_perfDraws = s_perfVerts = s_perfUnique = s_perfFrames = 0;
        s_perfGpuDraws = s_perfCpuDraws = s_perfGpuVerts = s_perfCpuVerts = 0;
    }
    s_frameActive = false;
    s_drawsThisFrame = 0;
    ++g_displayFrames;
}

} // namespace gx
