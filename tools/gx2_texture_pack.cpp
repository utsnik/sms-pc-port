#include "gx2_texture_pack.h"
#include "../platform/gx/src/gx2_texture_pack_format.h"
#include "../platform/gx/src/gx_texture_pack_common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <vector>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO_OVERRIDE
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "../platform/gx/src/third_party/stb_image.h"

namespace gx {

namespace {

struct Image {
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> pixels;
};

struct SourceFile {
    std::filesystem::path path;
    int level;
};

struct SourceGroup {
    TexturePackKey key;
    bool arbitrary;
    std::map<int, SourceFile> files;
    SourceGroup() : arbitrary(false) {}
};

static bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

static bool readFile(const std::filesystem::path& path, std::vector<uint8_t>* out, std::string* error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return fail(error, "cannot open PNG " + path.string());
    std::streamsize n = in.tellg();
    if (n < 0) return fail(error, "invalid PNG size " + path.string());
    out->resize(size_t(n)); in.seekg(0); in.read(reinterpret_cast<char*>(out->data()), n);
    return bool(in) || n == 0 ? true : fail(error, "cannot read PNG " + path.string());
}

static bool decodePng(const std::filesystem::path& path, Image* out, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, &bytes, error)) return false;
    int w = 0, h = 0, channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
    if (!decoded || w <= 0 || h <= 0) {
        std::string why = stbi_failure_reason() ? stbi_failure_reason() : "invalid PNG";
        if (decoded) stbi_image_free(decoded);
        return fail(error, "cannot decode " + path.string() + ": " + why);
    }
    out->width = uint32_t(w); out->height = uint32_t(h);
    out->pixels.assign(decoded, decoded + size_t(w) * h * 4); stbi_image_free(decoded);
    return true;
}

static Image scaleBox(const Image& src, uint32_t width, uint32_t height) {
    if (src.width == width && src.height == height) return src;
    Image out; out.width = width; out.height = height; out.pixels.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t y0 = uint32_t(uint64_t(y) * src.height / height);
        const uint32_t y1 = std::max(y0 + 1, uint32_t((uint64_t(y + 1) * src.height + height - 1) / height));
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t x0 = uint32_t(uint64_t(x) * src.width / width);
            const uint32_t x1 = std::max(x0 + 1, uint32_t((uint64_t(x + 1) * src.width + width - 1) / width));
            uint64_t sum[4] = {0, 0, 0, 0}; uint32_t n = 0;
            for (uint32_t sy = y0; sy < std::min(y1, src.height); ++sy)
                for (uint32_t sx = x0; sx < std::min(x1, src.width); ++sx) {
                    const uint8_t* p = &src.pixels[(size_t(sy) * src.width + sx) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c]; ++n;
                }
            uint8_t* p = &out.pixels[(size_t(y) * width + x) * 4];
            for (int c = 0; c < 4; ++c) p[c] = uint8_t((sum[c] + n / 2) / n);
        }
    }
    return out;
}

static Image nextMip(const Image& src) {
    const uint32_t width = std::max(1u, src.width / 2), height = std::max(1u, src.height / 2);
    Image out; out.width = width; out.height = height; out.pixels.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const uint32_t x0 = std::min(2 * x, src.width - 1), x1 = std::min(2 * x + 1, src.width - 1);
        const uint32_t y0 = std::min(2 * y, src.height - 1), y1 = std::min(2 * y + 1, src.height - 1);
        const uint8_t* a = &src.pixels[(size_t(y0) * src.width + x0) * 4];
        const uint8_t* b = &src.pixels[(size_t(y0) * src.width + x1) * 4];
        const uint8_t* c = &src.pixels[(size_t(y1) * src.width + x0) * 4];
        const uint8_t* d = &src.pixels[(size_t(y1) * src.width + x1) * 4];
        uint8_t* o = &out.pixels[(size_t(y) * width + x) * 4];
        const uint32_t alpha = a[3] + b[3] + c[3] + d[3];
        for (int k = 0; k < 3; ++k) o[k] = uint8_t((uint32_t(a[k]) + b[k] + c[k] + d[k] + 2) / 4);
        o[3] = uint8_t((alpha + 2) / 4);
    }
    return out;
}

static uint32_t bcBlockSize(uint32_t codec) { return codec == TEXTURE_PACK_CODEC_BC1 ? 8 : 16; }

