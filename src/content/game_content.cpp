#include "content/game_content.h"

#include <steampp/steampp.h>
#include <vpkpp/vpkpp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

#include "content/zip_writer.h"

namespace content {

namespace {

constexpr steampp::AppID kCssAppId = 240;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<std::vector<std::byte>> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < 0) return std::nullopt;
    file.seekg(0, std::ios::beg);
    std::vector<std::byte> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return file ? std::optional(std::move(data)) : std::nullopt;
}

bool isWorldMaterial(const std::string& name) {
    static constexpr std::array<const char*, 12> kSkip = {
        "models/", "vgui/", "sprites/", "particle/", "particles/", "effects/",
        "console/", "hud/", "debug/", "engine/", "editor/", "voice/",
    };
    for (const char* prefix : kSkip) {
        if (name.rfind(prefix, 0) == 0) return false;
    }
    return true;
}

}  // namespace

GameContent::GameContent() = default;
GameContent::~GameContent() = default;

bool isCssInstall(const std::filesystem::path& root) {
    std::error_code ec;
    return !root.empty() && std::filesystem::exists(root / "cstrike" / "cstrike_pak_dir.vpk", ec);
}

bool GameContent::mount(const std::filesystem::path& cssRoot) {
    unmount();
    if (!isCssInstall(cssRoot)) return false;
    root_ = cssRoot;

    std::error_code ec;
    std::filesystem::path cstrike = cssRoot / "cstrike";
    if (std::filesystem::is_directory(cstrike / "custom", ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(cstrike / "custom", ec)) {
            std::error_code entryError;
            if (entry.is_directory(entryError)) looseDirs_.push_back(entry.path());
        }
    }
    looseDirs_.push_back(cstrike / "download");  // custom content picked up from servers
    looseDirs_.push_back(cstrike);

    for (const char* vpk : {"cstrike/cstrike_pak_dir.vpk", "hl2/hl2_textures_dir.vpk", "hl2/hl2_misc_dir.vpk",
                            "platform/platform_misc_dir.vpk"}) {
        std::filesystem::path path = cssRoot / vpk;
        if (!std::filesystem::exists(path, ec)) continue;
        if (auto pack = vpkpp::PackFile::open(path.string())) {
            packs_.push_back(std::move(pack));
        } else {
            std::fprintf(stderr, "content: couldn't open %s\n", path.string().c_str());
        }
    }
    std::printf("content: mounted CS:S at %s (%zu archives)\n", cssRoot.string().c_str(), packs_.size());
    return mounted();
}

bool GameContent::mountPack(const std::filesystem::path& pack) {
    unmount();
    auto opened = vpkpp::PackFile::open(pack.string());
    if (!opened) {
        std::fprintf(stderr, "content: couldn't open texture pack %s\n", pack.string().c_str());
        return false;
    }
    packs_.push_back(std::move(opened));
    pack_ = pack;
    std::printf("content: mounted texture pack %s\n", pack.string().c_str());
    return true;
}

void GameContent::unmount() {
    packs_.clear();
    looseDirs_.clear();
    root_.clear();
    pack_.clear();
}

std::vector<std::string> GameContent::worldMaterialFiles(bool includeDownloads) const {
    std::set<std::string> files;
    auto consider = [&](std::string path) {
        path = lower(path);
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.rfind("materials/", 0) != 0 || path.size() < 15) return;
        std::string ext = path.substr(path.size() - 4);
        if (ext != ".vmt" && ext != ".vtf") return;
        if (isWorldMaterial(path.substr(10))) files.insert(path);
    };
    for (const auto& pack : packs_) {
        pack->runForAllEntries([&](const std::string& path, const vpkpp::Entry&) { consider(path); });
    }
    std::error_code ec;
    for (const auto& dir : looseDirs_) {
        if (!includeDownloads && dir.filename() == "download") continue;
        std::filesystem::path materials = dir / "materials";
        if (!std::filesystem::is_directory(materials, ec)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 materials, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            std::error_code entryError;
            if (it->is_regular_file(entryError)) consider(std::filesystem::relative(it->path(), dir, ec).generic_string());
        }
    }
    return {files.begin(), files.end()};
}

std::optional<std::vector<std::byte>> GameContent::read(const std::string& pathIn) const {
    std::string path = lower(pathIn);
    std::replace(path.begin(), path.end(), '\\', '/');
    for (const auto& dir : looseDirs_) {
        if (auto data = readFile(dir / path)) return data;
    }
    for (const auto& pack : packs_) {
        if (auto data = pack->readEntry(path)) return data;
    }
    return std::nullopt;
}

