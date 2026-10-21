#include "gx_internal.h"
#include "sms_gx/gxvert_pc.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gx {
extern void (*drawSyncCallback)(uint16_t);
}

static bool bpHasSideEffect(uint32_t reg, uint32_t data) {
    return reg == gx::BP_COPY_EXEC || reg == gx::BP_PE_TOKEN_INT || reg == gx::BP_PE_TOKEN || reg == gx::BP_DRAWDONE ||
           reg == gx::BP_LOADTLUT1 || reg == gx::BP_TEXINVALIDATE ||
           (reg >= gx::BP_TEV_REG && reg < gx::BP_TEV_REG + 8 && (data & 0x800000));
}

static void referencePureBP(gx::State& s, uint32_t reg, uint32_t data) {
    if (reg == gx::BP_MASK) {
        s.bpMask = data;
        return;
    }
    if (s.bpMask != 0xFFFFFF) {
        data = (s.bp[reg] & ~s.bpMask) | (data & s.bpMask);
        s.bpMask = 0xFFFFFF;
    }
    if (s.bp[reg] == data && !bpHasSideEffect(reg, data)) return;
    if (bpHasSideEffect(reg, data)) return;
    s.bp[reg] = data;
    if (reg == gx::BP_COPY_DST) s.copyDest = gx::physToPtr(data << 5);
    if ((reg >= gx::BP_TX_IMAGE3 && reg < gx::BP_TX_IMAGE3 + 4) ||
        (reg >= gx::BP_TX_IMAGE3 + 0x20 && reg < gx::BP_TX_IMAGE3 + 0x24)) {
        int map = (reg & 3) + ((reg & 0x20) ? 4 : 0);
        s.texImage[map] = static_cast<const uint8_t*>(gx::physToPtr(data << 5));
    }
}

static int s_drawSyncCalls;
static void countDrawSync(uint16_t) { ++s_drawSyncCalls; }

static void testBPFastPaths() {
    gx::resetState();
    gx::State expected = gx::g;
    const uint32_t writes[] = {
        (uint32_t(gx::BP_GENMODE) << 24) | 0x123456,
        (uint32_t(gx::BP_COPY_DST) << 24) | 0x001234,
        (uint32_t(gx::BP_TX_IMAGE3) << 24) | 0x002345,
        (uint32_t(gx::BP_TX_IMAGE3 + 0x20) << 24) | 0x003456,
        (uint32_t(gx::BP_GENMODE) << 24) | 0x123456,
        (uint32_t(gx::BP_COPY_DST) << 24) | 0x001234,
        (uint32_t(gx::BP_TX_IMAGE3) << 24) | 0x002345,
        (uint32_t(gx::BP_MASK) << 24) | 0x00FF00,
        (uint32_t(gx::BP_GENMODE) << 24) | 0x000056,
    };
    for (uint32_t value : writes) {
        gx::writeBP(value);
        referencePureBP(expected, value >> 24, value & 0xFFFFFF);
    }
    assert(memcmp(gx::g.bp, expected.bp, sizeof(gx::g.bp)) == 0);
    assert(gx::g.bpMask == expected.bpMask);
    assert(gx::g.copyDest == expected.copyDest);
    assert(memcmp(gx::g.texImage, expected.texImage, sizeof(gx::g.texImage)) == 0);

    gx::resetState();
    gx::drawSyncCallback = countDrawSync;
    s_drawSyncCalls = 0;
    gx::writeBP((gx::BP_PE_TOKEN_INT << 24) | 0x0042);
    gx::writeBP((gx::BP_PE_TOKEN_INT << 24) | 0x0042);
    assert(s_drawSyncCalls == 2);
    gx::drawSyncCallback = nullptr;
}

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
    testBPFastPaths();
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
    uint8_t pipe[32] = {};
    GXPC_PipeBuf = pipe;
    GXPC_PipeLen = 0;
    GXPC_PipeFast = sizeof(pipe);
    GXPosition3f32(1.0f, -2.5f, 3.25f);
    GXTexCoord2f32(-4.0f, 5.5f);
    GXColor4u8(6, 7, 8, 9);
    const uint8_t expectedPipe[] = {
        0x3f, 0x80, 0x00, 0x00, 0xc0, 0x20, 0x00, 0x00, 0x40, 0x50, 0x00, 0x00,
        0xc0, 0x80, 0x00, 0x00, 0x40, 0xb0, 0x00, 0x00, 6, 7, 8, 9,
    };
    assert(GXPC_PipeLen == sizeof(expectedPipe));
    assert(memcmp(pipe, expectedPipe, sizeof(expectedPipe)) == 0);
    GXPC_PipeBuf = nullptr;
    GXPC_PipeLen = GXPC_PipeFast = 0;
    std::puts("gx_optimization_test: BP, hashBytes, unpackVertex, and vertex-writer reference comparisons passed");
    return 0;
}
