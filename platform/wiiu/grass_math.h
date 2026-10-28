#ifndef SMS_WIIU_GRASS_MATH_H
#define SMS_WIIU_GRASS_MATH_H

#include <stdint.h>
#include <stddef.h>

namespace sms_wiiu_grass {

static const uint32_t kGrassSwingSlots = 10;
static const uint32_t kGrassRegisterPayloadBytes = 28;
static const uint32_t kGrassDrawPayloadBytes = 12 + kGrassSwingSlots * 4;
static const uint8_t kGrassUpperColor[4] = {0x3c, 0xc8, 0x3c, 0xff};
static const uint8_t kGrassLowerColor[4] = {0x28, 0x64, 0x28, 0xff};

/* Grass vertex-uniform block, after the generated program's live block
 * (C192..C212).  One value per vector, no lane selection:
 *   C213          (drawX, drawZ, 0, 0)
 *   C214 + k      (swing[k], k, 0, 0)        k = 0..9
 *   C224, C225    colour-table entries 0 and 1 (RGBA / 255)
 * The shader selects swing[slot] as sum_k (slot == C[214+k].y) * C[214+k].x
 * (float SETE/MULADD only), and the colour with CNDGT. */
static const uint32_t kGrassUniformDrawReg = 213;
static const uint32_t kGrassUniformSwingReg = 214;
static const uint32_t kGrassUniformColourReg = kGrassUniformSwingReg + kGrassSwingSlots;
static const uint32_t kGrassUniformRegs = 1 + kGrassSwingSlots + 2;
static_assert(kGrassUniformColourReg + 2 == kGrassUniformDrawReg + kGrassUniformRegs,
              "grass uniform block is contiguous");
static_assert(kGrassUniformDrawReg + kGrassUniformRegs <= 256, "grass uniform block fits the constant file");

/* Both passes use the same float-only program; the far pass is fed integers
 * stored exactly as floats (s16 positions, mDrawVecS16, unk24). */
static inline void grassPackUniforms(float drawX, float drawZ, const float* swing,
                                     float out[kGrassUniformRegs * 4]) {
    for (uint32_t i = 0; i < kGrassUniformRegs * 4; ++i) out[i] = 0.0f;
    out[0] = drawX;
    out[1] = drawZ;
    for (uint32_t k = 0; k < kGrassSwingSlots; ++k) {
        out[(1 + k) * 4 + 0] = swing[k];
        out[(1 + k) * 4 + 1] = float(k);
    }
    for (uint32_t c = 0; c < 4; ++c) {
        out[(1 + kGrassSwingSlots) * 4 + c] = float(kGrassUpperColor[c]) / 255.0f;
        out[(2 + kGrassSwingSlots) * 4 + c] = float(kGrassLowerColor[c]) / 255.0f;
    }
}

enum GrassCorner : uint8_t {
    kGrassLeft = 0,
    kGrassCenter = 1,
    kGrassRight = 2,
};

struct GrassNearBlade {
    float x;
    float z;
    float apexY;
    float baseY;
};

struct GrassFarBlade {
    int16_t x;
    int16_t z;
    int16_t baseY;
    int16_t apexY;
};

struct GrassNearDynamic {
    float drawX;
    float drawZ;
    float swing[kGrassSwingSlots];
};

struct GrassFarDynamic {
    int16_t drawX;
    int16_t drawZ;
    int16_t swing[kGrassSwingSlots];
};

struct GrassNearVertex {
    float x;
    float y;
    float z;
    uint8_t color;
};

struct GrassFarVertex {
    int16_t x;
    int16_t y;
    int16_t z;
    uint8_t color;
};

/* One record is one fetched vertex: position x, y, z as floats (fetch
 * location 0, w reads 1) and four UNSIGNED bytes (fetch location 1,
 * USCALED8 -> 0..255 as floats): draw coefficient + 1 (0/1/2), tip
 * coefficient (0/1), swing slot (0..9) and colour-table index (0/1).
 * 16 bytes per vertex. */
static const uint8_t kGrassDrawBias = 1;
struct GrassGpuRecord {
    float x;
    float y;
    float z;
    uint8_t drawBiased;
    uint8_t tip;
    uint8_t swingSlot;
    uint8_t color;
};
typedef GrassGpuRecord GrassNearRecord;
typedef GrassGpuRecord GrassFarRecord;
static_assert(sizeof(GrassGpuRecord) == 16, "grass GPU record must be 16 bytes");
static_assert(offsetof(GrassGpuRecord, y) == 4 && offsetof(GrassGpuRecord, z) == 8,
              "grass position is x, y, z");
static_assert(offsetof(GrassGpuRecord, drawBiased) == 12,
              "grass metadata must follow xyz");

static inline GrassGpuRecord grassMakeRecord(float x, float y, float z,
                                              int drawCoefficient,
                                              int tip,
                                              uint32_t swingSlot,
                                              uint8_t color) {
    GrassGpuRecord result = {};
    result.x = x;
    result.y = y;
    result.z = z;
    result.drawBiased = static_cast<uint8_t>(drawCoefficient + kGrassDrawBias);
    result.tip = static_cast<uint8_t>(tip);
    result.swingSlot = static_cast<uint8_t>(swingSlot % kGrassSwingSlots);
    result.color = color;
    return result;
}

/* Expands one registered group into its static records, three per blade in
 * the CPU loop's order (left, centre, right), from the game's own arrays:
 * near = TVec3<f32>[count] (x, y = apex, z), far = TVec3<s16>[count]
 * (x, y = base, z), apex = s16[count], baseY = mPosition.y. */
static inline void grassExpandGroup(uint32_t count, const float* near, const int16_t* far,
                                    const int16_t* apex, float baseY,
                                    GrassGpuRecord* nearOut, GrassGpuRecord* farOut) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = i % kGrassSwingSlots;
        const float nx = near[i * 3 + 0], ny = near[i * 3 + 1], nz = near[i * 3 + 2];
        nearOut[i * 3 + 0] = grassMakeRecord(nx, baseY, nz, -1, 0, slot, 1);
        nearOut[i * 3 + 1] = grassMakeRecord(nx, ny, nz, 0, 1, slot, 0);
        nearOut[i * 3 + 2] = grassMakeRecord(nx, baseY, nz, 1, 0, slot, 1);
        const float fx = float(far[i * 3 + 0]), fy = float(far[i * 3 + 1]);
        const float fz = float(far[i * 3 + 2]), fay = float(apex[i]);
        farOut[i * 3 + 0] = grassMakeRecord(fx, fy, fz, -1, 0, slot, 1);
        farOut[i * 3 + 1] = grassMakeRecord(fx, fay, fz, 0, 1, slot, 0);
        farOut[i * 3 + 2] = grassMakeRecord(fx, fy, fz, 1, 0, slot, 1);
    }
}

