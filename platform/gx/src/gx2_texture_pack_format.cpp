#include "gx2_texture_pack_format.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>

namespace gx {

namespace {

static const uint16_t kVersion = 1;
static const uint16_t kHeaderBytes = 72;
static const uint16_t kEntryBytes = 640;
static const uint32_t kMaxLevels = 16;
static const uint32_t kMaxEntries = 1u << 20;

static void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
static void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
static void put64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = uint8_t(v); v >>= 8; }
}
static uint16_t get16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
static uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static uint64_t get64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i != 8; ++i) v = (v << 8) | p[i];
    return v;
}

static bool addOk(uint64_t a, uint64_t b, uint64_t* out) {
    if (b > std::numeric_limits<uint64_t>::max() - a) return false;
    *out = a + b;
    return true;
}
static bool mulOk(uint64_t a, uint64_t b, uint64_t* out) {
    if (a && b > std::numeric_limits<uint64_t>::max() / a) return false;
    *out = a * b;
    return true;
}
static uint64_t align16(uint64_t v) { return (v + 15u) & ~uint64_t(15); }

static bool validFormat(uint32_t fmt) {
    return fmt == 0 || fmt == 1 || fmt == 2 || fmt == 3 || fmt == 4 || fmt == 5 ||
           fmt == 6 || fmt == 8 || fmt == 9 || fmt == 10 || fmt == 14;
}
static uint32_t bcBytes(uint32_t codec) {
    return codec == TEXTURE_PACK_CODEC_BC1 ? 8 : codec == TEXTURE_PACK_CODEC_NONE ? 0 :
           (codec == TEXTURE_PACK_CODEC_BC2 || codec == TEXTURE_PACK_CODEC_BC3 || codec == TEXTURE_PACK_CODEC_BC7) ? 16 : 0;
}
static bool validCodec(uint32_t codec, bool rgba) {
    return rgba ? codec == TEXTURE_PACK_CODEC_RGBA8_LINEAR : bcBytes(codec) != 0;
}
static bool expectedSizes(uint32_t w, uint32_t h, uint32_t bcCodec, uint64_t* rgba, uint64_t* bc) {
    uint64_t pixels, blocksX, blocksY, blocks;
    if (!mulOk(w, h, &pixels) || !mulOk(pixels, 4, rgba)) return false;
    if (!mulOk((w + 3u) / 4u, (h + 3u) / 4u, &blocks)) return false;
    if (!mulOk(blocks, bcBytes(bcCodec), bc)) return false;
    return true;
}

