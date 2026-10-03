#pragma once
#include <windows.h>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>

namespace companion {
// Only UTF-8 bytes cross this boundary. No WinRT object leaves the UI thread.
class AsyncLog {
    std::filesystem::path root;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> pending;
    size_t dropped = 0;
    bool stopping = false;
    std::thread worker;
    void run() noexcept {
        try {
            std::filesystem::create_directories(root);
            auto file = root / L"companion.jsonl";
            auto backup = root / L"companion.previous.jsonl";
            uintmax_t bytes = std::filesystem::exists(file) ? std::filesystem::file_size(file) : 0;
            std::ofstream stream(file, std::ios::binary | std::ios::app);
            if (!stream) throw std::runtime_error("Cannot open companion log");
            for (;;) {
                std::deque<std::string> batch;
                size_t lost = 0;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [&] { return stopping || !pending.empty() || dropped; });
                    if (stopping && pending.empty() && !dropped) break;
                    batch.swap(pending); lost = dropped; dropped = 0;
                }
                if (lost) batch.push_front("{\"event\":\"log_queue_overflow\",\"dropped\":" + std::to_string(lost) + "}");
                for (auto const& line : batch) {
                    if (bytes >= 5 * 1024 * 1024) {
                        stream.close();
                        if (std::filesystem::exists(backup)) std::filesystem::remove(backup);
                        std::filesystem::rename(file, backup);
                        stream.open(file, std::ios::binary | std::ios::trunc); bytes = 0;
                    }
                    stream << line << '\n'; bytes += line.size() + 1;
                }
                stream.flush();
                if (!stream) throw std::runtime_error("Cannot write companion log");
            }
        } catch (...) {
            OutputDebugStringW(L"Companion demo log unavailable\n");
        }
    }
public:
    explicit AsyncLog(std::filesystem::path folder) : root(std::move(folder)), worker([this] { run(); }) {}
    AsyncLog(AsyncLog const&) = delete;
    ~AsyncLog() {
        { std::lock_guard lock(mutex); stopping = true; }
        ready.notify_one();
        if (worker.joinable()) worker.join();
    }
    void write(std::string line) {
        {
            std::lock_guard lock(mutex);
            if (pending.size() < 2048) pending.push_back(std::move(line)); else ++dropped;
        }
        ready.notify_one();
    }
};
} // namespace companion
