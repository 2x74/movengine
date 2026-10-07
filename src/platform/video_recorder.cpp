#include "platform/video_recorder.h"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cstdlib>

#if defined(_WIN32)
#define BHOP_POPEN _popen
#define BHOP_PCLOSE _pclose
#define BHOP_DEVNULL "NUL"
#else
#define BHOP_POPEN popen
#define BHOP_PCLOSE pclose
#define BHOP_DEVNULL "/dev/null"
#endif

namespace platform {

namespace {

// 1..10 onto x264's crf, where 0 is lossless and higher throws away more. 18 is the usual "can't see the difference" mark and 23 is x264's own default, so the middle of the scale sits around there and the top end goes where a clip has to survive being re-encoded by a website
constexpr std::array<int, 10> kCrfForQuality = {0, 14, 17, 20, 23, 26, 29, 33, 37, 40};

constexpr std::array<const char*, 10> kQualityLabels = {
    "lossless", "near lossless", "very high", "high",         "good (default)",
    "medium",   "small",         "smaller",   "very small",   "twitter compression",
};

int clampQuality(int q) { return std::clamp(q, 1, 10); }

}  // namespace

VideoRecorder::~VideoRecorder() { stop(); }

const char* VideoRecorder::qualityLabel(int quality) { return kQualityLabels[clampQuality(quality) - 1]; }

bool VideoRecorder::ffmpegAvailable() {
    // worked out once: this shells out and it's asked every time a menu showing the record option is drawn
    static const bool available = [] {
        return std::system("ffmpeg -version > " BHOP_DEVNULL " 2>&1") == 0;
    }();
    return available;
}

bool VideoRecorder::start(const std::string& outputFile, int width, int height, int fps, int quality) {
    stop();
    error_.clear();
    if (!ffmpegAvailable()) {
        error_ = "ffmpeg not found on PATH -- install it to record video";
        return false;
    }
    // yuv420p needs even dimensions, and a resized window is often odd.
    width_ = width & ~1;
    height_ = height & ~1;
    if (width_ <= 0 || height_ <= 0) {
        error_ = "window is too small to record";
        return false;
    }

    const int crf = kCrfForQuality[clampQuality(quality) - 1];
    char command[1024];
    // -vf vflip because opengl reads bottom up, letting ffmpeg do it keeps the write here a single memcpy sized fwrite instead of one per row. ultrafast at lossless since that has to keep up with the game in real time, veryfast elsewhere buys a good deal of size back for little cost
    std::snprintf(command, sizeof(command),
                  "ffmpeg -hide_banner -loglevel error -y "
                  "-f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i - "
                  "-vf vflip -c:v libx264 -preset %s -crf %d -pix_fmt yuv420p \"%s\"",
                  width_, height_, fps, crf == 0 ? "ultrafast" : "veryfast", crf, outputFile.c_str());

    pipe_ = BHOP_POPEN(command, "w");
    if (!pipe_) {
        error_ = "couldn't start ffmpeg";
        return false;
    }
    outputPath_ = outputFile;
    frames_ = 0;
    current_ = 0;
    primed_ = false;

    const size_t frameBytes = static_cast<size_t>(width_) * height_ * 3;
    glGenBuffers(2, pbo_);
    for (unsigned id : pbo_) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, id);
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(frameBytes), nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

void VideoRecorder::captureFrame(int repeat) {
    if (!pipe_) return;
    const size_t frameBytes = static_cast<size_t>(width_) * height_ * 3;

    // ask for this frame without waiting for it
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[current_]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGB, GL_UNSIGNED_BYTE, nullptr);

    // collect the one asked for last frame, which the gpu has had a whole frame to finish
    if (primed_) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[1 - current_]);
        const void* src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(frameBytes), GL_MAP_READ_BIT);
        if (src) {
            for (int i = 0; i < std::max(repeat, 1); ++i) {
                std::fwrite(src, 1, frameBytes, pipe_);
                ++frames_;
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    current_ = 1 - current_;
    primed_ = true;
}

void VideoRecorder::destroyBuffers() {
    if (pbo_[0] != 0 || pbo_[1] != 0) {
        glDeleteBuffers(2, pbo_);
        pbo_[0] = pbo_[1] = 0;
    }
}

void VideoRecorder::stop() {
    if (!pipe_) {
        destroyBuffers();
        return;
    }
    // the last frame asked for is still in flight, write it so the clip doesn't end one frame early
    if (primed_) {
        const size_t frameBytes = static_cast<size_t>(width_) * height_ * 3;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[1 - current_]);
        const void* src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(frameBytes), GL_MAP_READ_BIT);
        if (src) {
            std::fwrite(src, 1, frameBytes, pipe_);
            ++frames_;
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    BHOP_PCLOSE(pipe_);
    pipe_ = nullptr;
    primed_ = false;
    destroyBuffers();
}

}  // namespace platform
