#include <cstdio>
#include "port_heap.h"
#ifndef SMS_GX2_DEBUG_DUMP
#define SMS_GX2_DEBUG_DUMP 1
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
#include <stdio.h>
#include <initializer_list>
#include <vector>

extern "C" const char* sms_wiiu_stage_path(void);
extern unsigned g_dbgDLCalls, g_dbgDLBytes;
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

static GX2TexClampMode clampMode(uint32_t value) {
    if (value & 2) return (value & 1) ? GX2_TEX_CLAMP_MODE_MIRROR_ONCE : GX2_TEX_CLAMP_MODE_CLAMP;
    return (value & 1) ? GX2_TEX_CLAMP_MODE_MIRROR : GX2_TEX_CLAMP_MODE_WRAP;
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
    // on the old path until its Latte implementation is added.  Comparison
    // forms (bias=3) need predicate ALU and are also a deliberate fallback.
    if (((k.genmode >> 16) & 7) != 0) return false;
    for (uint32_t s = 0; s < k.stages; ++s) {
        if (((k.order[s] >> 6) & 1) && (k.order[s] & 7) >= 8) return false;
        if (((k.colorEnv[s] >> 16) & 3) == 3 || ((k.alphaEnv[s] >> 16) & 3) == 3) return false;
    }
    // Alpha compare is not yet emitted in the GX2 pixel program, but keeping
    // the TEV shader active for the title's two-reference compare is important:
    // the old fallback uses white in place of the texture sample and turns the
    // sky into wedges.
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
    return {_GX2_C(32 + 4 * (value >> 2)), value & 3};
}

static TevSource tevKonst(const TevKey& k, uint32_t sel, uint32_t chan, bool alpha = false) {
    if (sel < 8) return tevFixed(sel);
    if (alpha) return tevFixed(12);
    if (sel >= 12 && sel < 16) return {_GX2_C(4 * (4 + sel - 12)), chan};
    if (sel >= 16) return {_GX2_C(4 * (4 + (sel & 3))), (sel - 16) >> 2};
    return tevFixed(12);
}

static uint32_t tevSwapChannel(const TevKey& k, uint32_t table, uint32_t channel) {
    return (k.swap[table & 3] >> (channel * 2)) & 3;
}

