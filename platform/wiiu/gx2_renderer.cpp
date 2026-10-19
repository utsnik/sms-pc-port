#include "port_heap.h"
#include <cstdio>
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
#include <whb/gfx.h>
#include <proc_ui/procui.h>
#include <whb/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <initializer_list>
#include <vector>

namespace gx {

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
static bool initTevVertex();
#if SMS_GX2_DEBUG_PIXEL_TRACE
static void debugPixelTrace(uint32_t draw);
#endif

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
};

struct TevShader {
    TevKey key;
    GX2PixelShader pixel;
    GX2UniformVar uniform;
    GX2SamplerVar samplers[8];
    void* psProgram;
    uint32_t textureMask;
};

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
    uint32_t fmt = 0, width = 0, height = 0, tlut = 0, tlutFmt = 0;
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
// mip filter. Only level 0 is uploaded, so mipmapping is off.
static void initSampler(GX2Sampler* sampler, uint32_t mode0) {
    GX2InitSampler(sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSamplerClamping(sampler, clampMode(mode0 & 3), clampMode((mode0 >> 2) & 3),
                           GX2_TEX_CLAMP_MODE_WRAP);
    const GX2TexXYFilterMode mag = ((mode0 >> 4) & 1) ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT;
    const GX2TexXYFilterMode min = ((mode0 >> 7) & 1) ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT;
    GX2InitSamplerXYFilter(sampler, mag, min, GX2_TEX_ANISO_RATIO_NONE);
    GX2InitSamplerZMFilter(sampler, GX2_TEX_Z_FILTER_MODE_NONE, GX2_TEX_MIP_FILTER_MODE_NONE);
}

static int tevSext11(uint32_t v) { return int(v << 21) >> 21; }

static uint32_t tevKsel(uint32_t stage) {
    const uint32_t r = g.bp[BP_TEV_KSEL + (stage >> 1)];
    return (r >> ((stage & 1) ? 14 : 4)) & 0x3ff;
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
    return k;
}

static uint32_t tevTextureMask(const TevKey& k) {
    uint32_t mask = 0;
    for (uint32_t s = 0; s < k.stages; ++s)
        if ((k.order[s] >> 6) & 1) mask |= 1u << (k.order[s] & 7);
    return mask;
}

static bool tevSupported(const TevKey& k) {
    // Indirect texturing changes the coordinates between stages and remains
    // on the old path until its Latte implementation is added.
    if (((k.genmode >> 16) & 7) != 0) return false;
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
// GPRs: R1-8 texcoords, R9-10 raster colours, R11 a stage's staged result,
// R12-15 the TEV registers, R16-19 temporaries, R20+ the stage textures,
// R36-39 8-bit views of R12-15 for the current stage, R40-43 constant-file
// operands copied to GPRs: the constant file has one read port per channel, so
// one instruction must not read the same channel of two different constants.
static const uint32_t kTevMaskBase = 36;
static const uint32_t kTevGprs = 44;

struct TevStageCtx {
    const TevKey* key;
    uint32_t stage;
    int texStage;  // the stage whose texture TEXC/TEXA read (-1: none yet)
};

static TevSource tevColorSource(const TevStageCtx& cx, uint32_t sel, uint32_t channel, bool masked) {
    const TevKey& k = *cx.key;
    const uint32_t stage = cx.stage;
    if (sel < 8) return {_R((masked ? kTevMaskBase : 12) + (sel >> 1)), (sel & 1) ? uint32_t(_w) : channel};
    const uint32_t order = k.order[stage];
    const uint32_t rasTable = k.alphaEnv[stage] & 3;
    if (sel == 8 || sel == 9) {
        // genFS keeps the last sampled texture for a stage without one.
        if (cx.texStage < 0) return tevFixed(12);
        const uint32_t texTable = (k.alphaEnv[cx.texStage] >> 2) & 3;
        const uint32_t ch = tevSwapChannel(k, texTable, sel == 8 ? channel : 3);
        return {_R(20 + uint32_t(cx.texStage)), ch};
    }
    if (sel == 10 || sel == 11) {
        const uint32_t chan = (order >> 7) & 7;
        if (chan > 1) return {ALU_SRC_0, 0};
        const uint32_t ch = tevSwapChannel(k, rasTable, sel == 10 ? channel : 3);
        return {_R(9 + chan), ch};
    }
    if (sel == 12) return tevFixed(12);
    if (sel == 13) return tevFixed(13);
    if (sel == 14) return tevKonst(k, k.ksel[stage] & 31, channel);
    return tevFixed(15);
}

static TevSource tevAlphaSource(const TevStageCtx& cx, uint32_t sel, bool masked) {
    const TevKey& k = *cx.key;
    const uint32_t stage = cx.stage;
    if (sel < 4) return {_R((masked ? kTevMaskBase : 12) + sel), _w};
    const uint32_t order = k.order[stage];
    const uint32_t rasTable = k.alphaEnv[stage] & 3;
    if (sel == 4) {
        if (cx.texStage < 0) return tevFixed(12);
        const uint32_t texTable = (k.alphaEnv[cx.texStage] >> 2) & 3;
        return {_R(20 + uint32_t(cx.texStage)), tevSwapChannel(k, texTable, 3)};
    }
    if (sel == 5) {
        const uint32_t chan = (order >> 7) & 7;
        if (chan > 1) return {ALU_SRC_0, 0};
        return {_R(9 + chan), tevSwapChannel(k, rasTable, 3)};
    }
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

// dst.ch = x & 255 for an integer x in -1024..1023: x - 256 * floor(x / 256),
// through the temporary tmp.ch.
static void tevEmitMask8(std::vector<uint64_t>& program, uint32_t dst, uint32_t ch,
                         TevSource x, uint32_t tmp) {
    tevEmitAlu(program, {ALU_MUL(_R(tmp), ch, x.sel, x.chan, _GX2_C(12), _y)});
    tevEmitAlu(program, {ALU_FLOOR(_R(tmp), ch, _R(tmp), ch)});
    tevEmitAlu(program, {ALU_MUL(_R(tmp), ch, _R(tmp), ch, _GX2_C(12), _x)});
    tevEmitAlu(program, {ALU_ADD(_R(dst), ch, x.sel, x.chan, _R(tmp) _NEG, ch)});
}

// dst.ch = 1.0 or 0.0 for a > b / a == b.
static bool tevIsConst(TevSource s) { return s.sel >= ALU_SRC_CONST_FILE_BASE; }
static TevSource tevToGpr(std::vector<uint64_t>& program, TevSource s, uint32_t gpr, uint32_t ch) {
    if (!tevIsConst(s)) return s;
    tevEmitAlu(program, {ALU_MOV(_R(gpr), ch, s.sel, s.chan)});
    return {_R(gpr), ch};
}

static void tevEmitCompareOp(std::vector<uint64_t>& program, uint32_t dst, uint32_t dstCh, bool equal,
                             TevSource a, TevSource b) {
    if (tevIsConst(a) && tevIsConst(b)) b = tevToGpr(program, b, 43, dstCh);
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

// R16.channel = the compare condition (0 or 1) of mode (scale << 1 | op).
static void tevEmitTevCompare(std::vector<uint64_t>& program, uint32_t channel,
                              const TevCompareSrc& cs, const TevCompareSrc& colorCs, uint32_t mode, bool scalar) {
    // Alpha compares of the 16/24-bit kinds read the stage's colour A/B
    // arguments (as genFS does); only the per-component kind reads alpha.
    const TevCompareSrc& src = (scalar && mode < 6) ? colorCs : cs;
    auto A = [&](uint32_t comp) { return src.get(false, comp); };
    auto B = [&](uint32_t comp) { return src.get(true, comp); };
    if (mode < 2 || mode >= 6) {
        // 8-bit forms compare R for every channel; the per-component forms
        // compare the channel itself (alpha for the alpha channel).
        const uint32_t component = mode < 2 ? 0 : channel;
        tevEmitCompareOp(program, 16, channel, (mode & 1) != 0, A(component), B(component));
        return;
    }

    const bool equal = (mode & 1) != 0;
    const uint32_t low = 0, mid = 1, high = mode >= 4 ? 2 : 1;
    if (equal) {
        tevEmitCompareOp(program, 17, _x, true, A(high), B(high));
        tevEmitCompareOp(program, 18, _x, true, A(mode >= 4 ? mid : low), B(mode >= 4 ? mid : low));
        tevEmitAlu(program, {ALU_MIN(_R(17), _x, _R(17), _x, _R(18), _x)});
        if (mode >= 4) {
            tevEmitCompareOp(program, 18, _x, true, A(low), B(low));
            tevEmitAlu(program, {ALU_MIN(_R(17), _x, _R(17), _x, _R(18), _x)});
        }
    } else {
        tevEmitCompareOp(program, 17, _x, false, A(high), B(high));
        tevEmitCompareOp(program, 18, _x, true, A(high), B(high));
        if (mode >= 4)
            tevEmitAlu(program, {ALU_MOV(_R(16), _x, _R(18), _x)});
        tevEmitCompareOp(program, 19, _x, false, A(mode >= 4 ? mid : low), B(mode >= 4 ? mid : low));
        tevEmitAlu(program, {ALU_MUL(_R(19), _x, mode >= 4 ? _R(16) : _R(18), _x, _R(19), _x)});
        tevEmitAlu(program, {ALU_MAX(_R(17), _x, _R(17), _x, _R(19), _x)});
        if (mode >= 4) {
            tevEmitCompareOp(program, 18, _x, true, A(mid), B(mid));
            tevEmitAlu(program, {ALU_MUL(_R(16), _x, _R(16), _x, _R(18), _x)});
            tevEmitCompareOp(program, 19, _x, false, A(low), B(low));
            tevEmitAlu(program, {ALU_MUL(_R(19), _x, _R(16), _x, _R(19), _x)});
            tevEmitAlu(program, {ALU_MAX(_R(17), _x, _R(17), _x, _R(19), _x)});
        }
    }
    tevEmitAlu(program, {ALU_MOV(_R(16), channel, _R(17), _x)});
}

// One channel of a TEV stage into R(dest).channel, in genFS's integer maths:
//   regular: D +/- lerp(A, B, C) (+/-128), then *2, *4 or >>1
//            lerp = (A * (256 - C') + B * C' + 128) >> 8, C' = C + (C >> 7)
//   compare: D + (cond ? C : 0)
// then clamped to 0..255 or to -1024..1023. A, B, C are 8-bit sources.
static void tevEmitChannel(std::vector<uint64_t>& program, uint32_t dest, uint32_t channel,
                           TevSource a, TevSource b, TevSource c, TevSource d,
                           uint32_t bias, uint32_t op, uint32_t scale, bool clamp,
                           const TevCompareSrc& cs, const TevCompareSrc& colorCs) {
    const uint32_t ch = channel;
    a = tevToGpr(program, a, 40, ch);
    b = tevToGpr(program, b, 41, ch);
    c = tevToGpr(program, c, 42, ch);
    if (bias == 3) {
        tevEmitTevCompare(program, ch, cs, colorCs, (scale << 1) | op, ch == _w);
        tevEmitAlu(program, {ALU_MUL(_R(16), ch, _R(16), ch, c.sel, c.chan)});
        tevEmitAlu(program, {ALU_ADD(_R(17), ch, d.sel, d.chan, _R(16), ch)});
    } else {
        // R16 = C' = C + floor(C / 128)
        tevEmitAlu(program, {ALU_MUL(_R(16), ch, c.sel, c.chan, _GX2_C(13), _y)});
        tevEmitAlu(program, {ALU_FLOOR(_R(16), ch, _R(16), ch)});
        tevEmitAlu(program, {ALU_ADD(_R(16), ch, _R(16), ch, c.sel, c.chan)});
        // R17 = (B - A) * C' + A * 256 + 128 = A * (256 - C') + B * C' + 128
        tevEmitAlu(program, {ALU_ADD(_R(17), ch, b.sel, b.chan, a.sel _NEG, a.chan)});
        tevEmitAlu(program, {ALU_MUL(_R(17), ch, _R(17), ch, _R(16), ch)});
        tevEmitAlu(program, {ALU_MUL(_R(18), ch, a.sel, a.chan, _GX2_C(12), _x)});
        tevEmitAlu(program, {ALU_ADD(_R(17), ch, _R(17), ch, _R(18), ch)});
        tevEmitAlu(program, {ALU_ADD(_R(17), ch, _R(17), ch, _GX2_C(12), _z)});
        // R17 = floor(R17 / 256), the lerp
        tevEmitAlu(program, {ALU_MUL(_R(17), ch, _R(17), ch, _GX2_C(12), _y)});
        tevEmitAlu(program, {ALU_FLOOR(_R(17), ch, _R(17), ch)});
        tevEmitAlu(program, {op ? ALU_ADD(_R(17), ch, d.sel, d.chan, _R(17) _NEG, ch)
                                : ALU_ADD(_R(17), ch, d.sel, d.chan, _R(17), ch)});
        if (bias == 1) tevEmitAlu(program, {ALU_ADD(_R(17), ch, _R(17), ch, _GX2_C(12), _z)});
        else if (bias == 2) tevEmitAlu(program, {ALU_ADD(_R(17), ch, _R(17), ch, _GX2_C(14), _x)});
        if (scale == 1) tevEmitAlu(program, {ALU_MUL_x2(_R(17), ch, _R(17), ch, ALU_SRC_1, _x)});
        else if (scale == 2) tevEmitAlu(program, {ALU_MUL_x4(_R(17), ch, _R(17), ch, ALU_SRC_1, _x)});
        else if (scale == 3) {
            tevEmitAlu(program, {ALU_MUL(_R(17), ch, _R(17), ch, ALU_SRC_0_5, _x)});
            tevEmitAlu(program, {ALU_FLOOR(_R(17), ch, _R(17), ch)});
        }
    }
    if (clamp) {
        tevEmitAlu(program, {ALU_MAX(_R(17), ch, _R(17), ch, ALU_SRC_0, _x)});
        tevEmitAlu(program, {ALU_MIN(_R(dest), ch, _R(17), ch, _GX2_C(12), _w)});
    } else {
        tevEmitAlu(program, {ALU_MAX(_R(17), ch, _R(17), ch, _GX2_C(13), _z)});
        tevEmitAlu(program, {ALU_MIN(_R(dest), ch, _R(17), ch, _GX2_C(13), _w)});
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

    tevEmitCompare(program, 16, func0, ref0, alphaSel, alphaChan);
    tevEmitCompare(program, 17, func1, ref1, alphaSel, alphaChan);
    switch (op) {
    case 0: // AND
        tevEmitAlu(program, {ALU_MIN(_R(16), _x, _R(16), _x, _R(17), _x)});
        break;
    case 1: // OR
        tevEmitAlu(program, {ALU_MAX(_R(16), _x, _R(16), _x, _R(17), _x)});
        break;
    case 2: // XOR: the sum is one exactly when one input is true.
        tevEmitAlu(program, {ALU_ADD(_R(18), _x, _R(16), _x, _R(17), _x)});
        tevEmitAlu(program, {ALU_SETE(_R(16), _x, _R(18), _x, ALU_SRC_1, _x)});
        break;
    default: // XNOR
        tevEmitAlu(program, {ALU_ADD(_R(18), _x, _R(16), _x, _R(17), _x)});
        tevEmitAlu(program, {ALU_SETE(_R(16), _x, _R(18), _x, ALU_SRC_1, _x)});
        tevEmitAlu(program, {ALU_ADD(_R(16), _x, ALU_SRC_1, _x, _R(16) _NEG, _x)});
        break;
    }
    tevEmitAlu(program, {ALU_KILLGT(__, _x, ALU_SRC_0_5, _x, _R(16), _x)});
    return true;
}

static bool initTevPixel(TevShader& shader) {
    std::vector<uint64_t> program(32, 0);
    const TevKey& key = shader.key;
    const size_t texOffset = program.size();
    uint32_t texCount = 0;
    for (uint32_t stage = 0; stage < key.stages; ++stage) {
        const uint32_t order = key.order[stage];
        if (!((order >> 6) & 1)) continue;
        const uint32_t map = order & 7, coord = (order >> 3) & 7;
        uint64_t sample[2] = {TEX_SAMPLE(_R(20 + stage), _x, _y, _z, _w, _R(1 + coord), _x, _y, _0, _x, map, map)};
        program.push_back(sample[0]);
        program.push_back(sample[1]);
        ++texCount;
    }
    const size_t aluOffset = program.size();
    // Texels and raster colours to 0..255 integers (genFS texi: round(clamp(c) * 255)).
    for (uint32_t stage = 0; stage < key.stages; ++stage) {
        if (!((key.order[stage] >> 6) & 1)) continue;
        for (uint32_t ch = 0; ch < 4; ++ch) {
            tevEmitAlu(program, {ALU_MUL(_R(20 + stage), ch, _R(20 + stage), ch, _GX2_C(12), _w)});
            tevEmitAlu(program, {ALU_RNDNE(_R(20 + stage), ch, _R(20 + stage), ch)});
        }
    }
    for (uint32_t reg = 9; reg <= 10; ++reg)
        for (uint32_t ch = 0; ch < 4; ++ch) {
            tevEmitAlu(program, {ALU_MOV(_R(reg), ch, _R(reg), ch) | to_QWORD(0, to_LE(1ull << 31))});
            tevEmitAlu(program, {ALU_MUL(_R(reg), ch, _R(reg), ch, _GX2_C(12), _w)});
            tevEmitAlu(program, {ALU_RNDNE(_R(reg), ch, _R(reg), ch)});
        }
    for (uint32_t reg = 0; reg < 4; ++reg)
        for (uint32_t ch = 0; ch < 4; ++ch)
            tevEmitAlu(program, {ALU_MOV(_R(12 + reg), ch, _GX2_C(reg), ch)});
    uint32_t lastColor = 0, lastAlpha = 0;
    int texStage = -1;
    for (uint32_t stage = 0; stage < key.stages; ++stage) {
        const uint32_t ce = key.colorEnv[stage], ae = key.alphaEnv[stage];
        if ((key.order[stage] >> 6) & 1) texStage = int(stage);
        const TevStageCtx cx{&key, stage, texStage};
        const uint32_t colorDest = (ce >> 22) & 3, alphaDest = (ae >> 22) & 3;
        const uint32_t colorBias = (ce >> 16) & 3, alphaBias = (ae >> 16) & 3;
        const uint32_t colorScale = (ce >> 20) & 3, alphaScale = (ae >> 20) & 3;
        const uint32_t ca = (ce >> 12) & 15, cb = (ce >> 8) & 15, cc = (ce >> 4) & 15, cd = ce & 15;
        const uint32_t aa = (ae >> 13) & 7, ab = (ae >> 10) & 7, ac = (ae >> 7) & 7, ad = (ae >> 4) & 7;
        // 8-bit views of the registers this stage reads as A, B or C.
        uint32_t maskRegs = 0;
        for (uint32_t sel : {ca, cb, cc}) if (sel < 8) maskRegs |= 1u << (sel >> 1);
        for (uint32_t sel : {aa, ab, ac}) if (sel < 4) maskRegs |= 1u << sel;
        for (uint32_t reg = 0; reg < 4; ++reg) {
            if (!(maskRegs & (1u << reg))) continue;
            for (uint32_t ch = 0; ch < 4; ++ch)
                tevEmitMask8(program, kTevMaskBase + reg, ch, {_R(12 + reg), ch}, 19);
        }
        // Results are staged in R11 and committed after the whole stage:
        // compare modes read several components of their inputs, which must
        // not already hold this stage's output.
        const TevCompareSrc colorCs{&cx, ca, cb, false};
        const TevCompareSrc alphaCs{&cx, aa, ab, true};
        for (uint32_t ch = 0; ch < 3; ++ch) {
            tevEmitChannel(program, 11, ch,
                           tevColorSource(cx, ca, ch, true), tevColorSource(cx, cb, ch, true),
                           tevColorSource(cx, cc, ch, true), tevColorSource(cx, cd, ch, false),
                           colorBias, (ce >> 18) & 1, colorScale, (ce >> 19) & 1, colorCs, colorCs);
        }
        tevEmitChannel(program, 11, _w,
                       tevAlphaSource(cx, aa, true), tevAlphaSource(cx, ab, true),
                       tevAlphaSource(cx, ac, true), tevAlphaSource(cx, ad, false),
                       alphaBias, (ae >> 18) & 1, alphaScale, (ae >> 19) & 1, alphaCs, colorCs);
        for (uint32_t ch = 0; ch < 3; ++ch)
            tevEmitAlu(program, {ALU_MOV(_R(12 + colorDest), ch, _R(11), ch)});
        tevEmitAlu(program, {ALU_MOV(_R(12 + alphaDest), _w, _R(11), _w)});
        lastColor = colorDest;
        lastAlpha = alphaDest;
    }
    // The output keeps the low 8 bits of the last stage's registers (genFS:
    // outc = ivec4(...) & 255), in R11.
    for (uint32_t ch = 0; ch < 3; ++ch) tevEmitMask8(program, 11, ch, {_R(12 + lastColor), ch}, 19);
    tevEmitMask8(program, 11, _w, {_R(12 + lastAlpha), _w}, 19);
    const size_t aluCount = program.size() - aluOffset;
    const size_t compareOffset = program.size();
    const bool killed = tevEmitAlphaCompare(program, key.alphaCompare, _R(11), _w);
    for (uint32_t ch = 0; ch < 4; ++ch)
        tevEmitAlu(program, {ALU_MUL(_R(11), ch, _R(11), ch, _GX2_C(13), _x)});
    const size_t compareCount = program.size() - compareOffset;
    size_t cf = 0;
    if (texCount) program[cf++] = TEX(texOffset, texCount) VALID_PIX;
    // An ALU clause holds at most 128 instruction slots; the main body has no
    // literals (every constant is in the constant file), so it splits anywhere.
    for (size_t at = 0; at < aluCount;) {
        size_t n = aluCount - at < 128 ? aluCount - at : 128;
        program[cf++] = ALU(aluOffset + at, n);
        at += n;
    }
    // The alpha compare (with its literal thresholds) is a few slots, one clause.
    for (size_t at = 0; at < compareCount;) {
        size_t n = compareCount - at < 128 ? compareCount - at : 128;
        program[cf++] = ALU(compareOffset + at, n);
        at += n;
    }
    if (cf + 1 > texOffset) return false;
    uint64_t exportOp = EXP_DONE(PIX0, _R(11), _x, _y, _z, _w);
    if (killed) exportOp |= to_QWORD(0, to_LE(1u << 22));
    program[cf++] = exportOp END_OF_PROGRAM;
    const size_t programSize = program.size() * sizeof(uint64_t);
    shader.psProgram = alignedZero(GX2_SHADER_PROGRAM_ALIGNMENT, programSize);
    if (!shader.psProgram) return false;
    memcpy(shader.psProgram, program.data(), programSize);
    memset(&shader.pixel, 0, sizeof(shader.pixel));
    shader.pixel.regs.sq_pgm_resources_ps = kTevGprs;
    shader.pixel.regs.sq_pgm_exports_ps = 2;
    shader.pixel.regs.spi_ps_in_control_0 = 11 | (1u << 26) | (1u << 28);
    shader.pixel.regs.num_spi_ps_input_cntl = 11;
    // Keep the unused fragment-position import slot.  Cemu rejects a
    // duplicate semantic here, while deleting it shifts every real input.
    shader.pixel.regs.spi_ps_input_cntls[0] = 0x17f;
    for (uint32_t i = 0; i < 10; ++i) shader.pixel.regs.spi_ps_input_cntls[i + 1] = i | 0x100;
    shader.pixel.regs.cb_shader_mask = 0xf;
    shader.pixel.regs.cb_shader_control = 1;
    shader.pixel.regs.db_shader_control = (1u << 4) | (1u << 6);
    shader.pixel.size = uint32_t(programSize);
    shader.pixel.program = shader.psProgram;
    shader.pixel.mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
    shader.uniform = {"tev_params", GX2_SHADER_VAR_TYPE_FLOAT4, 16, 0, -1};
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
    WHBLogPrintf("SMS Wii U: generated TEV pixel shader stages=%u tex=%02x cache=%u",
                 (unsigned)key.stages, (unsigned)shader->textureMask, (unsigned)s_tevShaders.size());
    return shader;
}

static void setTevShader(TevShader& shader, TextureEntry* const* textures) {
    GX2SetFetchShader(&s_tevFetch);
    GX2SetVertexShader(&s_tevVertex);
    GX2SetPixelShader(&shader.pixel);
    // Integer TEV operands (see the constant-file layout above tevColorSource).
    float params[64] = {};
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
    GX2SetPixelUniformReg(0, 64, params);
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
static void adjustCopyUv(const TextureEntry* texture, float* u, float* v) {
    const CopyEntry* e = copyEntryForTexture(texture);
    if (!e || !e->sourceW || !e->sourceH) return;
    *u = (float(e->sourceX) + *u * float(e->sourceW)) / s_tvW;
    *v = (float(e->sourceY) + *v * float(e->sourceH)) / s_tvH;
}
static bool runningUnderCemu() {
    static int result = -1;
    if (result < 0) result = (OSGetTitleID() >> 32) == 0xffffffffu;
    return result != 0;
}
static void copyToTexture(const void* dest, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool half, uint32_t fmt) {
    GX2ColorBuffer* cb = WHBGfxGetTVColourBuffer();
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
    }
    if (cemu) {
        GX2CopySurface(&cb->surface, 0, 0, &e->tex.texture.surface, 0, 0);
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

static TextureEntry* getTexture(int map) {
    SMS_PORT_HEAP_SCOPE();
    const uint32_t mode0 = g.bp[bpTexReg(BP_TX_MODE0, map)];
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
        initSampler(&c->sampler, mode0);
        return c;
    }
    for (size_t i = 0; i < s_textures.size(); ++i) {
        TextureEntry& e = s_textures[i];
        if (e.valid && e.source == source && e.fmt == fmt && e.width == w && e.height == h &&
            e.tlut == tlut && e.tlutFmt == tlutFmt) {
            // The sampler state belongs to the texmap, not the image: the same
            // texture may be bound with other wrap or filter modes.
            initSampler(&e.sampler, mode0);
            return &e;
        }
    }
    s_textures.push_back(TextureEntry());
    TextureEntry& e = s_textures.back();
    e.source = source; e.fmt = fmt; e.width = w; e.height = h; e.tlut = tlut; e.tlutFmt = tlutFmt;
    e.texture.surface.use = GX2_SURFACE_USE_TEXTURE;
    e.texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    e.texture.surface.width = w;
    e.texture.surface.height = h;
    e.texture.surface.depth = 1;
    e.texture.surface.mipLevels = 1;
    e.texture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    e.texture.surface.aa = GX2_AA_MODE1X;
    e.texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    e.texture.viewNumMips = 1;
    e.texture.viewNumSlices = 1;
    e.texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
    GX2CalcSurfaceSizeAndAlignment(&e.texture.surface);
    e.texture.surface.image = alignedZero(e.texture.surface.alignment, e.texture.surface.imageSize);
    if (!e.texture.surface.image) return nullptr;
    GX2InitTextureRegs(&e.texture);
    initSampler(&e.sampler, mode0);

    // Decode through an explicitly checked buffer: a failed std::vector
    // allocation returns null here (no exceptions), and the decoder then wrote
    // megabytes from address 0 and corrupted newlib's heap.
    uint8_t* rgba = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(w * h * 4, 64));
    // A texture address that did not survive the 30-bit GX physical address
    // (outside the main heap) must not be read: in Cemu that kills the title.
    const uint32_t srcBytes = gx2TexLevelBytes(fmt, w, h);
    // OSIsAddressValid rejects ordinary heap addresses under Cemu (every
    // texture came out white), so check the app's MEM2/MEM1 ranges directly.
    const uint32_t a0 = reinterpret_cast<uint32_t>(source), a1 = a0 + srcBytes;
    const bool readable = source && srcBytes && a1 > a0 &&
        ((a0 >= 0x10000000u && a1 <= 0x50000000u) || (a0 >= 0xF4000000u && a1 <= 0xF6000000u));
    if (!readable && rgba) { MEMFreeToDefaultHeap(rgba); rgba = nullptr; }
    if (!rgba) {
        memset(e.texture.surface.image, 0xff, e.texture.surface.imageSize);
    } else {
        const uint8_t* tlutPtr = indexed ? g.tlutMem + (tlut << 9) : nullptr;
        gx2DecodeTexture(source, fmt, w, h, tlutPtr, tlutFmt, rgba);
        uint8_t* dst = static_cast<uint8_t*>(e.texture.surface.image);
        for (uint32_t y = 0; y < h; ++y)
            memcpy(dst + size_t(y) * e.texture.surface.pitch * 4, rgba + size_t(y) * w * 4, size_t(w) * 4);
        MEMFreeToDefaultHeap(rgba);
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, e.texture.surface.image, e.texture.surface.imageSize);
    e.valid = true;
    return &e;
}

static void setShader(FixedShader& shader, TextureEntry* texture) {
    GX2SetFetchShader(&shader.fetch);
    GX2SetVertexShader(&shader.vertex);
    GX2SetPixelShader(&shader.pixel);
    static const float windowParams[4] = {1, 0, 0, 0};
    GX2SetPixelUniformReg(shader.pixel.uniformVars[0].offset, 4, windowParams);
    if (shader.textured && texture) {
        GX2SetPixelTexture(&texture->texture, 0);
        GX2SetPixelSampler(&texture->sampler, 0);
    }
}

// TV target size (WHBGfx picks 720p or 1080p) and the 4:3 game rectangle in it:
// the 640x480 EFB scaled to the target's height and centred.
static void updateTarget() {
    const GX2ColorBuffer* cb = WHBGfxGetTVColourBuffer();
    if (cb && cb->surface.width && cb->surface.height) {
        s_tvW = float(cb->surface.width);
        s_tvH = float(cb->surface.height);
    }
    s_gameScale = s_tvH / 480.0f;
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

    auto litPart = [&](uint32_t ctrl, int channel, bool alpha) {
        float materialColor[4];
        if (ctrl & 1) for (int k = 0; k < 4; ++k) materialColor[k] = float(in.clr[c][k]) / 255.0f;
        else packedColor(g.xfReg[XFR_MAT0 + c], materialColor);
        const float material = materialColor[alpha ? 3 : channel];
        if (!(ctrl & 2)) return material;
        float ambient[4];
        if (ctrl & 64) for (int k = 0; k < 4; ++k) ambient[k] = float(in.clr[c][k]) / 255.0f;
        else packedColor(g.xfReg[(alpha ? XFR_AMB0 : XFR_AMB0) + c], ambient);
        float sum = ambient[alpha ? 3 : channel];
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
            sum += factor * diffuse * lightCol[alpha ? 3 : channel];
        }
        return material * std::max(0.0f, std::min(sum, 1.0f));
    };
    for (int channel = 0; channel < 3; ++channel)
        out[channel] = clampi(int(litPart(colorCtrl, channel, false) * 255.0f + 0.5f), 0, 255);
    out[3] = clampi(int(litPart(alphaCtrl, 0, true) * 255.0f + 0.5f), 0, 255);
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
    return nullptr;
}

static void transformTev(const HostVertex& in, float* out, const TevKey& key, TextureEntry* const* textures) {
    transformPosition(in, out);
    int ras0[4], ras1[4];
    chanColor(in, 0, ras0);
    chanColor(in, 1, ras1);
    float texgen[8][3] = {};
    for (int coord = 0; coord < 8; ++coord) {
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

void rendererInit(float) {
    SMS_PORT_HEAP_SCOPE();
    if (s_ready) return;
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
void flushBatch() {
    SMS_PORT_HEAP_SCOPE();
    if (s_batch.indices.empty()) return;
    if (!s_ready || !s_frameActive || !ProcUIInForeground()) {
        s_batch.vertices.clear(); s_batch.indices.clear(); s_batch.count = 0;
        return;
    }
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
        if (allTextures) tevShader = tevShaderForKey(tevKey);
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
    const bool useTex = !tevPath && textured && texture;
    FixedShader& shader = useTex ? s_modulate : s_untextured;
    applyState();
    if (tevPath) setTevShader(*tevShader, tevTextures);
    else setShader(shader, texture);
    std::vector<float> vbo;
    // GX2DrawEx is the well-tested path on Wii U (and avoids relying on the
    // lifetime/placement rules of a CPU-side index pointer).  Expand the
    // already-decoded index list into a contiguous vertex stream for stage 1.
    const size_t vertexFloats = tevPath ? 28 : (useTex ? 10 : 8);
    const uint32_t vertexStride = uint32_t(vertexFloats * sizeof(float));
    vbo.reserve(s_batch.indices.size() * vertexFloats);
    HostVertex firstHv;
    float firstOut[28] = {};
    bool haveFirst = false;
    for (uint32_t index : s_batch.indices) {
        HostVertex hv;
        const uint8_t* packed = s_batch.vertices.data() + size_t(index) * s_batch.stride;
        // Vertices without per-vertex indices use the current matrix registers.
        const uint32_t ma = g.xfReg[XFR_MATIDX_A], mb = g.xfReg[XFR_MATIDX_B];
        const uint8_t defaults[9] = {uint8_t(ma & 63), uint8_t((ma >> 6) & 63), uint8_t((ma >> 12) & 63),
                                     uint8_t((ma >> 18) & 63), uint8_t((ma >> 24) & 63), uint8_t(mb & 63),
                                     uint8_t((mb >> 6) & 63), uint8_t((mb >> 12) & 63), uint8_t((mb >> 18) & 63)};
        unpackVertex(s_batch.fmt, packed, defaults, hv);
        const size_t at = vbo.size();
        vbo.resize(at + vertexFloats);
        if (tevPath) transformTev(hv, vbo.data() + at, tevKey, tevTextures);
        else transform(hv, vbo.data() + at, coord, useTex, texture);
        if (!haveFirst) {
            firstHv = hv;
            memcpy(firstOut, vbo.data() + at, vertexFloats * sizeof(float));
            haveFirst = true;
        }
    }
    const uint32_t debugDrawNo = s_debugDrawNo++;
#if SMS_GX2_DEBUG_PIXEL_TRACE
    if ((g_displayFrames % SMS_GX2_DEBUG_PIXEL_TRACE) == SMS_GX2_DEBUG_PIXEL_TRACE - 1 && g_displayFrames >= 1000 && tevPath) {
        float lo[5] = {9, 9, 9, 1e30f, 9}, hi[5] = {-9, -9, -9, -1e30f, -9};
        uint32_t neg = 0;
        for (size_t at = 0; at + 28 <= vbo.size(); at += 28) {
            for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], vbo[at + 20 + c]); hi[c] = std::max(hi[c], vbo[at + 20 + c]); }
            lo[3] = std::min(lo[3], vbo[at + 3]); hi[3] = std::max(hi[3], vbo[at + 3]);
            if (vbo[at + 3] <= 0.0f) ++neg;
        }
        WHBLogPrintf("SMS Wii U: DRAWSTAT frame=%u draw=%u st=%u ce0=%06x ae0=%06x ord0=%03x cm=%08x z=%02x xf5=%u n=%u ras0=(%.3f-%.3f,%.3f-%.3f,%.3f-%.3f) w=%.1f..%.1f neg=%u",
                     (unsigned)g_displayFrames, (unsigned)debugDrawNo, (unsigned)tevKey.stages,
                     (unsigned)tevKey.colorEnv[0], (unsigned)tevKey.alphaEnv[0], (unsigned)tevKey.order[0],
                     (unsigned)g.bp[BP_CMODE0], (unsigned)(g.bp[BP_ZMODE] & 0xff), (unsigned)g.xfReg[0x05],
                     (unsigned)(vbo.size() / 28), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3], (unsigned)neg);
    }
#endif
    debugDraw(tevKey, debugDrawNo, tevPath, map, texture, tevTextures,
              s_batch.count, haveFirst ? &firstHv : nullptr, firstOut);
    if (SMS_GX2_DEBUG_DRAW_LIMIT && debugDrawNo >= SMS_GX2_DEBUG_DRAW_LIMIT) {
        s_batch.vertices.clear(); s_batch.indices.clear(); s_batch.count = 0;
        return;
    }
    DCFlushRange(vbo.data(), uint32_t(vbo.size() * sizeof(float)));
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, vbo.data(), uint32_t(vbo.size() * sizeof(float)));
    GX2SetAttribBuffer(0, uint32_t(vbo.size() * sizeof(float)), tevPath ? vertexStride : shader.stride, vbo.data());
    const GX2PrimitiveMode mode = s_batch.cls == PRIM_LINES ? GX2_PRIMITIVE_MODE_LINES :
                                  s_batch.cls == PRIM_POINTS ? GX2_PRIMITIVE_MODE_POINTS : GX2_PRIMITIVE_MODE_TRIANGLES;
    GX2DrawEx(mode, uint32_t(s_batch.indices.size()), 0, 1);
    ++s_drawsThisFrame;
    GX2DrawDone();
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
    GX2SetContextState(WHBGfxGetTVContextState());
    GX2SetColorBuffer(WHBGfxGetTVColourBuffer(), GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(WHBGfxGetTVDepthBuffer());
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
        GX2ClearColor(WHBGfxGetTVColourBuffer(), float(ar & 255) / 255.0f, float((gb >> 8) & 255) / 255.0f,
                      float(gb & 255) / 255.0f,
                      ((g.bp[BP_PE_CONTROL] & 7) == 1) ? float((ar >> 8) & 255) / 255.0f : 1.0f);
    if ((g.bp[BP_ZMODE] >> 4) & 1)
        GX2ClearDepthStencilEx(WHBGfxGetTVDepthBuffer(), float(z & 0xffffff) / 16777215.0f, 0,
                               GX2_CLEAR_FLAGS_DEPTH);
    rebindTvTarget();
}
void markXfMemDirty() { SMS_PORT_HEAP_SCOPE(); if (!s_batch.indices.empty()) flushBatch(); }
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
    GX2ColorBuffer* cb = WHBGfxGetTVColourBuffer();
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
    // Make every queued draw land in memory before the copy reads the target.
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb->surface.image, cb->surface.imageSize);
    GX2DrawDone();
    GX2CopySurface(&cb->surface, 0, 0, &s_peekSurface, 0, 0);
    GX2DrawDone();
    if (cemu) GX2CopySurface(&cb->surface, 0, 0, &s_peekSpecial, 0, 0);
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
    if ((g_displayFrames % SMS_GX2_DEBUG_PIXEL_TRACE) != SMS_GX2_DEBUG_PIXEL_TRACE - 1 || g_displayFrames < 1000) return;
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
    GX2SetContextState(WHBGfxGetTVContextState());
    GX2SetColorBuffer(WHBGfxGetTVColourBuffer(), GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(WHBGfxGetTVDepthBuffer());
    GX2ClearDepthStencilEx(WHBGfxGetTVDepthBuffer(), 1.0f, 0, GX2_CLEAR_FLAGS_DEPTH);
    GX2SetContextState(WHBGfxGetTVContextState());
    GX2SetColorBuffer(WHBGfxGetTVColourBuffer(), GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(WHBGfxGetTVDepthBuffer());
    updateTarget();
    if (s_frameClearPending) {
        GX2ClearColor(WHBGfxGetTVColourBuffer(), s_frameClear[0], s_frameClear[1], s_frameClear[2], s_frameClear[3]);
        s_frameClearPending = false;
        GX2SetContextState(WHBGfxGetTVContextState());
        GX2SetColorBuffer(WHBGfxGetTVColourBuffer(), GX2_RENDER_TARGET_0);
        GX2SetDepthBuffer(WHBGfxGetTVDepthBuffer());
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
    s_frameActive = false;
    s_drawsThisFrame = 0;
    ++g_displayFrames;
}

} // namespace gx
