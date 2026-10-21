/* Fixed, endian-stable archive format shared by the host pack tool and the
 * Wii U reader.  Stage 1 payloads are deliberately linear: the console can
 * retile them with its existing GPU copy path while the archive stays useful
 * to other readers and easy to validate off-console. */
#ifndef SMS_GX2_TEXTURE_PACK_FORMAT_H
#define SMS_GX2_TEXTURE_PACK_FORMAT_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace gx {

enum TexturePackCodec {
    TEXTURE_PACK_CODEC_NONE = 0,
    TEXTURE_PACK_CODEC_RGBA8_LINEAR = 1,
    TEXTURE_PACK_CODEC_BC1 = 2,
    TEXTURE_PACK_CODEC_BC2 = 3,
    TEXTURE_PACK_CODEC_BC3 = 4,
    TEXTURE_PACK_CODEC_BC7 = 7
};

struct TexturePackKey {
    uint32_t sourceWidth;
    uint32_t sourceHeight;
    uint32_t format;
    bool mipmapped;
    uint64_t dataHash;
    uint64_t tlutHash;
    bool wildcardTlut;

    TexturePackKey()
        : sourceWidth(0), sourceHeight(0), format(0), mipmapped(false),
          dataHash(0), tlutHash(0), wildcardTlut(false) {}
};

bool operator==(const TexturePackKey& a, const TexturePackKey& b);
bool operator<(const TexturePackKey& a, const TexturePackKey& b);

struct TexturePackLevel {
    uint32_t width;
    uint32_t height;
    uint64_t rgbaOffset;
    uint32_t rgbaSize;
    uint64_t bcOffset;
    uint32_t bcSize;
    /* Writer-only source bytes.  A reader leaves these empty and reads the
     * validated ranges through TexturePackReader::read(). */
    std::vector<uint8_t> rgbaPayload;
    std::vector<uint8_t> bcPayload;

    TexturePackLevel()
        : width(0), height(0), rgbaOffset(0), rgbaSize(0), bcOffset(0), bcSize(0) {}
};

struct TexturePackEntry {
    TexturePackKey key;
    uint32_t width;
    uint32_t height;
    uint32_t mipCount;
    uint32_t rgbaCodec;
    uint32_t bcCodec;
    uint32_t surfaceFormat;
    uint32_t surfacePitch;
    uint64_t surfaceImageSize;
    uint64_t surfaceMipmapOffset;
    uint64_t surfaceMipmapSize;
    std::vector<TexturePackLevel> levels;

    TexturePackEntry()
        : width(0), height(0), mipCount(0), rgbaCodec(TEXTURE_PACK_CODEC_RGBA8_LINEAR),
          bcCodec(TEXTURE_PACK_CODEC_BC7), surfaceFormat(0), surfacePitch(0),
          surfaceImageSize(0), surfaceMipmapOffset(0), surfaceMipmapSize(0) {}
};

struct TexturePackArchiveInfo {
    uint32_t version;
    uint32_t layoutVersion;
    uint32_t maxDimension;
    uint32_t indexSlots;
    uint64_t indexOffset;
    uint64_t entriesOffset;
    uint64_t payloadOffset;
    uint64_t archiveSize;

    TexturePackArchiveInfo()
        : version(0), layoutVersion(0), maxDimension(0), indexSlots(0),
          indexOffset(0), entriesOffset(0), payloadOffset(0), archiveSize(0) {}
};

class TexturePackWriter {
public:
    static bool write(const std::string& path, const std::vector<TexturePackEntry>& entries,
                      uint32_t maxDimension, std::string* error);
    static bool serialize(const std::vector<TexturePackEntry>& entries, uint32_t maxDimension,
                          std::vector<uint8_t>* bytes, std::string* error);
};

class TexturePackReader {
public:
    TexturePackReader();

    bool open(const std::string& path, std::string* error);
    bool openBytes(const std::vector<uint8_t>& bytes, std::string* error);
    void close();
    bool valid() const { return valid_; }
    const TexturePackArchiveInfo& info() const { return info_; }
    const std::vector<TexturePackEntry>& entries() const { return entries_; }

    /* Exact TLUT entries win.  When allowWildcard is true, a wildcard entry
     * is considered only after an exact key is absent. */
    const TexturePackEntry* find(const TexturePackKey& key, bool allowWildcard = true) const;
    bool read(uint64_t offset, uint32_t size, std::vector<uint8_t>* out, std::string* error) const;
    bool read(uint64_t offset, uint32_t size, void* out, std::string* error) const;

private:
    bool parse(const uint8_t* data, size_t size, std::string* error);
    std::vector<uint8_t> bytes_;
    std::vector<TexturePackEntry> entries_;
    std::vector<uint64_t> indexHashes_;
    std::vector<uint32_t> indexEntries_;
    TexturePackArchiveInfo info_;
    bool valid_;
};

} // namespace gx

#endif