static std::vector<uint8_t> encodeBc(const Image& image, uint32_t codec) {
    const uint32_t bw = (image.width + 3) / 4, bh = (image.height + 3) / 4, blockSize = bcBlockSize(codec);
    std::vector<uint8_t> out(size_t(bw) * bh * blockSize);
    uint8_t block[64];
    for (uint32_t by = 0; by < bh; ++by) for (uint32_t bx = 0; bx < bw; ++bx) {
        for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t sx = std::min(bx * 4 + x, image.width - 1), sy = std::min(by * 4 + y, image.height - 1);
            std::copy_n(&image.pixels[(size_t(sy) * image.width + sx) * 4], 4, block + (y * 4 + x) * 4);
        }
        encodeBlock(int(codec), block, out.data() + (size_t(by) * bw + bx) * blockSize);
    }
    return out;
}

static bool parseName(const std::string& stem, SourceGroup* group, int* level, std::string* canonical, std::string* error) {
    if (stem.compare(0, 5, "tex1_") != 0) return fail(error, "malformed texture name: " + stem);
    std::string core = stem; bool arbitrary = false;
    if (core.size() >= 4 && core.compare(core.size() - 4, 4, "_arb") == 0) { arbitrary = true; core.resize(core.size() - 4); }
    *level = 0;
    const size_t mip = core.rfind("_mip");
    if (mip != std::string::npos && mip + 4 < core.size()) {
        const std::string digits = core.substr(mip + 4);
        if (digits.find_first_not_of("0123456789") == std::string::npos) { *level = std::stoi(digits); core.resize(mip); }
    }
    static const std::regex grammar("^tex1_([0-9]+)x([0-9]+)(_m)?_([0-9A-Fa-f]{16})(?:_([0-9A-Fa-f]{16}|\\$))?_([0-9]+)$");
    std::smatch m;
    if (!std::regex_match(core, m, grammar) || *level < 0) return fail(error, "malformed texture name: " + stem);
    uint64_t w = std::stoull(m[1].str()), h = std::stoull(m[2].str()), fmt = std::stoull(m[6].str());
    if (!w || !h || w > UINT32_MAX || h > UINT32_MAX || fmt > 14 || fmt == 7 || (fmt >= 11 && fmt <= 13))
        return fail(error, "unsupported source format or dimensions: " + stem);
    group->key.sourceWidth = uint32_t(w); group->key.sourceHeight = uint32_t(h); group->key.format = uint32_t(fmt);
    group->key.mipmapped = m[3].matched; group->key.dataHash = std::stoull(m[4].str(), nullptr, 16);
    group->key.wildcardTlut = m[5].matched && m[5].str() == "$";
    group->key.tlutHash = group->key.wildcardTlut || !m[5].matched ? 0 : std::stoull(m[5].str(), nullptr, 16);
    group->arbitrary = group->arbitrary || arbitrary;
    *canonical = core;
    return true;
}

static bool collect(const std::string& input, std::map<std::string, SourceGroup>* groups,
                    TexturePackConversionStats* stats, std::string* error) {
    std::error_code ec; std::filesystem::path root(input);
    if (!std::filesystem::is_directory(root, ec)) return fail(error, "input is not a directory: " + input);
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".png") continue;
        ++stats->files;
        SourceGroup parsed; int level = 0; std::string canonical;
        if (!parseName(it->path().stem().string(), &parsed, &level, &canonical, error)) return false;
        SourceGroup& g = (*groups)[canonical];
        if (!g.files.empty() && !(g.key == TexturePackKey())) {
            if (!(g.key == parsed.key)) return fail(error, "conflicting texture key: " + canonical);
        } else g.key = parsed.key;
        g.arbitrary = g.arbitrary || parsed.arbitrary;
        if (g.files.count(level)) return fail(error, "duplicate texture level: " + canonical);
        g.files[level] = SourceFile{it->path(), level};
    }
    if (ec) return fail(error, "cannot scan input directory: " + ec.message());
    if (groups->empty()) return fail(error, "input contains no texture PNGs");
    for (const auto& item : *groups) if (!item.second.files.count(0)) return fail(error, "missing level 0: " + item.first);
    return true;
}