static uint64_t keyHash(const TexturePackKey& k) {
    /* FNV-1a over the canonical big-endian key.  The hash is only an index
     * accelerator; the complete key is always compared after probing. */
    uint8_t b[40];
    put32(b + 0, k.sourceWidth); put32(b + 4, k.sourceHeight); put32(b + 8, k.format);
    put32(b + 12, k.mipmapped ? 1 : 0); put64(b + 16, k.dataHash);
    put64(b + 24, k.wildcardTlut ? 0 : k.tlutHash); put32(b + 32, k.wildcardTlut ? 1 : 0);
    put32(b + 36, 0);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i != sizeof b; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static bool keyMatches(const TexturePackKey& a, const TexturePackKey& b) {
    return a == b;
}
static bool keyMatchesWildcard(const TexturePackKey& stored, const TexturePackKey& query) {
    return stored.sourceWidth == query.sourceWidth && stored.sourceHeight == query.sourceHeight &&
           stored.format == query.format && stored.mipmapped == query.mipmapped &&
           stored.dataHash == query.dataHash && stored.wildcardTlut;
}
static uint32_t nextSlots(size_t n) {
    uint64_t want = std::max<uint64_t>(2, uint64_t(n) * 2);
    uint32_t slots = 1;
    while (slots < want && slots < (1u << 31)) slots <<= 1;
    return slots;
}
static bool rangeOk(uint64_t off, uint64_t len, uint64_t total) {
    uint64_t end;
    return addOk(off, len, &end) && off <= total && end <= total;
}
static bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

static void writeEntry(uint8_t* p, const TexturePackEntry& e) {
    std::memset(p, 0, kEntryBytes);
    put32(p + 0, e.key.sourceWidth); put32(p + 4, e.key.sourceHeight); put32(p + 8, e.key.format);
    put32(p + 12, (e.key.mipmapped ? 1u : 0u) | (e.key.wildcardTlut ? 2u : 0u));
    put64(p + 16, e.key.dataHash); put64(p + 24, e.key.tlutHash);
    put32(p + 32, e.width); put32(p + 36, e.height); put32(p + 40, e.mipCount);
    put32(p + 44, e.rgbaCodec); put32(p + 48, e.bcCodec); put32(p + 52, e.surfaceFormat);
    put32(p + 56, e.surfacePitch); put64(p + 60, e.surfaceImageSize);
    put64(p + 68, e.surfaceMipmapOffset); put64(p + 76, e.surfaceMipmapSize);
    for (uint32_t i = 0; i != kMaxLevels; ++i) {
        const TexturePackLevel* l = i < e.levels.size() ? &e.levels[i] : nullptr;
        uint8_t* q = p + 84 + i * 32;
        if (!l) continue;
        put32(q + 0, l->width); put32(q + 4, l->height); put64(q + 8, l->rgbaOffset);
        put32(q + 16, l->rgbaSize); put64(q + 20, l->bcOffset); put32(q + 28, l->bcSize);
    }
}

static TexturePackEntry readEntry(const uint8_t* p) {
    TexturePackEntry e;
    e.key.sourceWidth = get32(p + 0); e.key.sourceHeight = get32(p + 4); e.key.format = get32(p + 8);
    const uint32_t flags = get32(p + 12);
    e.key.mipmapped = (flags & 1) != 0; e.key.wildcardTlut = (flags & 2) != 0;
    e.key.dataHash = get64(p + 16); e.key.tlutHash = get64(p + 24);
    e.width = get32(p + 32); e.height = get32(p + 36); e.mipCount = get32(p + 40);
    e.rgbaCodec = get32(p + 44); e.bcCodec = get32(p + 48); e.surfaceFormat = get32(p + 52);
    e.surfacePitch = get32(p + 56); e.surfaceImageSize = get64(p + 60);
    e.surfaceMipmapOffset = get64(p + 68); e.surfaceMipmapSize = get64(p + 76);
    e.levels.resize(e.mipCount);
    for (uint32_t i = 0; i < e.mipCount; ++i) {
        const uint8_t* q = p + 84 + i * 32;
        e.levels[i].width = get32(q + 0); e.levels[i].height = get32(q + 4);
        e.levels[i].rgbaOffset = get64(q + 8); e.levels[i].rgbaSize = get32(q + 16);
        e.levels[i].bcOffset = get64(q + 20); e.levels[i].bcSize = get32(q + 28);
    }
    return e;
}

} // namespace

bool operator==(const TexturePackKey& a, const TexturePackKey& b) {
    return a.sourceWidth == b.sourceWidth && a.sourceHeight == b.sourceHeight && a.format == b.format &&
           a.mipmapped == b.mipmapped && a.dataHash == b.dataHash && a.tlutHash == b.tlutHash &&
           a.wildcardTlut == b.wildcardTlut;
}

bool operator<(const TexturePackKey& a, const TexturePackKey& b) {
    if (a.sourceWidth != b.sourceWidth) return a.sourceWidth < b.sourceWidth;
    if (a.sourceHeight != b.sourceHeight) return a.sourceHeight < b.sourceHeight;
    if (a.format != b.format) return a.format < b.format;
    if (a.mipmapped != b.mipmapped) return a.mipmapped < b.mipmapped;
    if (a.dataHash != b.dataHash) return a.dataHash < b.dataHash;
    if (a.wildcardTlut != b.wildcardTlut) return a.wildcardTlut < b.wildcardTlut;
    return a.tlutHash < b.tlutHash;
}

