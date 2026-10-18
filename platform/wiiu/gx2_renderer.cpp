#include <cstdio>
#include "port_heap.h"
#ifndef SMS_GX2_DEBUG_DUMP
#define SMS_GX2_DEBUG_DUMP 0
#endif
#include <coreinit/memdefaultheap.h>
#include <coreinit/memorymap.h>
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

static FixedShader s_untextured, s_modulate;
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
    const bool readable = source && srcBytes &&
        OSIsAddressValid(reinterpret_cast<uint32_t>(source)) &&
        OSIsAddressValid(reinterpret_cast<uint32_t>(source) + srcBytes - 1);
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
static float s_tvW = 1280.0f, s_tvH = 720.0f, s_gameScale = 1.5f, s_gameX = 160.0f;
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

static void chanColor(const HostVertex& in, int c, int out[4]) {
    const uint32_t numChans = g.xfReg[XFR_NUMCHANS] & 3;
    if (uint32_t(c) >= numChans) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    for (int part = 0; part < 2; part++) {
        const uint32_t ctrl = g.xfReg[(part ? XFR_ALPHA0CTRL : XFR_COLOR0CTRL) + c];
        const uint32_t mreg = g.xfReg[XFR_MAT0 + c];
        int mat[4];
        if (ctrl & 1) for (int k = 0; k < 4; k++) mat[k] = in.clr[c][k];
        else { mat[0] = (mreg >> 24) & 255; mat[1] = (mreg >> 16) & 255; mat[2] = (mreg >> 8) & 255; mat[3] = mreg & 255; }
        if (part) out[3] = mat[3];
        else { out[0] = mat[0]; out[1] = mat[1]; out[2] = mat[2]; }
    }
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

static void transform(const HostVertex& in, float* out, int texCoord, bool textured) {
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
    evalTev(in, out + 4, nullptr);
    if (textured) {
        float u = in.tex[texCoord][0], v = in.tex[texCoord][1];
        if (g.tcManual[texCoord]) {
            const uint32_t ss = g.bp[BP_SU_SSIZE + texCoord * 2] & 0xffff;
            const uint32_t ts = g.bp[BP_SU_SSIZE + texCoord * 2 + 1] & 0xffff;
            u *= float(ss + 1); v *= float(ts + 1);
        }
        out[8] = u; out[9] = v;
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
    const bool textured = usedTexture(nullptr, nullptr);
    int coord = 0, map = 0;
    TextureEntry* texture = textured ? getTexture(usedTexture(&coord, &map) ? map : 0) : nullptr;
    // The vertex layout must follow the shader actually bound: a material that
    // samples a texture we could not load still draws untextured (8 floats).
    const bool useTex = textured && texture;
    FixedShader& shader = useTex ? s_modulate : s_untextured;
    applyState();
    setShader(shader, texture);
    std::vector<float> vbo;
    // GX2DrawEx is the well-tested path on Wii U (and avoids relying on the
    // lifetime/placement rules of a CPU-side index pointer).  Expand the
    // already-decoded index list into a contiguous vertex stream for stage 1.
    vbo.reserve(s_batch.indices.size() * (useTex ? 10 : 8));
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
        vbo.resize(at + (useTex ? 10 : 8));
        transform(hv, vbo.data() + at, coord, useTex);
        { const float* o = vbo.data() + at; ++s_dbgVerts;
          if (o[3] > 0 && fabsf(o[0]) <= o[3] && fabsf(o[1]) <= o[3] && o[2] >= 0 && o[2] <= o[3]) ++s_dbgIn;
          else if (o[3] <= 0) ++s_dbgBehind; else if (o[2] < 0 || o[2] > o[3]) ++s_dbgZ; }
    }
    { // TEMP debug dump
        static int s_dbgDraws = 0; static FILE* s_dbg = nullptr;
        if (SMS_GX2_DEBUG_DUMP && g_displayFrames > 1450 && s_dbgDraws < 80) {
            if (!s_dbg) s_dbg = fopen("fs:/vol/external01/gx2dbg.txt", "w");
            if (s_dbg) {
                fprintf(s_dbg, "draw %d frame %u fmt %x n %u tex %d vp %g %g %g %g proj %g %g %g %g %g %g %g\n", s_dbgDraws,
                        (unsigned)g_displayFrames, (unsigned)s_batch.fmt, (unsigned)s_batch.indices.size(), (int)textured,
                        g.viewport[0], g.viewport[1], g.viewport[2], g.viewport[3], g.projection[0], g.projection[1],
                        g.projection[2], g.projection[3], g.projection[4], g.projection[5], g.projection[6]);
                { const uint8_t* ab = g.arrayBase[0]; fprintf(s_dbg, "  arr0 %p stride %u be %d vcd %08x/%08x vatA0 %08x bytes %02x%02x %02x%02x %02x%02x\n", (const void*)ab, (unsigned)g.arrayStride[0], (int)g.arrayBigEndian[0], (unsigned)g.cpVcdLo, (unsigned)g.cpVcdHi, (unsigned)g.cpVatA[0], ab?ab[0]:0, ab?ab[1]:0, ab?ab[2]:0, ab?ab[3]:0, ab?ab[4]:0, ab?ab[5]:0); }
                { const float* m = reinterpret_cast<const float*>(&g.xfMem[0]); fprintf(s_dbg, "  m0 %g %g %g %g | %g %g %g %g | %g %g %g %g matA %08x cpA %08x\n", m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11], (unsigned)g.xfReg[XFR_MATIDX_A], (unsigned)g.cpMatA); }
                const size_t st = useTex ? 10 : 8;
                for (size_t k = 0; k < 3 && k * st < vbo.size(); k++) {
                    const float* o = vbo.data() + k * st;
                    HostVertex hv; const uint8_t d9[9] = {0};
                    unpackVertex(s_batch.fmt, s_batch.vertices.data() + size_t(s_batch.indices[k]) * s_batch.stride, d9, hv);
                    fprintf(s_dbg, "  in %g %g %g mtx %u -> %g %g %g %g col %g %g %g %g\n", hv.pos[0], hv.pos[1], hv.pos[2],
                            (unsigned)hv.mtx[0], o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7]);
                }
                if (texture) { const uint8_t* t = static_cast<const uint8_t*>(texture->texture.surface.image); fprintf(s_dbg, "  src %p base %p image0 %08x image3 %08x\n", (const void*)texture->source, physToPtr(0x20), (unsigned)g.bp[BP_TX_IMAGE0], (unsigned)g.bp[BP_TX_IMAGE3]); fprintf(s_dbg, "  tex %ux%u fmt %u texel0 %u %u %u %u mid %u %u %u %u\n", (unsigned)texture->width, (unsigned)texture->height, (unsigned)texture->fmt, t[0], t[1], t[2], t[3], t[(texture->height/2*texture->texture.surface.pitch + texture->width/2)*4], t[(texture->height/2*texture->texture.surface.pitch + texture->width/2)*4+1], t[(texture->height/2*texture->texture.surface.pitch + texture->width/2)*4+2], t[(texture->height/2*texture->texture.surface.pitch + texture->width/2)*4+3]); }
                if (++s_dbgDraws == 80) { fclose(s_dbg); s_dbg = nullptr; }
            }
        }
    }
    DCFlushRange(vbo.data(), uint32_t(vbo.size() * sizeof(float)));
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, vbo.data(), uint32_t(vbo.size() * sizeof(float)));
    GX2SetAttribBuffer(0, uint32_t(vbo.size() * sizeof(float)), shader.stride, vbo.data());
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
    if (!clear || disp || !s_frameActive || !ProcUIInForeground()) return;
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
    ++g_displayFrames;
}

} // namespace gx
