#include "content/builtin_textures.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>

namespace content {

namespace {

std::string upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

uint32_t hash2(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float rand01(int x, int y, uint32_t seed) {
    return (hash2(x, y, seed) & 0xffffff) / 16777215.0f;
}

// value noise on a lattice that wraps every `period` cells so the texture tiles
float tileNoise(float x, float y, int period, uint32_t seed) {
    int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    float fx = x - x0, fy = y - y0;
    auto at = [&](int ix, int iy) {
        ix = ((ix % period) + period) % period;
        iy = ((iy % period) + period) % period;
        return rand01(ix, iy, seed);
    };
    float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * sx;
    float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * sx;
    return a + (b - a) * sy;
}

// fractal noise over a size x size texture, each octave's lattice divides the texture evenly so the result still tiles
float fbm(int px, int py, int size, int baseCells, int octaves, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    int cells = baseCells;
    for (int o = 0; o < octaves; ++o) {
        float scale = static_cast<float>(cells) / size;
        sum += amp * tileNoise(px * scale, py * scale, cells, seed + o * 101u);
        norm += amp;
        amp *= 0.5f;
        cells *= 2;
    }
    return sum / norm;
}

bsp::DecodedTexture makeTexture(int size, const std::function<glm::vec3(int, int)>& shade) {
    bsp::DecodedTexture t;
    t.width = t.height = size;
    t.rgba8888.resize(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            glm::vec3 c = glm::clamp(shade(x, y), 0.0f, 1.0f);
            uint8_t* p = &t.rgba8888[(static_cast<size_t>(y) * size + x) * 4];
            p[0] = static_cast<uint8_t>(std::round(c.r * 255.0f));
            p[1] = static_cast<uint8_t>(std::round(c.g * 255.0f));
            p[2] = static_cast<uint8_t>(std::round(c.b * 255.0f));
            p[3] = 255;
        }
    }
    return t;
}

// hammer dev texture style: flat color, bold line every 128 units, fine line every 32. 128px standing in for a 512 texel texture at 0.25 scale
BuiltinTexture devGrid(glm::vec3 base) {
    BuiltinTexture b;
    b.pixels = makeTexture(128, [&](int x, int y) {
        float shade = 1.0f;
        if (x < 2 || y < 2) shade = 0.68f;
        else if (x % 32 == 0 || y % 32 == 0) shade = 0.86f;
        return base * shade;
    });
    b.uvSize = glm::vec2(512.0f);
    return b;
}

BuiltinTexture grass() {
    BuiltinTexture b;
    b.pixels = makeTexture(256, [](int x, int y) {
        float n = fbm(x, y, 256, 8, 5, 11);
        float blades = rand01(x, y, 12);  // per-pixel speckle reads as blades up close
        glm::vec3 dark(0.18f, 0.36f, 0.12f), light(0.42f, 0.62f, 0.22f);
        glm::vec3 c = dark + (light - dark) * n;
        if (blades > 0.93f) c *= 1.25f;
        if (blades < 0.05f) c *= 0.75f;
        return c;
    });
    b.uvSize = glm::vec2(256.0f);
    return b;
}

BuiltinTexture sand() {
    BuiltinTexture b;
    b.pixels = makeTexture(256, [](int x, int y) {
        float n = fbm(x, y, 256, 4, 4, 21);
        float ripple = 0.5f + 0.5f * std::sin((y + 18.0f * fbm(x, y, 256, 4, 2, 22)) * 2.0f * 3.14159265f / 32.0f);
        float grain = rand01(x, y, 23);
        glm::vec3 c = glm::vec3(0.80f, 0.70f, 0.48f) * (0.88f + 0.12f * n) * (0.94f + 0.06f * ripple);
        return c * (0.94f + 0.08f * grain);
    });
    b.uvSize = glm::vec2(256.0f);
    return b;
}

BuiltinTexture brick() {
    constexpr int kRow = 32, kBrick = 64, kMortar = 3;
    BuiltinTexture b;
    b.pixels = makeTexture(256, [&](int x, int y) {
        int row = y / kRow;
        int shifted = x + (row % 2) * (kBrick / 2);  // running bond
        int col = (shifted / kBrick) % (256 / kBrick);
        int bx = shifted % kBrick, by = y % kRow;
        float n = fbm(x, y, 256, 16, 3, 31);
        if (bx < kMortar || by < kMortar) {
            return glm::vec3(0.62f, 0.60f, 0.56f) * (0.85f + 0.2f * n);
        }
        float v = rand01(col, row, 32);  // each brick its own shade
        glm::vec3 base = glm::vec3(0.55f, 0.24f, 0.17f) * (0.8f + 0.35f * v);
        return base * (0.85f + 0.25f * n);
    });
    b.uvSize = glm::vec2(256.0f);
    return b;
}

BuiltinTexture concrete() {
    BuiltinTexture b;
    b.pixels = makeTexture(256, [](int x, int y) {
        float n = fbm(x, y, 256, 8, 5, 41);
        float pits = rand01(x, y, 42) > 0.985f ? 0.8f : 1.0f;
        return glm::vec3(0.55f, 0.55f, 0.53f) * (0.82f + 0.3f * n) * pits;
    });
    b.uvSize = glm::vec2(256.0f);
    return b;
}

BuiltinTexture wood() {
    constexpr int kPlank = 32;
    BuiltinTexture b;
    b.pixels = makeTexture(256, [&](int x, int y) {
        int plank = y / kPlank;
        int end = static_cast<int>(rand01(plank, 0, 51) * 256);  // where this plank's board ends
        if (y % kPlank < 2 || ((x - end + 256) % 256) < 2) return glm::vec3(0.22f, 0.14f, 0.08f);
        float grain = 0.5f + 0.5f * std::sin((y + 6.0f * fbm(x, y, 256, 4, 3, 52 + plank)) * 1.3f);
        float tone = rand01(plank, 1, 53);
        glm::vec3 c = glm::vec3(0.58f, 0.40f, 0.24f) * (0.85f + 0.25f * tone);
        return c * (0.85f + 0.15f * grain);
    });
    b.uvSize = glm::vec2(256.0f);
    return b;
}

// 5x7 glyphs: "MT" for the missing texture, the rest to spell CLIP and
// TRIGGER across the tool textures those brushes are drawn with.
using Glyph = std::array<const char*, 7>;
constexpr Glyph kGlyphM = {"10001", "11011", "10101", "10101", "10001", "10001", "10001"};
constexpr Glyph kGlyphT = {"11111", "00100", "00100", "00100", "00100", "00100", "00100"};
constexpr Glyph kGlyphC = {"01110", "10001", "10000", "10000", "10000", "10001", "01110"};
constexpr Glyph kGlyphL = {"10000", "10000", "10000", "10000", "10000", "10000", "11111"};
constexpr Glyph kGlyphI = {"11111", "00100", "00100", "00100", "00100", "00100", "11111"};
constexpr Glyph kGlyphP = {"11110", "10001", "10001", "11110", "10000", "10000", "10000"};
constexpr Glyph kGlyphR = {"11110", "10001", "10001", "11110", "10100", "10010", "10001"};
constexpr Glyph kGlyphG = {"01110", "10001", "10000", "10111", "10001", "10001", "01111"};
constexpr Glyph kGlyphE = {"11111", "10000", "10000", "11110", "10000", "10000", "11111"};

const Glyph* glyphFor(char c) {
    switch (c) {
        case 'M': return &kGlyphM;
        case 'T': return &kGlyphT;
        case 'C': return &kGlyphC;
        case 'L': return &kGlyphL;
        case 'I': return &kGlyphI;
        case 'P': return &kGlyphP;
        case 'R': return &kGlyphR;
        case 'G': return &kGlyphG;
        case 'E': return &kGlyphE;
        default: return nullptr;
    }
}

// true when (x, y) lands on a lit pixel of `text` drawn centred in a `size` x `size` texture at `scale` pixels per glyph pixel
bool textAt(int x, int y, int size, const char* text, int scale) {
    const int glyphW = 5 * scale, glyphH = 7 * scale, gap = scale;
    int n = 0;
    for (const char* c = text; *c; ++c) ++n;
    if (n == 0) return false;
    const int totalW = n * glyphW + (n - 1) * gap;
    const int left = (size - totalW) / 2, top = (size - glyphH) / 2;
    int gy = y - top;
    if (gy < 0 || gy >= glyphH) return false;
    int gx = x - left;
    if (gx < 0) return false;
    const int cell = glyphW + gap;
    const int index = gx / cell;
    if (index >= n) return false;
    const int inGlyph = gx - index * cell;
    if (inGlyph >= glyphW) return false;  // the gap between letters
    const Glyph* g = glyphFor(text[index]);
    return g && (*g)[gy / scale][inGlyph / scale] == '1';
}

// source's tool textures: a flat colour with the brush's purpose written across it so showclips/showtriggers tells you what you're looking at instead of a wash of untextured colour. same material names css uses so a css install overrides them with the real ones
BuiltinTexture toolTexture(const glm::vec3& base, const char* label) {
    constexpr int kSize = 256, kScale = 5;
    BuiltinTexture b;
    b.pixels = makeTexture(kSize, [&](int x, int y) {
        // a border and a centre cross so one big brush face still reads as a brush and not a flat field of colour
        const bool edge = x < 3 || y < 3 || x >= kSize - 3 || y >= kSize - 3;
        glm::vec3 c = edge ? base * 1.45f : base;
        if (textAt(x, y, kSize, label, kScale)) c = glm::vec3(1.0f, 1.0f, 1.0f);
        return c;
    });
    b.uvSize = glm::vec2(256.0f);
    return b;
}

BuiltinTexture missing() {
    constexpr int kSize = 128, kScale = 6;
    BuiltinTexture b;
    b.pixels = makeTexture(kSize, [&](int x, int y) {
        if (textAt(x, y, kSize, "MT", kScale)) {
            return glm::vec3(1.0f, 0.82f, 0.15f);
        }
        float shade = 1.0f;
        if (x < 2 || y < 2) shade = 0.6f;
        else if (x % 32 == 0 || y % 32 == 0) shade = 0.8f;
        return glm::vec3(0.30f, 0.30f, 0.33f) * shade;
    });
    b.uvSize = glm::vec2(512.0f);
    return b;
}

struct Entry {
    const char* name;
    std::function<BuiltinTexture()> make;
};

const std::vector<Entry>& entries() {
    static const std::vector<Entry> kEntries = {
        {"TOOLS/TOOLSCLIP", [] { return toolTexture({0.52f, 0.20f, 0.20f}, "CLIP"); }},
        {"TOOLS/TOOLSPLAYERCLIP", [] { return toolTexture({0.52f, 0.26f, 0.14f}, "CLIP"); }},
        {"TOOLS/TOOLSTRIGGER", [] { return toolTexture({0.55f, 0.16f, 0.42f}, "TRIGGER"); }},
        {"DEV/DEV_MEASUREGENERIC01B", [] { return devGrid({0.62f, 0.63f, 0.68f}); }},
        {"DEV/DEV_MEASUREGENERIC01", [] { return devGrid({0.90f, 0.58f, 0.28f}); }},
        {"TOOLS/TOOLSNODRAW", [] { return devGrid({0.95f, 0.85f, 0.2f}); }},
        {"TOOLS/TOOLSCLIP", [] { return devGrid({0.65f, 0.35f, 0.9f}); }},
        {"TOOLS/TOOLSPLAYERCLIP", [] { return devGrid({0.85f, 0.3f, 0.75f}); }},
        {"TOOLS/TOOLSTRIGGER", [] { return devGrid({1.0f, 0.55f, 0.1f}); }},
        {"TOOLS/TOOLSSKYBOX", [] { return devGrid({0.45f, 0.7f, 1.0f}); }},
        {"TOOLS/TOOLSSKIP", [] { return devGrid({0.8f, 0.8f, 0.8f}); }},
        {"TOOLS/TOOLSHINT", [] { return devGrid({0.95f, 0.95f, 0.95f}); }},
        {"BHOP/GRASS01", grass},
        {"BHOP/SAND01", sand},
        {"BHOP/BRICK01", brick},
        {"BHOP/CONCRETE01", concrete},
        {"BHOP/WOOD01", wood},
    };
    return kEntries;
}

}  // namespace

const std::vector<std::string>& builtinMaterialNames() {
    static const std::vector<std::string> kNames = [] {
        std::vector<std::string> names;
        for (const auto& e : entries()) names.emplace_back(e.name);
        return names;
    }();
    return kNames;
}

std::optional<BuiltinTexture> builtinTexture(const std::string& material) {
    std::string m = upper(material);
    for (const auto& e : entries()) {
        if (m == e.name) return e.make();
    }
    return std::nullopt;
}

BuiltinTexture missingTexture() {
    return missing();
}

}  // namespace content
