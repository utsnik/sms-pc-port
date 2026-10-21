#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace gx {

uint64_t xxh64(const void* data, size_t len, uint64_t seed);
uint32_t texLevelBytes(uint32_t fmt, uint32_t w, uint32_t h);

// Inclusive range of palette entries referenced by a C4, C8, or C14X2
// texture. The input is the raw, padded GX base-level payload.
void paletteRange(const uint8_t* data, size_t bytes, uint32_t fmt, uint32_t* lo, uint32_t* hi);

// Construct the exact Dolphin texture-pack key without consulting a pack
// index. Hashes always cover the raw GX payload bytes.
std::string hiresName(const uint8_t* data, uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped,
                      const uint8_t* tlut, uint32_t tlutBytes);

} // namespace gx
