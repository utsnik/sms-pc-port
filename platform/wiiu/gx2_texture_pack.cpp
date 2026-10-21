#include "gx2_texture_pack.h"

#include "runtime.h"

#include <coreinit/memdefaultheap.h>
#include <whb/log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gx {

namespace {

static const uint32_t kHeaderBytes = 72;
static const uint32_t kEntryBytes = 640;
static const uint32_t kMaxEntries = 8192;
static const uint32_t kMaxIndexSlots = 16384;
static const uint32_t kRgbaCodec = 1;

struct State {
    FILE* file;
    uint64_t fileBytes;
    uint32_t maxDimension;
    uint32_t entryCount;
    uint32_t slotCount;
    uint64_t indexOffset;
    uint64_t entriesOffset;
    uint64_t payloadOffset;
    uint64_t archiveSize;
    uint64_t* hashes;
    uint32_t* indices;
    TexturePackEntry* entries;
    uint64_t readBytes;
    bool enabled;
};

State s_state = {};

static uint16_t get16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
static uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static uint64_t get64(const uint8_t* p) {
    uint64_t v = 0;
    for (unsigned i = 0; i != 8; ++i) v = (v << 8) | p[i];
    return v;
}
static bool addOk(uint64_t a, uint64_t b, uint64_t* out) {
    if (b > UINT64_MAX - a) return false;
    *out = a + b;
    return true;
}
static bool rangeOk(uint64_t off, uint64_t bytes, uint64_t total) {
    uint64_t end = 0;
    return addOk(off, bytes, &end) && off <= total && end <= total;
}
static bool powerOfTwo(uint32_t v) { return v && !(v & (v - 1)); }
static bool validFormat(uint32_t fmt) {
    return fmt == 0 || fmt == 1 || fmt == 2 || fmt == 3 || fmt == 4 || fmt == 5 ||
           fmt == 6 || fmt == 8 || fmt == 9 || fmt == 10 || fmt == 14;
}
static bool validCodec(uint32_t codec) { return codec == kRgbaCodec || codec == 2 || codec == 3 || codec == 4 || codec == 7; }

static uint64_t keyHash(const TexturePackKey& k) {
    uint8_t b[40];
    b[0] = uint8_t(k.width >> 24); b[1] = uint8_t(k.width >> 16); b[2] = uint8_t(k.width >> 8); b[3] = uint8_t(k.width);
    b[4] = uint8_t(k.height >> 24); b[5] = uint8_t(k.height >> 16); b[6] = uint8_t(k.height >> 8); b[7] = uint8_t(k.height);
    b[8] = uint8_t(k.format >> 24); b[9] = uint8_t(k.format >> 16); b[10] = uint8_t(k.format >> 8); b[11] = uint8_t(k.format);
    const uint32_t flags = (k.mipmapped ? 1u : 0u);
    b[12] = uint8_t(flags >> 24); b[13] = uint8_t(flags >> 16); b[14] = uint8_t(flags >> 8); b[15] = uint8_t(flags);
    for (unsigned i = 0; i != 8; ++i) b[16 + i] = uint8_t(k.dataHash >> (56 - i * 8));
    const uint64_t tlut = k.wildcardTlut ? 0 : k.tlutHash;
    for (unsigned i = 0; i != 8; ++i) b[24 + i] = uint8_t(tlut >> (56 - i * 8));
    const uint32_t wildcard = k.wildcardTlut ? 1u : 0u;
    b[32] = uint8_t(wildcard >> 24); b[33] = uint8_t(wildcard >> 16); b[34] = uint8_t(wildcard >> 8); b[35] = uint8_t(wildcard);
    std::memset(b + 36, 0, 4);
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i != sizeof b; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static bool readAt(uint64_t offset, void* out, uint32_t bytes) {
    if (!s_state.file || !rangeOk(offset, bytes, s_state.archiveSize)) return false;
    if (std::fseek(s_state.file, long(offset), SEEK_SET) != 0) return false;
    return bytes == 0 || std::fread(out, 1, bytes, s_state.file) == bytes;
}

static void freeIndex() {
    if (s_state.hashes) MEMFreeToDefaultHeap(s_state.hashes);
    if (s_state.indices) MEMFreeToDefaultHeap(s_state.indices);
    if (s_state.entries) MEMFreeToDefaultHeap(s_state.entries);
    s_state.hashes = nullptr;
    s_state.indices = nullptr;
    s_state.entries = nullptr;
}

static void reject(const char* why) {
    WHBLogPrintf("SMS Wii U: [texpack] rejected archive: %s", why);
    if (s_state.file) std::fclose(s_state.file);
    s_state.file = nullptr;
    freeIndex();
    s_state.enabled = false;
}

static bool parseEntry(const uint8_t* p, TexturePackEntry* e) {
    std::memset(e, 0, sizeof *e);
    e->key.width = get32(p + 0); e->key.height = get32(p + 4); e->key.format = get32(p + 8);
    const uint32_t flags = get32(p + 12);
    e->key.mipmapped = flags & 1; e->key.wildcardTlut = (flags >> 1) & 1;
    e->key.dataHash = get64(p + 16); e->key.tlutHash = get64(p + 24);
    e->width = get32(p + 32); e->height = get32(p + 36); e->mipCount = get32(p + 40);
    e->rgbaCodec = get32(p + 44); e->bcCodec = get32(p + 48); e->surfaceFormat = get32(p + 52);
    e->surfacePitch = get32(p + 56); e->surfaceImageSize = get64(p + 60);
    e->surfaceMipmapOffset = get64(p + 68); e->surfaceMipmapSize = get64(p + 76);
    if (!e->mipCount || e->mipCount > kTexturePackMaxLevels) return false;
    for (uint32_t i = 0; i != e->mipCount; ++i) {
        const uint8_t* q = p + 84 + i * 32;
        TexturePackLevel& l = e->levels[i];
        l.width = get32(q + 0); l.height = get32(q + 4); l.rgbaOffset = get64(q + 8);
        l.rgbaSize = get32(q + 16); l.bcOffset = get64(q + 20); l.bcSize = get32(q + 28);
    }
    return true;
}

} // namespace

bool texturePackKeyEqual(const TexturePackKey& a, const TexturePackKey& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format &&
           a.mipmapped == b.mipmapped && a.dataHash == b.dataHash && a.tlutHash == b.tlutHash &&
           a.wildcardTlut == b.wildcardTlut;
}

bool texturePackKeySameSource(const TexturePackKey& a, const TexturePackKey& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format &&
           a.mipmapped == b.mipmapped;
}

bool texturePackWildcardMatch(const TexturePackKey& stored, const TexturePackKey& query) {
    return stored.width == query.width && stored.height == query.height && stored.format == query.format &&
           stored.mipmapped == query.mipmapped && stored.dataHash == query.dataHash && stored.wildcardTlut;
}

TexturePackRequestQueue::TexturePackRequestQueue() : read_(0), write_(0) {}

bool TexturePackRequestQueue::contains(const TexturePackRequest& r) const {
    const uint32_t read = read_, write = write_;
    for (uint32_t i = read; i != write; ++i) {
        const TexturePackRequest& q = slots_[i % kTexturePackMaxQueue];
        if (q.source == r.source && q.tlut == r.tlut && q.width == r.width && q.height == r.height &&
            q.format == r.format && q.levels == r.levels && q.tlutFormat == r.tlutFormat)
            return true;
    }
    return false;
}

bool TexturePackRequestQueue::push(const TexturePackRequest& r) {
    if (contains(r)) return true;
    const uint32_t write = write_, read = read_;
    if (write - read >= kTexturePackMaxQueue) return false;
    slots_[write % kTexturePackMaxQueue] = r;
    __sync_synchronize();
    write_ = write + 1;
    return true;
}

bool TexturePackRequestQueue::pop(TexturePackRequest* r) {
    if (!r) return false;
    const uint32_t read = read_;
    if (read == write_) return false;
    __sync_synchronize();
    *r = slots_[read % kTexturePackMaxQueue];
    read_ = read + 1;
    return true;
}

uint32_t TexturePackRequestQueue::size() const { return write_ - read_; }
void TexturePackRequestQueue::clear() { read_ = write_; }

TexturePackLru::TexturePackLru() : budget_(0), bytes_(0) { reset(0); }
void TexturePackLru::reset(uint64_t budget) {
    budget_ = budget; bytes_ = 0;
    std::memset(nodes_, 0, sizeof nodes_);
}
bool TexturePackLru::add(uint32_t id, uint64_t bytes, uint32_t frame) {
    for (TexturePackLruNode& n : nodes_)
        if (n.resident && n.id == id) { bytes_ -= n.bytes; n.bytes = bytes; n.lastUsedFrame = frame; bytes_ += bytes; return bytes_ <= budget_; }
    for (TexturePackLruNode& n : nodes_) if (!n.resident) {
        n.id = id; n.bytes = bytes; n.lastUsedFrame = frame; n.resident = true; bytes_ += bytes; return bytes_ <= budget_;
    }
    return false;
}
void TexturePackLru::touch(uint32_t id, uint32_t frame) {
    for (TexturePackLruNode& n : nodes_) if (n.resident && n.id == id) { n.lastUsedFrame = frame; return; }
}
uint32_t TexturePackLru::evict(uint32_t frame, uint64_t needed, uint32_t* evicted, uint32_t capacity) {
    uint32_t count = 0;
    while (bytes_ + needed > budget_) {
        int victim = -1;
        for (unsigned i = 0; i != kTexturePackMaxQueue; ++i) {
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

bool texturePackInit() {
    if (s_state.enabled) return true;
    char path[256];
    std::snprintf(path, sizeof path, "%stextures/GMSE01.gx2pack", sms_wiiu_app_root());
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long end = std::ftell(f);
    if (end < 0) { std::fclose(f); return false; }
    std::rewind(f);
    s_state.file = f; s_state.fileBytes = uint64_t(end); s_state.archiveSize = uint64_t(end);
    uint8_t header[kHeaderBytes];
    if (s_state.fileBytes < sizeof header || std::fread(header, 1, sizeof header, f) != sizeof header) { reject("short header"); return false; }
    const uint16_t version = get16(header + 4), headerBytes = get16(header + 6), entryBytes = get16(header + 8);
    s_state.entryCount = get32(header + 12); s_state.slotCount = get32(header + 16); s_state.maxDimension = get32(header + 20);
    const uint32_t layout = get32(header + 24);
    s_state.indexOffset = get64(header + 28); s_state.entriesOffset = get64(header + 36); s_state.payloadOffset = get64(header + 44);
    const uint64_t declaredSize = get64(header + 52);
    if (std::memcmp(header, "GX2P", 4) || version != 1 || headerBytes != kHeaderBytes || entryBytes != kEntryBytes ||
        layout != 0 || !s_state.entryCount || s_state.entryCount > kMaxEntries || !powerOfTwo(s_state.slotCount) ||
        s_state.slotCount < s_state.entryCount * 2u || s_state.slotCount > kMaxIndexSlots || !s_state.maxDimension ||
        declaredSize != s_state.fileBytes) { reject("unsupported header"); return false; }
    uint64_t indexBytes = uint64_t(s_state.slotCount) * 16, entriesBytes = uint64_t(s_state.entryCount) * kEntryBytes;
    uint64_t tableEnd = 0;
    if (!rangeOk(s_state.indexOffset, indexBytes, s_state.archiveSize) || !rangeOk(s_state.entriesOffset, entriesBytes, s_state.archiveSize) ||
        !addOk(s_state.entriesOffset, entriesBytes, &tableEnd) || tableEnd > s_state.payloadOffset || s_state.payloadOffset > s_state.archiveSize) {
        reject("invalid archive tables"); return false;
    }
    s_state.hashes = static_cast<uint64_t*>(MEMAllocFromDefaultHeapEx(uint32_t(s_state.slotCount * sizeof(uint64_t)), 64));
    s_state.indices = static_cast<uint32_t*>(MEMAllocFromDefaultHeapEx(uint32_t(s_state.slotCount * sizeof(uint32_t)), 64));
    s_state.entries = static_cast<TexturePackEntry*>(MEMAllocFromDefaultHeapEx(uint32_t(s_state.entryCount * sizeof(TexturePackEntry)), 64));
    if (!s_state.hashes || !s_state.indices || !s_state.entries) { reject("index allocation failed"); return false; }
    uint8_t slot[16];
    for (uint32_t i = 0; i != s_state.slotCount; ++i) {
        if (!readAt(s_state.indexOffset + uint64_t(i) * 16, slot, sizeof slot)) { reject("truncated index"); return false; }
        s_state.hashes[i] = get64(slot); s_state.indices[i] = get32(slot + 8);
        if (get32(slot + 12) > 1 || (s_state.indices[i] > s_state.entryCount && s_state.indices[i] != 0)) { reject("invalid index slot"); return false; }
    }
    uint8_t raw[kEntryBytes];
    for (uint32_t i = 0; i != s_state.entryCount; ++i) {
        if (!readAt(s_state.entriesOffset + uint64_t(i) * kEntryBytes, raw, sizeof raw) || !parseEntry(raw, &s_state.entries[i])) { reject("truncated entry"); return false; }
        TexturePackEntry& e = s_state.entries[i];
        if (!e.width || !e.height || e.width > s_state.maxDimension || e.height > s_state.maxDimension ||
            !validFormat(e.key.format) || !validCodec(e.rgbaCodec) || e.rgbaCodec != kRgbaCodec ||
            (e.key.wildcardTlut && e.key.tlutHash)) { reject("invalid entry metadata"); return false; }
        for (uint32_t l = 0; l != e.mipCount; ++l) {
            const TexturePackLevel& level = e.levels[l];
            if (!level.width || !level.height || level.width > s_state.maxDimension || level.height > s_state.maxDimension ||
                !level.rgbaSize || !rangeOk(level.rgbaOffset, level.rgbaSize, s_state.archiveSize) ||
                level.rgbaOffset < s_state.payloadOffset || !rangeOk(level.bcOffset, level.bcSize, s_state.archiveSize) ||
                (level.bcSize && level.bcOffset < s_state.payloadOffset)) { reject("invalid payload range"); return false; }
        }
    }
    s_state.enabled = true;
    WHBLogPrintf("SMS Wii U: [texpack] archive v%u entries=%u max=%u codec=RGBA8-linear index=%u",
                 unsigned(version), unsigned(s_state.entryCount), unsigned(s_state.maxDimension), unsigned(s_state.slotCount));
    return true;
}

void texturePackShutdown() {
    if (s_state.file) std::fclose(s_state.file);
    s_state.file = nullptr; freeIndex(); s_state = State();
}
bool texturePackEnabled() { return s_state.enabled; }
uint32_t texturePackMaxDimension() { return s_state.maxDimension; }
uint32_t texturePackEntryCount() { return s_state.entryCount; }
uint64_t texturePackReadBytes() { return s_state.readBytes; }

const TexturePackEntry* texturePackLookup(const TexturePackKey& query) {
    if (!s_state.enabled || !s_state.slotCount) return nullptr;
    TexturePackKey key = query; key.wildcardTlut = 0;
    const uint32_t mask = s_state.slotCount - 1;
    for (unsigned pass = 0; pass != 2; ++pass) {
        if (pass) { key.wildcardTlut = 1; key.tlutHash = 0; }
        const uint64_t hash = keyHash(key);
        for (uint32_t n = 0; n != s_state.slotCount; ++n) {
            const uint32_t slot = uint32_t(hash + n) & mask, number = s_state.indices[slot];
            if (!number) break;
            if (s_state.hashes[slot] != hash) continue;
            const TexturePackEntry& e = s_state.entries[number - 1];
            if ((!pass && texturePackKeyEqual(e.key, key)) || (pass && texturePackWildcardMatch(e.key, query))) return &e;
        }
        if (query.wildcardTlut) break;
    }
    return nullptr;
}

bool texturePackReadChunk(uint64_t offset, uint32_t bytes, void* out) {
    if (!s_state.enabled || bytes > kTexturePackMaxChunk || (bytes && !out)) return false;
    if (!readAt(offset, out, bytes)) return false;
    s_state.readBytes += bytes;
    return true;
}

} // namespace gx