static bool makeEntry(const SourceGroup& group, const TexturePackConversionOptions& options,
                      TexturePackEntry* out, TexturePackConversionStats* stats, std::string* error) {
    if (!options.maxSize || (options.bcCodec != TEXTURE_PACK_CODEC_BC1 && options.bcCodec != TEXTURE_PACK_CODEC_BC2 &&
                             options.bcCodec != TEXTURE_PACK_CODEC_BC3 && options.bcCodec != TEXTURE_PACK_CODEC_BC7))
        return fail(error, "invalid converter options");
    Image base;
    if (!decodePng(group.files.at(0).path, &base, error)) return false;
    if (std::max(base.width, base.height) > options.maxSize) {
        const uint32_t longest = std::max(base.width, base.height);
        const uint32_t w = std::max(1u, uint32_t((uint64_t(base.width) * options.maxSize + longest / 2) / longest));
        const uint32_t h = std::max(1u, uint32_t((uint64_t(base.height) * options.maxSize + longest / 2) / longest));
        base = scaleBox(base, w, h);
    }
    std::vector<Image> levels;
    levels.push_back(std::move(base));
    const uint32_t chain = [&] { uint32_t n = 1, longest = std::max(levels[0].width, levels[0].height); while (longest > 1) { longest >>= 1; ++n; } return n; }();
    for (uint32_t i = 1; i < chain; ++i) {
        auto explicitMip = group.files.find(int(i));
        if (explicitMip != group.files.end()) {
            Image supplied;
            if (!decodePng(explicitMip->second.path, &supplied, error)) return false;
            const uint32_t w = std::max(1u, levels[0].width >> i), h = std::max(1u, levels[0].height >> i);
            levels.push_back(scaleBox(supplied, w, h));
        } else levels.push_back(nextMip(levels.back()));
    }
    TexturePackEntry e; e.key = group.key; e.width = levels[0].width; e.height = levels[0].height;
    e.mipCount = uint32_t(levels.size()); e.rgbaCodec = TEXTURE_PACK_CODEC_RGBA8_LINEAR; e.bcCodec = options.bcCodec;
    e.surfaceFormat = 0x1a; e.surfacePitch = e.width * 4;
    for (const Image& image : levels) {
        TexturePackLevel l; l.width = image.width; l.height = image.height;
        l.rgbaPayload = image.pixels; l.bcPayload = encodeBc(image, options.bcCodec);
        stats->rgbaBytes += l.rgbaPayload.size(); stats->bcBytes += l.bcPayload.size(); ++stats->levels;
        e.levels.push_back(std::move(l));
    }
    *out = std::move(e);
    return true;
}

} // namespace

bool convertTexturePack(const std::string& inputDirectory, const std::string& outputArchive,
                       const TexturePackConversionOptions& options,
                       TexturePackConversionStats* stats, std::string* error) {
    TexturePackConversionStats local; if (!stats) stats = &local; *stats = TexturePackConversionStats();
    std::map<std::string, SourceGroup> groups;
    if (!collect(inputDirectory, &groups, stats, error)) return false;
    std::vector<TexturePackEntry> entries; entries.reserve(groups.size());
    for (const auto& item : groups) { TexturePackEntry e; if (!makeEntry(item.second, options, &e, stats, error)) return false; entries.push_back(std::move(e)); }
    stats->entries = uint32_t(entries.size());
    return TexturePackWriter::write(outputArchive, entries, options.maxSize, error);
}

} // namespace gx

#ifndef GX2_TEXTURE_PACK_NO_MAIN
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <GMSE01-png-dir> <output.gx2pack> [--max-size N] [--bc 1|2|3|7]\n", argv[0]);
        return 2;
    }
    gx::TexturePackConversionOptions options;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-size" && i + 1 < argc) options.maxSize = uint32_t(std::stoul(argv[++i]));
        else if (arg == "--bc" && i + 1 < argc) options.bcCodec = uint32_t(std::stoul(argv[++i]));
        else { std::fprintf(stderr, "unknown option: %s\n", arg.c_str()); return 2; }
    }
    gx::TexturePackConversionStats stats; std::string error;
    if (!gx::convertTexturePack(argv[1], argv[2], options, &stats, &error)) {
        std::fprintf(stderr, "gx2_texture_pack: %s\n", error.c_str()); return 1;
    }
    std::printf("converted %u files into %u entries, %u levels, %llu RGBA bytes, %llu BC bytes\n",
                stats.files, stats.entries, stats.levels, (unsigned long long)stats.rgbaBytes,
                (unsigned long long)stats.bcBytes);
    return 0;
}
#endif
