#include "content/materials.h"

#include <bsppp/PakLump.h>
#include <kvpp/KV1.h>
#include <vtfpp/VTF.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <algorithm>
#include <memory>

#include "content/builtin_textures.h"

namespace content {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "materials/foo/bar.vmt", "foo/bar" or "foo\bar.vtf" -> "foo/bar"
std::string normalizeMaterialPath(std::string path) {
    path = lower(std::move(path));
    for (char& c : path)
        if (c == '\\') c = '/';
    if (path.rfind("materials/", 0) == 0) path = path.substr(10);
    for (const char* ext : {".vmt", ".vtf"}) {
        if (path.size() > 4 && path.compare(path.size() - 4, 4, ext) == 0) path.resize(path.size() - 4);
    }
    return path;
}

struct VmtInfo {
    std::string shader;
    std::string baseTexture;  // empty if the material has none
    // $color and $color2 multiply the texture: maps often reuse one white
    // or grey texture in many colours this way.
    glm::vec3 tint{1.0f};
    bool alphaTest = false;
    float alphaTestRef = 0.5f;
    bool translucent = false;
};

// vmt booleans are "1"/"0" and the key is only ever present when it means something, so anything non zero counts
bool vmtTruthy(std::string_view v) {
    for (char c : v) {
        if (c != '0' && c != ' ' && c != '"' && c != '.') return true;
    }
    return false;
}

template <typename Node>
void readAlphaKeys(const Node& node, VmtInfo& info) {
    const auto& at = node("$alphatest");
    if (!at.isInvalid() && vmtTruthy(at.getValue())) info.alphaTest = true;
    const auto& ref = node("$alphatestreference");
    if (!ref.isInvalid()) {
        try {
            info.alphaTestRef = std::stof(std::string(ref.getValue()));
        } catch (const std::exception&) {
            // keep the default
        }
    }
    const auto& tr = node("$translucent");
    if (!tr.isInvalid() && vmtTruthy(tr.getValue())) info.translucent = true;
}

// "[1 0.5 0]" (0-1 floats) or "{255 128 0}" (0-255 ints); a bare "1 0 0"
// is read like the bracketed form.
std::optional<glm::vec3> parseVmtColor(std::string_view value) {
    std::string text(value);
    bool bytes = text.find('{') != std::string::npos;
    for (char& c : text) {
        if (c == '[' || c == ']' || c == '{' || c == '}') c = ' ';
    }
    glm::vec3 color;
    if (std::sscanf(text.c_str(), "%f %f %f", &color.r, &color.g, &color.b) != 3) {
        float grey;
        if (std::sscanf(text.c_str(), "%f", &grey) != 1) return std::nullopt;
        color = glm::vec3(grey);
    }
    if (bytes) color /= 255.0f;
    return glm::max(color, glm::vec3(0.0f));
}

template <typename Block>
void readTint(const Block& block, glm::vec3& tint) {
    for (const char* key : {"$color", "$color2"}) {
        const auto& v = block(key);
        if (!v.isInvalid()) {
            if (auto c = parseVmtColor(v.getValue())) tint *= *c;
        }
    }
}

// follows "patch" materials (include another .vmt, optionally replacing keys) up to a few levels deep
std::optional<VmtInfo> readVmt(const std::string& material, const FileReader& read, int depth = 0) {
    if (depth > 4) return std::nullopt;
    auto bytes = read("materials/" + normalizeMaterialPath(material) + ".vmt");
    if (!bytes) return std::nullopt;
    std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    kvpp::KV1<> vmt(text);
    if (vmt.getChildren().empty()) return std::nullopt;
    const auto& root = vmt[0];

    VmtInfo info;
    info.shader = lower(std::string(root.getKey()));
    if (info.shader == "patch") {
        // keys in the patch's own blocks win over the included material's
        bool hasColor = false;
        glm::vec3 patchTint(1.0f);
        for (const char* block : {"replace", "insert"}) {
            const auto& b = root(block);
            if (b.isInvalid()) continue;
            if (!b("$basetexture").isInvalid()) info.baseTexture = std::string(b("$basetexture").getValue());
            if (!b("$color").isInvalid() || !b("$color2").isInvalid()) {
                hasColor = true;
                readTint(b, patchTint);
            }
            readAlphaKeys(b, info);
        }
        const auto& include = root("include");
        if (!include.isInvalid()) {
            if (auto base = readVmt(std::string(include.getValue()), read, depth + 1)) {
                if (info.baseTexture.empty()) info.baseTexture = base->baseTexture;
                info.shader = base->shader;
                info.tint = base->tint;
                // the patch's own blocks were read first and win
                info.alphaTest = info.alphaTest || base->alphaTest;
                info.translucent = info.translucent || base->translucent;
                if (info.alphaTestRef == 0.5f) info.alphaTestRef = base->alphaTestRef;
            }
        }
        if (hasColor) info.tint = patchTint;
        return info;
    }
    const auto& base = root("$basetexture");
    if (!base.isInvalid()) info.baseTexture = std::string(base.getValue());
    readTint(root, info.tint);
    readAlphaKeys(root, info);
    return info;
}

std::optional<ResolvedMaterial> decodeVtf(std::vector<std::byte> bytes) {
    vtfpp::VTF vtf(std::move(bytes));
    if (!vtf) return std::nullopt;
    ResolvedMaterial out;
    out.pixels.width = vtf.getWidth();
    out.pixels.height = vtf.getHeight();
    auto rgba = vtf.getImageDataAsRGBA8888();
    if (rgba.empty()) return std::nullopt;
    out.pixels.rgba8888.resize(rgba.size());
    std::memcpy(out.pixels.rgba8888.data(), rgba.data(), rgba.size());
    out.uvSize = glm::vec2(static_cast<float>(out.pixels.width), static_cast<float>(out.pixels.height));
    return out;
}

ResolvedMaterial flatColor(glm::vec3 c, ResolvedMaterial::Source source) {
    ResolvedMaterial out;
    out.pixels.width = out.pixels.height = 1;
    out.pixels.rgba8888 = {static_cast<uint8_t>(c.r * 255), static_cast<uint8_t>(c.g * 255),
                           static_cast<uint8_t>(c.b * 255), 255};
    out.uvSize = glm::vec2(512.0f);
    out.source = source;
    return out;
}

}  // namespace

