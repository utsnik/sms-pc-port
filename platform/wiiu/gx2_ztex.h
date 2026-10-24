#ifndef SMS_WIIU_GX2_ZTEX_H
#define SMS_WIIU_GX2_ZTEX_H

#include <stdint.h>

namespace gx {

// GX expands a Z8/Z16/Z24X8 texel into the low bits of a 24-bit integer,
// then adds the unsigned 24-bit bias.  GX2 samples the decoded texture as
// normalized RGBA8, so this is the host-side reference for the generated
// Latte ALU sequence in gx2_renderer.cpp.
inline float gx2ZTextureValue(float r, float g, float b, float a, uint32_t format, uint32_t bias) {
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
    return (z + float(bias & 0x00ffffffu)) * (1.0f / 16777216.0f);
}

} // namespace gx

#endif
