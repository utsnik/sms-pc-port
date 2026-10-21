#include "gx2_texture_pack_format.h"

#include <cstring>
#include <iostream>

namespace {
int failures;
void expect(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << "FAIL: " << message << "\n"; } }

gx::TexturePackEntry entry(uint32_t w, uint32_t h, uint32_t fmt, uint64_t hash, bool wildcard) {
    gx::TexturePackEntry e; e.key.sourceWidth = w; e.key.sourceHeight = h; e.key.format = fmt;
    e.key.dataHash = hash; e.key.wildcardTlut = wildcard; e.key.tlutHash = wildcard ? 0 : 0x1122334455667788ull;
    e.width = w; e.height = h; e.mipCount = 2; e.rgbaCodec = gx::TEXTURE_PACK_CODEC_RGBA8_LINEAR; e.bcCodec = gx::TEXTURE_PACK_CODEC_BC7;
    for (uint32_t i = 0; i != 2; ++i) {
        gx::TexturePackLevel l; l.width = w >> i ? w >> i : 1; l.height = h >> i ? h >> i : 1;
        l.rgbaPayload.assign(size_t(l.width) * l.height * 4, uint8_t(hash + i));
        l.bcPayload.assign(size_t((l.width + 3) / 4) * ((l.height + 3) / 4) * 16, uint8_t(hash + i + 9));
        e.levels.push_back(l);
    }
    return e;
}
}

int main() {
    gx::TexturePackEntry exact = entry(8, 4, 6, 0x0102030405060708ull, false);
    gx::TexturePackEntry wildcard = entry(16, 8, 9, 0x1112131415161718ull, true);
    std::vector<gx::TexturePackEntry> entries = {exact, wildcard}; std::vector<uint8_t> bytes; std::string error;
    expect(gx::TexturePackWriter::serialize(entries, 64, &bytes, &error), "two-entry archive serializes");
    gx::TexturePackReader reader; expect(reader.openBytes(bytes, &error), "serialized archive reopens");
    expect(reader.entries().size() == 2 && reader.info().archiveSize == bytes.size(), "header and fixed table round trip");
    gx::TexturePackKey query = exact.key; const gx::TexturePackEntry* found = reader.find(query, false);
    expect(found && found->key == exact.key, "exact key lookup");
    query = wildcard.key; query.wildcardTlut = false; query.tlutHash = 0x9988776655443322ull;
    found = reader.find(query); expect(found && found->key.wildcardTlut, "wildcard TLUT lookup");
    if (found) {
        std::vector<uint8_t> payload; expect(reader.read(found->levels[0].rgbaOffset, found->levels[0].rgbaSize, &payload, &error), "payload range read");
        expect(payload == wildcard.levels[0].rgbaPayload, "payload bytes round trip exactly");
    }
    std::vector<gx::TexturePackEntry> duplicate = {exact, exact}; expect(!gx::TexturePackWriter::serialize(duplicate, 64, &bytes, &error), "duplicate key rejected");
    std::vector<uint8_t> truncated(bytes); if (!truncated.empty()) truncated.pop_back(); expect(!reader.openBytes(truncated, &error), "truncated archive rejected");
    expect(gx::TexturePackWriter::serialize(entries, 64, &bytes, &error), "archive can be serialized again");
    bytes[5] = 2; expect(!reader.openBytes(bytes, &error), "bad version rejected");
    if (failures) return failures;
    std::cout << "gx2_texture_pack_format_test: PASS\n";
    return 0;
}