bool TexturePackWriter::serialize(const std::vector<TexturePackEntry>& input, uint32_t maxDimension,
                                  std::vector<uint8_t>* bytes, std::string* error) {
    if (!bytes) return fail(error, "null output");
    if (input.empty()) return fail(error, "archive has no entries");
    if (input.size() > kMaxEntries || !maxDimension) return fail(error, "invalid archive limits");
    std::vector<TexturePackEntry> entries = input;
    std::sort(entries.begin(), entries.end(), [](const TexturePackEntry& a, const TexturePackEntry& b) { return a.key < b.key; });
    std::set<TexturePackKey> seen;
    for (TexturePackEntry& e : entries) {
        if (!seen.insert(e.key).second) return fail(error, "duplicate archive key");
        if (!e.width || !e.height || e.width > maxDimension || e.height > maxDimension ||
            e.mipCount != e.levels.size() || !e.mipCount || e.mipCount > kMaxLevels ||
            !validFormat(e.key.format) || !validCodec(e.rgbaCodec, true) || !validCodec(e.bcCodec, false))
            return fail(error, "invalid entry metadata");
        if (e.key.wildcardTlut && e.key.tlutHash) return fail(error, "wildcard has a TLUT hash");
        e.surfacePitch = e.surfacePitch ? e.surfacePitch : e.width * 4;
        uint64_t imageSize = 0;
        for (TexturePackLevel& l : e.levels) {
            if (!l.width || !l.height || l.width > maxDimension || l.height > maxDimension) return fail(error, "level exceeds dimension cap");
            uint64_t rgbaSize, bcSize;
            if (!expectedSizes(l.width, l.height, e.bcCodec, &rgbaSize, &bcSize) ||
                rgbaSize > std::numeric_limits<uint32_t>::max() || bcSize > std::numeric_limits<uint32_t>::max() ||
                l.rgbaPayload.size() != rgbaSize || l.bcPayload.size() != bcSize)
                return fail(error, "payload size does not match level");
            if (!addOk(imageSize, rgbaSize, &imageSize)) return fail(error, "image size overflow");
        }
        if (!e.surfaceImageSize) e.surfaceImageSize = e.levels.front().rgbaPayload.size();
        if (!e.surfaceMipmapSize && e.levels.size() > 1) {
            uint64_t mip = 0;
            for (size_t i = 1; i < e.levels.size(); ++i) if (!addOk(mip, e.levels[i].rgbaPayload.size(), &mip)) return fail(error, "mipmap size overflow");
            e.surfaceMipmapSize = mip;
        }
    }
    const uint32_t slots = nextSlots(entries.size());
    uint64_t indexBytes, entryBytes, n;
    if (!mulOk(slots, 16, &indexBytes) || !mulOk(entries.size(), kEntryBytes, &entryBytes) ||
        !addOk(kHeaderBytes, indexBytes, &n) || !addOk(n, entryBytes, &n)) return fail(error, "archive size overflow");
    const uint64_t indexOffset = kHeaderBytes, entriesOffset = indexOffset + indexBytes;
    const uint64_t payloadOffset = align16(n);
    std::vector<uint8_t> index(size_t(indexBytes), 0), table(size_t(entryBytes), 0);
    uint64_t cursor = payloadOffset;
    for (TexturePackEntry& e : entries) {
        for (TexturePackLevel& l : e.levels) {
            cursor = align16(cursor); l.rgbaOffset = cursor; l.rgbaSize = uint32_t(l.rgbaPayload.size());
            cursor += l.rgbaPayload.size(); cursor = align16(cursor); l.bcOffset = cursor; l.bcSize = uint32_t(l.bcPayload.size());
            cursor += l.bcPayload.size();
        }
        if (e.levels.size() > 1) {
            e.surfaceMipmapOffset = e.levels[1].rgbaOffset;
            if (!e.surfaceMipmapSize) {
                for (size_t i = 1; i < e.levels.size(); ++i) e.surfaceMipmapSize += e.levels[i].rgbaPayload.size();
            }
        }
    }
    const uint64_t archiveSize = cursor;
    std::vector<uint8_t> result(size_t(archiveSize), 0);
    std::memcpy(result.data(), "GX2P", 4); put16(result.data() + 4, kVersion); put16(result.data() + 6, kHeaderBytes);
    put16(result.data() + 8, kEntryBytes); put16(result.data() + 10, 0); put32(result.data() + 12, uint32_t(entries.size()));
    put32(result.data() + 16, slots); put32(result.data() + 20, maxDimension); put32(result.data() + 24, 0); // linear payload layout
    put64(result.data() + 28, indexOffset); put64(result.data() + 36, entriesOffset); put64(result.data() + 44, payloadOffset);
    put64(result.data() + 52, archiveSize); put32(result.data() + 60, 0); put32(result.data() + 64, 0); put32(result.data() + 68, 0);
    for (size_t i = 0; i < entries.size(); ++i) writeEntry(result.data() + entriesOffset + i * kEntryBytes, entries[i]);
    const uint64_t mask = slots - 1;
    for (size_t i = 0; i < entries.size(); ++i) {
        uint64_t h = keyHash(entries[i].key); uint64_t slot = h & mask;
        while (get32(index.data() + slot * 16 + 12)) slot = (slot + 1) & mask;
        put64(index.data() + slot * 16, h); put32(index.data() + slot * 16 + 8, uint32_t(i + 1)); put32(index.data() + slot * 16 + 12, 1);
    }
    std::memcpy(result.data() + indexOffset, index.data(), index.size());
    for (const TexturePackEntry& e : entries) for (const TexturePackLevel& l : e.levels) {
        std::memcpy(result.data() + l.rgbaOffset, l.rgbaPayload.data(), l.rgbaPayload.size());
        std::memcpy(result.data() + l.bcOffset, l.bcPayload.data(), l.bcPayload.size());
    }
    *bytes = std::move(result);
    return true;
}

