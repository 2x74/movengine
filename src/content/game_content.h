#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vpkpp {
class PackFile;
}

namespace content {

// a css install's files, mounted read only the way gmod mounts games: loose files (cstrike/custom/*, cstrike/download, cstrike) first, then the vpk archives (cstrike, then the hl2 base content css builds on)
class GameContent {
public:
    GameContent();
    ~GameContent();
    GameContent(const GameContent&) = delete;
    GameContent& operator=(const GameContent&) = delete;

    bool mount(const std::filesystem::path& cssRoot);
    // a texture pack instead of an install: a .zip made by exportTexturePack (or any zip/vpk laid out like the game folder)
    bool mountPack(const std::filesystem::path& pack);
    void unmount();
    bool mounted() const { return !packs_.empty(); }
    const std::filesystem::path& root() const { return root_; }  // empty for a pack
    const std::filesystem::path& pack() const { return pack_; }  // empty for an install

    // every material file (.vmt/.vtf under materials/) brush faces could use, as game relative paths, each once. downloaded server content (cstrike/download) is left out unless asked for
    std::vector<std::string> worldMaterialFiles(bool includeDownloads = false) const;

    // path relative to the game root, e.g. "materials/brick/brickwall001a.vmt". case insensitive (looked up lower case, like the engine)
    std::optional<std::vector<std::byte>> read(const std::string& path) const;

    // every material a brush face could use ("brick/brickwall001a"), sorted, skipping folders that only hold model/ui/sprite/particle materials
    std::vector<std::string> worldMaterialNames() const;

private:
    std::filesystem::path root_;
    std::filesystem::path pack_;
    std::vector<std::filesystem::path> looseDirs_;
    std::vector<std::unique_ptr<vpkpp::PackFile>> packs_;
};

// "Counter-Strike Source" folder that has cstrike/cstrike_pak_dir.vpk?
bool isCssInstall(const std::filesystem::path& root);

// the css folder to mount: the one saved in `configFile` if still valid, then the BHOP_CSS_PATH env var, then steam's own library records, then the usual default install locations
std::optional<std::filesystem::path> findCssInstall(const std::string& configFile);

// remembers (or with an empty path, forgets) a folder chosen by hand
bool saveCssPath(const std::string& configFile, const std::filesystem::path& root);

// the texture pack to use when there's no css: the one saved in `configFile`, else css_textures.zip in the app's folder
inline constexpr const char* kDefaultPackName = "css_textures.zip";
std::optional<std::filesystem::path> findTexturePack(const std::string& configFile, const std::filesystem::path& appDir);
bool saveTexturePackPath(const std::string& configFile, const std::filesystem::path& pack);

// remembers (or with an empty path, forgets) a source sdk bin folder holding vbsp/vvis/vrad, for the editor's compile. kept beside the content paths cause it's the same kind of "where did you put the game files" answer
bool saveCompileToolsPath(const std::string& configFile, const std::filesystem::path& binDir);
std::string loadCompileToolsPath(const std::string& configFile);

// any other remembered editor preference, in the same "key value" file. an unset key reads back as `fallback`
bool saveSetting(const std::string& configFile, const std::string& key, const std::string& value);
std::string loadSetting(const std::string& configFile, const std::string& key, const std::string& fallback = "");

// css if it's installed, else a texture pack, whichever was mounted is printed. false if neither (built-in textures only)
bool mountConfiguredContent(GameContent& content, const std::string& configFile, const std::filesystem::path& appDir);

// packs every world material of a css install into one .zip, for using its textures somewhere css isn't installed. runs on any thread (it opens the install itself), progress is readable while it runs
struct ExportProgress {
    std::atomic<size_t> done{0};
    std::atomic<size_t> total{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};
};
bool exportTexturePack(const std::filesystem::path& cssRoot, const std::string& zipPath, ExportProgress& progress);

}  // namespace content
