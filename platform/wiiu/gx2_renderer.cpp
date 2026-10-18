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
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
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
    {"aInput1", GX2_SHADER_VAR_TYPE_FLOAT4, 0, 5},
};
static GX2SamplerVar s_samplerVar = {"uTex0", GX2_SAMPLER_VAR_TYPE_SAMPLER_2D, 0};
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

static void* alignedZero(size_t alignment, size_t size) {
    void* p = memalign(alignment, size);
    if (p) memset(p, 0, size);
    return p;
}

static void destroyShader(FixedShader& shader) {
    free(shader.vsProgram);
    free(shader.psProgram);
    free(shader.fetch.program);
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
    shader.vertex.size = uint32_t(vsSize);
    shader.vertex.program = shader.vsProgram;
    shader.vertex.mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
    shader.vertex.attribVarCount = textured ? 3 : 2;
    shader.vertex.attribVars = s_attribVars;
    shader.pixel.size = uint32_t(psSize);
    shader.pixel.program = shader.psProgram;
    shader.pixel.mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
    shader.pixel.uniformVarCount = 1;
    shader.pixel.uniformVars = &s_pixelUniform;
    shader.pixel.samplerVarCount = textured ? 1 : 0;
    shader.pixel.samplerVars = textured ? &s_samplerVar : nullptr;

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

    std::vector<uint8_t> rgba(size_t(w) * h * 4, 0xff);
    const uint8_t* tlutPtr = indexed ? g.tlutMem + (tlut << 9) : nullptr;
    gx2DecodeTexture(source, fmt, w, h, tlutPtr, tlutFmt, rgba.data());
    uint8_t* dst = static_cast<uint8_t*>(e.texture.surface.image);
    for (uint32_t y = 0; y < h; ++y)
        memcpy(dst + size_t(y) * e.texture.surface.pitch * 4, rgba.data() + size_t(y) * w * 4, size_t(w) * 4);
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

static void applyState() {
    const uint32_t zmode = g.bp[BP_ZMODE];
    const bool ztest = (zmode & 1) != 0;
    const bool zwrite = (zmode & 16) != 0;
    GX2SetRasterizerClipControl(TRUE, FALSE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_SRC_ALPHA,
                       GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetDepthOnlyControl(ztest, zwrite, ztest ? GX2_COMPARE_FUNC_LEQUAL : GX2_COMPARE_FUNC_ALWAYS);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetPolygonOffset(0, 0, 0, 0, 0);
    GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE,
                         GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
    const float scaleX = 1.5f, scaleY = 1.5f;
    const int vx = int(160.0f + g.viewport[0] * scaleX);
    const int vy = int(g.viewport[1] * scaleY);
    const int vw = int(g.viewport[2] * scaleX);
    const int vh = int(g.viewport[3] * scaleY);
    GX2SetViewport(float(vx), float(vy), float(vw), float(vh), 0.0f, 1.0f);
    const int sx = int(160.0f + g.scissor[0] * scaleX);
    const int sy = int(g.scissor[1] * scaleY);
    const int sw = int(g.scissor[2] * scaleX);
    const int sh = int(g.scissor[3] * scaleY);
    GX2SetScissor(sx, sy, sw, sh);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, (g.bp[BP_CMODE0] & 0x18) ? 0xff : 0, FALSE, TRUE);
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
    if (p[0] == 0.0f) {
        cx = p[1] * x + p[2] * z;
        cy = p[3] * y + p[4] * z;
        cz = p[5] * z + p[6];
        cw = 1.0f;
    } else {
        cx = p[1] * x + p[2];
        cy = p[3] * y + p[4];
        cz = p[5] * z + p[6];
        cw = -z;
    }
    if (fabsf(cw) < 0.00001f) cw = 0.00001f;
    out[0] = cx; out[1] = cy;
    // GX's XF viewport writes a 24-bit depth range. GX2 wants [0,w].
    float* vp = reinterpret_cast<float*>(&g.xfReg[XFR_VIEWPORT]);
    out[2] = (vp[5] * cw + vp[2] * cz) / 16777215.0f;
    out[3] = cw;
    const float r = in.clr[0][0] / 255.0f, gcol = in.clr[0][1] / 255.0f;
    const float b = in.clr[0][2] / 255.0f, a = in.clr[0][3] / 255.0f;
    out[4] = r; out[5] = gcol; out[6] = b; out[7] = a;
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
    if (s_ready) return;
    s_ready = initShaders();
    if (!s_ready) WHBLogPrintf("SMS Wii U: GX2 shader setup failed");
}

float efbScale() { return 1.0f; }
bool rendererReady() { return s_ready; }
void releaseGlThread() {}

void onStateChange() {
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
    const PrimClass cls = primClass(op);
    if (!s_batch.indices.empty() && (cls != s_batch.cls || fmt != s_batch.fmt)) flushBatch();
    s_batch.cls = cls; s_batch.fmt = fmt; s_batch.stride = stride;
    const size_t old = s_batch.vertices.size();
    s_batch.vertices.resize(old + size_t(n) * stride);
    return s_batch.vertices.data() + old;
}

void primitiveEnd(uint8_t op, uint32_t n) {
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

void flushBatch() {
    if (s_batch.indices.empty()) return;
    if (!s_ready || !s_frameActive || !ProcUIInForeground()) {
        s_batch.vertices.clear(); s_batch.indices.clear(); s_batch.count = 0;
        return;
    }
    const bool textured = usedTexture(nullptr, nullptr);
    int coord = 0, map = 0;
    TextureEntry* texture = textured ? getTexture(usedTexture(&coord, &map) ? map : 0) : nullptr;
    FixedShader& shader = textured && texture ? s_modulate : s_untextured;
    applyState();
    setShader(shader, texture);
    std::vector<float> vbo;
    vbo.reserve(size_t(s_batch.count) * (textured ? 10 : 8));
    for (uint32_t i = 0; i < s_batch.count; ++i) {
        HostVertex hv;
        const uint8_t* packed = s_batch.vertices.data() + size_t(i) * s_batch.stride;
        const uint8_t defaults[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        unpackVertex(s_batch.fmt, packed, defaults, hv);
        const size_t at = vbo.size();
        vbo.resize(at + (textured ? 10 : 8));
        transform(hv, vbo.data() + at, coord, textured);
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, vbo.data(), uint32_t(vbo.size() * sizeof(float)));
    GX2SetAttribBuffer(0, uint32_t(vbo.size() * sizeof(float)), shader.stride, vbo.data());
    const GX2PrimitiveMode mode = s_batch.cls == PRIM_LINES ? GX2_PRIMITIVE_MODE_LINES :
                                  s_batch.cls == PRIM_POINTS ? GX2_PRIMITIVE_MODE_POINTS : GX2_PRIMITIVE_MODE_TRIANGLES;
    GX2DrawIndexedEx(mode, uint32_t(s_batch.indices.size()), GX2_INDEX_TYPE_U32, s_batch.indices.data(), 0, 1);
    GX2DrawDone();
    s_batch.vertices.clear(); s_batch.indices.clear(); s_batch.count = 0;
}

void executeCopy(uint32_t) { flushBatch(); }
void markXfMemDirty() {}
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
    if (!s_ready || !ProcUIInForeground()) return;
    s_frameActive = true;
    GX2SetRasterizerClipControl(TRUE, FALSE);
}
extern "C" void sms_wiiu_gx2_renderer_end(void) {
    if (!s_frameActive) return;
    flushBatch();
    GX2Flush();
    s_frameActive = false;
    ++g_displayFrames;
}

} // namespace gx
