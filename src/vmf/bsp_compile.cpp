#include "vmf/bsp_compile.h"

#include <algorithm>
#include <filesystem>

#include "platform/process.h"

namespace vmf {

namespace {

namespace fs = std::filesystem;

// vbsp/vvis/vrad ship as windows executables in every source sdk, some distributions also carry a linux build next to them under a _linux suffix, which is worth preferring since it needs no wine
std::string findTool(const fs::path& binDir, const std::string& base, bool& isWindowsExe) {
    std::error_code ec;
    // a native linux build first, no wine in the way
#ifndef _WIN32
    for (const char* suffix : {"_linux", ""}) {
        fs::path candidate = binDir / (base + suffix);
        if (fs::is_regular_file(candidate, ec)) {
            isWindowsExe = false;
            return candidate.string();
        }
    }
#endif
    fs::path exe = binDir / (base + ".exe");
    if (fs::is_regular_file(exe, ec)) {
        isWindowsExe = true;
        return exe.string();
    }
    return {};
}

// the folder holding gameinfo.txt is what vbsp wants for -game. in a css install that's <root>/cstrike
std::string findGameDir(const fs::path& cssRoot) {
    std::error_code ec;
    if (cssRoot.empty()) return {};
    for (const char* mod : {"cstrike", "hl2"}) {
        fs::path candidate = cssRoot / mod;
        if (fs::is_regular_file(candidate / "gameinfo.txt", ec)) return candidate.string();
    }
    if (fs::is_regular_file(cssRoot / "gameinfo.txt", ec)) return cssRoot.string();
    return {};
}

}  // namespace

const char* backendName(CompileBackend backend) {
    switch (backend) {
        case CompileBackend::Auto: return "auto";
        case CompileBackend::ValveTools: return "vbsp/vvis/vrad";
        case CompileBackend::Native: return "built-in (no vis, fullbright)";
    }
    return "?";
}

std::vector<std::string> compileToolSearchPaths(const std::string& cssRoot, const std::string& manualBin) {
    std::vector<std::string> out;
    if (!manualBin.empty()) out.push_back(manualBin);
    if (!cssRoot.empty()) {
        fs::path root(cssRoot);
        out.push_back((root / "bin").string());
        // the 2013 sdk is a separate steam install so it sits beside css in steamapps/common instead of inside it
        fs::path common = root.parent_path();
        for (const char* sibling : {"Source SDK Base 2013 Multiplayer", "Source SDK Base 2013 Singleplayer"}) {
            out.push_back((common / sibling / "bin").string());
        }
        out.push_back((common / "SourceSDK" / "bin" / "source2013" / "bin").string());
        out.push_back((common / "SourceSDK" / "bin" / "orangebox" / "bin").string());
    }
    return out;
}

CompileTools findCompileTools(const std::string& cssRoot, const std::string& manualBin) {
    CompileTools tools;
    tools.gameDir = findGameDir(cssRoot);

    bool windowsExe = false;
    for (const std::string& dir : compileToolSearchPaths(cssRoot, manualBin)) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) continue;
        bool vbspIsExe = false;
        std::string vbsp = findTool(dir, "vbsp", vbspIsExe);
        if (vbsp.empty()) continue;
        bool ignored = false;
        tools.vbsp = vbsp;
        tools.vvis = findTool(dir, "vvis", ignored);
        tools.vrad = findTool(dir, "vrad", ignored);
        tools.binDir = dir;
        windowsExe = vbspIsExe;
        break;
    }

    if (tools.vbsp.empty()) {
        tools.note = "no vbsp found. Point \"compile tools...\" at a Source SDK bin folder, "
                     "or compile with the built-in writer.";
        return tools;
    }

#ifndef _WIN32
    if (windowsExe) {
        // the tools are windows binaries and this isn't windows
        tools.launcher = platform::findOnPath("wine");
        if (tools.launcher.empty()) {
            tools.note = "found " + tools.vbsp + ", but it is a Windows .exe and wine is not installed. "
                         "Install wine, or compile with the built-in writer.";
            return tools;
        }
    }
#else
    (void)windowsExe;
#endif

    if (tools.gameDir.empty()) {
        tools.note = "found " + tools.vbsp +
                     ", but no gameinfo.txt to pass as -game. Set the cs:s folder under content first.";
        return tools;
    }

    tools.found = true;
    tools.note = "using " + tools.binDir;
    if (!tools.launcher.empty()) tools.note += " through wine";
    if (tools.vvis.empty()) tools.note += "; no vvis, so the map will have no visibility data";
    if (tools.vrad.empty()) tools.note += "; no vrad, so the map will be unlit";
    return tools;
}

