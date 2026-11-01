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
    for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
    pal[2][3] = 255;
    for (int k = 0; k < 3; ++k) pal[3][k] = pal[2][k];
    pal[3][3] = 0;
    for (uint32_t y = 0; y < 4; ++y) {
        const uint8_t bits = block[4 + y];
        for (uint32_t x = 0; x < 4; ++x)
            std::memcpy(out + size_t(y) * pitch + x * 4, pal[(bits >> (2 * x)) & 3], 4);
    }
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
        p[0] = 0x00; p[1] = 0x1f; // c0 = blue, c0 <= c1: 3-colour + transparent mode
        p[2] = 0xf8; p[3] = 0x00; // c1 = red
        std::memcpy(p + 4, rows[block], 4);
    }

    std::array<uint8_t, 8 * 8 * 4> rgba{};
    std::array<uint8_t, 4 * 4 * 8> bc1{};
    gx::gx2DecodeTexture(cmpr.data(), 14, 8, 8, nullptr, 0, rgba.data());
    gx::gx2ConvertTextureNative(cmpr.data(), 14, 8, 8, bc1.data());

    std::array<uint8_t, 8 * 8 * 4> decoded{};
    for (uint32_t by = 0; by < 2; ++by) {
        for (uint32_t bx = 0; bx < 2; ++bx) {
            const uint8_t* block = bc1.data() + (by * 2 + bx) * 8;
            decodeBc1(block, decoded.data() + (by * 4 * 8 + bx * 4) * 4, 8 * 4);
        }
    }
    for (size_t i = 0; i < rgba.size(); ++i) {
        if (rgba[i] > decoded[i] + 1 || decoded[i] > rgba[i] + 1) {
            std::cerr << "gx2_texture_native_test: mismatch at byte " << i << "\n";
            return 1;
        }
    }

    // R8 and R8G8 must reproduce the fallback decoder's component mapping,
    // not merely its colour channels: I8's intensity is also alpha, while
    // IA8 stores intensity then alpha in the GameCube big-endian word.
    const std::array<uint8_t, 32> i8 = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x01,
    };
    std::array<uint8_t, 8 * 4 * 4> i8Rgba{};
    gx::gx2DecodeTexture(i8.data(), 1, 8, 4, nullptr, 0, i8Rgba.data());
    std::array<uint8_t, 8 * 4> i8Native{};
    gx::gx2ConvertTextureNative(i8.data(), 1, 8, 4, i8Native.data());
    for (size_t p = 0; p < i8Native.size(); ++p)
        for (int c = 0; c != 4; ++c)
            if (i8Native[p] != i8Rgba[p * 4 + c]) {
                std::cerr << "gx2_texture_native_test: I8 alpha/component mismatch\n";
                return 1;
            }

    const std::array<uint8_t, 32> ia8 = {
        0x00, 0x10, 0x20, 0x21, 0x40, 0x32, 0x60, 0x43,
        0x80, 0x54, 0xa0, 0x65, 0xc0, 0x76, 0xe0, 0x87,
        0xff, 0x98, 0x12, 0xa9, 0x34, 0xba, 0x56, 0xcb,
        0x78, 0xdc, 0x9a, 0xed, 0xbc, 0xfe, 0xde, 0x0f,
    };
    std::array<uint8_t, 4 * 4 * 4> ia8Rgba{};
    gx::gx2DecodeTexture(ia8.data(), 3, 4, 4, nullptr, 0, ia8Rgba.data());
    std::array<uint8_t, 4 * 4 * 2> ia8Native{};
    gx::gx2ConvertTextureNative(ia8.data(), 3, 4, 4, ia8Native.data());
    for (size_t p = 0; p < ia8Native.size() / 2; ++p) {
        const uint8_t* expected = ia8Rgba.data() + p * 4;
        if (ia8Native[p * 2] != expected[0] || ia8Native[p * 2 + 1] != expected[3] ||
            expected[0] != expected[1] || expected[0] != expected[2]) {
            std::cerr << "gx2_texture_native_test: IA8 alpha/component mismatch\n";
            return 1;
        }
    }
    std::cout << "gx2_texture_native_test: PASS\n";
    return 0;
}
