#include "core/log.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

namespace rvk::log {

namespace {

std::vector<spdlog::sink_ptr> g_sinks;
std::shared_ptr<spdlog::logger> g_syncLogger;  // kept alive after the switch (a thread may still hold the pointer it read)

// WS22: the asynchronous logger = a sink that queues every message (level, time, thread, text) and one below-normal thread that writes them to the real sinks in order. The caller still
// formats the text (spdlog's logger does that before the sink); the file write, the OutputDebugString and the flush of every info line leave the caller. `flushSync` waits for the writer.
// Shared state: queue_ / submitted_ / processed_ / quit_ under m_ (cv_ wakes the writer, doneCv_ wakes drain); real_ is touched by the writer thread only (after start()).
class QueueSink final : public spdlog::sinks::sink {
public:
    explicit QueueSink(std::vector<spdlog::sink_ptr> real) : real_(std::move(real)) {}
    ~QueueSink() override { stop(); }
    void start() {
        worker_ = std::thread([this] { run(); });
    }
    void stop() {
        {
            const std::lock_guard lock(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    void log(const spdlog::details::log_msg& msg) override {
        push(Item{msg.level, msg.time, msg.thread_id, std::string(msg.logger_name.begin(), msg.logger_name.end()), std::string(msg.payload.begin(), msg.payload.end()), false});
    }
    void flush() override { push(Item{spdlog::level::off, {}, 0, {}, {}, true}); }
    void set_pattern(const std::string& p) override {
        for (auto& s : real_) s->set_pattern(p);
    }
    void set_formatter(std::unique_ptr<spdlog::formatter> f) override {
        for (auto& s : real_) s->set_formatter(f->clone());
    }
    // every item queued so far is written and a flush after the last one done, at most `timeoutMs`
    void drain(int timeoutMs) {
        std::unique_lock lock(m_);
        queue_.push_back(Item{spdlog::level::off, {}, 0, {}, {}, true});
        const std::uint64_t target = ++submitted_;
        cv_.notify_one();
        doneCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return processed_ >= target || quit_; });
    }

private:
    struct Item {
        spdlog::level::level_enum level;
        spdlog::log_clock::time_point time;
        std::size_t thread;
        std::string name, payload;
        bool flush;
    };
    void push(Item&& it) {
        {
            const std::lock_guard lock(m_);
            if (queue_.size() >= 200000) return;  // (a stalled disk: lines are dropped rather than the game blocked)
            queue_.push_back(std::move(it));
            ++submitted_;
        }
        cv_.notify_one();
    }
    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (const auto set = reinterpret_cast<HRESULT(WINAPI*)(HANDLE, PCWSTR)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")); set != nullptr) {
            set(GetCurrentThread(), L"rvk log");
        }
        std::deque<Item> batch;
        for (;;) {
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [&] { return !queue_.empty() || quit_; });
                if (queue_.empty() && quit_) return;
                batch.swap(queue_);
            }
            for (const Item& it : batch) {
                if (it.flush) {
                    for (auto& s : real_) s->flush();
                } else {
                    spdlog::details::log_msg m(it.time, spdlog::source_loc{}, it.name, it.level, it.payload);
                    m.thread_id = it.thread;
                    for (auto& s : real_) {
                        if (s->should_log(m.level)) s->log(m);
                    }
                }
            }
            {
                const std::lock_guard lock(m_);
                processed_ += batch.size();
            }
            doneCv_.notify_all();
            batch.clear();
        }
    }
    std::vector<spdlog::sink_ptr> real_;
    std::mutex m_;
    std::condition_variable cv_, doneCv_;
    std::deque<Item> queue_;
    std::uint64_t submitted_ = 0, processed_ = 0;
    bool quit_ = false;
    std::thread worker_;
};

std::shared_ptr<QueueSink> g_queueSink;  // never destroyed with a running thread at DLL unload (stop() is explicit; the pointer is leaked otherwise)
std::atomic<bool> g_async{false};

}  // namespace

void init(const std::filesystem::path& file, std::string_view level) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    std::vector<spdlog::sink_ptr> sinks;
    try {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file.string(), /*truncate=*/true));
    } catch (const std::exception&) {
        // no log file (read-only directory?): the debugger sink still works
    }
    sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>(/*check_debugger_present=*/true));
    g_sinks = sinks;

    auto logger = std::make_shared<spdlog::logger>("rvk", sinks.begin(), sinks.end());
    logger->set_pattern("[%H:%M:%S.%e] [%t] [%l] %v");
    logger->set_level(spdlog::level::from_str(std::string(level)));
    // the game may die without a clean shutdown (crash, TerminateProcess): flush every line from info up
    logger->flush_on(spdlog::level::info);
    g_syncLogger = logger;
    spdlog::set_default_logger(std::move(logger));
}

void startAsync() {
    if (g_async.load() || g_sinks.empty() || !g_syncLogger) return;
    if (char v[8]; GetEnvironmentVariableA("RVK_LOG_SYNC", v, sizeof(v)) != 0 && v[0] == '1') return;
    try {
        // (the real sinks keep the formatter of the synchronous logger: the queue sink hands them the finished message)
        auto* leaked = new std::shared_ptr<QueueSink>(std::make_shared<QueueSink>(g_sinks));  // (a static with a running thread would terminate at DLL unload)
        g_queueSink = *leaked;
        auto logger = std::make_shared<spdlog::logger>("rvk", g_queueSink);
        logger->set_level(g_syncLogger->level());
        logger->flush_on(spdlog::level::info);  // (a flush request in the queue: the logger thread flushes after the line)
        g_queueSink->start();
        g_async.store(true, std::memory_order_release);
        spdlog::info("log: asynchronous logger started (one below-normal thread; RVK_LOG_SYNC=1 = synchronous)");
        spdlog::default_logger()->flush();
        spdlog::set_default_logger(logger);
    } catch (const std::exception& e) {
        spdlog::warn("log: the asynchronous logger could not start: {}", e.what());
    }
}

bool asyncActive() { return g_async.load(std::memory_order_acquire); }

void flushSync() {
    if (!g_async.load(std::memory_order_acquire) || !g_queueSink) return;
    g_queueSink->drain(2000);
}

void shutdown(bool processTerminating) {
    // (a terminating process has already lost the logger thread: the ExitProcess / NtTerminateProcess hooks drained the queue before that)
    if (!processTerminating) flushSync();
    if (auto l = spdlog::default_logger()) {
        if (!processTerminating || !g_async.load()) l->flush();
    }
}

}  // namespace rvk::log
