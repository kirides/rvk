#pragma once

// The renderer side of the tool API (include/rvk_api.h, docs/research/reference-compare.md 4.1): the screenshot queue that
// rvk_screenshot fills and HalDevice::SwapScreen drains, image encoding, the output-path guard, and the live numbers behind
// rvk_stats. Process-wide; the exports live in src/proxy/d3d9_exports.cpp.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/config.h"

namespace rvk::api {

enum class ImageFormat { Png, Bmp };

// a path as UTF-8 (path::string() uses the ANSI code page and throws on characters it cannot map)
std::string utf8(const std::filesystem::path& p);

// ".png" / ".bmp" (case-insensitive), otherwise nullopt
std::optional<ImageFormat> formatFromExtension(const std::filesystem::path& file);

// Tightly packed BGRA8, top-down -> BMP (BITMAPFILEHEADER + BITMAPINFOHEADER, BI_RGB, bottom-up rows padded to 4 bytes).
// bitsPerPixel 24 (alpha dropped) or 32 (BGRA as is).
std::vector<std::uint8_t> encodeBmp(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height,
                                    std::uint32_t bitsPerPixel);
// The file D3DXSaveSurfaceToFileInMemory(D3DXIFF_BMP) writes for the game's X8R8G8B8 back buffer (measured, D3D9 mode):
// 32 bpp BI_RGB bottom-up with the 4th byte as stored, biSizeImage 0, 2834 pixels per metre (72 dpi), and 2 zero bytes
// after the pixels that bfSize counts. eCDX9Device::SaveBackbufferToFile (the engine snapshot) writes exactly this.
std::vector<std::uint8_t> encodeBmpLikeD3DX(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height);
// Writes `bytes` to "<file>.tmp", then renames it over `file`; creates missing directories.
bool writeFileReplace(const std::filesystem::path& file, std::span<const std::uint8_t> bytes, std::string* error);
// PNG (opaque RGB) or 24-bit BMP of BGRA8 pixels
bool writeImage(const std::filesystem::path& file, ImageFormat format, std::span<const std::uint8_t> bgra, std::uint32_t width,
                std::uint32_t height, std::string* error);

// Lexical, component-wise, case-insensitive: `p` equals `root` or lies below it.
bool pathWithin(const std::filesystem::path& p, const std::filesystem::path& root);
// Junction guard: the reason when `file` lies in <gameDir>\data, either as written or after resolving links / junctions
// (the test copy's data\common and data\compiled are junctions into the real install). nullopt = may be written.
std::optional<std::string> refuseDataPath(const std::filesystem::path& file, const std::filesystem::path& gameDir);
// rvk_screenshot's checks: absolute, .png / .bmp, refuseDataPath.
std::optional<std::string> refuseOutputPath(const std::filesystem::path& file, const std::filesystem::path& gameDir);

// Screenshot requests: queued from any thread, captured by the render thread before a present, encoded and written by a
// writer thread of its own (started on the first capture).
class CaptureQueue {
public:
    static constexpr std::size_t kMaxPending = 64;
    static constexpr std::size_t kMaxResults = 1024;
    // reads the back buffer as tightly packed BGRA8, top-down
    using ReadBack = std::function<bool(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height)>;

    CaptureQueue() = default;
    ~CaptureQueue();  // joins the writer (tests); the process-wide queue is never destroyed
    CaptureQueue(const CaptureQueue&) = delete;
    CaptureQueue& operator=(const CaptureQueue&) = delete;

    // a ticket (> 0), or 0 with `reason` when the queue is full. The path is not checked here (refuseOutputPath).
    std::uint32_t request(std::filesystem::path file, ImageFormat format, std::uint32_t waitFrames, std::string* reason);
    // 0 pending, 1 done, -1 failed / unknown ticket
    int status(std::uint32_t ticket, std::string* msg) const;
    // render thread, before the present of frame `frame` (1-based): captures the requests that are due, counts the others down
    void onPresent(std::uint64_t frame, const ReadBack& read);
    // fails every request not captured yet (the renderer is gone)
    void failWaiting(std::string_view reason);
    std::uint32_t pending() const;
    // tests: waits until nothing is pending
    bool waitIdle(std::chrono::milliseconds timeout) const;

private:
    struct Job {
        std::uint32_t ticket = 0;
        std::filesystem::path file;
        ImageFormat format = ImageFormat::Png;
        std::uint32_t wait = 0;    // presents still to skip
        std::uint32_t waited = 0;  // presents skipped so far
    };
    struct Write {
        Job job;
        std::uint64_t frame = 0;
        std::shared_ptr<const std::vector<std::uint8_t>> pixels;
        std::uint32_t width = 0, height = 0;
    };
    struct Result {
        int state = 0;
        std::string msg;
    };
    void finishLocked(std::uint32_t ticket, int state, std::string msg);
    void writerLoop();

    mutable std::mutex mutex_;
    mutable std::condition_variable idle_;
    std::condition_variable work_;
    std::vector<Job> jobs_;
    std::deque<Write> writes_;
    std::uint32_t writing_ = 0;
    std::map<std::uint32_t, Result> results_;
    std::deque<std::uint32_t> finished_;
    std::uint32_t nextTicket_ = 1;
    std::thread writer_;
    bool stop_ = false;
};

// ---- process-wide state ------------------------------------------------------------------------------------------------

CaptureQueue& captures();

// the renderer mode (bootstrap: at attach and when Vulkan falls back to passthrough); leaving Vulkan fails waiting captures
void setMode(Mode mode);
Mode mode();
// <Risen> (the parent of bin): the root of the data-directory guard
void setGameDir(std::filesystem::path dir);
const std::filesystem::path& gameDir();

struct FrameNumbers {
    std::uint32_t drawsLastFrame = 0;
    std::uint32_t unimplementedSlots = 0;
    std::uint64_t drawsTotal = 0;
    std::uint64_t drawsSkippedTotal = 0;
    std::uint64_t textureCpuBytes = 0;
};
// render thread, after each present: frame = presents so far
void publishPresent(std::uint64_t frame, const FrameNumbers& numbers);
std::uint64_t frameIndex();
struct Snapshot {
    std::uint64_t frame = 0;
    float fps = 0.0f;
    FrameNumbers numbers;
};
Snapshot snapshot();

}  // namespace rvk::api
