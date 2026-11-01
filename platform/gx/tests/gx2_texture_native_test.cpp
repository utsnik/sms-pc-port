#include "../../wiiu/gx2_texture.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

void rgb565(uint16_t v, uint8_t* out) {
    out[0] = uint8_t((v >> 11) << 3 | (v >> 13));
    out[1] = uint8_t(((v >> 5) & 63) << 2 | ((v >> 9) & 3));
    out[2] = uint8_t((v & 31) << 3 | ((v >> 2) & 7));
    out[3] = 255;
}

void decodeBc1(const uint8_t* block, uint8_t* out, uint32_t pitch) {
    const uint16_t c0 = uint16_t(block[0] | block[1] << 8);
    const uint16_t c1 = uint16_t(block[2] | block[3] << 8);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    if (c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) pal[2][k] = pal[3][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255; pal[3][3] = 0;
    }
    for (uint32_t y = 0; y < 4; ++y) {
        const uint8_t bits = block[4 + y];
        for (uint32_t x = 0; x < 4; ++x)
            std::memcpy(out + size_t(y) * pitch + x * 4, pal[(bits >> (2 * x)) & 3], 4);
    }
}

bool sameCmprPixel(const uint8_t* rgba, const uint8_t* bc1) {
    if (rgba[3] != bc1[3]) return false;
    // GX2 is allowed to return a different RGB value for a transparent BC1
    // texel; its alpha and every non-transparent channel remain exact.
    return rgba[3] == 0 || std::memcmp(rgba, bc1, 3) == 0;
}

} // namespace

int main() {
    std::array<uint8_t, 32> cmpr{};
    const uint8_t rows[4][4] = {
        {0x1b, 0xe4, 0x00, 0xff}, {0x55, 0xaa, 0x33, 0xcc},
        {0x7e, 0x81, 0x18, 0xe7}, {0xc6, 0x39, 0xf0, 0x0f},
    };
    for (uint32_t block = 0; block < 4; ++block) {
        uint8_t* p = cmpr.data() + block * 8;
        if (block & 1) {
            p[0] = 0x00; p[1] = 0x1f; // blue <= red: 3-colour + transparent
            p[2] = 0xf8; p[3] = 0x00;
        } else {
            p[0] = 0xf8; p[1] = 0x00; // red > blue: four-colour mode
            p[2] = 0x00; p[3] = 0x1f;
        }
        std::memcpy(p + 4, rows[block], 4);
    }

    std::array<uint8_t, 8 * 8 * 4> rgba{};
    std::array<uint8_t, 4 * 8> bc1{};
    gx::gx2DecodeTexture(cmpr.data(), 14, 8, 8, nullptr, 0, rgba.data());
    gx::gx2ConvertTextureNative(cmpr.data(), 14, 8, 8, bc1.data());
    if (!gx::gx2NativeTextureFormat(14) || gx::gx2NativeTextureBytes(14, 8, 8) != bc1.size()) {
        std::cerr << "gx2_texture_native_test: CMPR size/format failure\n";
        return 1;
    }

    std::array<uint8_t, 8 * 8 * 4> decoded{};
    for (uint32_t by = 0; by < 2; ++by)
        for (uint32_t bx = 0; bx < 2; ++bx)
            decodeBc1(bc1.data() + (by * 2 + bx) * 8,
                     decoded.data() + (by * 4 * 8 + bx * 4) * 4, 8 * 4);
    for (size_t p = 0; p < rgba.size(); p += 4) {
        if (!sameCmprPixel(rgba.data() + p, decoded.data() + p)) {
            std::cerr << "gx2_texture_native_test: decoded CMPR/BC1 mismatch at byte " << p << "\n";
            return 1;
        }
    }

    // Non-multiple dimensions still consume whole source tiles and whole BC1
    // blocks, but only the logical block rows/columns are emitted.
    std::array<uint8_t, 32> partial{};
    std::array<uint8_t, 2 * 2 * 8> partialBc1{};
    gx::gx2ConvertTextureNative(partial.data(), 14, 5, 7, partialBc1.data());
    if (gx::gx2NativeTextureBytes(14, 5, 7) != partialBc1.size()) {
        std::cerr << "gx2_texture_native_test: partial CMPR size failure\n";
        return 1;
    }
    std::cout << "gx2_texture_native_test: PASS\n";
    return 0;
}
