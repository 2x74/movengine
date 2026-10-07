#pragma once

#include <cstdint>
#include <string>

namespace platform {

// discord rich presence, spoken straight down discord's local ipc socket instead of through its sdk: the protocol is a length prefixed json frame over a unix socket (a named pipe on windows), which is less code than vendoring a library and leaves nothing to keep up to date
//
// everything here is best effort and silent. discord not running, not installed, or refusing the handshake are all ordinary, the app must not care and must never block waiting for it
class DiscordPresence {
public:
    ~DiscordPresence();
    DiscordPresence() = default;
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    // `applicationId` is the discord application's id, empty turns the whole thing off
    void start(std::string applicationId);

    // the big picture next to the text. `key` is the asset name from the dev portal's rich presence art assets, `hoverText` shows on hover. empty key = no image. goes out with the next activity update
    void setLargeImage(const std::string& key, const std::string& hoverText = {});

    // the two lines discord shows. cheap to call every frame: it only writes when the text actually changed, and no more often than discord's rate limit allows
    void setActivity(const std::string& details, const std::string& state);

    // call once a frame, handles connecting and reconnecting
    void update();

    void shutdown();

private:
    bool connect();
    bool writeFrame(int32_t opcode, const std::string& payload);
    void sendActivity();

    std::string applicationId_;
    std::string details_;
    std::string state_;
    std::string largeImage_;
    std::string largeText_;
    bool dirty_ = false;
    bool handshaken_ = false;
    // -1 when not connected. a SOCKET/HANDLE on windows, an fd elsewhere
    intptr_t pipe_ = -1;
    uint64_t startedAt_ = 0;     // unix seconds, for discord's "elapsed"
    uint64_t lastSendMs_ = 0;    // rate limiting
    uint64_t nextConnectMs_ = 0; // don't retry a missing discord every frame
    uint32_t nonce_ = 0;
};

}  // namespace platform
