// Round-trip tests for the hand-rolled OpenEXR writer and the tonemap helpers.
//
// writeEXR emits exactly one format variant (single-part scanline, FLOAT,
// NO_COMPRESSION), and the parser below re-reads it from the raw bytes,
// written independently against the OpenEXR file layout spec. The writer is
// trusted because this parser disagrees with it whenever either is wrong —
// not because a library vouches for it.
#include <Vapor/image_io.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

// ── Minimal independent EXR parser (test-only) ─────────────────────────────

struct ParsedEXR {
    Uint32 version = 0;
    std::map<std::string, std::pair<std::string, std::vector<unsigned char>>> attributes;
    std::vector<Uint64> lineOffsets;
    // pixels[y][channel][x], channels in file order (B, G, R)
    std::vector<std::vector<std::vector<float>>> scanlines;
    Uint32 width = 0;
    Uint32 height = 0;
    bool valid = false;
};

Uint32 read32(const unsigned char* p) {
    return Uint32(p[0]) | Uint32(p[1]) << 8 | Uint32(p[2]) << 16 | Uint32(p[3]) << 24;
}

Uint64 read64(const unsigned char* p) {
    Uint64 v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

float readFloat(const unsigned char* p) {
    Uint32 bits = read32(p);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

ParsedEXR parseEXR(const std::string& path) {
    ParsedEXR out;
    std::ifstream file(path, std::ios::binary);
    if (!file) return out;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    if (data.size() < 8) return out;

    const unsigned char magic[4] = { 0x76, 0x2F, 0x31, 0x01 };
    if (std::memcmp(data.data(), magic, 4) != 0) return out;
    out.version = read32(data.data() + 4);

    // Attributes: name\0 type\0 int32 size, payload — until a lone \0.
    size_t pos = 8;
    while (pos < data.size() && data[pos] != 0) {
        std::string name(reinterpret_cast<const char*>(data.data() + pos));
        pos += name.size() + 1;
        std::string type(reinterpret_cast<const char*>(data.data() + pos));
        pos += type.size() + 1;
        Uint32 size = read32(data.data() + pos);
        pos += 4;
        std::vector<unsigned char> payload(data.begin() + pos, data.begin() + pos + size);
        pos += size;
        out.attributes[name] = { type, std::move(payload) };
    }
    pos += 1;  // header terminator

    auto dw = out.attributes.find("dataWindow");
    if (dw == out.attributes.end() || dw->second.second.size() != 16) return out;
    const unsigned char* box = dw->second.second.data();
    out.width = read32(box + 8) - read32(box + 0) + 1;
    out.height = read32(box + 12) - read32(box + 4) + 1;

    for (Uint32 y = 0; y < out.height; y++) {
        out.lineOffsets.push_back(read64(data.data() + pos));
        pos += 8;
    }

    // Scanline blocks, addressed through the offset table (not sequentially —
    // that is the point: a wrong table must fail the test).
    for (Uint32 y = 0; y < out.height; y++) {
        size_t block = size_t(out.lineOffsets[y]);
        if (block + 8 > data.size()) return out;
        Uint32 blockY = read32(data.data() + block);
        Uint32 payloadSize = read32(data.data() + block + 4);
        if (blockY != y) return out;
        if (payloadSize != out.width * 3 * 4) return out;
        if (block + 8 + payloadSize > data.size()) return out;

        std::vector<std::vector<float>> channels(3, std::vector<float>(out.width));
        const unsigned char* p = data.data() + block + 8;
        for (int c = 0; c < 3; c++) {
            for (Uint32 x = 0; x < out.width; x++) {
                channels[c][x] = readFloat(p);
                p += 4;
            }
        }
        out.scanlines.push_back(std::move(channels));
    }

    out.valid = true;
    return out;
}

std::string tempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

} // namespace

// ============================================================
// EXR round-trip
// ============================================================

TEST_CASE("writeEXR: 輸出可被獨立 parser 重讀且像素 bit-exact", "[imageio]") {
    // 非正方形 + 含負值/大值/精確小數 —— float32 全精度必須保留
    const Uint32 w = 5, h = 3;
    std::vector<float> rgb(w * h * 3);
    for (Uint32 i = 0; i < w * h; i++) {
        rgb[i * 3 + 0] = float(i) * 1.25f;
        rgb[i * 3 + 1] = 1000.0f + float(i) * 0.001f;
        rgb[i * 3 + 2] = -0.5f + float(i);
    }
    const std::string path = tempPath("vapor_test_roundtrip.exr");
    REQUIRE(Vapor::writeEXR(path, rgb.data(), w, h));

    ParsedEXR exr = parseEXR(path);
    REQUIRE(exr.valid);
    REQUIRE(exr.version == 2u);
    REQUIRE(exr.width == w);
    REQUIRE(exr.height == h);

    for (Uint32 y = 0; y < h; y++) {
        for (Uint32 x = 0; x < w; x++) {
            const float* px = &rgb[(y * w + x) * 3];
            // 檔內 channel 順序是 B, G, R
            REQUIRE(exr.scanlines[y][0][x] == px[2]);
            REQUIRE(exr.scanlines[y][1][x] == px[1]);
            REQUIRE(exr.scanlines[y][2][x] == px[0]);
        }
    }
    std::filesystem::remove(path);
}

TEST_CASE("writeEXR: header 屬性完整且符合規範", "[imageio]") {
    std::vector<float> rgb(4 * 2 * 3, 0.5f);
    const std::string path = tempPath("vapor_test_header.exr");
    REQUIRE(Vapor::writeEXR(path, rgb.data(), 4, 2));

    ParsedEXR exr = parseEXR(path);
    REQUIRE(exr.valid);

    // OpenEXR 規範要求的必要屬性
    for (const char* required : { "channels", "compression", "dataWindow", "displayWindow",
                                  "lineOrder", "pixelAspectRatio", "screenWindowCenter",
                                  "screenWindowWidth" }) {
        INFO("attribute: " << required);
        REQUIRE(exr.attributes.count(required) == 1);
    }

    REQUIRE(exr.attributes["compression"].second.at(0) == 0);  // NO_COMPRESSION
    REQUIRE(exr.attributes["lineOrder"].second.at(0) == 0);    // INCREASING_Y

    // chlist：字母序 B, G, R，各為 FLOAT (pixelType 2)、sampling 1x1
    const auto& chlist = exr.attributes["channels"].second;
    size_t p = 0;
    for (const char* expected : { "B", "G", "R" }) {
        std::string name(reinterpret_cast<const char*>(chlist.data() + p));
        REQUIRE(name == expected);
        p += name.size() + 1;
        REQUIRE(read32(chlist.data() + p) == 2u);  // FLOAT
        p += 4;
        p += 4;  // pLinear + reserved
        REQUIRE(read32(chlist.data() + p) == 1u);  // xSampling
        p += 4;
        REQUIRE(read32(chlist.data() + p) == 1u);  // ySampling
        p += 4;
    }
    REQUIRE(chlist.at(p) == 0);  // list terminator

    std::filesystem::remove(path);
}

TEST_CASE("writeEXR: 拒絕空輸入", "[imageio]") {
    std::vector<float> rgb(3, 0.0f);
    REQUIRE_FALSE(Vapor::writeEXR(tempPath("vapor_test_bad.exr"), nullptr, 1, 1));
    REQUIRE_FALSE(Vapor::writeEXR(tempPath("vapor_test_bad.exr"), rgb.data(), 0, 1));
    REQUIRE_FALSE(Vapor::writeEXR(tempPath("vapor_test_bad.exr"), rgb.data(), 1, 0));
}

// ============================================================
// PNG 預覽與 tonemap
// ============================================================

TEST_CASE("writeTonemappedPNG: 產出合法 PNG 檔頭", "[imageio]") {
    std::vector<float> rgb(8 * 8 * 3);
    for (size_t i = 0; i < rgb.size(); i++) rgb[i] = float(i) / rgb.size() * 4.0f;

    const std::string path = tempPath("vapor_test_preview.png");
    REQUIRE(Vapor::writeTonemappedPNG(path, rgb.data(), 8, 8));

    std::ifstream file(path, std::ios::binary);
    unsigned char sig[8] = {};
    file.read(reinterpret_cast<char*>(sig), 8);
    const unsigned char pngMagic[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    REQUIRE(std::memcmp(sig, pngMagic, 8) == 0);
    std::filesystem::remove(path);
}

TEST_CASE("Tonemap helpers: 單調、有界、端點正確", "[imageio]") {
    REQUIRE(Vapor::acesTonemap(0.0f) == 0.0f);
    REQUIRE(Vapor::linearToSRGB(0.0f) == 0.0f);
    REQUIRE(std::abs(Vapor::linearToSRGB(1.0f) - 1.0f) < 1e-5f);

    float prevAces = -1.0f, prevSrgb = -1.0f;
    for (int i = 0; i <= 1000; i++) {
        float x = float(i) / 100.0f;  // 0..10（HDR 範圍）
        float a = Vapor::acesTonemap(x);
        REQUIRE(a >= prevAces);  // 單調不減
        REQUIRE(a >= 0.0f);
        REQUIRE(a <= 1.0f);
        prevAces = a;
        if (x <= 1.0f) {
            float s = Vapor::linearToSRGB(x);
            REQUIRE(s >= prevSrgb);
            prevSrgb = s;
        }
    }
    // 亮部要壓縮但不能死黑：中灰以上仍有層次
    REQUIRE(Vapor::acesTonemap(1.0f) > 0.7f);
    REQUIRE(Vapor::acesTonemap(10.0f) > Vapor::acesTonemap(1.0f));
}
