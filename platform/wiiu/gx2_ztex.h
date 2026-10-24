#ifndef SMS_WIIU_GX2_ZTEX_H
#define SMS_WIIU_GX2_ZTEX_H

#include <stdint.h>

namespace gx {

// These are the scalar values used by the generated z-texture ALU.  The
// emitter and the host regression test both walk the same constexpr table;
// keep the table in operation order because the intermediate precision and
// the placement of the floor operations are part of the GX emulation.
enum Gx2ZTextureValue : uint8_t {
    kGx2ZTexR,
    kGx2ZTexG,
    kGx2ZTexB,
    kGx2ZTexA,
    kGx2ZTexZ,
    kGx2ZTexQ,
    kGx2ZTexByteMax,
    kGx2ZTexByteScale,
    kGx2ZTexWrapInv,
    kGx2ZTexWrapRange,
    kGx2ZTexRangeInv,
    kGx2ZTexRange,
    kGx2ZTexBias,
    kGx2ZTexRaster,
};

enum Gx2ZTextureAlu : uint8_t {
    kGx2ZMul,
    kGx2ZAdd,
    kGx2ZFloor,
};

struct Gx2ZTextureOp {
    uint8_t alu;
    uint8_t dst;
    uint8_t lhs;
    uint8_t rhs;
    uint8_t negateRhs;
};

struct Gx2ZTextureOpTable {
    const Gx2ZTextureOp* ops;
    uint32_t count;
};

// Z8/Z16/Z24X8 decoding, GX_ZT_REPLACE, and the 24-bit wrap/normalisation.
static constexpr Gx2ZTextureOp kGx2Z8ReplaceOps[] = {
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexR, kGx2ZTexByteMax, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexBias, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexZ, kGx2ZTexWrapInv, 0},
    {kGx2ZFloor, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexWrapRange, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 1},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexRangeInv, 0},
};

static constexpr Gx2ZTextureOp kGx2Z8AddOps[] = {
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexR, kGx2ZTexByteMax, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexBias, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexRaster, kGx2ZTexRange, 0},
    {kGx2ZFloor, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexQ, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexZ, kGx2ZTexWrapInv, 0},
    {kGx2ZFloor, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexWrapRange, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 1},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexRangeInv, 0},
};

static constexpr Gx2ZTextureOp kGx2Z16ReplaceOps[] = {
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexA, kGx2ZTexByteMax, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexR, kGx2ZTexByteMax, 0},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexByteScale, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexBias, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexZ, kGx2ZTexWrapInv, 0},
    {kGx2ZFloor, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexWrapRange, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 1},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexRangeInv, 0},
};

static constexpr Gx2ZTextureOp kGx2Z16AddOps[] = {
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexA, kGx2ZTexByteMax, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexR, kGx2ZTexByteMax, 0},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexByteScale, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexBias, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexRaster, kGx2ZTexRange, 0},
    {kGx2ZFloor, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexQ, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexZ, kGx2ZTexWrapInv, 0},
    {kGx2ZFloor, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexWrapRange, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 1},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexRangeInv, 0},
};

static constexpr Gx2ZTextureOp kGx2Z24ReplaceOps[] = {
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexR, kGx2ZTexByteMax, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexG, kGx2ZTexByteMax, 0},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexByteScale, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexB, kGx2ZTexByteMax, 0},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexByteScale, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexBias, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexZ, kGx2ZTexWrapInv, 0},
    {kGx2ZFloor, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexWrapRange, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 1},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexRangeInv, 0},
};

static constexpr Gx2ZTextureOp kGx2Z24AddOps[] = {
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexR, kGx2ZTexByteMax, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexG, kGx2ZTexByteMax, 0},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexByteScale, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexB, kGx2ZTexByteMax, 0},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexByteScale, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexBias, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexRaster, kGx2ZTexRange, 0},
    {kGx2ZFloor, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexQ, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexZ, kGx2ZTexWrapInv, 0},
    {kGx2ZFloor, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexQ, 0},
    {kGx2ZMul, kGx2ZTexQ, kGx2ZTexQ, kGx2ZTexWrapRange, 0},
    {kGx2ZAdd, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexQ, 1},
    {kGx2ZMul, kGx2ZTexZ, kGx2ZTexZ, kGx2ZTexRangeInv, 0},
};

inline Gx2ZTextureOpTable gx2ZTextureOpTable(uint32_t format, bool add) {
    switch (format & 3) {
    case 0: return {add ? kGx2Z8AddOps : kGx2Z8ReplaceOps,
                    add ? uint32_t(sizeof(kGx2Z8AddOps) / sizeof(kGx2Z8AddOps[0]))
                        : uint32_t(sizeof(kGx2Z8ReplaceOps) / sizeof(kGx2Z8ReplaceOps[0]))};
    case 1: return {add ? kGx2Z16AddOps : kGx2Z16ReplaceOps,
                    add ? uint32_t(sizeof(kGx2Z16AddOps) / sizeof(kGx2Z16AddOps[0]))
                        : uint32_t(sizeof(kGx2Z16ReplaceOps) / sizeof(kGx2Z16ReplaceOps[0]))};
    default: return {add ? kGx2Z24AddOps : kGx2Z24ReplaceOps,
                     add ? uint32_t(sizeof(kGx2Z24AddOps) / sizeof(kGx2Z24AddOps[0]))
                         : uint32_t(sizeof(kGx2Z24ReplaceOps) / sizeof(kGx2Z24ReplaceOps[0]))};
    }
}

// GX expands a Z8/Z16/Z24X8 texel into the low bits of a 24-bit integer.
// GX2 samples the decoded texture as normalized RGBA8, so these helpers are
// the host-side reference for the generated Latte ALU sequence in
// gx2_renderer.cpp.
inline uint32_t gx2ZTextureInteger(float r, float g, float b, float a, uint32_t format) {
    const float byte = 255.0f;
    float z;
    switch (format & 3) {
    case 0: // GX_TF_Z8: one byte in the low 8 bits
        z = r * byte;
        break;
    case 1: // GX_TF_Z16: IA8 decoder returns low byte in RGB, high in A
        z = (a * byte) * 256.0f + r * byte;
        break;
    default: // GX_TF_Z24X8: RGB is the 24-bit value, alpha is padding
        z = ((r * byte) * 256.0f + g * byte) * 256.0f + b * byte;
        break;
    }
    return uint32_t(z);
}

inline float gx2ZTextureValue(float r, float g, float b, float a, uint32_t format, uint32_t bias) {
    const uint32_t value = (gx2ZTextureInteger(r, g, b, a, format) + (bias & 0x00ffffffu)) & 0x00ffffffu;
    return float(value) * (1.0f / 16777215.0f);
}

inline float gx2ZTextureAddValue(float rasterZ, float r, float g, float b, float a,
                                 uint32_t format, uint32_t bias) {
    const uint32_t raster = uint32_t(rasterZ * 16777215.0f);
    const uint32_t value = (raster + gx2ZTextureInteger(r, g, b, a, format) +
                            (bias & 0x00ffffffu)) & 0x00ffffffu;
    return float(value) * (1.0f / 16777215.0f);
}

} // namespace gx

#endif
