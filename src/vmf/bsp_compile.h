#pragma once

#include <functional>
#include <string>
#include <vector>

#include "vmf/bsp_write.h"
#include "vmf/document.h"

namespace vmf {

// how the .bsp gets made
enum class CompileBackend {
    Auto,        // valve's tools when they're installed, else native
    ValveTools,  // vbsp (+ vvis, vrad): a real, server-ready compile
    Native,      // vmf::writeBsp: no visibility, fullbright. See bsp_write.h.
};

const char* backendName(CompileBackend backend);

// where vbsp/vvis/vrad are, and how to run them
struct CompileTools {
    bool found = false;
    std::string vbsp, vvis, vrad;
    std::string gameDir;   // vbsp's -game argument: the folder with gameinfo.txt
    std::string launcher;  // empty to run directly, else the wine binary
    std::string binDir;    // where they were found, for showing the user
    std::string note;      // what was found, or why nothing was
};

// hunts for the source compile tools. `cssRoot` is the detected css folder (may be empty) and `manualBin` a bin folder the user picked by hand, which wins when it has the tools.
//
// the tools are windows executables so running them on linux needs wine, when the search finds them but no wine `found` stays false and `note` says so instead of leaving the caller to work it out
CompileTools findCompileTools(const std::string& cssRoot, const std::string& manualBin);

// folders findCompileTools looked in, for a "nothing found, i looked here" message in the ui
std::vector<std::string> compileToolSearchPaths(const std::string& cssRoot, const std::string& manualBin);

struct CompileOptions {
    CompileBackend backend = CompileBackend::Auto;
    bool runVvis = true;  // skip for a fast iteration compile
    bool runVrad = true;
    bool fast = true;     // -fast for vvis and vrad
};

struct CompileResult {
    bool ok = false;
    CompileBackend usedBackend = CompileBackend::Native;
    std::string bspPath;
    std::string error;
};

// compiles `doc` to `outputBsp`. blocking, and a real compile takes minutes so run it on a worker thread: every line of tool output plus our own progress notes go to `onLine` as they happen. `vmfPath` is where the .vmf for vbsp to read should be written (the valve tools work from a file)
CompileResult compileMap(const Document& doc, const std::string& vmfPath, const std::string& outputBsp,
                         const CompileOptions& options, const CompileTools& tools,
                         const std::function<void(const std::string&)>& onLine,
                         const TextureSizeLookup& textureSize = {},
                         const std::function<bool()>& shouldCancel = {});

}  // namespace vmf
