#include "platform/file_dialog.h"

#include <tinyfiledialogs.h>

namespace platform {

std::optional<std::string> chooseMapFile() {
    char const* filterPatterns[2] = {"*.bsp", "*.vmf"};

    char* path = tinyfd_openFileDialog(
        "choose your map",
        "",
        2,
        filterPatterns,
        "source maps (.bsp, .vmf)",
        0);

    if (!path) {
        return std::nullopt;
    }
    return std::string(path);
}

std::optional<std::string> chooseVmfToOpen() {
    char const* filterPatterns[2] = {"*.vmf", "*.bsp"};
    char* path = tinyfd_openFileDialog("open map", "", 2, filterPatterns, "maps (.vmf, or a compiled .bsp)", 0);
    if (!path) {
        return std::nullopt;
    }
    return std::string(path);
}

std::optional<std::string> chooseVmfToSave(const std::string& defaultPath) {
    char const* filterPatterns[1] = {"*.vmf"};
    char* path = tinyfd_saveFileDialog("save map as", defaultPath.c_str(), 1, filterPatterns, "hammer maps (.vmf)");
    if (!path) {
        return std::nullopt;
    }
    std::string result(path);
    if (result.size() < 4 || result.compare(result.size() - 4, 4, ".vmf") != 0) {
        result += ".vmf";
    }
    return result;
}

std::optional<std::string> chooseFolder(const char* title) {
    char* path = tinyfd_selectFolderDialog(title, "");
    if (!path) {
        return std::nullopt;
    }
    return std::string(path);
}

int askYesNoCancel(const char* title, const char* message) {
    return tinyfd_messageBox(title, message, "yesnocancel", "question", 1);
}

std::optional<std::string> chooseConfigFile() {
    char const* filterPatterns[1] = {"*.cfg"};

    char* path = tinyfd_openFileDialog(
        "choose a config",
        "",
        1,
        filterPatterns,
        "source config files",
        0);

    if (!path) {
        return std::nullopt;
    }
    return std::string(path);
}

std::optional<std::string> chooseSoundFile() {
    char const* filterPatterns[4] = {"*.mp3", "*.wav", "*.ogg", "*.flac"};
    char* path = tinyfd_openFileDialog("choose a sound", "", 4, filterPatterns, "sounds (.mp3, .wav, .ogg, .flac)", 0);
    if (!path) return std::nullopt;
    return std::string(path);
}

std::optional<std::string> chooseBspToSave(const std::string& defaultPath) {
    char const* filterPatterns[1] = {"*.bsp"};
    char* path = tinyfd_saveFileDialog("compile map to", defaultPath.c_str(), 1, filterPatterns, "compiled maps (.bsp)");
    if (!path) return std::nullopt;
    std::string result(path);
    if (result.size() < 4 || result.compare(result.size() - 4, 4, ".bsp") != 0) result += ".bsp";
    return result;
}

std::optional<std::string> chooseZipToOpen(const char* title) {
    char const* filterPatterns[1] = {"*.zip"};
    char* path = tinyfd_openFileDialog(title, "", 1, filterPatterns, "zip files", 0);
    if (!path) return std::nullopt;
    return std::string(path);
}

std::optional<std::string> chooseZipToSave(const char* title, const std::string& defaultPath) {
    char const* filterPatterns[1] = {"*.zip"};
    char* path = tinyfd_saveFileDialog(title, defaultPath.c_str(), 1, filterPatterns, "zip files");
    if (!path) return std::nullopt;
    std::string result(path);
    if (result.size() < 4 || result.compare(result.size() - 4, 4, ".zip") != 0) result += ".zip";
    return result;
}

}  // namespace platform
