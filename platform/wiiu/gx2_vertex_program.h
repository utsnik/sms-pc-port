/*
 * Generated Latte vertex programs (GPU-XF path), host-compilable.
 *
 * Everything here builds plain uint64 Latte words and a POD description of
 * the GX2VertexShader registers and fetch attributes.  gx2_renderer.cpp turns
 * that description into GX2 objects; platform/wiiu/tests runs the very same
 * words through a host Latte interpreter.  Nothing in this header may include
 * a GX2/wut header.
 */
#ifndef SMS_GX2_VERTEX_PROGRAM_H
#define SMS_GX2_VERTEX_PROGRAM_H

#include "gx2_shader_inl.h"
#include "../gx/src/gx_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <initializer_list>
#include <vector>

#ifndef SMS_GX2_VX_ORACLE
#define SMS_GX2_VX_ORACLE 0
#endif

namespace gx {

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

static constexpr uint32_t kXfUniformRegs = 192;
static constexpr uint32_t kXfUniformFloats = kXfUniformRegs * 4;
static constexpr uint32_t kVtxLiveBase = kXfUniformRegs;
static constexpr uint32_t kVtxLiveRegs = 21;
static constexpr uint32_t kXfNormalBase = 64, kXfPostTexBase = 96, kXfLightBase = 160;

/* The GX2VertexShader register words a program needs, without GX2 types. */
struct LatteVertexRegs {
    uint32_t sq_pgm_resources_vs;
    uint32_t spi_vs_out_config;
    uint32_t num_spi_vs_out_id;
    uint32_t spi_vs_out_id[10];
    uint32_t sq_vtx_semantic_clear;
    uint32_t num_sq_vtx_semantic;
    uint32_t sq_vtx_semantic[32];
    uint32_t vgt_vertex_reuse_block_cntl;
    uint32_t vgt_hos_reuse_depth;
};

/* One fetch attribute, mapped to GX2AttribStream by the renderer.  The
 * selects use GX2_SQ_SEL numbering (X, Y, Z, W, 0, 1). */
enum LatteAttribKind : uint32_t {
    kLatteAttribFloat32x2,
    kLatteAttribFloat32x3,
    kLatteAttribFloat32x4,
    kLatteAttribUnorm8x4,     // GX2_ATTRIB_FORMAT_UNORM_8_8_8_8
    kLatteAttribUscaled8x4,   // GX2_ATTRIB_FLAG_SCALED | GX2_ATTRIB_TYPE_8_8_8_8
};
enum LatteSel : uint8_t { kLatteSelX, kLatteSelY, kLatteSelZ, kLatteSelW, kLatteSel0, kLatteSel1 };
struct LatteAttrib {
    uint32_t location;
    uint32_t buffer;
    uint32_t offset;
    uint32_t kind;
    uint8_t sel[4];
};

struct LatteVertexImage {
    std::vector<uint64_t> words;  // CF, then ALU clauses (one uint64 per slot)
    uint32_t aluCount;
    LatteVertexRegs regs;
    LatteAttrib attribs[16];
    uint32_t attribCount;
    uint32_t stride;
    const char* reject;
};

struct LatteVertexFlags {
    bool nameXf;      // name every XF vector statically (Cemu or gpuxfname)
    bool oneXf;       // name C0 only
    bool oneClause;   // reject multi-clause programs
};

static inline void vertexEmit(std::vector<uint64_t>& ops, std::initializer_list<uint64_t> group) {
    for (uint64_t op : group) ops.push_back(op);
    if (!ops.empty()) ops.back() |= to_QWORD(to_LE(1ull << 31), 0);
}

static inline void vertexEmitDot4(std::vector<uint64_t>& ops, uint32_t dst, uint32_t dstChan,
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

static inline uint32_t vertexInputForRow(uint32_t row) {
    if (row == 0) return 1; // position
    if (row == 1) return 2; // normal
    if (row == 3) return 3; // binormal
    if (row == 4) return 4; // tangent
    if (row >= 5) return RAW_ATTR_TEX01 + (row - 5) / 2 + 1; // packed source texcoord pair
    return 29; // raster colour 0, selected by the caller for row 2
}

static inline void vertexCopySource(std::vector<uint64_t>& ops, uint32_t source, uint32_t dst, bool colour) {
    vertexEmit(ops, {ALU_MOV(_R(dst), _x, _R(source), _x)});
    vertexEmit(ops, {ALU_MOV(_R(dst), _y, _R(source), _y)});
    // Geometric rows keep z (texgen form ABC1); the caller forces z = 1 for
    // form AB11.  w is 1 for every geometric source.
    vertexEmit(ops, {ALU_MOV(_R(dst), _z, _R(source), _z)});
    vertexEmit(ops, {ALU_MOV(_R(dst), _w, colour ? _R(source) : ALU_SRC_1, colour ? _w : _x)});
}

static inline void vertexCopyTexSource(std::vector<uint64_t>& ops, uint32_t coord, uint32_t dst) {
    const uint32_t source = RAW_ATTR_TEX01 + coord / 2 + 1;
    const uint32_t chan = (coord & 1) * 2;
    vertexEmit(ops, {ALU_MOV(_R(dst), _x, _R(source), chan)});
    vertexEmit(ops, {ALU_MOV(_R(dst), _y, _R(source), chan + 1)});
    vertexEmit(ops, {ALU_MOV(_R(dst), _z, ALU_SRC_1, _x)});
    vertexEmit(ops, {ALU_MOV(_R(dst), _w, ALU_SRC_1, _x)});
}

static inline void vertexLoadAbsoluteMatrix(std::vector<uint64_t>& ops, uint32_t dst, uint32_t base,
                                     uint32_t rows = 4);

static inline void vertexLoadRelativeMatrix(std::vector<uint64_t>& ops, uint32_t indexReg, uint32_t indexChan,
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

static inline void vertexLoadAbsoluteMatrix(std::vector<uint64_t>& ops, uint32_t dst, uint32_t base,
                                     uint32_t rows) {
    for (uint32_t row = 0; row < rows; ++row)
        for (uint32_t ch = 0; ch < 4; ++ch)
            vertexEmit(ops, {ALU_MOV(_R(dst + row), ch, _GX2_C(base + row), ch)});
}

static inline void vertexMatrixTransform(std::vector<uint64_t>& ops, uint32_t source, uint32_t indexReg,
                                  uint32_t indexChan, uint32_t dst, uint32_t base, bool relative) {
    // Rows go to R58..R60, clear of every destination and source the
    // callers use (texgen writes R55, which used to alias matrix row 2).
    if (relative) vertexLoadRelativeMatrix(ops, indexReg, indexChan, 58, base, 3);
    else vertexLoadAbsoluteMatrix(ops, 58, base, 3);
    for (uint32_t ch = 0; ch < 3; ++ch)
        vertexEmitDot4(ops, dst, ch, _R(source), _R(58 + ch));
}

static inline void vertexNormalTransform(std::vector<uint64_t>& ops) {
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

static inline void vertexEmitLighting(std::vector<uint64_t>& ops, uint32_t ctrl, uint32_t channel,
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

static inline void vertexEmitColour(std::vector<uint64_t>& ops, const VertexKey& key, uint32_t channel, uint32_t dst) {
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

static inline bool vertexKeySupported(const VertexKey& key) {
    if (key.numTexGens > 8 || key.fixedCoord > 7 || key.fixedTextured > 1) return false;
    return true;
}


struct VertexAluGroup {
    size_t begin;
    size_t end;
    bool readsPvPs;
};

struct VertexAluClause {
    size_t offset;
    size_t count;
};

static inline bool vertexAluPvPs(uint32_t sel) {
    return sel == ALU_SRC_PV || sel == ALU_SRC_PS;
}

static inline bool vertexAluReadsPvPs(uint64_t op) {
    const uint32_t word0 = __builtin_bswap32(uint32_t(op >> 32));
    const uint32_t word1 = __builtin_bswap32(uint32_t(op));
    if (vertexAluPvPs(word0 & 0x1ff) || vertexAluPvPs((word0 >> 13) & 0x1ff))
        return true;
    // OP3 puts its third source in word1.  OP2 uses these bits for the
    // instruction flags, so only inspect them when the OP3 encoding field is
    // nonzero.
    return ((word1 >> 15) & 7) != 0 && vertexAluPvPs(word1 & 0x1ff);
}

// AR (set by MOVA) does not survive a clause boundary either, so a group that
// addresses a source or destination relatively must stay with the MOVA that
// precedes it.
static inline bool vertexAluUsesAr(uint64_t op) {
    const uint32_t word0 = __builtin_bswap32(uint32_t(op >> 32));
    const uint32_t word1 = __builtin_bswap32(uint32_t(op));
    if (((word0 >> 9) & 1) || ((word0 >> 22) & 1) || ((word1 >> 28) & 1))
        return true;
    return ((word1 >> 15) & 7) != 0 && ((word1 >> 9) & 1);
}

static inline bool vertexBuildAluClauses(std::vector<uint64_t>& alu,
                                  std::vector<VertexAluClause>& clauses) {
    const uint64_t aluLast = to_QWORD(to_LE(1ull << 31), 0);
    std::vector<VertexAluGroup> groups;
    for (size_t begin = 0; begin < alu.size();) {
        bool readsPvPs = false, hasLiteral = false, ended = false;
        size_t end = begin;
        while (end < alu.size()) {
            readsPvPs = readsPvPs || vertexAluReadsPvPs(alu[end]) || vertexAluUsesAr(alu[end]);
            const uint32_t word0 = __builtin_bswap32(uint32_t(alu[end] >> 32));
            const uint32_t word1 = __builtin_bswap32(uint32_t(alu[end]));
            hasLiteral = hasLiteral || ((word0 & 0x1ff) == ALU_SRC_LITERAL) ||
                         (((word0 >> 13) & 0x1ff) == ALU_SRC_LITERAL);
            hasLiteral = hasLiteral || (((word1 >> 15) & 7) != 0 &&
                                        (word1 & 0x1ff) == ALU_SRC_LITERAL);
            ended = (alu[end] & aluLast) != 0;
            ++end;
            if (ended) break;
        }
        if (!ended) return false;
        // A literal is a slot belonging to the group, immediately following
        // its LAST-marked instruction.  Vertex programs currently do not emit
        // literals, but keep the splitter correct if one is added later.
        if (hasLiteral) {
            if (end == alu.size()) return false;
            ++end;
        }
        groups.push_back({begin, end, readsPvPs});
        begin = end;
    }
    if (groups.empty()) return false;

    // A boundary before a PV/PS reader (or an AR user) would discard the
    // producer's value.
    // Turn each such adjacency into an indivisible component, then pack whole
    // components.  This also naturally keeps every boundary at a LAST bit.
    std::vector<std::pair<size_t, size_t>> components;
    size_t componentBegin = 0;
    for (size_t i = 1; i < groups.size(); ++i) {
        if (groups[i].readsPvPs) continue;
        components.push_back({componentBegin, i});
        componentBegin = i;
    }
    components.push_back({componentBegin, groups.size()});

    std::vector<uint64_t> rebuilt;
    rebuilt.reserve(alu.size() + components.size());
    clauses.clear();
    size_t clauseBegin = 0, clauseCount = 0;
    for (const auto& component : components) {
        const size_t first = component.first, last = component.second;
        const size_t componentSlots = groups[last - 1].end - groups[first].begin;
        if (componentSlots > 128) return false;
        if (clauseCount && clauseCount + componentSlots > 128) {
            clauses.push_back({clauseBegin, clauseCount});
            clauseBegin = rebuilt.size();
            clauseCount = 0;
        }
        // This is only reachable for a malformed/hand-built program whose
        // first group itself consumes PV/PS.  Start it with a harmless scratch
        // group so the reader is never the first group in a clause.
        if (!clauseCount && groups[first].readsPvPs) {
            if (componentSlots == 128) return false;
            vertexEmit(rebuilt, {ALU_MOV(_R(57), _x, ALU_SRC_0, _x)});
            ++clauseCount;
        }
        if (clauseCount + componentSlots > 128) return false;
        rebuilt.insert(rebuilt.end(), alu.begin() + groups[first].begin,
                       alu.begin() + groups[last - 1].end);
        clauseCount += componentSlots;
    }
    if (clauseCount) clauses.push_back({clauseBegin, clauseCount});
    alu.swap(rebuilt);
    return !clauses.empty();
}

/* Builds the generated GPU-XF vertex program for `key`.  On failure
 * image.reject names the reason (the renderer logs it). */
static inline bool buildGeneratedVertexImage(const VertexKey& key, const LatteVertexFlags& flags,
                                      LatteVertexImage& image) {
    image.reject = "unknown";
    image.words.clear();
    image.aluCount = 0;
    image.attribCount = 0;
    image.stride = 0;
    memset(&image.regs, 0, sizeof(image.regs));
    if (!vertexKeySupported(key)) {
        image.reject = "key-range";
        return false;
    }
    std::vector<uint64_t> alu;
    alu.reserve(512);
    // Only the uniform registers a program names statically are valid for
    // it, so matrices reached through AR read stale values otherwise.  Name
    // every XF vector under Cemu or when explicitly enabled for hardware.
    if (flags.nameXf) {
        for (uint32_t r = 0; r < kXfUniformRegs; ++r)
            vertexEmit(alu, {ALU_MOV(_R(57), _x, _GX2_C(r), _x)});
    } else if (flags.oneXf) {
        vertexEmit(alu, {ALU_MOV(_R(57), _x, _GX2_C(0), _x)});
    }

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
    std::vector<VertexAluClause> aluClauses;
    if (!vertexBuildAluClauses(alu, aluClauses) || alu.size() > 2048) {
        image.reject = alu.size() > 2048 ? "alu-limit" : "clause-layout";
        return false;
    }
    image.aluCount = uint32_t(alu.size());
    const size_t clauses = aluClauses.size();
    if (!clauses || clauses > 16) {
        image.reject = "clause-limit";
        return false;
    }
    if (clauses > 1 && flags.oneClause) {
        image.reject = "multi-clause (gpuxf1clause marker)";
        return false;
    }
    // Control flow first, ALU code after it: the CF stream must end in the
    // END_OF_PROGRAM export, or execution runs on into the ALU words.
    // The first export keeps its barrier: it reads GPRs the ALU clauses just
    // wrote, and without one Latte may start it before they retire (Cemu runs
    // CF in order, so only hardware shows the stale positions).  Later exports
    // follow it and need none, as in initTevVertex.
    std::vector<uint64_t> exports;
    exports.push_back(EXP_DONE(POS0, _R(20), _x, _y, _z, _w));
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
    std::vector<uint64_t>& program = image.words;
    program.assign(aluOffset, 0);
    program[0] = CALL_FS NO_BARRIER;
    for (size_t clause = 0; clause < clauses; ++clause)
        program[1 + clause] = ALU(aluOffset + aluClauses[clause].offset,
                                  aluClauses[clause].count);
    for (size_t i = 0; i < exports.size(); ++i) program[1 + clauses + i] = exports[i];
    program.insert(program.end(), alu.begin(), alu.end());

    LatteVertexRegs& regs = image.regs;
    regs.sq_pgm_resources_vs = 64 | (1u << 8);
    regs.spi_vs_out_config = key.outputMode ? (4 << 1) : (9 << 1);
    regs.num_spi_vs_out_id = 3;
    memset(regs.spi_vs_out_id, 0xff, sizeof(regs.spi_vs_out_id));
    regs.spi_vs_out_id[0] = 0x03020100;
    regs.spi_vs_out_id[1] = 0x07060504;
    // Keep the fixed path's export map identical to the packed shader that
    // the legacy path binds. Cemu rejects the fixed pixel shader when the
    // unused slots are advertised as absent rather than as its ABI's 0x0a.
    regs.spi_vs_out_id[2] = key.outputMode ? 0xff0a0908 : 0xffff0908;
    regs.sq_vtx_semantic_clear = ~((1u << RAW_ATTR_COUNT) - 1);
    regs.num_sq_vtx_semantic = RAW_ATTR_COUNT;
    for (uint32_t i = 0; i < RAW_ATTR_COUNT; ++i) regs.sq_vtx_semantic[i] = i;
    regs.vgt_vertex_reuse_block_cntl = 14;
    regs.vgt_hos_reuse_depth = 16;

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
        LatteAttrib& attr = image.attribs[i];
        attr.location = i;
        attr.buffer = 0;
        attr.offset = offsets[i];
        attr.kind = colourAttr[i] ? kLatteAttribUnorm8x4 :
                    indexAttr[i] ? kLatteAttribUscaled8x4 :
                    components[i] == 3 ? kLatteAttribFloat32x3 : kLatteAttribFloat32x4;
        // Positions are fetched as three floats; w must read as 1 so the
        // dot4 with a matrix row picks up its translation (a missing
        // component's SEL_W reads 0).  Normals keep w = 0.
        attr.sel[0] = kLatteSelX;
        attr.sel[1] = kLatteSelY;
        attr.sel[2] = kLatteSelZ;
        attr.sel[3] = i == RAW_ATTR_POS ? kLatteSel1 : kLatteSelW;
    }
    image.attribCount = RAW_ATTR_COUNT;
    image.stride = kRawVertexStride;
    return true;
}

/* The CPU XF position path (transformPosition) and the live block the
 * generated program reads (packVertexLiveUniforms C192..C196.y), as pure
 * functions of their inputs, so the host oracle evaluates the same code. */
struct XfViewInputs {
    const float* proj;        // XF projection p1..p6
    bool ortho;               // XF projection type bit
    const float* viewport;    // decoder viewport: left, top, width, height
    const float* xfViewport;  // XF viewport block (six floats)
    float xmapA, xmapB, xmapClip;
    float gameScale, gameX, tvW, tvH;
};

static inline void xfTransformPositionMath(const float* m, const float* pos,
                                           const XfViewInputs& v, float* out) {
    const float x = m[0] * pos[0] + m[1] * pos[1] + m[2] * pos[2] + m[3];
    const float y = m[4] * pos[0] + m[5] * pos[1] + m[6] * pos[2] + m[7];
    const float z = m[8] * pos[0] + m[9] * pos[1] + m[10] * pos[2] + m[11];
    // The game can update projection through display-list XF writes rather
    // than GXSetProjection(), so g.projection may lag the live registers.
    // The PC shader reads XF_PROJ directly for this reason.
    const float* xfProj = v.proj;
    const float p1 = xfProj[0], p2 = xfProj[1], p3 = xfProj[2], p4 = xfProj[3],
                p5 = xfProj[4], p6 = xfProj[5];
    const bool ortho = v.ortho;
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
    // Match the PC renderer's XF viewport conversion, then apply the current
    // draw XMap before mapping the original game rectangle into the TV target.
    const float viewportCenter = v.viewport[0] + v.viewport[2] * 0.5f;
    const float mappedCenter = viewportCenter * v.xmapA + v.xmapB;
    const float halfW = v.viewport[2] * 0.5f * v.xmapA * v.xmapClip * v.gameScale;
    const float halfH = v.viewport[3] * 0.5f * v.gameScale;
    const float centerX = v.gameX + mappedCenter * v.gameScale;
    const float centerY = v.viewport[1] * v.gameScale + halfH;
    out[0] = ((centerX * cw + halfW * cx) * (2.0f / v.tvW)) - cw;
    // GX screen y grows downwards; GX2 clip-space y grows upwards.
    out[1] = cw - ((centerY * cw - halfH * cy) * (2.0f / v.tvH));
    // GX's XF viewport writes a 24-bit depth range. GX2's clip-space depth
    // uses the same positive-w convention as the lus/DKR shader path.
    const float* vp = v.xfViewport;
    // XF viewport: depth * w = (zoffset * w + zscale * z) / 2^24-1, already in
    // GX2's 0..w clip range (the PC GL path maps the same value to -w..w).
    out[2] = (vp[5] * cw + vp[2] * cz) / 16777215.0f;
    out[3] = cw;
}

/* live[0..17]: C192 = projection p1..p4, C193 = p5, p6, ortho, 0,
 * C194 = mapped viewport, C195 = game scale/X and target size,
 * C196.xy = depth scale/offset. */
static inline void xfPackLiveView(const XfViewInputs& v, float* live) {
    memcpy(&live[0], v.proj, 6 * sizeof(float));
    live[6] = v.ortho ? 1.0f : 0.0f;
    // C186 is left/top/width/height in game coordinates.  Map its horizontal
    // centre and half-width exactly as transformPosition does so the GPU
    // vertex-program path lands on the same pixels as the CPU path.
    const float viewportCenter = v.viewport[0] + v.viewport[2] * 0.5f;
    const float mappedCenter = viewportCenter * v.xmapA + v.xmapB;
    live[8] = mappedCenter - v.viewport[2] * 0.5f * v.xmapA * v.xmapClip;
    live[9] = v.viewport[1];
    live[10] = v.viewport[2] * v.xmapA * v.xmapClip;
    live[11] = v.viewport[3];
    // C187 is the viewport mapping block consumed by the generated vertex
    // program: game scale/X followed by the target dimensions.  Do not leave
    // the XF viewport's depth pair in front of it; that made the generated
    // path use z-offset/scale as its 2D scale and sent every vertex offscreen.
    live[12] = v.gameScale;
    live[13] = v.gameX;
    live[14] = v.tvW;
    live[15] = v.tvH;
    // C188.xy are depth scale/offset, already divided by the XF 24-bit
    // depth range.  This keeps the live block self-contained without
    // spending a constant register on 1 / 16777215.  The generated shader
    // evaluates C188.x * z + C188.y * w, matching transformPosition.
    live[16] = v.xfViewport[2] / 16777215.0f;
    live[17] = v.xfViewport[5] / 16777215.0f;
}

/* Which interpolated values a TEV pixel shader imports: texture coordinate n
 * is semantic n, rasterised colour channel c is semantic 8 + c (the TEV
 * vertex shaders export PARAMn with spi_vs_out_id byte n = n). */
static inline void tevScanInputs(uint32_t stages, const uint32_t* order, const uint32_t* colorEnv,
                                 const uint32_t* alphaEnv, uint32_t numInd, uint32_t iref,
                                 bool useCoord[8], bool useRas[2]) {
    for (uint32_t i = 0; i < numInd; ++i) useCoord[(iref >> (6 * i + 3)) & 7] = true;
    for (uint32_t stage = 0; stage < stages; ++stage) {
        const uint32_t o = order[stage], ce = colorEnv[stage], ae = alphaEnv[stage];
        if ((o >> 6) & 1) useCoord[(o >> 3) & 7] = true;
        const uint32_t chan = (o >> 7) & 7;
        const uint32_t cs[4] = {(ce >> 12) & 15, (ce >> 8) & 15, (ce >> 4) & 15, ce & 15};
        const uint32_t as[4] = {(ae >> 13) & 7, (ae >> 10) & 7, (ae >> 7) & 7, (ae >> 4) & 7};
        for (int i = 0; i < 4; ++i) {
            if (cs[i] == 10 || cs[i] == 11 || as[i] == 5) { if (chan <= 1) useRas[chan] = true; }
        }
    }
}

/* Returns the number of imported values; semantic[] lists them in GPR order
 * and coordSlot/rasSlot receive each input's GPR (left untouched when not
 * imported). */
static inline uint32_t tevImportSemantics(const bool useCoord[8], const bool useRas[2],
                                          uint32_t semantic[10], uint32_t coordSlot[8],
                                          uint32_t rasSlot[2]) {
    uint32_t next = 0;
    for (uint32_t c = 0; c < 8; ++c)
        if (useCoord[c]) { coordSlot[c] = next; semantic[next++] = c; }
    for (uint32_t c = 0; c < 2; ++c)
        if (useRas[c]) { rasSlot[c] = next; semantic[next++] = 8 + c; }
    if (!next) { semantic[0] = 8; next = 1; }  // a pixel shader imports at least one value
    return next;
}

} // namespace gx

#endif
