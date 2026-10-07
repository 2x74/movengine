#pragma once

#include <functional>
#include <string>
#include <vector>

namespace platform {

// starts another executable that ships next to this one (the game and the editor launch each other), detached. `baseName` has no extension, .exe is added on windows. returns false if it couldn't be started
bool launchSibling(const std::string& baseName, const std::vector<std::string>& args);

// the same, keeping hold of the process to ask whether it's still running
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const std::string& baseName, const std::vector<std::string>& args);
    bool running();

private:
    void* process_ = nullptr;  // SDL_Process
};

struct RunResult {
    bool started = false;
    int exitCode = -1;
    bool cancelled = false;
};

// runs a program to completion with its stdout and stderr piped back, handing over each line as it arrives instead of at the end (a map compile takes minutes and its progress is the only sign it's alive). blocks, so call it off the thread drawing the ui. `shouldCancel`, when given, is polled as output is read and the process is killed if it returns true
RunResult runCapturingOutput(const std::vector<std::string>& argv,
                             const std::function<void(const std::string&)>& onLine,
                             const std::function<bool()>& shouldCancel = {});

// looks `name` up the way a shell would, for finding wine and the source sdk tools. returns an empty string when it's not on PATH
std::string findOnPath(const std::string& name);

}  // namespace platform
