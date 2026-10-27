#include "../grass_math.h"

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
    GrassNearRecord r = {};
    r.x = b.x;
    r.z = b.z;
    r.y = corner == kGrassCenter ? b.apexY : b.baseY;
    r.drawCoefficient = corner == kGrassLeft ? -1.0f : corner == kGrassRight ? 1.0f : 0.0f;
    r.swingCoefficient = corner == kGrassCenter ? 1.0f : 0.0f;
    r.swingSlot = static_cast<uint8_t>(slot % kGrassSwingSlots);
    r.color = corner == kGrassCenter ? 0 : 1;
    return r;
}

static GrassFarRecord farRecord(const GrassFarBlade& b, uint32_t slot,
                                GrassCorner corner) {
    GrassFarRecord r = {};
    r.x = b.x;
    r.z = b.z;
    r.y = corner == kGrassCenter ? b.apexY : b.baseY;
    r.drawCoefficient = corner == kGrassLeft ? -1 : corner == kGrassRight ? 1 : 0;
    r.swingCoefficient = corner == kGrassCenter ? 1 : 0;
    r.swingSlot = static_cast<uint8_t>(slot % kGrassSwingSlots);
    r.color = corner == kGrassCenter ? 0 : 1;
    return r;
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

} // namespace

int main() {
    static_assert(sizeof(GrassNearRecord) == 32, "near record ABI changed");
    static_assert(sizeof(GrassFarRecord) == 16, "far record ABI changed");
    if (!runSamples() || !runOverflow()) return 1;
    std::puts("grass GPU math oracle: PASS");
    return 0;
}
