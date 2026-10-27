#include "../grass_math.h"
#include "../gx2_shader_inl.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace sms_wiiu_grass;

namespace {

static uint32_t bits(float value) {
    uint32_t result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

static bool equalNear(const GrassNearVertex& a, const GrassNearVertex& b,
                      uint32_t blade, GrassCorner corner) {
    if (bits(a.x) != bits(b.x) || bits(a.y) != bits(b.y) || bits(a.z) != bits(b.z) ||
        a.color != b.color) {
        std::fprintf(stderr, "near blade=%u corner=%u mismatch x=%08x/%08x y=%08x/%08x z=%08x/%08x color=%u/%u\n",
                     blade, unsigned(corner), bits(a.x), bits(b.x), bits(a.y), bits(b.y),
                     bits(a.z), bits(b.z), unsigned(a.color), unsigned(b.color));
        return false;
    }
    return true;
}

static bool equalFar(const GrassFarVertex& a, const GrassFarVertex& b,
                     uint32_t blade, GrassCorner corner) {
    if (a.x != b.x || a.y != b.y || a.z != b.z || a.color != b.color) {
        std::fprintf(stderr, "far blade=%u corner=%u mismatch x=%d/%d y=%d/%d z=%d/%d color=%u/%u\n",
                     blade, unsigned(corner), a.x, b.x, a.y, b.y, a.z, b.z,
                     unsigned(a.color), unsigned(b.color));
        return false;
    }
    return true;
}

static GrassNearRecord nearRecord(const GrassNearBlade& b, uint32_t slot,
                                  GrassCorner corner) {
    return grassMakeRecord(b.x, b.z, corner == kGrassCenter ? b.apexY : b.baseY,
                           corner == kGrassLeft ? -1 : corner == kGrassRight ? 1 : 0,
                           corner == kGrassCenter ? 1 : 0, slot,
                           corner == kGrassCenter ? 0 : 1);
}

static GrassFarRecord farRecord(const GrassFarBlade& b, uint32_t slot,
                                GrassCorner corner) {
    return grassMakeRecord(float(b.x), float(b.z),
                           float(corner == kGrassCenter ? b.apexY : b.baseY),
                           corner == kGrassLeft ? -1 : corner == kGrassRight ? 1 : 0,
                           corner == kGrassCenter ? 1 : 0, slot,
                           corner == kGrassCenter ? 0 : 1);
}

static bool runWireRoundTrip() {
    GrassDrawWirePayload source = {0x12345678u, 0x80000001u, 0x7fffffffu,
                                   {0x00000000u, 0xffffffffu, 0x3f800000u,
                                    0x80000000u, 0x01020304u, 0xa5a5a5a5u,
                                    0x7fc00001u, 0xdeadbeefu, 0x11223344u,
                                    0x55667788u}};
    uint8_t bytes[kGrassDrawPayloadBytes] = {};
    grassEncodeDrawPayload(bytes, source);
    const GrassDrawWirePayload decoded = grassDecodeDrawPayload(bytes);
    if (std::memcmp(&source, &decoded, sizeof source) != 0) {
        std::fprintf(stderr, "grass escape payload round-trip lost one of 10 swings\n");
        return false;
    }
    return true;
}

static bool runArEncodingCheck() {
    const uint64_t toPv = ALU_MOV(__, _x, ALU_SRC_PS, _x);
    const uint64_t mova = ALU_MOVA_INT(__, _x, ALU_SRC_PV, _x);
    const uint32_t toPvWord = __builtin_bswap32(uint32_t(toPv >> 32));
    const uint32_t movaWord = __builtin_bswap32(uint32_t(mova));
    return (toPvWord & 0x1ffu) == (ALU_SRC_PS & 0x1ffu) &&
           ((movaWord >> 7) & 0xffu) == ALU_OP2_INST_MOVA_INT;
}

static bool runSamples() {
    GrassNearDynamic nearDynamic = {-13.25f, 27.5f,
                                    { -31.0f, 0.25f, 17.75f, -8.5f, 44.0f,
                                      -19.0f, 2.0f, 61.5f, -73.0f, 5.5f }};
    GrassFarDynamic farDynamic = {-327, 291,
                                  { -32768, -17, 4, 32767, -901, 1200,
                                    -2, 8, 16200, -30000 }};
    const uint32_t indices[] = {0, 1, 9, 10, 63, 127, 3, 28, 54, 91};
    for (uint32_t n : indices) {
        GrassNearBlade nb = {float(n) * 1.25f - 90.0f, float(n) * -0.75f + 7.0f,
                             float(n) * 0.5f + 160.0f, float(n) * 0.25f - 20.0f};
        GrassFarBlade fb = {static_cast<int16_t>(n * 733 - 24000),
                            static_cast<int16_t>(n * -511 + 19000),
                            static_cast<int16_t>(n * 29 - 7000),
                            static_cast<int16_t>(n * 37 + 8000)};
        for (GrassCorner c : {kGrassLeft, kGrassCenter, kGrassRight}) {
            if (!equalNear(grassCpuNearVertex(nb, nearDynamic, n, c),
                           grassShaderNearVertex(nearRecord(nb, n, c), nearDynamic), n, c))
                return false;
            if (!equalFar(grassCpuFarVertex(fb, farDynamic, n, c),
                          grassShaderFarVertex(farRecord(fb, n, c), farDynamic), n, c))
                return false;
        }
    }
    return true;
}

static bool runOverflow() {
    GrassFarBlade blade = {32767, -32768, 7, 19};
    GrassFarDynamic dynamic = {1, -1, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
    GrassFarRecord right = farRecord(blade, 0, kGrassRight);
    GrassFarRecord left = farRecord(blade, 0, kGrassLeft);
    const GrassFarVertex r = grassShaderFarVertex(right, dynamic);
    const GrassFarVertex l = grassShaderFarVertex(left, dynamic);
    if (r.x != static_cast<int16_t>(static_cast<uint16_t>(32768)) ||
        l.z != static_cast<int16_t>(static_cast<uint16_t>(-32767))) {
        std::fprintf(stderr, "far signed-16 overflow was widened: right.x=%d left.z=%d\n", r.x, l.z);
        return false;
    }
    return true;
}

static bool runGenerationAndHookSequence() {
    GrassNearBlade blade = {10.0f, -4.0f, 33.0f, 2.0f};
    GrassNearRecord record = nearRecord(blade, 9, kGrassCenter);
    GrassNearDynamic first = {1.0f, -2.0f,
                              {0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, 0.0f, 0.0f, 7.0f, 0.0f}};
    GrassNearDynamic second = first;
    second.swing[9] = -11.0f;
    const GrassNearVertex a = grassShaderNearVertex(record, first);
    const GrassNearVertex b = grassShaderNearVertex(record, second);
    if (bits(a.x) == bits(b.x) || a.color != 0) {
        std::fprintf(stderr, "hook sequence did not carry changing near uniform/center color\n");
        return false;
    }
    GrassFarBlade farBlade = {100, -100, 4, 8};
    GrassFarRecord farRecordValue = farRecord(farBlade, 9, kGrassCenter);
    GrassFarDynamic farFirst = {3, -5, {0, 0, 0, 0, 0, 0, 0, 0, 0, 120}};
    GrassFarDynamic farSecond = farFirst;
    farSecond.swing[9] = -120;
    if (grassShaderFarVertex(farRecordValue, farFirst).x ==
            grassShaderFarVertex(farRecordValue, farSecond).x ||
        grassShaderFarVertex(farRecordValue, farFirst).color != 0) {
        std::fprintf(stderr, "hook sequence did not carry changing far uniform\n");
        return false;
    }
    return true;
}

} // namespace

int main() {
    static_assert(sizeof(GrassNearRecord) == 16, "near record ABI changed");
    static_assert(sizeof(GrassFarRecord) == 16, "far record ABI changed");
    static_assert(offsetof(GrassNearRecord, drawCoefficient) == 12,
                  "near packed metadata offset changed");
    static_assert(offsetof(GrassNearRecord, swingCoefficient) == 13,
                  "near swing coefficient offset changed");
    static_assert(offsetof(GrassFarRecord, swingSlot) == 14,
                  "far swing slot offset changed");
    static_assert(kGrassDrawPayloadBytes == 52,
                  "escape payload must carry 12 + 10*4 bytes");
    static_assert(kGrassUniformDrawOffset == 0 && kGrassUniformModeOffset == 8 &&
                      kGrassUniformSwingOffset == 16,
                  "grass dynamic uniform offsets changed");
    if (kGrassUpperColor[0] != 0x3c || kGrassUpperColor[1] != 0xc8 ||
        kGrassUpperColor[2] != 0x3c || kGrassUpperColor[3] != 0xff ||
        kGrassLowerColor[0] != 0x28 || kGrassLowerColor[1] != 0x64 ||
        kGrassLowerColor[2] != 0x28 || kGrassLowerColor[3] != 0xff) {
        std::fprintf(stderr, "grass color contract changed\n");
        return 1;
    }
    if (!runSamples() || !runOverflow() || !runGenerationAndHookSequence() ||
        !runWireRoundTrip() || !runArEncodingCheck()) return 1;
    std::puts("grass GPU math oracle: PASS");
    return 0;
}