bool TexturePackWriter::write(const std::string& path, const std::vector<TexturePackEntry>& entries,
                              uint32_t maxDimension, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!serialize(entries, maxDimension, &bytes, error)) return false;
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return fail(error, "cannot open archive for writing");
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out) return fail(error, "cannot write archive");
    return true;
}

TexturePackReader::TexturePackReader() : valid_(false) {}

void TexturePackReader::close() {
    bytes_.clear(); entries_.clear(); indexHashes_.clear(); indexEntries_.clear(); info_ = TexturePackArchiveInfo(); valid_ = false;
}

bool TexturePackReader::open(const std::string& path, std::string* error) {
    close();
    std::ifstream in(path.c_str(), std::ios::binary | std::ios::ate);
    if (!in) return fail(error, "cannot open archive");
    std::streamsize n = in.tellg();
    if (n < 0 || uint64_t(n) > std::numeric_limits<size_t>::max()) return fail(error, "invalid archive size");
    bytes_.resize(size_t(n)); in.seekg(0); in.read(reinterpret_cast<char*>(bytes_.data()), n);
    if (!in) return fail(error, "cannot read archive");
    if (!parse(bytes_.data(), bytes_.size(), error)) { close(); return false; }
    return true;
}

bool TexturePackReader::openBytes(const std::vector<uint8_t>& bytes, std::string* error) {
    close(); bytes_ = bytes;
    if (!parse(bytes_.data(), bytes_.size(), error)) { close(); return false; }
    return true;
}

