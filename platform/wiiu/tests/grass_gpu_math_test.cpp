#include "../grass_math.h"
#include "../gx2_vertex_program.h"

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
    return grassMakeRecord(b.x, corner == kGrassCenter ? b.apexY : b.baseY, b.z,
                           corner == kGrassLeft ? -1 : corner == kGrassRight ? 1 : 0,
                           corner == kGrassCenter ? 1 : 0, slot,
                           corner == kGrassCenter ? 0 : 1);
}

static GrassFarRecord farRecord(const GrassFarBlade& b, uint32_t slot,
                                GrassCorner corner) {
    return grassMakeRecord(float(b.x), float(corner == kGrassCenter ? b.apexY : b.baseY),
                           float(b.z),
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

static bool runNegEncodingCheck() {
    // `_R(n) _NEG, ch` sets the source negate bit (round 1's `ch _NEG` shifted
    // it out of the channel field; that form no longer appears anywhere).
    if (!gx::latteNegEncodingOk()) {
        std::fprintf(stderr, "_R(n) _NEG, ch does not set the negate bit\n");
        return false;
    }
    return true;
}

static bool runGameLayoutExpansion() {
    // The game's arrays: TVec3<f32> (x, apex y, z), TVec3<s16> (x, base y, z),
    // s16 apex, base y.  Records must follow the CPU loop's vertex order.
    const uint32_t count = 23;
    float near[count * 3];
    int16_t far[count * 3], apex[count];
    for (uint32_t i = 0; i < count; ++i) {
        near[i * 3 + 0] = float(i) * 3.5f - 40.0f;
        near[i * 3 + 1] = 250.0f + float(i);
        near[i * 3 + 2] = -float(i) * 2.25f + 9.0f;
        far[i * 3 + 0] = int16_t(near[i * 3 + 0]);
        far[i * 3 + 1] = int16_t(-60);
        far[i * 3 + 2] = int16_t(near[i * 3 + 2]);
        apex[i] = int16_t(-60 + int(near[i * 3 + 1]));
    }
    GrassGpuRecord nearRec[count * 3], farRec[count * 3];
    grassExpandGroup(count, near, far, apex, -60.0f, nearRec, farRec);
    GrassNearDynamic nd = {-7.5f, 11.25f, {1, -2, 3, -4, 5, -6, 7, -8, 9, -10}};
    GrassFarDynamic fd = {-7, 11, {1, -2, 3, -4, 5, -6, 7, -8, 9, -10}};
    for (uint32_t i = 0; i < count; ++i) {
        const GrassNearBlade nb = {near[i * 3 + 0], near[i * 3 + 2], near[i * 3 + 1], -60.0f};
        const GrassFarBlade fb = {far[i * 3 + 0], far[i * 3 + 2], far[i * 3 + 1], apex[i]};
        for (GrassCorner c : {kGrassLeft, kGrassCenter, kGrassRight}) {
            if (!equalNear(grassCpuNearVertex(nb, nd, i, c), grassShaderNearVertex(nearRec[i * 3 + c], nd), i, c) ||
                !equalFar(grassCpuFarVertex(fb, fd, i, c), grassShaderFarVertex(farRec[i * 3 + c], fd), i, c))
                return false;
        }
    }
    return true;
}

static bool runUniformLayout() {
    float swing[kGrassSwingSlots];
    for (uint32_t k = 0; k < kGrassSwingSlots; ++k) swing[k] = float(k) * 10.0f - 45.0f;
    float block[kGrassUniformRegs * 4];
    grassPackUniforms(-3.0f, 4.0f, swing, block);
    if (block[0] != -3.0f || block[1] != 4.0f) return false;
    for (uint32_t k = 0; k < kGrassSwingSlots; ++k) {
        // One swing per vector: C[214 + k] = (swing[k], k, 0, 0).
        const float* v = block + (kGrassUniformSwingReg - kGrassUniformDrawReg + k) * 4;
        if (v[0] != swing[k] || v[1] != float(k) || v[2] != 0.0f || v[3] != 0.0f) {
            std::fprintf(stderr, "swing slot %u is not alone in C%u.x\n", k, kGrassUniformSwingReg + k);
            return false;
        }
    }
    const float* upper = block + (kGrassUniformColourReg - kGrassUniformDrawReg) * 4;
    for (uint32_t c = 0; c < 4; ++c)
        if (upper[c] != float(kGrassUpperColor[c]) / 255.0f || upper[4 + c] != float(kGrassLowerColor[c]) / 255.0f)
            return false;
    return kGrassUniformDrawReg >= gx::kVtxLiveBase + gx::kVtxLiveRegs;
}

static bool runS16Guard() {
    // mWidth 12, mSwingWidth 50: |coordinate| up to 32702 fits, 32703 does not.
    if (!grassFarFitsS16(32702, 12.0f, 50.0f) || grassFarFitsS16(32703, 12.0f, 50.0f)) return false;
    const int16_t far[6] = {-32000, 0, 100, 5, 0, -32703};
    if (grassFarMaxAbs(2, far) != 32703) return false;
    const int16_t swing[kGrassSwingSlots] = {0, 0, 0, -50, 0, 0, 0, 0, 0, 0};
    // maxAbs + max(|drawX|, |drawZ|) + max|swing| must stay below 32767.
    return grassFarDrawFitsS16(32704, 12, 0, swing) && !grassFarDrawFitsS16(32705, 12, 0, swing) &&
           grassFarDrawFitsS16(32704, 0, -12, swing) && !grassFarDrawFitsS16(32705, 0, -12, swing) &&
           grassFarDrawFitsS16(32716, 0, 0, swing) && !grassFarDrawFitsS16(32717, 0, 0, swing);
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
    static_assert(offsetof(GrassNearRecord, drawBiased) == 12,
                  "near packed metadata offset changed");
    static_assert(offsetof(GrassNearRecord, tip) == 13,
                  "near tip coefficient offset changed");
    static_assert(offsetof(GrassFarRecord, swingSlot) == 14,
                  "far swing slot offset changed");
    static_assert(kGrassDrawPayloadBytes == 52,
                  "escape payload must carry 12 + 10*4 bytes");
    if (kGrassUpperColor[0] != 0x3c || kGrassUpperColor[1] != 0xc8 ||
        kGrassUpperColor[2] != 0x3c || kGrassUpperColor[3] != 0xff ||
        kGrassLowerColor[0] != 0x28 || kGrassLowerColor[1] != 0x64 ||
        kGrassLowerColor[2] != 0x28 || kGrassLowerColor[3] != 0xff) {
        std::fprintf(stderr, "grass color contract changed\n");
        return 1;
    }
    if (!runSamples() || !runOverflow() || !runGenerationAndHookSequence() ||
        !runWireRoundTrip() || !runNegEncodingCheck() || !runGameLayoutExpansion() ||
        !runUniformLayout() || !runS16Guard()) return 1;
    std::puts("grass GPU math oracle: PASS");
    return 0;
}
