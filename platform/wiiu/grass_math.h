#ifndef SMS_WIIU_GRASS_MATH_H
#define SMS_WIIU_GRASS_MATH_H

#include <stdint.h>
#include <stddef.h>

namespace sms_wiiu_grass {

static const uint32_t kGrassSwingSlots = 10;
static const uint32_t kGrassRegisterPayloadBytes = 28;
static const uint32_t kGrassDrawPayloadBytes = 12 + kGrassSwingSlots * 4;
static const uint32_t kGrassUniformDrawOffset = 0;
static const uint32_t kGrassUniformModeOffset = 8;
static const uint32_t kGrassUniformSwingOffset = 16;
static const uint8_t kGrassUpperColor[4] = {0x3c, 0xc8, 0x3c, 0xff};
static const uint8_t kGrassLowerColor[4] = {0x28, 0x64, 0x28, 0xff};

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

/* One record is one fetched vertex. The final word is four signed/scaled
 * bytes: draw coefficient (-1/0/+1), tip coefficient (0/1), swing slot, and
 * color-table index. This keeps every static vertex at 16 bytes. */
struct GrassGpuRecord {
    float x;
    float z;
    float y;
    uint8_t drawCoefficient;
    uint8_t swingCoefficient;
    uint8_t swingSlot;
    uint8_t color;
};
typedef GrassGpuRecord GrassNearRecord;
typedef GrassGpuRecord GrassFarRecord;
static_assert(sizeof(GrassGpuRecord) == 16, "grass GPU record must be 16 bytes");
static_assert(offsetof(GrassGpuRecord, drawCoefficient) == 12,
              "grass metadata must follow xyz");

static inline uint8_t grassCoefficientByte(int coefficient) {
    return static_cast<uint8_t>(static_cast<int8_t>(coefficient));
}

static inline GrassGpuRecord grassMakeRecord(float x, float z, float y,
                                              int drawCoefficient,
                                              int swingCoefficient,
                                              uint32_t swingSlot,
                                              uint8_t color) {
    GrassGpuRecord result = {};
    result.x = x;
    result.z = z;
    result.y = y;
    result.drawCoefficient = grassCoefficientByte(drawCoefficient);
    result.swingCoefficient = static_cast<uint8_t>(swingCoefficient);
    result.swingSlot = static_cast<uint8_t>(swingSlot % kGrassSwingSlots);
    result.color = color;
    return result;
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

static inline GrassNearVertex grassShaderNearVertex(const GrassNearRecord& record,
                                                     const GrassNearDynamic& dynamic) {
    GrassNearVertex result;
    const uint32_t slot = record.swingSlot % kGrassSwingSlots;
    const int drawCoefficient = static_cast<int8_t>(record.drawCoefficient);
    const int swingCoefficient = record.swingCoefficient;
    result.x = record.x + float(drawCoefficient) * dynamic.drawX +
               float(swingCoefficient) * dynamic.swing[slot];
    result.y = record.y;
    result.z = record.z + float(drawCoefficient) * dynamic.drawZ;
    result.color = record.color;
    return result;
}

static inline GrassFarVertex grassShaderFarVertex(const GrassFarRecord& record,
                                                   const GrassFarDynamic& dynamic) {
    GrassFarVertex result;
    const uint32_t slot = record.swingSlot % kGrassSwingSlots;
    const int32_t drawCoefficient = static_cast<int8_t>(record.drawCoefficient);
    const int32_t swingCoefficient = record.swingCoefficient;
    const int32_t baseX = static_cast<int32_t>(record.x);
    const int32_t baseZ = static_cast<int32_t>(record.z);
    const int32_t x = baseX + drawCoefficient * dynamic.drawX +
                      swingCoefficient * dynamic.swing[slot];
    const int32_t z = baseZ + drawCoefficient * dynamic.drawZ;
    result.x = grassWrapS16(x);
    result.y = grassWrapS16(static_cast<int32_t>(record.y));
    result.z = grassWrapS16(z);
    result.color = record.color;
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
