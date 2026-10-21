#include "gx2_texture_pack_format.h"
#include "gx_texture_pack_common.h"
#include "../../../tools/gx2_texture_pack.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "../src/third_party/bcdec.h"

namespace {

int failures;

void expect(bool condition, const std::string& message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << "\n"; }
}

void be32(std::vector<uint8_t>* out, uint32_t v) {
    out->push_back(uint8_t(v >> 24)); out->push_back(uint8_t(v >> 16)); out->push_back(uint8_t(v >> 8)); out->push_back(uint8_t(v));
}

void putLe32(std::vector<uint8_t>* out, size_t offset, uint32_t v) {
    (*out)[offset] = uint8_t(v); (*out)[offset + 1] = uint8_t(v >> 8);
    (*out)[offset + 2] = uint8_t(v >> 16); (*out)[offset + 3] = uint8_t(v >> 24);
}

void writeDds(const std::filesystem::path& path, uint32_t width, uint32_t height, const char fourcc[4],
              const std::vector<std::vector<uint8_t>>& levels) {
    std::vector<uint8_t> dds(128, 0);
    std::copy_n(reinterpret_cast<const uint8_t*>("DDS "), 4, dds.data());
    putLe32(&dds, 4, 124); putLe32(&dds, 8, 0x00021007); putLe32(&dds, 12, height); putLe32(&dds, 16, width);
    putLe32(&dds, 28, uint32_t(levels.size())); putLe32(&dds, 76, 32); putLe32(&dds, 80, 4);
    std::copy_n(reinterpret_cast<const uint8_t*>(fourcc), 4, dds.data() + 84);
    putLe32(&dds, 108, 0x00401008);
    for (const auto& level : levels) dds.insert(dds.end(), level.begin(), level.end());
    std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(dds.data()), dds.size());
}

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b != 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

uint32_t adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < size; ++i) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
    return b << 16 | a;
}

void pngChunk(std::vector<uint8_t>* png, const char type[4], const std::vector<uint8_t>& data) {
    be32(png, uint32_t(data.size()));
    const size_t start = png->size();
    png->insert(png->end(), type, type + 4); png->insert(png->end(), data.begin(), data.end());
    be32(png, crc32(png->data() + start, 4 + data.size()));
}

void writePng(const std::filesystem::path& path, uint32_t width, uint32_t height, uint8_t seed) {
    std::vector<uint8_t> raw; raw.reserve(size_t(height) * (width * 4 + 1));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x) {
            raw.push_back(uint8_t(seed + x * 13 + y * 7)); raw.push_back(uint8_t(seed + x * 3));
            raw.push_back(uint8_t(seed + y * 11)); raw.push_back(uint8_t(255 - ((x + y) & 31)));
        }
    }
    std::vector<uint8_t> zlib = {0x78, 0x01};
    for (size_t off = 0; off < raw.size();) {
        const uint16_t n = uint16_t(std::min<size_t>(65535, raw.size() - off));
        zlib.push_back(uint8_t(off + n == raw.size())); zlib.push_back(uint8_t(n)); zlib.push_back(uint8_t(n >> 8));
        const uint16_t inv = uint16_t(~n); zlib.push_back(uint8_t(inv)); zlib.push_back(uint8_t(inv >> 8));
        zlib.insert(zlib.end(), raw.begin() + off, raw.begin() + off + n); off += n;
    }
    const uint32_t adler = adler32(raw.data(), raw.size());
    zlib.push_back(uint8_t(adler >> 24)); zlib.push_back(uint8_t(adler >> 16)); zlib.push_back(uint8_t(adler >> 8)); zlib.push_back(uint8_t(adler));
    std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr; be32(&ihdr, width); be32(&ihdr, height); ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    pngChunk(&png, "IHDR", ihdr); pngChunk(&png, "IDAT", zlib); pngChunk(&png, "IEND", {});
    std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(png.data()), png.size());
}

std::string keyName(const std::vector<uint8_t>& source, uint32_t fmt, uint32_t w, uint32_t h, bool mip = false,
                    const std::vector<uint8_t>* tlut = nullptr) {
    return gx::hiresName(source.data(), fmt, w, h, mip, tlut ? tlut->data() : nullptr,
                         tlut ? uint32_t(tlut->size()) : 0);
}

std::string wildcardName(const std::string& exact) {
    const size_t last = exact.rfind('_');
    return exact.substr(0, last) + "_$" + exact.substr(last);
}

