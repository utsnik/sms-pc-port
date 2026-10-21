#ifndef SMS_GX2_TEXTURE_PACK_TOOL_H
#define SMS_GX2_TEXTURE_PACK_TOOL_H

#include <stdint.h>
#include <string>

namespace gx {

struct TexturePackConversionOptions {
    uint32_t maxSize;
    uint32_t bcCodec;

    TexturePackConversionOptions() : maxSize(512), bcCodec(7) {}
};

struct TexturePackConversionStats {
    uint32_t files;
    uint32_t entries;
    uint32_t levels;
    uint64_t rgbaBytes;
    uint64_t bcBytes;

    TexturePackConversionStats() : files(0), entries(0), levels(0), rgbaBytes(0), bcBytes(0) {}
};

bool convertTexturePack(const std::string& inputDirectory, const std::string& outputArchive,
                       const TexturePackConversionOptions& options,
                       TexturePackConversionStats* stats, std::string* error);

} // namespace gx

#endif
