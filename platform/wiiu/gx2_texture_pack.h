#ifndef SMS_WIIU_GX2_TEXTURE_PACK_H
#define SMS_WIIU_GX2_TEXTURE_PACK_H

#include <stdint.h>
#include <stddef.h>

namespace gx {

static const uint32_t kTexturePackMaxLevels = 16;
static const uint32_t kTexturePackMaxQueue = 64;
static const uint32_t kTexturePackMaxChunk = 256u << 10;

struct TexturePackKey {
    uint32_t width, height, format;
    uint32_t mipmapped;
    uint64_t dataHash, tlutHash;
    uint32_t wildcardTlut;
};

struct TexturePackLevel {
    uint32_t width, height;
    uint64_t rgbaOffset;
    uint32_t rgbaSize;
    uint64_t bcOffset;
    uint32_t bcSize;
};

struct TexturePackEntry {
    TexturePackKey key;
    uint32_t width, height, mipCount;
    uint32_t rgbaCodec, bcCodec, surfaceFormat, surfacePitch;
    uint64_t surfaceImageSize, surfaceMipmapOffset, surfaceMipmapSize;
    TexturePackLevel levels[kTexturePackMaxLevels];
};

bool texturePackKeyEqual(const TexturePackKey& a, const TexturePackKey& b);
bool texturePackKeySameSource(const TexturePackKey& a, const TexturePackKey& b);
bool texturePackWildcardMatch(const TexturePackKey& stored, const TexturePackKey& query);

/* Fixed storage queue used by the console worker and by host tests.  The
 * producer and consumer are single-threaded at their respective ends; the
 * acquire/release barriers in the implementation are deliberately 32-bit. */
struct TexturePackRequest {
    const uint8_t* source;
    const uint8_t* tlut;
    uint32_t width, height, format, levels;
    uint32_t tlutFormat, writeClock, generation;
    uint32_t requestId;
};

class TexturePackRequestQueue {
public:
    TexturePackRequestQueue();
    bool push(const TexturePackRequest& request);
    bool pop(TexturePackRequest* request);
    bool contains(const TexturePackRequest& request) const;
    uint32_t size() const;
    void clear();

private:
    TexturePackRequest slots_[kTexturePackMaxQueue];
    volatile uint32_t read_, write_;
};

struct TexturePackLruNode {
    uint32_t id;
    uint64_t bytes;
    uint32_t lastUsedFrame;
    bool resident;
};

/* Host-testable eviction policy.  The renderer supplies the actual free
 * callback; only nodes unused in the immediately preceding frame are eligible
 * for eviction. */
class TexturePackLru {
public:
    TexturePackLru();
    void reset(uint64_t budget);
    bool add(uint32_t id, uint64_t bytes, uint32_t frame);
    void touch(uint32_t id, uint32_t frame);
    uint64_t residentBytes() const { return bytes_; }
    uint64_t budget() const { return budget_; }
    uint32_t evict(uint32_t frame, uint64_t needed, uint32_t* evicted, uint32_t capacity);

private:
    TexturePackLruNode nodes_[kTexturePackMaxQueue];
    uint64_t budget_, bytes_;
};

struct TexturePackUploadBudget {
    uint32_t byteLimit;
    uint32_t bytes;
    uint32_t jobs;
    TexturePackUploadBudget(uint32_t limit = kTexturePackMaxChunk)
        : byteLimit(limit), bytes(0), jobs(0) {}
    bool consume(uint32_t n) {
        if (n > byteLimit - bytes) return false;
        bytes += n;
        ++jobs;
        return true;
    }
};

/* The archive is deliberately inert until this succeeds. */
bool texturePackInit();
void texturePackShutdown();
bool texturePackEnabled();
const TexturePackEntry* texturePackLookup(const TexturePackKey& key);
bool texturePackReadChunk(uint64_t offset, uint32_t bytes, void* out);
uint32_t texturePackMaxDimension();
uint32_t texturePackEntryCount();
uint64_t texturePackReadBytes();

} // namespace gx

#endif