std::vector<uint8_t> bytes(uint32_t n, uint8_t seed) {
    std::vector<uint8_t> out(n); for (uint32_t i = 0; i < n; ++i) out[i] = uint8_t(seed + i * 17); return out;
}

void checkHashesAndNames() {
    expect(gx::xxh64(nullptr, 0, 0) == 0xef46db3751d8e999ull, "XXH64 empty vector");
    const uint8_t shortData[] = {'a', 'b', 'c'};
    expect(gx::xxh64(shortData, sizeof shortData, 0) == 0x44bc2cf5ad770999ull, "XXH64 short vector");
    uint8_t thirtyTwo[32]; for (int i = 0; i < 32; ++i) thirtyTwo[i] = uint8_t(i);
    expect(gx::xxh64(thirtyTwo, sizeof thirtyTwo, 0) == 0xcbf59c5116ff32b4ull, "XXH64 32-byte vector");
    std::vector<uint8_t> padded(64, 0); for (size_t i = 0; i < padded.size(); ++i) padded[i] = uint8_t(i ^ 0xa5);
    expect(gx::xxh64(padded.data(), padded.size(), 0) == 0xf03259a6f9ffc792ull, "XXH64 GX-padded vector");
    const std::string name = keyName(bytes(gx::texLevelBytes(6, 4, 4), 3), 6, 4, 4);
    expect(name.find("tex1_4x4_") == 0 && name.rfind("_6") == name.size() - 2, "shared PC/Wii U name construction");
}

void checkBc() {
    uint8_t rgba[64]; for (int i = 0; i < 16; ++i) { rgba[i * 4 + 0] = 80; rgba[i * 4 + 1] = 120; rgba[i * 4 + 2] = 200; rgba[i * 4 + 3] = uint8_t(i * 16); }
    uint8_t block[16], decoded[64]; gx::encodeBlock(7, rgba, block); bcdec_bc7(block, decoded, 16);
    int worst = 0; for (int i = 0; i < 64; ++i) worst = std::max(worst, std::abs(int(rgba[i]) - int(decoded[i])));
    expect(worst <= 40, "BC7 alpha-ramp round trip");
}

