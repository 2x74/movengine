#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace platform {

// pipes finished frames to ffmpeg as raw video. used both to export a replay and to record live play, it only knows about frames not which
//
// readback goes through two pixel buffer objects: glReadPixels straight to system memory blocks until the gpu has finished the frame, which at 1080p is a stall every frame. reading into a pbo is asynchronous so each frame maps the buffer the PREVIOUS frame asked for, by which time it's ready
class VideoRecorder {
public:
    ~VideoRecorder();

    // quality is 1..10: 1 lossless, 10 the smallest file worth keeping. false if ffmpeg is missing or the file can't be opened, `error()` says which
    bool start(const std::string& outputFile, int width, int height, int fps, int quality);
    // call once per rendered frame, after drawing and before the swap. `repeat` writes the same frame more than once, for when the game is running slower than the recording rate, without it the clip comes out short and sped up
    void captureFrame(int repeat = 1);
    void stop();

    bool recording() const { return pipe_ != nullptr; }
    const std::string& outputPath() const { return outputPath_; }
    const std::string& error() const { return error_; }
    int frameCount() const { return frames_; }

    // is there an ffmpeg on PATH at all? checked once and remembered
    static bool ffmpegAvailable();
    // what quality `q` means, for a menu: "lossless", "twitter compression"...
    static const char* qualityLabel(int quality);

private:
    void destroyBuffers();

    std::FILE* pipe_ = nullptr;
    std::string outputPath_;
    std::string error_;
    int width_ = 0, height_ = 0, frames_ = 0;
    unsigned pbo_[2] = {0, 0};
    int current_ = 0;
    bool primed_ = false;  // the first frame has nothing queued to collect yet
};

}  // namespace platform