CompileResult compileMap(const Document& doc, const std::string& vmfPath, const std::string& outputBsp,
                         const CompileOptions& options, const CompileTools& tools,
                         const std::function<void(const std::string&)>& onLine,
                         const TextureSizeLookup& textureSize, const std::function<bool()>& shouldCancel) {
    CompileResult result;
    auto say = [&](const std::string& line) {
        if (onLine) onLine(line);
    };

    CompileBackend backend = options.backend;
    if (backend == CompileBackend::Auto) {
        backend = tools.found ? CompileBackend::ValveTools : CompileBackend::Native;
        if (!tools.found) {
            say("vbsp not available, falling back to the built-in writer.");
            if (!tools.note.empty()) say("  " + tools.note);
        }
    }
    if (backend == CompileBackend::ValveTools && !tools.found) {
        result.error = tools.note.empty() ? "the Source compile tools are not available" : tools.note;
        return result;
    }

    if (backend == CompileBackend::Native) {
        result.usedBackend = CompileBackend::Native;
        say("compiling with the built-in writer (no visibility data, fullbright).");
        BspWriteResult written = writeBsp(outputBsp, doc, textureSize);
        if (!written.ok) {
            result.error = written.error;
            say("failed: " + written.error);
            return result;
        }
        const auto& s = written.stats;
        say("wrote " + std::to_string(s.faces) + " faces, " + std::to_string(s.brushes) + " brushes over " +
            std::to_string(s.models) + " models, " + std::to_string(s.materials) + " materials.");
        if (s.skippedSolids > 0) {
            say("skipped " + std::to_string(s.skippedSolids) + " hint/skip or degenerate brushes.");
        }
        say("note: no vis and no baked lighting -- see docs/bsp-compile.md.");
        result.ok = true;
        result.bspPath = outputBsp;
        return result;
    }

    // valve's tools read a .vmf off disk so write one for them
    result.usedBackend = CompileBackend::ValveTools;
    say("saving " + vmfPath + " for vbsp");
    if (!saveDocument(vmfPath, doc)) {
        result.error = "couldn't write " + vmfPath;
        say("failed: " + result.error);
        return result;
    }

    std::error_code ec;
    fs::path vmf(vmfPath);
    // vbsp writes <name>.bsp beside the .vmf it was given, whatever we would
    // rather call it, so compile there and move the result afterwards.
    fs::path producedBsp = fs::path(vmf).replace_extension(".bsp");
    fs::remove(producedBsp, ec);

    struct Step {
        const char* name;
        const std::string& exe;
        std::vector<std::string> extraArgs;
        bool run;
    };
    std::vector<std::string> fastArgs;
    if (options.fast) fastArgs.push_back("-fast");

    const std::vector<Step> steps = {
        {"vbsp", tools.vbsp, {}, true},
        {"vvis", tools.vvis, fastArgs, options.runVvis && !tools.vvis.empty()},
        {"vrad", tools.vrad, fastArgs, options.runVrad && !tools.vrad.empty()},
    };

    for (const Step& step : steps) {
        if (!step.run) {
            if (options.runVvis || options.runVrad) say(std::string("skipping ") + step.name);
            continue;
        }
        std::vector<std::string> argv;
        if (!tools.launcher.empty()) argv.push_back(tools.launcher);
        argv.push_back(step.exe);
        for (const auto& a : step.extraArgs) argv.push_back(a);
        argv.push_back("-game");
        argv.push_back(tools.gameDir);
        argv.push_back(vmfPath);

        say("");
        say(std::string("running ") + step.name + "...");
        platform::RunResult run = platform::runCapturingOutput(argv, onLine, shouldCancel);
        if (run.cancelled) {
            result.error = "cancelled";
            say("cancelled.");
            return result;
        }
        if (!run.started) {
            result.error = std::string("couldn't start ") + step.name;
            return result;
        }
        if (run.exitCode != 0) {
            result.error = std::string(step.name) + " failed (exit code " + std::to_string(run.exitCode) + ")";
            say(result.error);
            return result;
        }
    }

    if (!fs::is_regular_file(producedBsp, ec)) {
        result.error = "the tools reported success but produced no " + producedBsp.string();
        say(result.error);
        return result;
    }
    if (producedBsp != fs::path(outputBsp)) {
        fs::remove(outputBsp, ec);
        fs::rename(producedBsp, outputBsp, ec);
        if (ec) {
            // across filesystems rename fails where a copy still works
            ec.clear();
            fs::copy_file(producedBsp, outputBsp, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                result.error = "compiled, but couldn't move the map to " + outputBsp + ": " + ec.message();
                say(result.error);
                return result;
            }
            fs::remove(producedBsp, ec);
        }
    }

    say("");
    say("compiled " + outputBsp);
    result.ok = true;
    result.bspPath = outputBsp;
    return result;
}

}  // namespace vmf
