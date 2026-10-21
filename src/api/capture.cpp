#include "api/capture.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <format>
#include <fstream>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "core/png.h"

namespace rvk::api {
namespace fs = std::filesystem;

std::string utf8(const fs::path& p) {
    const std::wstring& w = p.native();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), char{});
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// ---- images -------------------------------------------------------------------------------------------------------------

std::optional<ImageFormat> formatFromExtension(const fs::path& file) {
    const std::wstring ext = file.extension().wstring();
    if (_wcsicmp(ext.c_str(), L".png") == 0) return ImageFormat::Png;
    if (_wcsicmp(ext.c_str(), L".bmp") == 0) return ImageFormat::Bmp;
    return std::nullopt;
}

std::vector<std::uint8_t> encodeBmp(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height,
                                    std::uint32_t bitsPerPixel) {
    std::vector<std::uint8_t> out;
    if (width == 0 || height == 0 || (bitsPerPixel != 24 && bitsPerPixel != 32) ||
        bgra.size() < static_cast<std::size_t>(width) * height * 4) {
        return out;
    }
    const std::uint32_t bpp = bitsPerPixel / 8;
    const std::uint32_t stride = (width * bpp + 3u) & ~3u;
    const std::uint32_t imageSize = stride * height;
    constexpr std::uint32_t kHeaders = 14 + 40;
    out.resize(kHeaders + static_cast<std::size_t>(imageSize));
    const auto put16 = [&](std::size_t at, std::uint16_t v) {
        out[at] = static_cast<std::uint8_t>(v);
        out[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    // BITMAPFILEHEADER
    out[0] = 'B';
    out[1] = 'M';
    put32(2, static_cast<std::uint32_t>(out.size()));
    put32(10, kHeaders);
    // BITMAPINFOHEADER (positive height: bottom-up)
    put32(14, 40);
    put32(18, width);
    put32(22, height);
    put16(26, 1);
    put16(28, static_cast<std::uint16_t>(bitsPerPixel));
    put32(30, 0);  // BI_RGB
    put32(34, imageSize);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* src = bgra.data() + static_cast<std::size_t>(height - 1 - y) * width * 4;
        std::uint8_t* dst = out.data() + kHeaders + static_cast<std::size_t>(y) * stride;
        if (bpp == 4) {
            std::memcpy(dst, src, static_cast<std::size_t>(width) * 4);
        } else {
            for (std::uint32_t x = 0; x < width; ++x) {
                dst[x * 3] = src[x * 4];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> encodeBmpLikeD3DX(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> out = encodeBmp(bgra, width, height, 32);
    if (out.empty()) {
        return out;
    }
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    out.push_back(0);
    out.push_back(0);
    put32(2, static_cast<std::uint32_t>(out.size()));
    put32(34, 0);     // biSizeImage
    put32(38, 2834);  // biXPelsPerMeter
    put32(42, 2834);  // biYPelsPerMeter
    return out;
}

bool writeFileReplace(const fs::path& file, std::span<const std::uint8_t> bytes, std::string* error) {
    const auto fail = [&](std::string why) {
        if (error != nullptr) *error = std::move(why);
        return false;
    };
    std::error_code ec;
    if (file.has_parent_path()) {
        fs::create_directories(file.parent_path(), ec);
    }
    fs::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(std::format("cannot create the file (error {})", GetLastError()));
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            fs::remove(tmp, ec);
            return fail("write failed");
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        const DWORD e = GetLastError();
        fs::remove(tmp, ec);
        return fail(std::format("cannot rename the temporary file (error {})", e));
    }
    return true;
}

bool writeImage(const fs::path& file, ImageFormat format, std::span<const std::uint8_t> bgra, std::uint32_t width,
                std::uint32_t height, std::string* error) {
    const std::vector<std::uint8_t> bytes =
        format == ImageFormat::Png ? encodePngBgra(bgra, width, height) : encodeBmp(bgra, width, height, 24);
    if (bytes.empty()) {
        if (error != nullptr) *error = "encoding failed";
        return false;
    }
    return writeFileReplace(file, bytes, error);
}

// ---- path guard ---------------------------------------------------------------------------------------------------------

namespace {

// "\\?\B:\x" -> "B:\x" (canonical() may return verbatim paths)
fs::path stripVerbatim(const fs::path& p) {
    const std::wstring& s = p.native();
    if (s.starts_with(L"\\\\?\\UNC\\")) return fs::path(L"\\\\" + s.substr(8));
    if (s.starts_with(L"\\\\?\\")) return fs::path(s.substr(4));
    return p;
}

std::vector<std::wstring> components(const fs::path& p) {
    std::vector<std::wstring> out;
    for (const auto& part : stripVerbatim(p).lexically_normal()) {
        std::wstring s = part.native();
        if (s.empty()) continue;  // trailing separator
        if (s == L"/") s = L"\\";
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace

bool pathWithin(const fs::path& p, const fs::path& root) {
    const auto a = components(p);
    const auto b = components(root);
    if (b.empty() || a.size() < b.size()) return false;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (CompareStringOrdinal(a[i].c_str(), static_cast<int>(a[i].size()), b[i].c_str(), static_cast<int>(b[i].size()),
                                 TRUE) != CSTR_EQUAL) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> refuseDataPath(const fs::path& file, const fs::path& gameDir) {
    if (gameDir.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    const fs::path data = (gameDir / L"data").lexically_normal();
    std::vector<fs::path> roots{data};
    if (const fs::path c = fs::canonical(data, ec); !ec) {
        roots.push_back(stripVerbatim(c));
    }
    // data\common, data\compiled (junctions into the real install): their targets are off limits too
    for (fs::directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (const fs::path c = fs::canonical(it->path(), e2); !e2 && !pathWithin(c, data)) {
            roots.push_back(stripVerbatim(c));
        }
    }
    const fs::path lexical = file.lexically_normal();
    fs::path resolved = fs::weakly_canonical(file, ec);
    resolved = ec ? lexical : stripVerbatim(resolved);
    for (const auto& root : roots) {
        if (pathWithin(lexical, root) || pathWithin(resolved, root)) {
            std::string r = "the path lies in the game's data directory (";
            r += utf8(root);
            r += ")";
            return r;
        }
    }
    return std::nullopt;
}

std::optional<std::string> refuseOutputPath(const fs::path& file, const fs::path& gameDir) {
    if (file.empty() || !file.is_absolute()) {
        return "the path is not absolute";
    }
    if (!file.has_filename() || !formatFromExtension(file)) {
        return "the file name must end in .png or .bmp";
    }
    return refuseDataPath(file, gameDir);
}

// ---- capture queue ------------------------------------------------------------------------------------------------------

CaptureQueue::~CaptureQueue() {
    {
        const std::lock_guard lock(mutex_);
        stop_ = true;
    }
    work_.notify_all();
    if (writer_.joinable()) {
        writer_.join();
    }
}

std::uint32_t CaptureQueue::request(fs::path file, ImageFormat format, std::uint32_t waitFrames, std::string* reason) {
    const std::lock_guard lock(mutex_);
    if (jobs_.size() + writes_.size() + writing_ >= kMaxPending) {
        if (reason != nullptr) *reason = std::format("{} requests pending", kMaxPending);
        return 0;
    }
    const std::uint32_t ticket = nextTicket_++;
    if (nextTicket_ == 0) nextTicket_ = 1;
    jobs_.push_back(Job{ticket, std::move(file), format, waitFrames, 0});
    results_[ticket] = Result{0, {}};
    return ticket;
}

int CaptureQueue::status(std::uint32_t ticket, std::string* msg) const {
    const std::lock_guard lock(mutex_);
    const auto it = results_.find(ticket);
    if (it == results_.end()) {
        if (msg != nullptr) *msg = "unknown ticket";
        return -1;
    }
    if (msg != nullptr) {
        if (it->second.state != 0) {
            *msg = it->second.msg;
        } else if (const auto j = std::ranges::find(jobs_, ticket, &Job::ticket); j != jobs_.end()) {
            *msg = std::format("waiting {} presents", j->wait);
        } else {
            *msg = "writing";
        }
    }
    return it->second.state;
}

void CaptureQueue::finishLocked(std::uint32_t ticket, int state, std::string msg) {
    results_[ticket] = Result{state, std::move(msg)};
    finished_.push_back(ticket);
    while (finished_.size() > kMaxResults) {
        results_.erase(finished_.front());
        finished_.pop_front();
    }
}

void CaptureQueue::onPresent(std::uint64_t frame, const ReadBack& read) {
    std::vector<Job> due;
    {
        const std::lock_guard lock(mutex_);
        if (jobs_.empty()) {
            return;
        }
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->wait == 0) {
                due.push_back(std::move(*it));
                it = jobs_.erase(it);
            } else {
                --it->wait;
                ++it->waited;
                ++it;
            }
        }
        writing_ += static_cast<std::uint32_t>(due.size());  // keeps pending() honest during the read-back
    }
    if (due.empty()) {
        return;
    }
    auto pixels = std::make_shared<std::vector<std::uint8_t>>();
    std::uint32_t w = 0, h = 0;
    const bool ok = read(*pixels, w, h) && w > 0 && h > 0 && pixels->size() >= static_cast<std::size_t>(w) * h * 4;
    {
        const std::lock_guard lock(mutex_);
        writing_ -= static_cast<std::uint32_t>(due.size());
        for (auto& job : due) {
            if (!ok) {
                finishLocked(job.ticket, -1, "back-buffer read-back failed");
            } else {
                writes_.push_back(Write{std::move(job), frame, pixels, w, h});
            }
        }
        if (ok && !writer_.joinable()) {
            writer_ = std::thread([this] { writerLoop(); });
        }
    }
    work_.notify_one();
    idle_.notify_all();
}

void CaptureQueue::writerLoop() {
    SetThreadDescription(GetCurrentThread(), L"rvk screenshot writer");
    std::unique_lock lock(mutex_);
    for (;;) {
        work_.wait(lock, [this] { return stop_ || !writes_.empty(); });
        if (writes_.empty()) {
            return;  // stop_
        }
        Write w = std::move(writes_.front());
        writes_.pop_front();
        ++writing_;
        lock.unlock();
        std::string error;
        const bool ok = writeImage(w.job.file, w.job.format, *w.pixels, w.width, w.height, &error);
        if (ok) {
            spdlog::info("screenshot #{}: frame {} -> {}", w.job.ticket, w.frame, utf8(w.job.file));
        } else {
            spdlog::warn("screenshot #{}: {}: {}", w.job.ticket, utf8(w.job.file), error);
        }
        lock.lock();
        --writing_;
        finishLocked(w.job.ticket, ok ? 1 : -1,
                     ok ? std::format("frame {}, waited {} presents, {}x{}", w.frame, w.job.waited, w.width, w.height) : error);
        idle_.notify_all();
    }
}

void CaptureQueue::failWaiting(std::string_view reason) {
    {
        const std::lock_guard lock(mutex_);
        for (const auto& job : jobs_) {
            finishLocked(job.ticket, -1, std::string(reason));
        }
        jobs_.clear();
    }
    idle_.notify_all();
}

std::uint32_t CaptureQueue::pending() const {
    const std::lock_guard lock(mutex_);
    return static_cast<std::uint32_t>(jobs_.size() + writes_.size() + writing_);
}

bool CaptureQueue::waitIdle(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return idle_.wait_for(lock, timeout, [this] { return jobs_.empty() && writes_.empty() && writing_ == 0; });
}

// ---- process-wide state -------------------------------------------------------------------------------------------------

namespace {

std::atomic<Mode> g_mode{Mode::Passthrough};
fs::path g_gameDir;
std::atomic<std::uint64_t> g_frame{0};

struct Live {
    std::mutex mutex;
    FrameNumbers numbers;
    float fps = 0.0f;
    std::chrono::steady_clock::time_point windowStart{};
    std::uint64_t windowFrame = 0;
};
Live& live() {
    static Live* l = new Live;  // never destroyed: used until the process ends
    return *l;
}

}  // namespace

CaptureQueue& captures() {
    static CaptureQueue* q = new CaptureQueue;  // never destroyed: the writer thread may outlive static destruction
    return *q;
}

void setMode(Mode mode) {
    const Mode before = g_mode.exchange(mode);
    if (before == Mode::Vulkan && mode != Mode::Vulkan) {
        captures().failWaiting("the Vulkan renderer is not active (fell back to the game's D3D9 renderer)");
    }
}

Mode mode() { return g_mode.load(); }

void setGameDir(fs::path dir) { g_gameDir = std::move(dir); }

const fs::path& gameDir() { return g_gameDir; }

void publishPresent(std::uint64_t frame, const FrameNumbers& numbers) {
    auto& l = live();
    const auto now = std::chrono::steady_clock::now();
    {
        const std::lock_guard lock(l.mutex);
        l.numbers = numbers;
        if (l.windowFrame == 0 || frame < l.windowFrame) {
            l.windowStart = now;
            l.windowFrame = frame;
        } else if (const double secs = std::chrono::duration<double>(now - l.windowStart).count(); secs >= 1.0) {
            l.fps = static_cast<float>(static_cast<double>(frame - l.windowFrame) / secs);
            l.windowStart = now;
            l.windowFrame = frame;
        }
    }
    g_frame.store(frame);
}

std::uint64_t frameIndex() { return g_frame.load(); }

Snapshot snapshot() {
    auto& l = live();
    Snapshot s;
    s.frame = g_frame.load();
    const std::lock_guard lock(l.mutex);
    s.fps = l.fps;
    s.numbers = l.numbers;
    return s;
}

}  // namespace rvk::api
