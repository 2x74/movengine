#include "platform/process.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace platform {

namespace {

SDL_Process* startSibling(const std::string& baseName, const std::vector<std::string>& args) {
    const char* base = SDL_GetBasePath();
    std::string exe = std::string(base ? base : "") + baseName;
#ifdef _WIN32
    exe += ".exe";
#endif
    std::vector<const char*> argv;
    argv.push_back(exe.c_str());
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    SDL_Process* process = SDL_CreateProcess(argv.data(), false);
    if (!process) std::fprintf(stderr, "couldn't start %s: %s\n", exe.c_str(), SDL_GetError());
    return process;
}

}  // namespace

bool launchSibling(const std::string& baseName, const std::vector<std::string>& args) {
    SDL_Process* process = startSibling(baseName, args);
    if (!process) return false;
    // only drops our handle, the process keeps running on its own
    SDL_DestroyProcess(process);
    return true;
}

ChildProcess::~ChildProcess() {
    if (process_) SDL_DestroyProcess(static_cast<SDL_Process*>(process_));
}

bool ChildProcess::start(const std::string& baseName, const std::vector<std::string>& args) {
    if (process_) SDL_DestroyProcess(static_cast<SDL_Process*>(process_));
    process_ = startSibling(baseName, args);
    return process_ != nullptr;
}

bool ChildProcess::running() {
    if (!process_) return false;
    if (SDL_WaitProcess(static_cast<SDL_Process*>(process_), false, nullptr)) {
        SDL_DestroyProcess(static_cast<SDL_Process*>(process_));
        process_ = nullptr;
        return false;
    }
    return true;
}

RunResult runCapturingOutput(const std::vector<std::string>& argv,
                             const std::function<void(const std::string&)>& onLine,
                             const std::function<bool()>& shouldCancel) {
    RunResult result;
    if (argv.empty()) return result;

    std::vector<const char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) args.push_back(a.c_str());
    args.push_back(nullptr);

    // stdout piped back to us with stderr folded into it: the compilers put their warnings and their "Error:" lines on stderr and those are the ones worth reading
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) return result;
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) {
        if (onLine) onLine(std::string("couldn't start ") + argv[0] + ": " + SDL_GetError());
        return result;
    }
    result.started = true;

    SDL_IOStream* out = SDL_GetProcessOutput(process);
    std::string pending;
    auto flushLines = [&](bool last) {
        size_t start = 0;
        for (size_t i = 0; i < pending.size(); ++i) {
            if (pending[i] != '\n') continue;
            std::string line = pending.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (onLine) onLine(line);
            start = i + 1;
        }
        pending.erase(0, start);
        if (last && !pending.empty()) {
            if (pending.back() == '\r') pending.pop_back();
            if (onLine) onLine(pending);
            pending.clear();
        }
    };

    char buffer[4096];
    while (out) {
        if (shouldCancel && shouldCancel()) {
            SDL_KillProcess(process, true);
            result.cancelled = true;
            break;
        }
        size_t read = SDL_ReadIO(out, buffer, sizeof(buffer));
        if (read > 0) {
            pending.append(buffer, read);
            flushLines(false);
            continue;
        }
        SDL_IOStatus status = SDL_GetIOStatus(out);
        if (status == SDL_IO_STATUS_NOT_READY) {
            SDL_Delay(10);  // nothing yet; the tools go quiet for whole seconds
            continue;
        }
        break;  // EOF or error: the process closed its end
    }
    flushLines(true);

    int exitCode = -1;
    SDL_WaitProcess(process, true, &exitCode);
    SDL_DestroyProcess(process);
    result.exitCode = exitCode;
    return result;
}

std::string findOnPath(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
#ifdef _WIN32
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    std::string paths(path);
    size_t start = 0;
    while (start <= paths.size()) {
        size_t end = paths.find(kSeparator, start);
        if (end == std::string::npos) end = paths.size();
        if (end > start) {
            std::filesystem::path candidate = std::filesystem::path(paths.substr(start, end - start)) / name;
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
#ifdef _WIN32
            candidate += ".exe";
            if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
#endif
        }
        start = end + 1;
    }
    return {};
}

}  // namespace platform