/* Largest |x| or |z| of a group's far base positions. */
static inline int32_t grassFarMaxAbs(uint32_t count, const int16_t* far) {
    int32_t result = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t x = far[i * 3 + 0] < 0 ? -int32_t(far[i * 3 + 0]) : far[i * 3 + 0];
        const int32_t z = far[i * 3 + 2] < 0 ? -int32_t(far[i * 3 + 2]) : far[i * 3 + 2];
        if (x > result) result = x;
        if (z > result) result = z;
    }
    return result;
}

/* The float shader never wraps to 16 bits, so a far sum must not be able to
 * leave the s16 range: |coordinate| + |mWidth| + |mSwingWidth| + 2 < 32767
 * (mDrawVecS16 is camera right x mWidth, truncated; unk24 is a truncated
 * mSwingWidth * sin).  A group that fails this uses the CPU loop. */
static inline bool grassFarFitsS16(int32_t maxAbs, float width, float swingWidth) {
    const float w = width < 0.0f ? -width : width;
    const float s = swingWidth < 0.0f ? -swingWidth : swingWidth;
    return float(maxAbs) + w + s + 2.0f < 32767.0f;
}

/* Per-draw form of the same bound with the values actually sent. */
static inline bool grassFarDrawFitsS16(int32_t maxAbs, int32_t drawX, int32_t drawZ,
                                       const int16_t* swing) {
    int32_t m = drawX < 0 ? -drawX : drawX;
    if ((drawZ < 0 ? -drawZ : drawZ) > m) m = drawZ < 0 ? -drawZ : drawZ;
    int32_t s = 0;
    for (uint32_t k = 0; k < kGrassSwingSlots; ++k) {
        const int32_t a = swing[k] < 0 ? -int32_t(swing[k]) : swing[k];
        if (a > s) s = a;
    }
    return maxAbs + m + s < 32767;
}

static inline int16_t grassWrapS16(int32_t value) {
    return static_cast<int16_t>(static_cast<uint16_t>(value));
}

static inline GrassNearVertex grassCpuNearVertex(const GrassNearBlade& blade,
                                                  const GrassNearDynamic& dynamic,
                                                  uint32_t bladeIndex,
                                                  GrassCorner corner) {
    GrassNearVertex result;
    const uint32_t slot = bladeIndex % kGrassSwingSlots;
    if (corner == kGrassLeft) {
        result.x = blade.x - dynamic.drawX;
        result.y = blade.baseY;
        result.z = blade.z - dynamic.drawZ;
        result.color = 1;
    } else if (corner == kGrassCenter) {
        result.x = blade.x + dynamic.swing[slot];
        result.y = blade.apexY;
        result.z = blade.z;
        result.color = 0;
    } else {
        result.x = blade.x + dynamic.drawX;
        result.y = blade.baseY;
        result.z = blade.z + dynamic.drawZ;
        result.color = 1;
    }
    return result;
}