std::vector<std::string> GameContent::worldMaterialNames() const {
    std::set<std::string> names;
    auto consider = [&](std::string path) {
        path = lower(path);
        if (path.rfind("materials/", 0) != 0 || path.size() < 15 || path.compare(path.size() - 4, 4, ".vmt") != 0) {
            return;
        }
        std::string name = path.substr(10, path.size() - 14);
        if (isWorldMaterial(name)) names.insert(name);
    };
    for (const auto& pack : packs_) {
        pack->runForAllEntries([&](const std::string& path, const vpkpp::Entry&) { consider(path); });
    }
    std::error_code ec;
    for (const auto& dir : looseDirs_) {
        std::filesystem::path materials = dir / "materials";
        if (!std::filesystem::is_directory(materials, ec)) continue;
        // downloaded content can be anything, symlink loops included: an entry that can't be looked at is skipped, never thrown
        for (auto it = std::filesystem::recursive_directory_iterator(
                 materials, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            std::error_code entryError;
            if (it->is_regular_file(entryError)) {
                consider(std::filesystem::relative(it->path(), dir, ec).generic_string());
            }
        }
    }
    return {names.begin(), names.end()};
}

std::optional<std::filesystem::path> findCssInstall(const std::string& configFile) {
    if (std::ifstream file(configFile); file) {
        std::string line;
        while (std::getline(file, line)) {
            if (line.rfind("css_path ", 0) == 0) {
                std::filesystem::path saved = line.substr(9);
                if (isCssInstall(saved)) return saved;
            }
        }
    }
    if (const char* env = std::getenv("BHOP_CSS_PATH"); env && isCssInstall(env)) {
        return std::filesystem::path(env);
    }
    {
        steampp::Steam steam;
        if (steam && steam.isAppInstalled(kCssAppId)) {
            std::filesystem::path dir = steam.getAppInstallDir(kCssAppId);
            if (isCssInstall(dir)) return dir;
        }
    }
    std::vector<std::filesystem::path> guesses;
    if (const char* home = std::getenv("HOME")) {
        guesses.push_back(std::filesystem::path(home) / ".local/share/Steam/steamapps/common/Counter-Strike Source");
        guesses.push_back(std::filesystem::path(home) / ".steam/steam/steamapps/common/Counter-Strike Source");
        guesses.push_back(std::filesystem::path(home) /
                          ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Counter-Strike Source");
    }
    guesses.emplace_back("C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Source");
    guesses.emplace_back("C:/Program Files/Steam/steamapps/common/Counter-Strike Source");
    for (const auto& g : guesses) {
        if (isCssInstall(g)) return g;
    }
    return std::nullopt;
}

namespace {

// content.cfg holds "css_path <folder>" and "texture_pack <file>" lines;
// setting one keeps the other.
std::string readConfigValue(const std::string& configFile, const std::string& key) {
    std::ifstream file(configFile);
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind(key + " ", 0) == 0) return line.substr(key.size() + 1);
    }
    return "";
}

bool writeConfigValue(const std::string& configFile, const std::string& key, const std::string& value) {
    std::vector<std::string> kept;
    {
        std::ifstream file(configFile);
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.rfind(key + " ", 0) != 0) kept.push_back(line);
        }
    }
    std::ofstream file(configFile, std::ios::trunc);
    if (!file) return false;
    for (const auto& line : kept) file << line << '\n';
    if (!value.empty()) file << key << ' ' << value << '\n';
    return true;
}

}  // namespace

bool saveCompileToolsPath(const std::string& configFile, const std::filesystem::path& binDir) {
    return writeConfigValue(configFile, "sdk_bin", binDir.string());
}

std::string loadCompileToolsPath(const std::string& configFile) {
    return readConfigValue(configFile, "sdk_bin");
}

bool saveSetting(const std::string& configFile, const std::string& key, const std::string& value) {
    return writeConfigValue(configFile, key, value);
}

std::string loadSetting(const std::string& configFile, const std::string& key, const std::string& fallback) {
    std::string value = readConfigValue(configFile, key);
    return value.empty() ? fallback : value;
}

bool saveCssPath(const std::string& configFile, const std::filesystem::path& root) {
    return writeConfigValue(configFile, "css_path", root.string());
}

bool saveTexturePackPath(const std::string& configFile, const std::filesystem::path& pack) {
    return writeConfigValue(configFile, "texture_pack", pack.string());
}

std::optional<std::filesystem::path> findTexturePack(const std::string& configFile, const std::filesystem::path& appDir) {
    std::error_code ec;
    std::filesystem::path saved = readConfigValue(configFile, "texture_pack");
    if (!saved.empty() && std::filesystem::is_regular_file(saved, ec)) return saved;
    std::filesystem::path beside = appDir / kDefaultPackName;
    if (!appDir.empty() && std::filesystem::is_regular_file(beside, ec)) return beside;
    return std::nullopt;
}

bool mountConfiguredContent(GameContent& content, const std::string& configFile, const std::filesystem::path& appDir) {
    if (auto css = findCssInstall(configFile); css && content.mount(*css)) return true;
    if (auto pack = findTexturePack(configFile, appDir); pack && content.mountPack(*pack)) return true;
    std::printf("content: no CS:S install or texture pack found, using built-in textures only\n");
    return false;
}

bool exportTexturePack(const std::filesystem::path& cssRoot, const std::string& zipPath, ExportProgress& progress) {
    auto fail = [&]() {
        progress.failed = true;
        progress.finished = true;
        return false;
    };
    GameContent game;  // its own, so this can run beside the app's
    if (!game.mount(cssRoot)) return fail();
    std::vector<std::string> files = game.worldMaterialFiles();
    progress.total = files.size();
    // written aside and renamed when complete so a cancelled or failed export never leaves a broken pack where the app would pick it up
    std::string temp = zipPath + ".part";
    ZipWriter zip(temp);
    if (!zip.ok()) return fail();
    for (const auto& path : files) {
        if (auto data = game.read(path)) {
            if (!zip.add(path, *data)) return fail();
        }
        progress.done++;
    }
    if (!zip.finish()) return fail();
    std::error_code ec;
    std::filesystem::rename(temp, zipPath, ec);
    if (ec) return fail();
    std::printf("content: exported %zu files to %s\n", files.size(), zipPath.c_str());
    progress.finished = true;
    return true;
}

}  // namespace content
