#include "platform/user_files.h"

#include <SDL3/SDL.h>

#include <filesystem>

namespace platform {

namespace {

std::string sdlPrefPath(const char* app) {
    char* pref = SDL_GetPrefPath("lunae", app);
    std::string path = pref ? pref : "";
    SDL_free(pref);
    return path;
}

// everything the old name saved, moved over once. only into an empty folder so a later run can never tread on settings made under the new name, and copied not moved so a downgrade still finds the originals
void carryOverOldPrefs(const std::string& destination) {
    std::error_code ec;
    if (!std::filesystem::is_empty(destination, ec) || ec) return;

    const std::string previous = sdlPrefPath("bhop-localhops");
    if (previous.empty() || previous == destination) return;
    if (!std::filesystem::exists(previous, ec) || std::filesystem::is_empty(previous, ec)) return;

    std::filesystem::copy(previous, destination,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::skip_existing,
                          ec);
    if (ec) return;
    // the editor's layout file carried the old name in it
    const std::filesystem::path oldIni = std::filesystem::path(destination) / "bhop_editor_imgui.ini";
    const std::filesystem::path newIni = std::filesystem::path(destination) / "movengine_editor_imgui.ini";
    if (std::filesystem::exists(oldIni, ec) && !std::filesystem::exists(newIni, ec)) {
        std::filesystem::copy_file(oldIni, newIni, ec);
    }
}

}  // namespace

const std::string& prefDir() {
    static const std::string dir = [] {
        std::string path = sdlPrefPath("movengine");
        if (!path.empty()) carryOverOldPrefs(path);
        return path;
    }();
    return dir;
}

std::string prefFile(const std::string& name) {
    return prefDir() + name;
}

}  // namespace platform
