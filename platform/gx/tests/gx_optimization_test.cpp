#include "gx_internal.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static void referenceUnpack(uint32_t fmt, const uint8_t* src, const uint8_t defMtx[9], gx::HostVertex& hv,
                            bool zeroed) {
    const gx::VtxFmtLayout& l = gx::vtxFmtLayout(fmt);
    if (!zeroed) memset(&hv, 0, sizeof(hv));
    hv.nrm[2] = 1.0f;
    memset(hv.clr, 255, sizeof(hv.clr));
    memcpy(hv.mtx, defMtx, 9);
    memcpy(hv.pos, src, 12);
    if (fmt & gx::VF_NRM) memcpy(hv.nrm, src + l.nrm, 12);
    if (fmt & gx::VF_NBT) {
        memcpy(hv.bin, src + l.nrm + 12, 12);
        memcpy(hv.tan, src + l.nrm + 24, 12);
    }
    for (int c = 0; c < 2; c++)
        if (fmt & (gx::VF_CLR0 << c)) memcpy(hv.clr[c], src + l.clr[c], 4);
    for (int t = 0; t < 8; t++)
        if (fmt & (gx::VF_TEX0 << t)) memcpy(hv.tex[t], src + l.tex[t], 8);
    if (fmt & gx::VF_MTX) memcpy(hv.mtx, src + l.mtx, 12);
}

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
    const uint32_t formats[] = {
        0,
        gx::VF_NRM,
        gx::VF_NRM | gx::VF_NBT | gx::VF_CLR0 | (gx::VF_CLR0 << 1) | gx::VF_TEX0 | (gx::VF_TEX0 << 3) | gx::VF_MTX,
        (gx::VF_CLR0 << 1) | (gx::VF_TEX0 << 7),
    };
    const uint8_t defaults[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (uint32_t fmt : formats) {
        const gx::VtxFmtLayout& layout = gx::vtxFmtLayout(fmt);
        std::vector<uint8_t> packed(layout.stride + 3);
        for (size_t i = 0; i < packed.size(); ++i) packed[i] = uint8_t(i * 19 + 7);
        for (bool zeroed : {false, true}) {
            gx::HostVertex got, expected;
            memset(&got, 0xA5, sizeof(got));
            memset(&expected, 0xA5, sizeof(expected));
            gx::unpackVertex(layout, fmt, packed.data(), defaults, got, zeroed);
            referenceUnpack(fmt, packed.data(), defaults, expected, zeroed);
            assert(memcmp(&got, &expected, sizeof(got)) == 0);
        }
    }
    std::puts("gx_optimization_test: hashBytes and unpackVertex reference comparisons passed");
    return 0;
}
