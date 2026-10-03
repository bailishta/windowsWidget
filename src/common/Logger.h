#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <cstdio>
#include <stdexcept>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace ww {
enum class LogLevel { Debug, Info, Warning, Error, Fatal };
inline const char *log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Fatal:
        return "FATAL";
    default:
        return "INFO";
    }
}
struct LogOptions {
    size_t queue_capacity = 4096;
    uintmax_t file_bytes = 5 * 1024 * 1024;
    unsigned backups = 3;
};
// No WinRT dependency: logging also works before XAML startup and on worker threads.
class AsyncLogger {
    struct Entry {
        std::filesystem::path path;
        std::string line;
        LogLevel level;
    };
    LogOptions options_;
    std::mutex mutex_;
    std::condition_variable ready_, drained_;
    std::deque<Entry> queue_;
    bool stopping_ = false, writing_ = false;
    uint64_t pending_dropped_ = 0;
    std::atomic<LogLevel> minimum_ = LogLevel::Info;
    std::atomic<uint64_t> failed_ = 0, dropped_ = 0;
    std::thread worker_;
    static std::string bounded(std::string_view text, size_t max) {
        if (text.size() <= max)
            return std::string(text);
        size_t end = max;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
            --end;
        return std::string(text.substr(0, end)) + "...[truncated]";
    }
    static std::string single_line(std::string_view text, size_t max) {
        std::string result;
        for (unsigned char c : bounded(text, max)) {
            if (c == '\n')
                result += "\\n";
            else if (c == '\r')
                result += "\\r";
            else if (c == '\t')
                result += "\\t";
            else if (c < 32)
                result += '?';
            else
                result += char(c);
        }
        return result;
    }
    struct NativeHandle {
        HANDLE value = nullptr;
        ~NativeHandle() {
            if (value && value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    };
    void append(Entry const &entry) {
        // Separate manager launches may briefly overlap. Serialize append + rotation
        // across processes, without making the UI thread wait for the filesystem.
        uint64_t hash = 14695981039346656037ull;
        for (wchar_t c : entry.path.lexically_normal().wstring()) {
            hash ^= uint64_t(std::towlower(c));
            hash *= 1099511628211ull;
        }
        auto name = L"Local\\WindowsWidget.Log." + std::to_wstring(hash);
        NativeHandle mutex{CreateMutexW(nullptr, FALSE, name.c_str())};
        if (!mutex.value)
            throw std::runtime_error("Log mutex");
        auto wait = WaitForSingleObject(mutex.value, 2000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            throw std::runtime_error("Log mutex timeout");
        struct Release {
            HANDLE value;
            ~Release() {
                ReleaseMutex(value);
            }
        } release{mutex.value};
        std::filesystem::create_directories(entry.path.parent_path());
        auto line = bounded(entry.line, size_t(options_.file_bytes - 32)) + "\n";
        if (std::filesystem::exists(entry.path) &&
            std::filesystem::file_size(entry.path) + line.size() > options_.file_bytes) {
            for (unsigned n = options_.backups; n > 0; --n) {
                auto destination = entry.path;
                destination += L"." + std::to_wstring(n);
                auto source = entry.path;
                if (n > 1)
                    source += L"." + std::to_wstring(n - 1);
                if (std::filesystem::exists(destination))
                    std::filesystem::remove(destination);
                if (std::filesystem::exists(source))
                    std::filesystem::rename(source, destination);
            }
        }
        NativeHandle file{CreateFileW(entry.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Log file open");
        LARGE_INTEGER end{};
        if (!SetFilePointerEx(file.value, end, nullptr, FILE_END))
            throw std::runtime_error("Log append position");
        DWORD written = 0;
        if (!WriteFile(file.value, line.data(), DWORD(line.size()), &written, nullptr) ||
            written != line.size())
            throw std::runtime_error("Log file write");
        if (entry.level >= LogLevel::Error && !FlushFileBuffers(file.value))
            throw std::runtime_error("Log file flush");
    }
    void run() noexcept {
        for (;;) {
            Entry entry;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty() && stopping_)
                    return;
                entry = std::move(queue_.front());
                queue_.pop_front();
                writing_ = true;
            }
            try {
                append(entry);
            } catch (...) {
                ++failed_;
                OutputDebugStringA("WindowsWidget: diagnostic log write failed\n");
            }
            {
                std::lock_guard lock(mutex_);
                writing_ = false;
                if (queue_.empty())
                    drained_.notify_all();
            }
        }
    }

  public:
    explicit AsyncLogger(LogOptions options = {}) : options_(options) {
        options_.queue_capacity = std::max<size_t>(1, options_.queue_capacity);
        options_.file_bytes = std::max<uintmax_t>(512, options_.file_bytes);
        options_.backups = std::clamp(options_.backups, 1u, 10u);
        worker_ = std::thread([this] { run(); });
    }
    ~AsyncLogger() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        worker_.join();
    }
    void minimum(LogLevel level) noexcept {
        minimum_ = level;
    }
    uint64_t failed() const noexcept {
        return failed_;
    }
    uint64_t dropped() const noexcept {
        return dropped_;
    }
    bool flush(std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept {
        try {
            std::unique_lock lock(mutex_);
            return drained_.wait_for(lock, timeout, [&] { return queue_.empty() && !writing_; });
        } catch (...) {
            return false;
        }
    }
    void write(std::filesystem::path const &path, std::string_view text, LogLevel level = LogLevel::Info,
               std::string_view component = "app", std::string_view instance = {}) noexcept {
        if (level < minimum_)
            return;
        try {
            SYSTEMTIME time{};
            GetSystemTime(&time);
            char prefix[180];
            sprintf_s(prefix, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [%s] pid=%lu tid=%lu uptime_ms=%llu ",
                      time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                      time.wMilliseconds, log_level_name(level), GetCurrentProcessId(), GetCurrentThreadId(),
                      GetTickCount64());
            Entry entry{path, std::string(prefix) + "[" + single_line(component, 64) + "]", level};
            if (!instance.empty())
                entry.line += " instance=" + single_line(instance, 128);
            entry.line += " " + single_line(text, 8192);
            std::lock_guard lock(mutex_);
            if (stopping_)
                return;
            if (queue_.size() >= options_.queue_capacity) {
                ++dropped_;
                ++pending_dropped_;
                auto lower = std::find_if(queue_.begin(), queue_.end(),
                                          [&](auto const &e) { return e.level < level; });
                if (lower == queue_.end())
                    return;
                queue_.erase(lower);
            }
            if (pending_dropped_) {
                entry.line += " [logger dropped=" + std::to_string(pending_dropped_) + " records]";
                pending_dropped_ = 0;
            }
            queue_.push_back(std::move(entry));
            ready_.notify_one();
        } catch (...) {
            ++failed_;
            OutputDebugStringA("WindowsWidget: cannot queue diagnostic log\n");
        }
    }
};
inline AsyncLogger &logger() {
    static AsyncLogger value;
    return value;
}
inline void log_file(std::filesystem::path const &path, std::string_view text,
                     LogLevel level = LogLevel::Info, std::string_view component = "app",
                     std::string_view instance = {}) noexcept {
    try {
        logger().write(path, text, level, component, instance);
    } catch (...) {
        OutputDebugStringA("WindowsWidget: diagnostic logger unavailable\n");
    }
}
} // namespace ww
