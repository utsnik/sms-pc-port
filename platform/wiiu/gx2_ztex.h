#ifndef SMS_WIIU_GX2_ZTEX_H
#define SMS_WIIU_GX2_ZTEX_H

#include <stdint.h>

namespace gx {

// GX expands a Z8/Z16/Z24X8 texel into the low bits of a 24-bit integer.
// GX2 samples the decoded texture as normalized RGBA8, so these helpers are
// the host-side reference for the generated Latte ALU sequence in
// gx2_renderer.cpp.
inline uint32_t gx2ZTextureInteger(float r, float g, float b, float a, uint32_t format) {
    const float byte = 255.0f;
    float z;
    switch (format & 3) {
    case 0: // GX_TF_Z8: one byte in the low 8 bits
        z = r * byte;
        break;
    case 1: // GX_TF_Z16: IA8 decoder returns low byte in RGB, high in A
        z = (a * byte) * 256.0f + r * byte;
        break;
    default: // GX_TF_Z24X8: RGB is the 24-bit value, alpha is padding
        z = ((r * byte) * 256.0f + g * byte) * 256.0f + b * byte;
        break;
    }
    return uint32_t(z);
}

inline float gx2ZTextureValue(float r, float g, float b, float a, uint32_t format, uint32_t bias) {
    const uint32_t value = (gx2ZTextureInteger(r, g, b, a, format) + (bias & 0x00ffffffu)) & 0x00ffffffu;
    return float(value) * (1.0f / 16777215.0f);
}

inline float gx2ZTextureAddValue(float rasterZ, float r, float g, float b, float a,
                                 uint32_t format, uint32_t bias) {
    const uint32_t raster = uint32_t(rasterZ * 16777215.0f);
    const uint32_t value = (raster + gx2ZTextureInteger(r, g, b, a, format) +
                            (bias & 0x00ffffffu)) & 0x00ffffffu;
    return float(value) * (1.0f / 16777215.0f);
}

} // namespace gx

#endif
