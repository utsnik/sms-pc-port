#pragma once

#include <stdint.h>

namespace gx {

void gx2DecodeTexture(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h,
                      const uint8_t* tlut, uint32_t tlutFmt, uint8_t* out);
bool gx2DecodeTextureDirect(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h,
                            const uint8_t* tlut, uint32_t tlutFmt, uint8_t* out,
                            uint32_t pitch);
void gx2DecodeTextureR8(const uint8_t* src, uint32_t w, uint32_t h,
                        uint8_t* out, uint32_t pitch);

// CMPR's four GameCube sub-blocks are reordered into GX2's row-major BC1
// block order, with the endpoint and two-bit index byte order converted for
// GX2.  The output is tightly packed; the renderer supplies surface pitches.
bool gx2NativeTextureFormat(uint32_t fmt);
uint32_t gx2NativeTextureBytes(uint32_t fmt, uint32_t w, uint32_t h);
void gx2ConvertTextureNative(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h,
                             uint8_t* out);

} // namespace gx