ResolvedMaterial resolveMaterial(const std::string& material, const GameContent* game, const FileReader& mapPak) {
    if (!material.empty()) {
        // a map's material can point at a stock texture and vice versa so the .vmt and the .vtf are each looked up through the whole chain
        auto read = [&](const std::string& path) -> std::optional<std::vector<std::byte>> {
            if (mapPak) {
                if (auto data = mapPak(path)) return data;
            }
            if (game && game->mounted()) return game->read(path);
            return std::nullopt;
        };
        bool vmtInMap = mapPak && mapPak("materials/" + normalizeMaterialPath(material) + ".vmt").has_value();
        if (auto vmt = readVmt(material, read)) {
            auto source = vmtInMap ? ResolvedMaterial::Source::Map : ResolvedMaterial::Source::Game;
            if (!vmt->baseTexture.empty()) {
                if (auto vtf = read("materials/" + normalizeMaterialPath(vmt->baseTexture) + ".vtf")) {
                    if (auto decoded = decodeVtf(std::move(*vtf))) {
                        decoded->source = source;
                        decoded->alphaTest = vmt->alphaTest;
                        decoded->alphaTestRef = vmt->alphaTestRef;
                        decoded->translucent = vmt->translucent;
                        if (vmt->tint != glm::vec3(1.0f)) {
                            auto& px = decoded->pixels.rgba8888;
                            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                                for (int c = 0; c < 3; ++c) {
                                    float v = static_cast<float>(px[i + c]) * vmt->tint[c];
                                    px[i + c] = static_cast<uint8_t>(std::min(v, 255.0f));
                                }
                            }
                        }
                        return *decoded;
                    }
                }
            } else if (vmt->shader == "water") {
                // water has no base texture to show, it's not missing either
                return flatColor({0.18f, 0.34f, 0.45f}, source);
            }
        }
        if (auto builtin = builtinTexture(material)) {
            ResolvedMaterial out;
            out.pixels = std::move(builtin->pixels);
            out.uvSize = builtin->uvSize;
            out.source = ResolvedMaterial::Source::Builtin;
            return out;
        }
    }
    BuiltinTexture mt = missingTexture();
    ResolvedMaterial out;
    out.pixels = std::move(mt.pixels);
    out.uvSize = mt.uvSize;
    out.source = ResolvedMaterial::Source::Missing;
    return out;
}

FileReader bspPakReader(const std::string& bspPath) {
    std::shared_ptr<vpkpp::PackFile> pack(bsppp::PakLump::open(bspPath));
    if (!pack) return nullptr;
    return [pack](const std::string& path) -> std::optional<std::vector<std::byte>> {
        std::string p = lower(path);
        return pack->readEntry(p);
    };
}

}  // namespace content