bool TexturePackReader::parse(const uint8_t* data, size_t size, std::string* error) {
    if (!data || size < kHeaderBytes || std::memcmp(data, "GX2P", 4) != 0) return fail(error, "bad archive header");
    if (get16(data + 4) != kVersion || get16(data + 6) != kHeaderBytes || get16(data + 8) != kEntryBytes) return fail(error, "unsupported archive version");
    const uint32_t count = get32(data + 12), slots = get32(data + 16);
    if (!count || count > kMaxEntries || !slots || (slots & (slots - 1)) || slots < count * 2u) return fail(error, "invalid archive index");
    info_.version = get16(data + 4); info_.layoutVersion = get32(data + 24); info_.maxDimension = get32(data + 20); info_.indexSlots = slots;
    info_.indexOffset = get64(data + 28); info_.entriesOffset = get64(data + 36); info_.payloadOffset = get64(data + 44); info_.archiveSize = get64(data + 52);
    if (info_.archiveSize != size || info_.indexOffset < kHeaderBytes || info_.entriesOffset < info_.indexOffset ||
        info_.payloadOffset < info_.entriesOffset || info_.payloadOffset > size)
        return fail(error, "archive offsets are inconsistent");
    uint64_t indexBytes, entryBytes, end;
    if (!mulOk(slots, 16, &indexBytes) || !mulOk(count, kEntryBytes, &entryBytes) ||
        !rangeOk(info_.indexOffset, indexBytes, size) || !rangeOk(info_.entriesOffset, entryBytes, size) ||
        !addOk(info_.entriesOffset, entryBytes, &end) || end > info_.payloadOffset)
        return fail(error, "archive table is outside file");
    indexHashes_.resize(slots); indexEntries_.resize(slots);
    for (uint32_t i = 0; i < slots; ++i) {
        const uint8_t* p = data + info_.indexOffset + uint64_t(i) * 16;
        indexHashes_[i] = get64(p); indexEntries_[i] = get32(p + 8);
        if (get32(p + 12) > 1 || (indexEntries_[i] > count && indexEntries_[i] != 0)) return fail(error, "invalid archive index slot");
    }
    entries_.reserve(count); std::set<TexturePackKey> seen;
    for (uint32_t i = 0; i < count; ++i) {
        TexturePackEntry e = readEntry(data + info_.entriesOffset + uint64_t(i) * kEntryBytes);
        if (!e.width || !e.height || e.width > info_.maxDimension || e.height > info_.maxDimension ||
            e.mipCount == 0 || e.mipCount > kMaxLevels || e.mipCount != e.levels.size() || !validFormat(e.key.format) ||
            !validCodec(e.rgbaCodec, true) || !validCodec(e.bcCodec, false) || (e.key.wildcardTlut && e.key.tlutHash))
            return fail(error, "invalid archive entry");
        if (!seen.insert(e.key).second) return fail(error, "duplicate archive key");
        for (const TexturePackLevel& l : e.levels) {
            uint64_t rgba, bc;
            if (!l.width || !l.height || l.width > info_.maxDimension || l.height > info_.maxDimension ||
                !expectedSizes(l.width, l.height, e.bcCodec, &rgba, &bc) || rgba != l.rgbaSize || bc != l.bcSize ||
                !rangeOk(l.rgbaOffset, l.rgbaSize, size) || !rangeOk(l.bcOffset, l.bcSize, size) ||
                l.rgbaOffset < info_.payloadOffset || l.bcOffset < info_.payloadOffset)
                return fail(error, "invalid archive payload range");
        }
        entries_.push_back(e);
    }
    valid_ = true;
    return true;
}

const TexturePackEntry* TexturePackReader::find(const TexturePackKey& key, bool allowWildcard) const {
    if (!valid_ || indexEntries_.empty()) return nullptr;
    const uint32_t mask = uint32_t(indexEntries_.size() - 1);
    TexturePackKey exact = key; exact.wildcardTlut = false;
    auto lookup = [&](const TexturePackKey& wanted, bool wildcard) -> const TexturePackEntry* {
        uint64_t h = keyHash(wanted);
        for (uint32_t n = 0; n != indexEntries_.size(); ++n) {
            const uint32_t slot = uint32_t((h + n) & mask), number = indexEntries_[slot];
            if (!number) return nullptr;
            if (indexHashes_[slot] != h) continue;
            const TexturePackEntry& e = entries_[number - 1];
            if ((!wildcard && keyMatches(e.key, wanted)) || (wildcard && keyMatchesWildcard(e.key, wanted))) return &e;
        }
        return nullptr;
    };
    if (!key.wildcardTlut) {
        if (const TexturePackEntry* e = lookup(exact, false)) return e;
        if (allowWildcard) { exact.wildcardTlut = true; exact.tlutHash = 0; return lookup(exact, true); }
        return nullptr;
    }
    return lookup(key, false);
}

bool TexturePackReader::read(uint64_t offset, uint32_t size, std::vector<uint8_t>* out, std::string* error) const {
    if (!out || !valid_ || !rangeOk(offset, size, bytes_.size())) return fail(error, "archive read is outside file");
    out->assign(bytes_.begin() + size_t(offset), bytes_.begin() + size_t(offset + size));
    return true;
}

bool TexturePackReader::read(uint64_t offset, uint32_t size, void* out, std::string* error) const {
    if (!valid_ || (size && !out) || !rangeOk(offset, size, bytes_.size())) return fail(error, "archive read is outside file");
    if (size) std::memcpy(out, bytes_.data() + offset, size);
    return true;
}

} // namespace gx
