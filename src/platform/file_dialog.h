#pragma once

#include <optional>
#include <string>

namespace platform {

// opens a native "choose your map" file picker for compiled .bsp maps or editor .vmf maps (played directly, no compile). returns the chosen path, or nullopt if the user cancelled
std::optional<std::string> chooseMapFile();

// editor: open (.vmf, or a .bsp to import) / save as (.vmf) pickers
std::optional<std::string> chooseVmfToOpen();
std::optional<std::string> chooseVmfToSave(const std::string& defaultPath);

// editor: where a compiled map should go
std::optional<std::string> chooseBspToSave(const std::string& defaultPath);

// folder picker, nullopt if cancelled
std::optional<std::string> chooseFolder(const char* title);

// yes/no/cancel question box: 1 = yes, 2 = no, 0 = cancel
int askYesNoCancel(const char* title, const char* message);

// opens a native "choose a config" file picker filtered to .cfg files. returns the chosen path, or nullopt if the user cancelled
std::optional<std::string> chooseConfigFile();

// audio file (.mp3/.wav/.ogg/.flac) picker
std::optional<std::string> chooseSoundFile();

// .zip open / save-as pickers (texture packs).
std::optional<std::string> chooseZipToOpen(const char* title);
std::optional<std::string> chooseZipToSave(const char* title, const std::string& defaultPath);

}  // namespace platform