void checkArchiveAndConversion() {
    const auto root = std::filesystem::temp_directory_path() / ("sms-gx2-pack-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto pack = root / "GMSE01"; std::filesystem::create_directories(pack / "nested");
    auto normalSource = bytes(gx::texLevelBytes(6, 4, 4), 3);
    auto mipSource = bytes(gx::texLevelBytes(6, 16, 8), 17);
    auto ciSource = bytes(gx::texLevelBytes(8, 8, 8), 1); std::vector<uint8_t> tlut(64); for (size_t i = 0; i < tlut.size(); ++i) tlut[i] = uint8_t(i * 5);
    auto wildSource = bytes(gx::texLevelBytes(9, 8, 4), 9);
    auto pairSource = bytes(gx::texLevelBytes(6, 2, 2), 31), pairMipSource = bytes(gx::texLevelBytes(6, 2, 2), 44);
    auto hugeSource = bytes(gx::texLevelBytes(6, 1024, 1), 91);
    const std::string normal = keyName(normalSource, 6, 4, 4);
    const std::string mip = keyName(mipSource, 6, 16, 8, true);
    const std::string ci = keyName(ciSource, 8, 8, 8, false, &tlut);
    const std::string wild = wildcardName(keyName(wildSource, 9, 8, 4));
    const std::string plain = keyName(pairSource, 6, 2, 2);
    const std::string marked = keyName(pairMipSource, 6, 2, 2, true);
    const std::string huge = keyName(hugeSource, 6, 1024, 1);
    writePng(pack / (normal + ".png"), 4, 4, 3); writePng(pack / (mip + ".png"), 16, 8, 17); writePng(pack / (mip + "_mip1.png"), 8, 4, 33);
    writePng(pack / (ci + ".png"), 8, 8, 51); writePng(pack / "nested" / (wild + ".png"), 8, 4, 65);
    writePng(pack / (plain + ".png"), 2, 2, 79); writePng(pack / (marked + ".png"), 2, 2, 83); writePng(pack / (huge + ".png"), 1024, 1, 97);
    gx::TexturePackConversionOptions options; options.maxSize = 8; gx::TexturePackConversionStats stats; std::string error;
    const auto archive = root / "GMSE01.gx2pack";
    expect(gx::convertTexturePack(pack.string(), archive.string(), options, &stats, &error), "synthetic PNG conversion: " + error);
    expect(stats.entries == 7 && stats.files == 8, "converter counts all source files and entries");
    gx::TexturePackReader reader; expect(reader.open(archive.string(), &error), "archive reader opens converter output: " + error);
    expect(reader.entries().size() == 7 && reader.info().layoutVersion == 0, "archive metadata and entry count");
    const gx::TexturePackEntry* mipEntry = nullptr; for (const auto& e : reader.entries()) if (e.key.dataHash == gx::xxh64(mipSource.data(), mipSource.size(), 0)) mipEntry = &e;
    expect(mipEntry && mipEntry->width == 8 && mipEntry->height == 4 && mipEntry->mipCount == 4, "max-size scaling and generated/explicit mip chain");
    if (mipEntry) for (const auto& l : mipEntry->levels) expect(l.rgbaSize == l.width * l.height * 4 && l.bcSize == ((l.width + 3) / 4) * ((l.height + 3) / 4) * 16, "linear payload sizes");
    gx::TexturePackKey wildcardQuery; wildcardQuery.sourceWidth = 8; wildcardQuery.sourceHeight = 4; wildcardQuery.format = 9; wildcardQuery.dataHash = gx::xxh64(wildSource.data(), wildSource.size(), 0);
    const auto* wildcard = reader.find(wildcardQuery); expect(wildcard && wildcard->key.wildcardTlut, "wildcard TLUT lookup");
    uint32_t ciLo = 0, ciHi = 0; gx::paletteRange(ciSource.data(), ciSource.size(), 8, &ciLo, &ciHi);
    gx::TexturePackKey exactQuery; exactQuery.sourceWidth = 8; exactQuery.sourceHeight = 8; exactQuery.format = 8; exactQuery.dataHash = gx::xxh64(ciSource.data(), ciSource.size(), 0);
    exactQuery.tlutHash = gx::xxh64(tlut.data() + 2 * ciLo, 2 * (ciHi + 1 - ciLo), 0);
    const auto* exact = reader.find(exactQuery, false); expect(exact && !exact->key.wildcardTlut, "exact TLUT lookup");
    if (exact) { std::vector<uint8_t> rgba; expect(reader.read(exact->levels[0].rgbaOffset, exact->levels[0].rgbaSize, &rgba, &error), "RGBA fallback read"); expect(rgba.size() == exact->levels[0].rgbaSize, "RGBA fallback byte count"); }
    std::vector<uint8_t> raw; { std::ifstream in(archive, std::ios::binary); in.seekg(0, std::ios::end); const size_t n = size_t(in.tellg()); raw.resize(n); in.seekg(0); in.read(reinterpret_cast<char*>(raw.data()), n); }
    raw.pop_back(); gx::TexturePackReader corrupt; expect(!corrupt.openBytes(raw, &error), "truncated archive rejected");
    raw.push_back(0); raw[5] = 2; expect(!corrupt.openBytes(raw, &error), "bad-version archive rejected");
    writePng(pack / "nested" / (normal + ".png"), 4, 4, 4); expect(gx::convertTexturePack(pack.string(), (root / "duplicate.gx2pack").string(), options, &stats, &error), "duplicate level keeps first match: " + error);
    std::filesystem::remove_all(root);
}

void checkDdsConversion() {
    const auto root = std::filesystem::temp_directory_path() / ("sms-gx2-dds-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto dxt1Dir = root / "dxt1"; std::filesystem::create_directories(dxt1Dir / "environment/secrets");
    auto source = bytes(gx::texLevelBytes(6, 4, 4), 101);
    const std::string dxt1Name = keyName(source, 6, 4, 4, true);
    const std::vector<uint8_t> redBlock = {0x00, 0xf8, 0xe0, 0x07, 0, 0, 0, 0};
    std::vector<uint8_t> dxt1Base; for (int i = 0; i < 4; ++i) dxt1Base.insert(dxt1Base.end(), redBlock.begin(), redBlock.end());
    writeDds(dxt1Dir / (dxt1Name + ".dds"), 8, 8, "DXT1", {dxt1Base, redBlock});
    writePng(dxt1Dir / "environment/secrets/w2048 - rend 2.png", 2, 2, 7);

    gx::TexturePackConversionOptions options; options.maxSize = 16; options.bcCodec = gx::TEXTURE_PACK_CODEC_BC1;
    gx::TexturePackConversionStats stats; std::string error; const auto dxt1Archive = root / "dxt1.gx2pack";
    expect(gx::convertTexturePack(dxt1Dir.string(), dxt1Archive.string(), options, &stats, &error), "DXT1 conversion: " + error);
    expect(stats.files == 1 && stats.entries == 1, "non-texture PNG ignored during DDS collection");
    gx::TexturePackReader reader; expect(reader.open(dxt1Archive.string(), &error), "DXT1 archive opens: " + error);
    const gx::TexturePackEntry* entry = reader.entries().empty() ? nullptr : &reader.entries()[0];
    expect(entry && entry->key.sourceWidth == 4 && entry->key.sourceHeight == 4 && entry->width == 8 && entry->height == 8,
           "DDS header dimensions drive image while filename dimensions remain key");
    if (entry) {
        std::vector<uint8_t> payload, rgba;
        expect(reader.read(entry->levels[0].bcOffset, entry->levels[0].bcSize, &payload, &error) && payload == dxt1Base,
               "DXT1 base blocks pass through unchanged");
        expect(reader.read(entry->levels[1].bcOffset, entry->levels[1].bcSize, &payload, &error) && payload == redBlock,
               "DXT1 embedded mip blocks pass through unchanged");
        expect(reader.read(entry->levels[0].rgbaOffset, entry->levels[0].rgbaSize, &rgba, &error) && rgba.size() >= 4 &&
                   rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255,
               "DXT1 decodes to RGBA8");
    }

    options.maxSize = 4; const auto scaledArchive = root / "scaled.gx2pack";
    expect(gx::convertTexturePack(dxt1Dir.string(), scaledArchive.string(), options, &stats, &error), "size-capped DXT1 conversion: " + error);
    gx::TexturePackReader scaled; expect(scaled.open(scaledArchive.string(), &error), "size-capped DXT1 archive opens: " + error);
    expect(!scaled.entries().empty() && scaled.entries()[0].width == 4 && scaled.entries()[0].height == 4 &&
               scaled.entries()[0].levels[0].bcSize == 8,
           "DDS obeys size cap and recompresses scaled level");

    const auto dxt5Dir = root / "dxt5"; std::filesystem::create_directories(dxt5Dir);
    auto alphaSource = bytes(gx::texLevelBytes(6, 4, 4), 119); const std::string dxt5Name = keyName(alphaSource, 6, 4, 4);
    const std::vector<uint8_t> dxt5Block = {255, 0, 0, 0, 0, 0, 0, 0, 0x1f, 0x00, 0x00, 0xf8, 0, 0, 0, 0};
    writeDds(dxt5Dir / (dxt5Name + ".dds"), 4, 4, "DXT5", {dxt5Block});
    options.maxSize = 512; options.bcCodec = gx::TEXTURE_PACK_CODEC_BC3; const auto dxt5Archive = root / "dxt5.gx2pack";
    expect(gx::convertTexturePack(dxt5Dir.string(), dxt5Archive.string(), options, &stats, &error), "DXT5 conversion: " + error);
    gx::TexturePackReader dxt5; expect(dxt5.open(dxt5Archive.string(), &error), "DXT5 archive opens: " + error);
    if (!dxt5.entries().empty()) {
        std::vector<uint8_t> payload, rgba; const auto& level = dxt5.entries()[0].levels[0];
        expect(dxt5.read(level.bcOffset, level.bcSize, &payload, &error) && payload == dxt5Block, "DXT5 blocks pass through unchanged");
        expect(dxt5.read(level.rgbaOffset, level.rgbaSize, &rgba, &error) && rgba.size() >= 4 &&
                   rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 255 && rgba[3] == 255,
               "DXT5 decodes color and alpha to RGBA8");
    }

    const auto badDir = root / "bad"; std::filesystem::create_directories(badDir);
    writeDds(badDir / (dxt1Name + ".dds"), 4, 4, "DXT3", {std::vector<uint8_t>(16)});
    expect(!gx::convertTexturePack(badDir.string(), (root / "bad.gx2pack").string(), options, &stats, &error),
           "unsupported DDS FourCC rejected");
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    checkHashesAndNames(); checkBc(); checkArchiveAndConversion(); checkDdsConversion();
    if (failures) return failures;
    std::cout << "gx2_texture_pack_test: PASS\n";
    return 0;
}
