#include "gx_texture_pack_common.h"

#include <algorithm>
#include <stdio.h>

namespace gx {

namespace {

const uint64_t kPrime1 = 0x9E3779B185EBCA87ull;
const uint64_t kPrime2 = 0xC2B2AE3D27D4EB4Full;
const uint64_t kPrime3 = 0x165667B19E3779F9ull;
const uint64_t kPrime4 = 0x85EBCA77C2B2AE63ull;
const uint64_t kPrime5 = 0x27D4EB2F165667C5ull;

inline uint64_t rotateLeft(uint64_t value, int bits) {
    return value << bits | value >> (64 - bits);
}

inline uint64_t readLe64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = value << 8 | p[i];
    return value;
}

inline uint32_t readLe32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

inline uint64_t round(uint64_t accumulator, uint64_t input) {
    return rotateLeft(accumulator + input * kPrime2, 31) * kPrime1;
}

inline uint64_t mergeRound(uint64_t accumulator, uint64_t value) {
    return (accumulator ^ round(0, value)) * kPrime1 + kPrime4;
}

const uint8_t kTileW[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
const uint8_t kTileH[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
const uint8_t kTileBytes[16] = {32, 32, 32, 32, 32, 32, 64, 0, 32, 32, 32, 0, 0, 0, 32, 0};

} // namespace

uint64_t xxh64(const void* data, size_t len, uint64_t seed) {
    static const uint8_t empty = 0;
    const uint8_t* p = data ? static_cast<const uint8_t*>(data) : &empty;
    const uint8_t* end = p + len;
    uint64_t hash;
    if (len >= 32) {
        uint64_t v1 = seed + kPrime1 + kPrime2;
        uint64_t v2 = seed + kPrime2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - kPrime1;
        do {
            v1 = round(v1, readLe64(p));
            v2 = round(v2, readLe64(p + 8));
            v3 = round(v3, readLe64(p + 16));
            v4 = round(v4, readLe64(p + 24));
            p += 32;
        } while (end - p >= 32);
        hash = rotateLeft(v1, 1) + rotateLeft(v2, 7) + rotateLeft(v3, 12) + rotateLeft(v4, 18);
        hash = mergeRound(hash, v1);
        hash = mergeRound(hash, v2);
        hash = mergeRound(hash, v3);
        hash = mergeRound(hash, v4);
    } else {
        hash = seed + kPrime5;
    }
    hash += uint64_t(len);
    while (end - p >= 8) {
        hash ^= round(0, readLe64(p));
        hash = rotateLeft(hash, 27) * kPrime1 + kPrime4;
        p += 8;
    }
    if (end - p >= 4) {
        hash ^= uint64_t(readLe32(p)) * kPrime1;
        hash = rotateLeft(hash, 23) * kPrime2 + kPrime3;
        p += 4;
    }
    while (p < end) {
        hash ^= uint64_t(*p++) * kPrime5;
        hash = rotateLeft(hash, 11) * kPrime1;
    }
    hash ^= hash >> 33;
    hash *= kPrime2;
    hash ^= hash >> 29;
    hash *= kPrime3;
    hash ^= hash >> 32;
    return hash;
}

uint32_t texLevelBytes(uint32_t fmt, uint32_t w, uint32_t h) {
    fmt &= 15;
    if (!kTileW[fmt]) return 0;
    const uint32_t cols = (w + kTileW[fmt] - 1) / kTileW[fmt];
    const uint32_t rows = (h + kTileH[fmt] - 1) / kTileH[fmt];
    return cols * rows * kTileBytes[fmt];
}

void paletteRange(const uint8_t* data, size_t bytes, uint32_t fmt, uint32_t* lo, uint32_t* hi) {
    uint32_t minimum = 0xFFFF;
    uint32_t maximum = 0;
    if (fmt == 8) {
        for (size_t i = 0; i < bytes; ++i) {
            const uint32_t high = data[i] >> 4;
            const uint32_t low = data[i] & 15;
            minimum = std::min(minimum, std::min(high, low));
            maximum = std::max(maximum, std::max(high, low));
        }
    } else if (fmt == 9) {
        for (size_t i = 0; i < bytes; ++i) {
            minimum = std::min<uint32_t>(minimum, data[i]);
            maximum = std::max<uint32_t>(maximum, data[i]);
        }
    } else {
        for (size_t i = 0; i + 1 < bytes; i += 2) {
            const uint32_t value = (uint32_t(data[i]) << 8 | data[i + 1]) & 0x3FFF;
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
    }
    if (minimum > maximum) minimum = maximum = 0;
    *lo = minimum;
    *hi = maximum;
}

std::string hiresName(const uint8_t* data, uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped,
                      const uint8_t* tlut, uint32_t tlutBytes) {
    const size_t bytes = texLevelBytes(fmt, w, h);
    char base[96];
    snprintf(base, sizeof base, "tex1_%ux%u%s_%016llx", w, h, mipmapped ? "_m" : "",
             (unsigned long long)xxh64(data, bytes, 0));

    char format[8];
    snprintf(format, sizeof format, "_%u", fmt);
    if ((fmt == 8 || fmt == 9 || fmt == 10) && tlut) {
        uint32_t lo, hi;
        paletteRange(data, bytes, fmt, &lo, &hi);
        const uint32_t offset = 2 * lo;
        uint32_t length = 2 * (hi + 1 - lo);
        if (offset + length > tlutBytes) length = offset < tlutBytes ? tlutBytes - offset : 0;
        const uint8_t* palette = length ? tlut + offset : tlut;
        char paletteHash[24];
        snprintf(paletteHash, sizeof paletteHash, "_%016llx",
                 (unsigned long long)xxh64(palette, length, 0));
        return std::string(base) + paletteHash + format;
    }
    return std::string(base) + format;
}

} // namespace gx