static inline GrassFarVertex grassCpuFarVertex(const GrassFarBlade& blade,
                                                const GrassFarDynamic& dynamic,
                                                uint32_t bladeIndex,
                                                GrassCorner corner) {
    GrassFarVertex result;
    const int32_t slot = static_cast<int32_t>(bladeIndex % kGrassSwingSlots);
    if (corner == kGrassLeft) {
        result.x = grassWrapS16(static_cast<int32_t>(blade.x) - dynamic.drawX);
        result.y = blade.baseY;
        result.z = grassWrapS16(static_cast<int32_t>(blade.z) - dynamic.drawZ);
        result.color = 1;
    } else if (corner == kGrassCenter) {
        result.x = grassWrapS16(static_cast<int32_t>(blade.x) + dynamic.swing[slot]);
        result.y = blade.apexY;
        result.z = blade.z;
        result.color = 0;
    } else {
        result.x = grassWrapS16(static_cast<int32_t>(blade.x) + dynamic.drawX);
        result.y = blade.baseY;
        result.z = grassWrapS16(static_cast<int32_t>(blade.z) + dynamic.drawZ);
        result.color = 1;
    }
    return result;
}

/* C++ model of the shader arithmetic (the Latte interpreter test runs the
 * real program; this model documents the intended float sequence). */
static inline GrassNearVertex grassShaderNearVertex(const GrassNearRecord& record,
                                                     const GrassNearDynamic& dynamic) {
    GrassNearVertex result;
    const uint32_t slot = record.swingSlot % kGrassSwingSlots;
    const float coefficient = float(record.drawBiased) - 1.0f;
    const float tip = float(record.tip);
    float x = coefficient * dynamic.drawX + record.x;
    x = tip * dynamic.swing[slot] + x;
    result.x = x;
    result.y = record.y;
    result.z = coefficient * dynamic.drawZ + record.z;
    result.color = record.color;
    return result;
}

static inline GrassFarVertex grassShaderFarVertex(const GrassFarRecord& record,
                                                   const GrassFarDynamic& dynamic) {
    GrassNearDynamic asFloat;
    asFloat.drawX = float(dynamic.drawX);
    asFloat.drawZ = float(dynamic.drawZ);
    for (uint32_t k = 0; k < kGrassSwingSlots; ++k) asFloat.swing[k] = float(dynamic.swing[k]);
    const GrassNearVertex v = grassShaderNearVertex(record, asFloat);
    GrassFarVertex result;
    result.x = static_cast<int16_t>(v.x);
    result.y = static_cast<int16_t>(v.y);
    result.z = static_cast<int16_t>(v.z);
    result.color = v.color;
    return result;
}

/* The stream escape is deliberately specified beside the host oracle. Both
 * the game-thread producer and worker parser use this byte contract. */
struct GrassDrawWirePayload {
    uint32_t group;
    uint32_t drawX;
    uint32_t drawZ;
    uint32_t swing[kGrassSwingSlots];
};

static inline void grassWirePutBE32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

static inline uint32_t grassWireGetBE32(const uint8_t* in) {
    return uint32_t(in[0]) << 24 | uint32_t(in[1]) << 16 |
           uint32_t(in[2]) << 8 | uint32_t(in[3]);
}

static inline void grassEncodeDrawPayload(uint8_t out[kGrassDrawPayloadBytes],
                                           const GrassDrawWirePayload& value) {
    grassWirePutBE32(out + 0, value.group);
    grassWirePutBE32(out + 4, value.drawX);
    grassWirePutBE32(out + 8, value.drawZ);
    for (uint32_t i = 0; i < kGrassSwingSlots; ++i)
        grassWirePutBE32(out + 12 + i * 4, value.swing[i]);
}

static inline GrassDrawWirePayload grassDecodeDrawPayload(
    const uint8_t in[kGrassDrawPayloadBytes]) {
    GrassDrawWirePayload value = {};
    value.group = grassWireGetBE32(in + 0);
    value.drawX = grassWireGetBE32(in + 4);
    value.drawZ = grassWireGetBE32(in + 8);
    for (uint32_t i = 0; i < kGrassSwingSlots; ++i)
        value.swing[i] = grassWireGetBE32(in + 12 + i * 4);
    return value;
}

} // namespace sms_wiiu_grass

#endif
