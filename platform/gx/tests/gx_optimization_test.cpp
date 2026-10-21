#include "gx_internal.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static uint64_t referenceHash(const void* data, size_t n, uint64_t seed) {
#if defined(__WIIU__)
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t a = uint32_t(seed) ^ uint32_t(n) * 0x9E3779B1u;
    uint32_t b = uint32_t(seed >> 32) ^ 0x85EBCA77u;
    uint32_t c = a ^ 0xC2B2AE3Du, d = b ^ 0x27D4EB2Fu;
    for (; n >= 16; n -= 16, p += 16) {
        uint32_t w[4];
        memcpy(w, p, 16);
        a = (a ^ w[0]) * 0x9E3779B1u; a ^= a >> 15;
        b = (b ^ w[1]) * 0x85EBCA77u; b ^= b >> 13;
        c = (c ^ w[2]) * 0xC2B2AE3Du; c ^= c >> 16;
        d = (d ^ w[3]) * 0x27D4EB2Fu; d ^= d >> 15;
    }
    for (; n; --n, ++p) { a = (a ^ *p) * 0x9E3779B1u; a ^= a >> 15; }
    uint32_t lo = (a ^ (c >> 7)) * 0x85EBCA77u, hi = (b ^ (d >> 9)) * 0xC2B2AE3Du;
    lo ^= hi >> 13; hi ^= lo >> 16;
    return uint64_t(hi) << 32 | lo;
#else
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed ^ (0x9E3779B97F4A7C15ull * (n + 1));
    if (n >= 32) {
        uint64_t a = h, b = h ^ 0x2545F4914F6CDD1Dull, c = h ^ 0x632BE59BD9B4E019ull;
        uint64_t d = h ^ 0x85EBCA77C2B2AE63ull;
        do {
            uint64_t w[4];
            memcpy(w, p, 32);
            a = (a ^ w[0] * 0xBF58476D1CE4E5B9ull);
            b = (b ^ w[1] * 0xBF58476D1CE4E5B9ull);
            c = (c ^ w[2] * 0xBF58476D1CE4E5B9ull);
            d = (d ^ w[3] * 0xBF58476D1CE4E5B9ull);
            a = (a << 27 | a >> 37) * 0x94D049BB133111EBull;
            b = (b << 27 | b >> 37) * 0x94D049BB133111EBull;
            c = (c << 27 | c >> 37) * 0x94D049BB133111EBull;
            d = (d << 27 | d >> 37) * 0x94D049BB133111EBull;
            p += 32;
            n -= 32;
        } while (n >= 32);
        h = a ^ (b << 17 | b >> 47) ^ (c << 31 | c >> 33) ^ (d << 45 | d >> 19);
        h *= 0x94D049BB133111EBull;
    }
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h ^= w * 0xBF58476D1CE4E5B9ull;
        h = (h << 27 | h >> 37) * 0x94D049BB133111EBull;
        p += 8;
        n -= 8;
    }
    uint64_t w = 0;
    memcpy(&w, p, n);
    h ^= w * 0xBF58476D1CE4E5B9ull + n;
    h ^= h >> 31;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return h;
#endif
}

int main() {
    std::vector<uint8_t> bytes(512 + 3);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 37 + 11);
    for (size_t offset = 0; offset < 4; ++offset) {
        for (size_t n = 0; n <= 257; ++n) {
            for (uint64_t seed : {uint64_t(0), uint64_t(0x0123456789ABCDEFull), uint64_t(-1)}) {
                const void* p = bytes.data() + offset;
                assert(gx::hashBytes(p, n, seed) == referenceHash(p, n, seed));
            }
        }
    }
    std::puts("gx_optimization_test: hashBytes reference comparisons passed");
    return 0;
}
