#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <random>
#include <vector>

namespace gx {
void gx2DecodeTexture(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h,
                      const uint8_t* tlut, uint32_t tlutFmt, uint8_t* out);
}

static const uint8_t kTileW[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
static const uint8_t kTileH[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
static const uint8_t kTileBytes[16] = {32, 32, 32, 32, 32, 32, 64, 0, 32, 32, 32, 0, 0, 0, 32, 0};

static uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

static void oldDecode(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h, uint8_t* out) {
    const uint32_t cols = (w + kTileW[fmt] - 1) / kTileW[fmt];
    const uint32_t rows = (h + kTileH[fmt] - 1) / kTileH[fmt];
    const uint8_t* p = src;
    for (uint32_t ty = 0; ty < rows; ++ty) {
        for (uint32_t tx = 0; tx < cols; ++tx) {
            const uint32_t x0 = tx * kTileW[fmt], y0 = ty * kTileH[fmt];
            auto put = [&](uint32_t x, uint32_t y, const uint8_t* c) {
                if (x < w && y < h) memcpy(out + (size_t(y) * w + x) * 4, c, 4);
            };
            uint8_t c[4];
            switch (fmt) {
            case 0:
                for (uint32_t i = 0; i < 64; ++i) {
                    const uint8_t b = p[i >> 1], v = (i & 1) ? b & 15 : b >> 4;
                    c[0] = c[1] = c[2] = c[3] = uint8_t(v * 17);
                    put(x0 + (i & 7), y0 + (i >> 3), c);
                }
                break;
            case 1:
                for (uint32_t i = 0; i < 32; ++i) {
                    c[0] = c[1] = c[2] = c[3] = p[i];
                    put(x0 + (i & 7), y0 + (i >> 3), c);
                }
                break;
            case 3:
                for (uint32_t i = 0; i < 16; ++i) {
                    const uint16_t v = be16(p + 2 * i);
                    c[0] = c[1] = c[2] = uint8_t(v & 0xff);
                    c[3] = uint8_t(v >> 8);
                    put(x0 + (i & 3), y0 + (i >> 2), c);
                }
                break;
            default:
                break;
            }
            p += kTileBytes[fmt];
        }
    }
}

static uint64_t checksum(const std::vector<uint8_t>& data) {
    uint64_t result = 0;
    for (uint8_t byte : data) result = result * 131 + byte;
    return result;
}

int main() {
    const uint32_t formats[] = {0, 1, 3};
    const uint32_t sizes[][2] = {{640, 448}, {320, 224}, {8, 4}, {13, 7}, {1, 1}};
    std::mt19937 random(0x534d5354u);
    unsigned cases = 0;
    for (uint32_t fmt : formats) {
        for (const auto& size : sizes) {
            const uint32_t w = size[0], h = size[1];
            const size_t bytes = size_t((w + kTileW[fmt] - 1) / kTileW[fmt]) *
                                 ((h + kTileH[fmt] - 1) / kTileH[fmt]) * kTileBytes[fmt];
            std::vector<uint8_t> src(bytes), expected(size_t(w) * h * 4), actual(expected.size());
            for (uint8_t& byte : src) byte = uint8_t(random());
            oldDecode(src.data(), fmt, w, h, expected.data());
            gx::gx2DecodeTexture(src.data(), fmt, w, h, nullptr, 0, actual.data());
            if (actual != expected) {
                fprintf(stderr, "FAIL fmt=%u size=%ux%u\n", fmt, w, h);
                return 1;
            }
            ++cases;
        }
    }

    const uint32_t w = 640, h = 448, fmt = 1;
    const size_t bytes = size_t(w / 8) * (h / 4) * kTileBytes[fmt];
    std::vector<uint8_t> src(bytes), oldOut(size_t(w) * h * 4), newOut(oldOut.size());
    for (uint8_t& byte : src) byte = uint8_t(random());
    constexpr unsigned iterations = 20;
    uint64_t sink = 0;
    const auto oldStart = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i) {
        oldDecode(src.data(), fmt, w, h, oldOut.data());
        sink += checksum(oldOut);
    }
    const auto oldEnd = std::chrono::steady_clock::now();
    const auto newStart = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i) {
        gx::gx2DecodeTexture(src.data(), fmt, w, h, nullptr, 0, newOut.data());
        sink += checksum(newOut);
    }
    const auto newEnd = std::chrono::steady_clock::now();
    if (oldOut != newOut || sink == 0) {
        fprintf(stderr, "FAIL benchmark output\n");
        return 1;
    }
    const double oldMs = std::chrono::duration<double, std::milli>(oldEnd - oldStart).count();
    const double newMs = std::chrono::duration<double, std::milli>(newEnd - newStart).count();
    printf("texture decode: PASS (%u cases)\n", cases);
    printf("texture decode timing: old=%.3f ms new=%.3f ms speedup=%.2fx\n", oldMs / iterations,
           newMs / iterations, oldMs / newMs);
    return 0;
}
