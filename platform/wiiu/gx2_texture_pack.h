#ifndef SMS_WIIU_GX2_TEXTURE_PACK_H
#define SMS_WIIU_GX2_TEXTURE_PACK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace gx {

static const uint32_t kTexturePackMaxLevels = 16;
static const uint32_t kTexturePackMaxQueue = 64;
static const uint32_t kTexturePackMaxResidents = 512;
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

inline bool texturePackKeyEqual(const TexturePackKey& a, const TexturePackKey& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format && a.mipmapped == b.mipmapped &&
           a.dataHash == b.dataHash && a.tlutHash == b.tlutHash && a.wildcardTlut == b.wildcardTlut;
}
inline bool texturePackKeySameSource(const TexturePackKey& a, const TexturePackKey& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format && a.mipmapped == b.mipmapped;
}
inline bool texturePackWildcardMatch(const TexturePackKey& stored, const TexturePackKey& query) {
    return stored.width == query.width && stored.height == query.height && stored.format == query.format &&
           stored.mipmapped == query.mipmapped && stored.dataHash == query.dataHash && stored.wildcardTlut;
}

/* Fixed storage queue used by the console worker and by host tests.  The
 * producer and consumer are single-threaded at their respective ends; the
 * acquire/release barriers in the implementation are deliberately 32-bit. */
struct TexturePackRequest {
    const uint8_t* source;
    const uint8_t* tlut;
    uint32_t tlutBytes;
    uint32_t width, height, format, levels;
    uint32_t tlutFormat, writeClock, generation;
    uint32_t requestId;
    uint32_t retry;
};

struct TexturePackResult {
    TexturePackRequest request;
    TexturePackKey key;
    const TexturePackEntry* entry;
    uint32_t retry;
    bool found;
    bool changed;
};

struct TexturePackUploadChunk {
    uint32_t requestId, level, offset, bytes;
    const uint8_t* data;
    const TexturePackEntry* entry;
    bool last;
    bool ok;
};

struct TexturePackStats {
    uint32_t hashBytes, hashMicros, archiveReadBytes, queuedJobs;
};

class TexturePackRequestQueue {
public:
    TexturePackRequestQueue() : read_(0), write_(0) {}
    bool contains(const TexturePackRequest& r) const {
        const uint32_t read = read_, write = write_;
        for (uint32_t i = read; i != write; ++i) {
            const TexturePackRequest& q = slots_[i % kTexturePackMaxQueue];
            if (q.requestId == r.requestId && q.source == r.source && q.tlut == r.tlut && q.tlutBytes == r.tlutBytes && q.width == r.width &&
                q.height == r.height && q.format == r.format && q.levels == r.levels && q.tlutFormat == r.tlutFormat)
                return true;
        }
        return false;
    }
    bool push(const TexturePackRequest& r) {
        if (contains(r)) return true;
        const uint32_t write = write_, read = read_;
        if (write - read >= kTexturePackMaxQueue) return false;
        slots_[write % kTexturePackMaxQueue] = r;
        __sync_synchronize();
        write_ = write + 1;
        return true;
    }
    bool pop(TexturePackRequest* r) {
        if (!r) return false;
        const uint32_t read = read_;
        if (read == write_) return false;
        __sync_synchronize();
        *r = slots_[read % kTexturePackMaxQueue];
        read_ = read + 1;
        return true;
    }
    uint32_t size() const { return write_ - read_; }
    void clear() { read_ = write_; }

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
    TexturePackLru() : budget_(0), bytes_(0) { reset(0); }
    void reset(uint64_t budget) { budget_ = budget; bytes_ = 0; memset(nodes_, 0, sizeof nodes_); }
    bool add(uint32_t id, uint64_t bytes, uint32_t frame) {
        if (bytes > budget_) return false;
        for (TexturePackLruNode& n : nodes_)
            if (n.resident && n.id == id) {
                if (bytes_ - n.bytes + bytes > budget_) return false;
                bytes_ -= n.bytes; n.bytes = bytes; n.lastUsedFrame = frame; bytes_ += bytes; return true;
            }
        for (TexturePackLruNode& n : nodes_) if (!n.resident) {
            n.id = id; n.bytes = bytes; n.lastUsedFrame = frame; n.resident = true; bytes_ += bytes; return bytes_ <= budget_;
        }
        return false;
    }
    // O(1) when the caller keeps the slot returned last time (hint); a linear
    // scan per bind cost ~1 ms per frame in Delfino Plaza with the pack on.
    uint32_t touchHint(uint32_t id, uint32_t frame, uint32_t hint) {
        if (hint < kTexturePackMaxResidents && nodes_[hint].resident && nodes_[hint].id == id) {
            nodes_[hint].lastUsedFrame = frame;
            return hint;
        }
        for (uint32_t i = 0; i != kTexturePackMaxResidents; ++i)
            if (nodes_[i].resident && nodes_[i].id == id) { nodes_[i].lastUsedFrame = frame; return i; }
        return UINT32_MAX;
    }
    void touch(uint32_t id, uint32_t frame) {
        for (TexturePackLruNode& n : nodes_) if (n.resident && n.id == id) { n.lastUsedFrame = frame; return; }
    }
    void remove(uint32_t id) {
        for (TexturePackLruNode& n : nodes_) {
            if (!n.resident || n.id != id) continue;
            bytes_ -= n.bytes;
            n = TexturePackLruNode();
            return;
        }
    }
    uint64_t residentBytes() const { return bytes_; }
    uint64_t budget() const { return budget_; }
    uint32_t evict(uint32_t frame, uint64_t needed, uint32_t* evicted, uint32_t capacity) {
        uint32_t count = 0;
        while (bytes_ + needed > budget_) {
            int victim = -1;
            for (unsigned i = 0; i != kTexturePackMaxResidents; ++i) {
                const TexturePackLruNode& n = nodes_[i];
                if (!n.resident || n.lastUsedFrame + 1 >= frame) continue;
                if (victim < 0 || n.lastUsedFrame < nodes_[victim].lastUsedFrame) victim = int(i);
            }
            if (victim < 0) break;
            TexturePackLruNode& n = nodes_[victim];
            if (evicted && count < capacity) evicted[count] = n.id;
            ++count; bytes_ -= n.bytes; n = TexturePackLruNode();
        }
        return count;
    }

private:
    TexturePackLruNode nodes_[kTexturePackMaxResidents];
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
bool texturePackEnqueue(const TexturePackRequest& request);
bool texturePackPopResult(TexturePackResult* result);
bool texturePackPopUploadChunk(TexturePackUploadChunk* chunk);
void texturePackReleaseUploadChunk();
void texturePackGetStats(TexturePackStats* stats);
void texturePackNoteFrame(uint32_t frame);
uint32_t texturePackMaxDimension();
uint32_t texturePackEntryCount();
uint64_t texturePackReadBytes();

} // namespace gx

#endif
