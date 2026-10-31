#pragma once

#include <stdint.h>

namespace gx {

void gx2DecodeTexture(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h,
                      const uint8_t* tlut, uint32_t tlutFmt, uint8_t* out);

// Convert the native GameCube formats to tightly packed GX2 texel/block rows.
// The renderer supplies GX2's pitch and mip offsets when copying these rows
// into the surface allocation.
bool gx2NativeTextureFormat(uint32_t fmt);
uint32_t gx2NativeTextureBytes(uint32_t fmt, uint32_t w, uint32_t h);
void gx2ConvertTextureNative(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h,
                             uint8_t* out);

} // namespace gx