static TevSource tevColorSource(const TevKey& k, uint32_t stage, uint32_t sel, uint32_t channel) {
    if (sel < 8) return {_R(12 + (sel >> 1)), (sel & 1) ? _w : channel};
    const uint32_t order = k.order[stage];
    const uint32_t rasTable = k.alphaEnv[stage] & 3;
    const uint32_t texTable = (k.alphaEnv[stage] >> 2) & 3;
    if (sel == 8 || sel == 9) {
        if (!((order >> 6) & 1)) return tevFixed(12);
        const uint32_t ch = tevSwapChannel(k, texTable, sel == 8 ? channel : 3);
        return {_R(20 + stage), ch};
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

static TevSource tevAlphaSource(const TevKey& k, uint32_t stage, uint32_t sel) {
    if (sel < 4) return {_R(12 + sel), _w};
    const uint32_t order = k.order[stage];
    const uint32_t rasTable = k.alphaEnv[stage] & 3;
    const uint32_t texTable = (k.alphaEnv[stage] >> 2) & 3;
    if (sel == 4) {
        if (!((order >> 6) & 1)) return tevFixed(12);
        return {_R(20 + stage), tevSwapChannel(k, texTable, 3)};
    }
    if (sel == 5) {
        const uint32_t chan = (order >> 7) & 7;
        if (chan > 1) return {ALU_SRC_0, 0};
        return {_R(9 + chan), tevSwapChannel(k, rasTable, 3)};
    }
    if (sel == 6) return tevKonst(k, (k.ksel[stage] >> 5) & 31, 0, true);
    return tevFixed(15);
}

static void tevEmitAlu(std::vector<uint64_t>& program, std::initializer_list<uint64_t> ops) {
    if (ops.size() == 0) return;
    for (uint64_t op : ops) program.push_back(op);
    program.back() |= to_QWORD(to_LE(1ull << 31), 0);
}

static uint64_t tevClamp(uint64_t op, bool clamp) {
    return clamp ? op | to_QWORD(0, to_LE(1ull << 31)) : op;
}

static void tevEmitChannel(std::vector<uint64_t>& program, uint32_t dest, uint32_t channel,
                           TevSource a, TevSource b, TevSource c, TevSource d,
                           uint32_t bias, uint32_t op, uint32_t scale, bool clamp) {
    tevEmitAlu(program, {ALU_ADD(_R(16), channel, a.sel, a.chan, b.sel, b.chan _NEG)});
    tevEmitAlu(program, {ALU_MUL(_R(16), channel, ALU_SRC_PV, channel, c.sel, c.chan)});
    uint64_t result = op ? ALU_ADD(_R(dest), channel, d.sel, d.chan, ALU_SRC_PV, channel _NEG)
                         : ALU_ADD(_R(dest), channel, d.sel, d.chan, ALU_SRC_PV, channel);
    tevEmitAlu(program, {result});
    if (bias == 1 || bias == 2) {
        const uint32_t sign = bias == 2 ? (ALU_SRC_0_5 | (1 << 12)) : ALU_SRC_0_5;
        tevEmitAlu(program, {ALU_ADD(_R(dest), channel, ALU_SRC_PV, channel, sign, _x)});
    }
    if (scale == 1 || scale == 2 || scale == 3) {
        const uint32_t one = ALU_SRC_1;
        uint64_t scaled = scale == 1 ? ALU_MUL_x2(_R(dest), channel, ALU_SRC_PV, channel, one, _x)
                                     : scale == 2 ? ALU_MUL_x4(_R(dest), channel, ALU_SRC_PV, channel, one, _x)
                                                  : ALU_ADD_D2(_R(dest), channel, ALU_SRC_PV, channel, ALU_SRC_0, _x);
        tevEmitAlu(program, {tevClamp(scaled, clamp)});
    } else if (bias == 1 || bias == 2) {
        // The bias instruction is now the last operation for this channel.
        if (clamp) {
            // Re-emit the preceding clamp by moving PV through the same
            // destination.  This keeps the operation sequence independent of
            // whether a scale was requested.
            tevEmitAlu(program, {tevClamp(ALU_MOV(_R(dest), channel, ALU_SRC_PV, channel), true)});
        }
    } else if (clamp) {
        // The third instruction is already the final result when no bias or
        // scale follows; a second move supplies the clamp bit without relying
        // on macro token concatenation.
        tevEmitAlu(program, {tevClamp(ALU_MOV(_R(dest), channel, ALU_SRC_PV, channel), true)});
    }
}

static bool initTevVertex() {
    std::vector<uint64_t> program;
    program.reserve(64);
    program.push_back(CALL_FS NO_BARRIER);
    program.push_back(EXP_DONE(POS0, _R(1), _x, _y, _z, _w));
    for (uint32_t i = 0; i < 10; ++i) {
        const uint64_t exp = EXP(i == 9 ? PARAM(9) : PARAM(i), _R(2 + i), _x, _y, _z, _w);
        if (i == 9) program.push_back(exp END_OF_PROGRAM);
        else program.push_back(exp NO_BARRIER);
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

static bool initTevPixel(TevShader& shader) {
    std::vector<uint64_t> program(32, 0);
    const uint32_t textureMask = shader.textureMask;
    const size_t texOffset = program.size();
    uint32_t texCount = 0;
    for (uint32_t stage = 0; stage < shader.key.stages; ++stage) {
        const uint32_t order = shader.key.order[stage];
        if (!((order >> 6) & 1)) continue;
        const uint32_t map = order & 7, coord = (order >> 3) & 7;
        uint64_t sample[2] = {TEX_SAMPLE(_R(20 + stage), _x, _y, _z, _w, _R(1 + coord), _x, _y, _0, _x, map, map)};
        program.push_back(sample[0]);
        program.push_back(sample[1]);
        ++texCount;
    }
    const size_t aluOffset = program.size();
    uint32_t lastColor = 0, lastAlpha = 0;
    for (uint32_t reg = 0; reg < 4; ++reg)
        for (uint32_t ch = 0; ch < 4; ++ch)
            tevEmitAlu(program, {ALU_MOV(_R(12 + reg), ch, _GX2_C(4 * reg), ch)});
    for (uint32_t stage = 0; stage < shader.key.stages; ++stage) {
        const uint32_t ce = shader.key.colorEnv[stage], ae = shader.key.alphaEnv[stage];
        const uint32_t colorDest = (ce >> 22) & 3, alphaDest = (ae >> 22) & 3;
        const uint32_t colorBias = (ce >> 16) & 3, alphaBias = (ae >> 16) & 3;
        const uint32_t colorScale = (ce >> 20) & 3, alphaScale = (ae >> 20) & 3;
        for (uint32_t ch = 0; ch < 3; ++ch) {
            tevEmitChannel(program, 12 + colorDest, ch,
                           tevColorSource(shader.key, stage, (ce >> 12) & 15, ch),
                           tevColorSource(shader.key, stage, (ce >> 8) & 15, ch),
                           tevColorSource(shader.key, stage, (ce >> 4) & 15, ch),
                           tevColorSource(shader.key, stage, ce & 15, ch),
                           colorBias, (ce >> 18) & 1, colorScale, (ce >> 19) & 1);
        }
        tevEmitChannel(program, 12 + alphaDest, _w,
                       tevAlphaSource(shader.key, stage, (ae >> 13) & 7),
                       tevAlphaSource(shader.key, stage, (ae >> 10) & 7),
                       tevAlphaSource(shader.key, stage, (ae >> 7) & 7),
                       tevAlphaSource(shader.key, stage, (ae >> 4) & 7),
                       alphaBias, (ae >> 18) & 1, alphaScale, (ae >> 19) & 1);
        lastColor = colorDest;
        lastAlpha = alphaDest;
    }
    if (lastColor != lastAlpha)
        tevEmitAlu(program, {ALU_MOV(_R(12 + lastColor), _w, _R(12 + lastAlpha), _w)});
    const size_t aluCount = program.size() - aluOffset;
    size_t cf = 0;
    if (texCount) program[cf++] = TEX(texOffset, texCount) VALID_PIX;
    if (aluCount) program[cf++] = ALU(aluOffset, aluCount);
    program[cf++] = EXP_DONE(PIX0, _R(12 + lastColor), _x, _y, _z, _w) END_OF_PROGRAM;
    const size_t programSize = program.size() * sizeof(uint64_t);
    shader.psProgram = alignedZero(GX2_SHADER_PROGRAM_ALIGNMENT, programSize);
    if (!shader.psProgram) return false;
    memcpy(shader.psProgram, program.data(), programSize);
    memset(&shader.pixel, 0, sizeof(shader.pixel));
    shader.pixel.regs.sq_pgm_resources_ps = 20 + shader.key.stages;
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
    float params[64] = {};
    for (int i = 0; i < 4; ++i) {
        const uint32_t ra = g.bp[BP_TEV_REG + 2 * i], bg = g.bp[BP_TEV_REG + 2 * i + 1];
        params[4 * i + 0] = float(tevSext11(ra & 0x7ff)) / 255.0f;
        params[4 * i + 1] = float(tevSext11((bg >> 12) & 0x7ff)) / 255.0f;
        params[4 * i + 2] = float(tevSext11(bg & 0x7ff)) / 255.0f;
        params[4 * i + 3] = float(tevSext11((ra >> 12) & 0x7ff)) / 255.0f;
    }
    for (int i = 0; i < 4; ++i) {
        const uint32_t ra = g.kreg[2 * i], bg = g.kreg[2 * i + 1];
        params[16 + 4 * i + 0] = float(ra & 0xff) / 255.0f;
        params[16 + 4 * i + 1] = float((bg >> 12) & 0xff) / 255.0f;
        params[16 + 4 * i + 2] = float(bg & 0xff) / 255.0f;
        params[16 + 4 * i + 3] = float((ra >> 12) & 0xff) / 255.0f;
    }
    static const int k8[8] = {255, 223, 191, 159, 127, 95, 63, 31};
    for (int i = 0; i < 8; ++i) params[32 + i] = float(k8[i]) / 255.0f;
    params[32 + 12] = 1.0f;
    params[32 + 13] = 128.0f / 255.0f;
    params[32 + 15] = 0.0f;
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
static void adjustCopyUv(const TextureEntry* texture, float* u, float* v) {
    const CopyEntry* e = copyEntryForTexture(texture);
    if (!e || !e->sourceW || !e->sourceH) return;
    *u = (float(e->sourceX) + *u * float(e->sourceW)) / s_tvW;
    *v = (float(e->sourceY) + *v * float(e->sourceH)) / s_tvH;
}
static void copyToTexture(const void* dest, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool half) {
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
    // Cemu's GX2CopySurfaceEx implementation only handles a full-surface
    // copy. Keep the backing texture the size of the TV target, copy that
    // surface in one operation, and apply the source rectangle in the
    // texture coordinates sent to the shader below.
    const uint32_t fullW = cb->surface.width, fullH = cb->surface.height;
    const uint32_t logicalW = half ? (w + 1) / 2 : w, logicalH = half ? (h + 1) / 2 : h;
    if (e && (e->tex.texture.surface.width != fullW || e->tex.texture.surface.height != fullH)) {
        MEMFreeToDefaultHeap(e->tex.texture.surface.image);
        e->tex.valid = false;
    }
    if (!e) { copyCache().push_back(CopyEntry()); e = &copyCache().back(); e->dest = dest; }
    e->tex.width = logicalW;
    e->tex.height = logicalH;
    e->sourceX = tx; e->sourceY = ty; e->sourceW = tw; e->sourceH = th;
    if (!e->tex.valid) {
        TextureEntry& t = e->tex;
        memset(&t.texture, 0, sizeof(t.texture));
        t.width = logicalW; t.height = logicalH;
        t.texture.surface.use = GX2_SURFACE_USE_TEXTURE;
        t.texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        t.texture.surface.width = fullW;
        t.texture.surface.height = fullH;
        t.texture.surface.depth = 1;
        t.texture.surface.mipLevels = 1;
        t.texture.surface.format = cb->surface.format;
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
    GX2CopySurface(&cb->surface, 0, 0, &e->tex.texture.surface, 0, 0);
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
        GX2InitSampler(&c->sampler, GX2_TEX_CLAMP_MODE_CLAMP,
                       (mode0 & 0x20) ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT);
        GX2InitSamplerClamping(&c->sampler, clampMode(mode0 & 3), clampMode((mode0 >> 2) & 3),
                               GX2_TEX_CLAMP_MODE_WRAP);
        return c;
    }
    for (size_t i = 0; i < s_textures.size(); ++i) {
        TextureEntry& e = s_textures[i];
        if (e.valid && e.source == source && e.fmt == fmt && e.width == w && e.height == h &&
            e.tlut == tlut && e.tlutFmt == tlutFmt) return &e;
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
    GX2InitSampler(&e.sampler, GX2_TEX_CLAMP_MODE_CLAMP,
                   (mode0 & 0x20) ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSamplerClamping(&e.sampler, clampMode(mode0 & 3), clampMode((mode0 >> 2) & 3),
                           GX2_TEX_CLAMP_MODE_WRAP);

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
    GX2SetRasterizerClipControlHalfZ(TRUE, FALSE, TRUE);
    // GX blend state (BP_CMODE0), as the PC renderer decodes it. Opaque 3D
    // materials have blending off and often alpha 0: always blending by source
    // alpha made the whole stage invisible. The EFB has no alpha channel, so
    // destination alpha reads as 1.
    const uint32_t cm = g.bp[BP_CMODE0];
    static const GX2BlendMode kSrc[8] = {GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_DST_COLOR,
                                         GX2_BLEND_MODE_INV_DST_COLOR, GX2_BLEND_MODE_SRC_ALPHA,
                                         GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO};
    static const GX2BlendMode kDst[8] = {GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_SRC_COLOR,
                                         GX2_BLEND_MODE_INV_SRC_COLOR, GX2_BLEND_MODE_SRC_ALPHA,
                                         GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO};
    const bool blend = (cm & 1) != 0;
    if (blend && ((cm >> 11) & 1))
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ONE,
                           GX2_BLEND_COMBINE_MODE_REV_SUB, FALSE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO,
                           GX2_BLEND_COMBINE_MODE_ADD);
    else
        GX2SetBlendControl(GX2_RENDER_TARGET_0, kSrc[(cm >> 8) & 7], kDst[(cm >> 5) & 7],
                           GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO,
                           GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetDepthOnlyControl(ztest, zwrite, ztest ? GX2_COMPARE_FUNC_LEQUAL : GX2_COMPARE_FUNC_ALWAYS);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetPolygonOffset(0, 0, 0, 0, 0);
    GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE,
                         GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
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
    // GX color-update enable is not encoded in the blend-mode bits.  In
    // particular GX_BM_NONE has CMODE0 blend bits clear while still writing
    // color; the DKR/lus GX2 path uses the full 0xff mask for this state.
    GX2SetColorControl(GX2_LOGIC_OP_COPY, blend ? 0xff : 0x00, FALSE, TRUE);
    GX2SetTargetChannelMasks(((cm >> 3) & 1) ? GX2_CHANNEL_MASK_RGB : (GX2ChannelMask)0,
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
    const float* nm = reinterpret_cast<const float*>(&g.xfMem[256 + matrix * 3]);
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
    const float* p = g.projection;
    float cx, cy, cz, cw;
    if (p[0] != 0.0f) {  // GX_ORTHOGRAPHIC == 1: p = {1, sx, tx, sy, ty, sz, tz}
        cx = p[1] * x + p[2];
        cy = p[3] * y + p[4];
        cz = p[5] * z + p[6];
        cw = 1.0f;
    } else {  // GX_PERSPECTIVE == 0
        cx = p[1] * x + p[2] * z;
        cy = p[3] * y + p[4] * z;
        cz = p[5] * z + p[6];
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

static uint32_t s_dbgPrim, s_dbgApp, s_dbgFlush, s_dbgDiscard;
uint8_t* primitiveBegin(uint8_t op, uint32_t n, uint32_t fmt, uint32_t stride) {
    SMS_PORT_HEAP_SCOPE();
    ++s_dbgPrim;
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
    ++s_dbgApp;
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
static uint32_t s_dbgVerts, s_dbgIn, s_dbgBehind, s_dbgZ;
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
        { const float* o = vbo.data() + at; ++s_dbgVerts;
          if (o[3] > 0 && fabsf(o[0]) <= o[3] && fabsf(o[1]) <= o[3] && o[2] >= 0 && o[2] <= o[3]) ++s_dbgIn;
          else if (o[3] <= 0) ++s_dbgBehind; else if (o[2] < 0 || o[2] > o[3]) ++s_dbgZ; }
    }
    { // TEMP debug dump
        static int s_dbgDraws = 0; static FILE* s_dbg = nullptr;
        if (SMS_GX2_DEBUG_DUMP && g_displayFrames > 650 && s_dbgDraws < 120) {
            if (!s_dbg) s_dbg = fopen("fs:/vol/external01/gx2dbg.txt", "w");
            if (s_dbg) {
                fprintf(s_dbg, "draw %d frame %u fmt %x n %u tex %d tev %d stages %u mask %02x gen %08x vp %g %g %g %g proj %g %g %g %g %g %g %g\n", s_dbgDraws,
                        (unsigned)g_displayFrames, (unsigned)s_batch.fmt, (unsigned)s_batch.indices.size(), (int)textured,
                        (int)tevPath, (unsigned)tevKey.stages, (unsigned)tevTextureMask(tevKey), (unsigned)tevKey.genmode,
                        g.viewport[0], g.viewport[1], g.viewport[2], g.viewport[3], g.projection[0], g.projection[1],
                        g.projection[2], g.projection[3], g.projection[4], g.projection[5], g.projection[6]);
                fprintf(s_dbg, "  state cm %08x blend %u cupd %u aupd %u z %08x ztest %u zwrite %u pe %08x ac %08x copy %08x\n",
                        (unsigned)g.bp[BP_CMODE0], (unsigned)(g.bp[BP_CMODE0] & 1), (unsigned)((g.bp[BP_CMODE0] >> 3) & 1),
                        (unsigned)((g.bp[BP_CMODE0] >> 4) & 1), (unsigned)g.bp[BP_ZMODE], (unsigned)(g.bp[BP_ZMODE] & 1),
                        (unsigned)((g.bp[BP_ZMODE] >> 4) & 1), (unsigned)g.bp[BP_PE_CONTROL], (unsigned)g.bp[BP_ALPHACOMPARE],
                        (unsigned)g.bp[BP_COPY_EXEC]);
                fprintf(s_dbg, "  tev");
                for (uint32_t st = 0; st < tevKey.stages && st < 4; ++st)
                    fprintf(s_dbg, " %u/%06x/%06x/%03x", (unsigned)st, (unsigned)tevKey.order[st],
                            (unsigned)tevKey.colorEnv[st], (unsigned)tevKey.alphaEnv[st]);
                fputc('\n', s_dbg);
                { const uint8_t* ab = g.arrayBase[0]; fprintf(s_dbg, "  arr0 %p stride %u be %d vcd %08x/%08x vatA0 %08x bytes %02x%02x %02x%02x %02x%02x\n", (const void*)ab, (unsigned)g.arrayStride[0], (int)g.arrayBigEndian[0], (unsigned)g.cpVcdLo, (unsigned)g.cpVcdHi, (unsigned)g.cpVatA[0], ab?ab[0]:0, ab?ab[1]:0, ab?ab[2]:0, ab?ab[3]:0, ab?ab[4]:0, ab?ab[5]:0); }
                { const float* m = reinterpret_cast<const float*>(&g.xfMem[0]); fprintf(s_dbg, "  m0 %g %g %g %g | %g %g %g %g | %g %g %g %g matA %08x cpA %08x\n", m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11], (unsigned)g.xfReg[XFR_MATIDX_A], (unsigned)g.cpMatA); }
                const size_t st = vertexFloats;
                for (size_t k = 0; k < 3 && k * st < vbo.size(); k++) {
                    const float* o = vbo.data() + k * st;
                    HostVertex hv; const uint8_t d9[9] = {0};
                    unpackVertex(s_batch.fmt, s_batch.vertices.data() + size_t(s_batch.indices[k]) * s_batch.stride, d9, hv);
                    const size_t co = tevPath ? 20 : (useTex ? 6 : 4);
                    const float* tm0 = reinterpret_cast<const float*>(&g.xfMem[0x500 + 60 * 4]);
                    const float* pm = reinterpret_cast<const float*>(&g.xfMem[(hv.mtx[0] & 63) * 4]);
                    const float pz = pm[8] * hv.pos[0] + pm[9] * hv.pos[1] + pm[10] * hv.pos[2] + pm[11];
                    fprintf(s_dbg, "  in %g %g %g mtx %u tc %g,%g %g,%g calcZ %g calcW %g tm60 %g,%g,%g,%g|%g,%g,%g,%g|%g,%g,%g,%g -> %g %g %g %g col %g %g %g %g uv %g,%g %g,%g\n", hv.pos[0], hv.pos[1], hv.pos[2],
                            (unsigned)hv.mtx[0], hv.tex[0][0], hv.tex[0][1], hv.tex[1][0], hv.tex[1][1], pz, -pz,
                            tm0[0], tm0[1], tm0[2], tm0[3], tm0[4], tm0[5], tm0[6], tm0[7], tm0[8], tm0[9], tm0[10], tm0[11], o[0], o[1], o[2], o[3],
                            o[co], o[co + 1], o[co + 2], o[co + 3], tevPath ? o[4] : 0.0f, tevPath ? o[5] : 0.0f,
                            tevPath ? o[6] : 0.0f, tevPath ? o[7] : 0.0f);
                }
                if (texture) {
                    const uint8_t* t = static_cast<const uint8_t*>(texture->texture.surface.image);
                    const uint8_t* raw = texture->source;
                    const uint32_t rawBytes = gx2TexLevelBytes(texture->fmt, texture->width, texture->height);
                    const uintptr_t rawAddr = reinterpret_cast<uintptr_t>(raw);
                    const bool rawReadable = raw && rawBytes && rawAddr + rawBytes > rawAddr &&
                        ((rawAddr >= 0x10000000u && rawAddr + rawBytes <= 0x50000000u) ||
                         (rawAddr >= 0xF4000000u && rawAddr + rawBytes <= 0xF6000000u));
                    fprintf(s_dbg, "  src %p base %p image0 %08x image3 %08x raw %02x%02x%02x%02x%02x%02x%02x%02x\n",
                            (const void*)texture->source, physToPtr(0x20), (unsigned)g.bp[BP_TX_IMAGE0], (unsigned)g.bp[BP_TX_IMAGE3],
                            rawReadable ? raw[0] : 0, rawReadable ? raw[1] : 0, rawReadable ? raw[2] : 0, rawReadable ? raw[3] : 0,
                            rawReadable ? raw[4] : 0, rawReadable ? raw[5] : 0, rawReadable ? raw[6] : 0, rawReadable ? raw[7] : 0);
                    fprintf(s_dbg, "  tex %ux%u fmt %u pitch %u texel0 %u %u %u %u mid %u %u %u %u\n", (unsigned)texture->width, (unsigned)texture->height, (unsigned)texture->fmt,
                            (unsigned)texture->texture.surface.pitch, t[0], t[1], t[2], t[3], t[(texture->height/2*texture->texture.surface.pitch + texture->width/2)*4], t[(texture->height/2*texture->texture.surface.pitch + texture->width/2)*4+1], t[(texture->height/2*texture->texture.surface.pitch + texture->width/2)*4+2], t[(texture->height/2*texture->texture.surface.pitch + texture->width/2)*4+3]);
                }
                for (const CopyEntry& c : copyCache())
                    fprintf(s_dbg, "  copycache dest %p tex %ux%u crop %u,%u %ux%u\n", c.dest,
                            (unsigned)c.tex.width, (unsigned)c.tex.height, (unsigned)c.sourceX,
                            (unsigned)c.sourceY, (unsigned)c.sourceW, (unsigned)c.sourceH);
                if (++s_dbgDraws == 120) { fclose(s_dbg); s_dbg = nullptr; }
            }
        }
    }
    DCFlushRange(vbo.data(), uint32_t(vbo.size() * sizeof(float)));
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, vbo.data(), uint32_t(vbo.size() * sizeof(float)));
    GX2SetAttribBuffer(0, uint32_t(vbo.size() * sizeof(float)), tevPath ? vertexStride : shader.stride, vbo.data());
    const GX2PrimitiveMode mode = s_batch.cls == PRIM_LINES ? GX2_PRIMITIVE_MODE_LINES :
                                  s_batch.cls == PRIM_POINTS ? GX2_PRIMITIVE_MODE_POINTS : GX2_PRIMITIVE_MODE_TRIANGLES;
    GX2DrawEx(mode, uint32_t(s_batch.indices.size()), 0, 1);
    ++s_drawsThisFrame;
    GX2DrawDone();
    s_batch.vertices.clear(); s_batch.indices.clear(); s_batch.count = 0;
}

// GX clears the EFB as part of a copy. Without it the depth of earlier passes
// (shadow and reflection renders) rejected the whole main view. The display
// copy is not cleared: the frame is presented at the next retrace.
static void rebindTvTarget() {
    GX2SetContextState(WHBGfxGetTVContextState());
    GX2SetColorBuffer(WHBGfxGetTVColourBuffer(), GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(WHBGfxGetTVDepthBuffer());
}
void executeCopy(uint32_t ctrl) {
    SMS_PORT_HEAP_SCOPE();
    flushBatch();
    const bool clear = (ctrl >> 11) & 1, disp = (ctrl >> 14) & 1;
    if (disp || !s_frameActive || !ProcUIInForeground()) return;
    {
        const uint32_t src = g.bp[BP_COPY_SRC_TL], size = g.bp[BP_COPY_SRC_WH];
        copyToTexture(g.copyDest, src & 0x3ff, (src >> 10) & 0x3ff, (size & 0x3ff) + 1,
                      ((size >> 10) & 0x3ff) + 1, (ctrl >> 9) & 1);
        if (SMS_GX2_DEBUG_DUMP && g_displayFrames > 650) {
            static uint32_t s_dbgCopies;
            if (s_dbgCopies < 16) {
                FILE* f = fopen("fs:/vol/external01/gx2dbg.txt", "a");
                if (f) {
                    fprintf(f, "copy frame %u ctrl %08x dest %p src %u,%u size %u,%u half %u\n",
                            (unsigned)g_displayFrames, (unsigned)ctrl, g.copyDest, (unsigned)(src & 0x3ff),
                            (unsigned)((src >> 10) & 0x3ff), (unsigned)((size & 0x3ff) + 1),
                            (unsigned)(((size >> 10) & 0x3ff) + 1), (unsigned)((ctrl >> 9) & 1));
                    for (uint32_t map = 0; map < 8; ++map) {
                        if (TextureEntry* e = copyTexture(g.texImage[map]))
                            fprintf(f, "  cache map %u source %p size %u,%u\n", (unsigned)map,
                                    (const void*)g.texImage[map], (unsigned)e->width, (unsigned)e->height);
                    }
                    fclose(f);
                }
                ++s_dbgCopies;
            }
        }
        rebindTvTarget();  // GX2CopySurface leaves its destination bound
    }
    if (!clear) return;
    const uint32_t ar = g.bp[BP_CLEAR_AR], gb = g.bp[BP_CLEAR_GB], z = g.bp[BP_CLEAR_Z];
    if ((g.bp[BP_CMODE0] >> 3) & 1)
        GX2ClearColor(WHBGfxGetTVColourBuffer(), float(ar & 255) / 255.0f, float((gb >> 8) & 255) / 255.0f,
                      float(gb & 255) / 255.0f, 1.0f);
    if ((g.bp[BP_ZMODE] >> 4) & 1)
        GX2ClearDepthStencilEx(WHBGfxGetTVDepthBuffer(), float(z & 0xffffff) / 16777215.0f, 0,
                               GX2_CLEAR_FLAGS_DEPTH);
    rebindTvTarget();
}
void markXfMemDirty() { SMS_PORT_HEAP_SCOPE(); if (!s_batch.indices.empty()) flushBatch(); }
uint32_t peekColor(int, int) { return 0; }
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
    GX2SetViewport(0.0f, 0.0f, s_tvW, s_tvH, 0.0f, 1.0f);
    GX2SetScissor(0, 0, uint32_t(s_tvW), uint32_t(s_tvH));
    s_frameActive = true;
    GX2SetRasterizerClipControlHalfZ(TRUE, FALSE, TRUE);
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
    { static int n = 0;
      if (SMS_GX2_DEBUG_DUMP && (g_displayFrames % 100) == 0 && n < 40) { FILE* f = fopen("fs:/vol/external01/gx2frames.txt", "a");
        if (f) { fprintf(f, "frame %u prim %u draws %u verts %u in %u behind %u zout %u stage %s\n", (unsigned)g_displayFrames, s_dbgPrim, s_drawsThisFrame, s_dbgVerts, s_dbgIn, s_dbgBehind, s_dbgZ, sms_wiiu_stage_path()); s_dbgVerts = s_dbgIn = s_dbgBehind = s_dbgZ = 0; g_dbgDLCalls = g_dbgDLBytes = 0; fclose(f); ++n; } }
      s_dbgPrim = s_dbgApp = 0; }
    GX2Flush();
    s_frameActive = false;
    s_drawsThisFrame = 0;
    ++g_displayFrames;
}

} // namespace gx
