#include "platform/discord.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace platform {

namespace {

// discord's frame opcodes, we only ever send the first two
constexpr int32_t kOpHandshake = 0;
constexpr int32_t kOpFrame = 1;

// discord drops presence updates sent faster than about one per 15 seconds
constexpr uint64_t kMinSendIntervalMs = 15000;
// and no point hammering a socket that isn't there
constexpr uint64_t kReconnectIntervalMs = 20000;

// json string escaping. map names come from file paths so quotes and backslashes aren't hypothetical
std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

#ifndef _WIN32
// discord listens on discord-ipc-0 through -9, under whichever of these directories the user's install happens to use, flatpak and snap nest it one deeper again
std::vector<std::string> candidateSocketDirs() {
    std::vector<std::string> roots;
    for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        if (const char* v = std::getenv(var); v && *v) roots.push_back(v);
    }
    roots.push_back("/tmp");

    std::vector<std::string> dirs;
    for (const std::string& root : roots) {
        std::string base = root;
        while (!base.empty() && base.back() == '/') base.pop_back();
        dirs.push_back(base);
        dirs.push_back(base + "/app/com.discordapp.Discord");
        dirs.push_back(base + "/app/com.discordapp.DiscordCanary");
        dirs.push_back(base + "/snap.discord");
    }
    return dirs;
}
#endif

uint64_t nowMs() {
    return SDL_GetTicks();
}

}  // namespace

DiscordPresence::~DiscordPresence() {
    shutdown();
}

void DiscordPresence::start(std::string applicationId) {
    applicationId_ = std::move(applicationId);
    startedAt_ = static_cast<uint64_t>(std::time(nullptr));
    nextConnectMs_ = 0;
}

void DiscordPresence::setActivity(const std::string& details, const std::string& state) {
    if (details == details_ && state == state_) return;
    details_ = details;
    state_ = state;
    dirty_ = true;
}

void DiscordPresence::setLargeImage(const std::string& key, const std::string& hoverText) {
    if (key == largeImage_ && hoverText == largeText_) return;
    largeImage_ = key;
    largeText_ = hoverText;
    dirty_ = true;
}

bool DiscordPresence::connect() {
#ifdef _WIN32
    for (int i = 0; i < 10; ++i) {
        std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
        HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            pipe_ = reinterpret_cast<intptr_t>(h);
            return true;
        }
    }
#else
    for (const std::string& dir : candidateSocketDirs()) {
        for (int i = 0; i < 10; ++i) {
            std::string path = dir + "/discord-ipc-" + std::to_string(i);
            if (path.size() + 1 > sizeof(sockaddr_un::sun_path)) continue;
            int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) continue;
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                pipe_ = fd;
                return true;
            }
            ::close(fd);
        }
    }
#endif
    return false;
}

bool DiscordPresence::writeFrame(int32_t opcode, const std::string& payload) {
    if (pipe_ == -1) return false;
    std::vector<char> frame(8 + payload.size());
    const int32_t length = static_cast<int32_t>(payload.size());
    std::memcpy(frame.data(), &opcode, 4);
    std::memcpy(frame.data() + 4, &length, 4);
    std::memcpy(frame.data() + 8, payload.data(), payload.size());

#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(pipe_), frame.data(), static_cast<DWORD>(frame.size()), &written,
                   nullptr) ||
        written != frame.size()) {
        shutdown();
        return false;
    }
#else
    size_t sent = 0;
    while (sent < frame.size()) {
        // MSG_NOSIGNAL: discord quitting mid write must not take us with it
        ssize_t n = ::send(static_cast<int>(pipe_), frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            shutdown();
            return false;
        }
        sent += static_cast<size_t>(n);
    }
#endif
    return true;
}

void DiscordPresence::sendActivity() {
    const std::string nonce = std::to_string(++nonce_);
    std::string assets;
    if (!largeImage_.empty()) {
        assets = ",\"assets\":{\"large_image\":\"" + jsonEscape(largeImage_) + "\"";
        if (!largeText_.empty()) assets += ",\"large_text\":\"" + jsonEscape(largeText_) + "\"";
        assets += "}";
    }
    std::string payload =
        "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + nonce + "\",\"args\":{\"pid\":" +
        std::to_string(static_cast<long long>(
#ifdef _WIN32
            GetCurrentProcessId()
#else
            ::getpid()
#endif
            )) +
        ",\"activity\":{\"details\":\"" + jsonEscape(details_) + "\",\"state\":\"" + jsonEscape(state_) +
        "\",\"timestamps\":{\"start\":" + std::to_string(startedAt_) + "}" + assets + "}}}";
    if (writeFrame(kOpFrame, payload)) {
        dirty_ = false;
        lastSendMs_ = nowMs();
    }
}

void DiscordPresence::update() {
    if (applicationId_.empty()) return;

    if (pipe_ == -1) {
        const uint64_t now = nowMs();
        if (now < nextConnectMs_) return;
        nextConnectMs_ = now + kReconnectIntervalMs;
        if (!connect()) return;
        handshaken_ = false;
    }

    if (!handshaken_) {
        if (!writeFrame(kOpHandshake, "{\"v\":1,\"client_id\":\"" + jsonEscape(applicationId_) + "\"}")) return;
        handshaken_ = true;
        dirty_ = true;
        lastSendMs_ = 0;  // the first update after connecting goes straight out
    }

    if (dirty_ && nowMs() - lastSendMs_ >= kMinSendIntervalMs) sendActivity();
}

void DiscordPresence::shutdown() {
    if (pipe_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(pipe_));
#else
    ::close(static_cast<int>(pipe_));
#endif
    pipe_ = -1;
    handshaken_ = false;
}

}  // namespace platform
