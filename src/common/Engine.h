#pragma once
#include "Model.h"
#include <condition_variable>
#include <map>
#include <optional>

namespace ww {
struct EngineOptions {
    unsigned concurrency = std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
    DWORD startup_timeout = 15000;
    bool offdesktop = false;
};
class Engine {
    struct Session {
        std::atomic<bool> cancel = false, done = false;
        std::mutex mutex;
        std::deque<JsonObject> outgoing;
        std::jthread worker;
        void enqueue(JsonObject j) {
            std::lock_guard l(mutex);
            outgoing.push_back(j);
        }
    };
    fs::path root_, host_;
    Store store_;
    EngineOptions options_;
    mutable std::mutex mutex_;
    std::vector<Plugin> plugins_;
    std::vector<Instance> instances_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    std::vector<std::shared_ptr<Session>> retired_;
    std::jthread service_;
    std::atomic<bool> closing_ = false;
    std::mutex slots_mutex_;
    std::condition_variable slots_ready_;
    unsigned active_ = 0;
    bool dirty_ = false;
    uint64_t save_due_ = 0;
    std::string warning_;
    bool scanned_ = false;
    void log(std::string_view text, LogLevel level = LogLevel::Info,
             std::string_view id = {}) const noexcept {
        try {
            log_file(root_ / L"logs" / L"manager.log", text, level, "engine", id);
        } catch (...) {
            OutputDebugStringA("WindowsWidget: engine diagnostic unavailable\n");
        }
    }
    template <class F> void update(std::string const &id, std::shared_ptr<Session> const &s, F f) {
        std::lock_guard l(mutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end() || it->second != s)
            return;
        for (auto &i : instances_)
            if (i.id == id) {
                f(i);
                break;
            }
    }
    bool slot(std::shared_ptr<Session> const &s) {
        std::unique_lock l(slots_mutex_);
        // The cancellation flags are atomics written while holding mutex_, not
        // slots_mutex_, so a notify can land between this predicate check and the
        // wait. The bounded wait makes that window harmless rather than leaving a
        // worker parked until some other session happens to release a slot.
        while (!s->cancel && !closing_ && active_ >= options_.concurrency)
            slots_ready_.wait_for(l, std::chrono::milliseconds(100));
        if (s->cancel || closing_)
            return false;
        ++active_;
        return true;
    }
    void release() {
        {
            std::lock_guard l(slots_mutex_);
            --active_;
        }
        slots_ready_.notify_one();
    }
    // Every wake condition is also a cancellation source, so a parked worker must
    // be notified whenever a session is retired or the engine starts closing;
    // otherwise it would wait for a slot that is never released again.
    void wake_slots() {
        slots_ready_.notify_all();
    }
    void mark_dirty() {
        dirty_ = true;
        save_due_ = now_ms() + 350;
    }
    void launch(Instance i, Plugin p, std::shared_ptr<Session> s) {
        bool held = false;
        Handle process, job;
        auto start = now_ms();
        try {
            // Inside the try block: an escaped exception would leave the worker
            // thread and terminate the whole manager instead of one instance.
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            if (!(held = slot(s))) {
                s->done = true;
                return;
            }
            start = now_ms();
            update(i.id, s, [](Instance &x) {
                x.status = "Starting";
                x.error.clear();
            });
            log("Starting plugin=" + p.id + " dll=" + utf8(p.dll.wstring()), LogLevel::Info, i.id);
            std::wstring name = L"\\\\.\\pipe\\WindowsWidget-" + wide(guid());
            auto pipe = Pipe::server(name);
            job.reset(CreateJobObjectW(nullptr, nullptr));
            check(bool(job), "Create job");
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            check(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
                  "Configure job");
            std::wstring cmd = L"\"" + host_.wstring() + L"\" --pipe \"" + name + L"\"";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            check(CreateProcessW(host_.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                 CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, host_.parent_path().c_str(),
                                 &si, &pi),
                  "Start host");
            process.reset(pi.hProcess);
            Handle thread(pi.hThread);
            if (!AssignProcessToJobObject(job, process)) {
                TerminateProcess(process, 1);
                check(false, "Assign host job");
            }
            check(ResumeThread(thread) != DWORD(-1), "Resume host");
            update(i.id, s, [&](Instance &x) { x.pid = pi.dwProcessId; });
            log("Host created pid=" + std::to_string(pi.dwProcessId), LogLevel::Info, i.id);
            while (!pipe.connect(100)) {
                if (s->cancel || closing_)
                    throw std::runtime_error("Cancelled");
                if (now_ms() - start > options_.startup_timeout)
                    throw std::runtime_error("Host connection timed out");
                if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
                    throw std::runtime_error("Host exited before handshake");
            }
            ULONG client = 0;
            check(GetNamedPipeClientProcessId(pipe.handle(), &client) && client == pi.dwProcessId,
                  "Unexpected pipe client");
            auto init = message("initialize", i.id, 1);
            init.SetNamedValue(L"instance", encode(i));
            put(init, L"dll", utf8(p.dll.wstring()));
            put(init, L"dataRoot", utf8((root_ / L"data" / wide(i.id)).wstring()));
            put(init, L"offdesktop", options_.offdesktop);
            put(init, L"language", language());
            {
                auto mode = theme_mode();
                put(init, L"dark", mode == 2 || (mode == 0 && system_dark()));
            }
            put(init, L"minWidth", p.min_width);
            put(init, L"minHeight", p.min_height);
            put(init, L"maxWidth", p.max_width);
            put(init, L"maxHeight", p.max_height);
            pipe.send(init);
            uint64_t seq = 1;
            bool ready = false;
            log("Handshake completed elapsed_ms=" + std::to_string(now_ms() - start), LogLevel::Info, i.id);
            while (!s->cancel && !closing_) {
                if (!ready && now_ms() - start > options_.startup_timeout)
                    throw std::runtime_error("Plugin initialization timed out");
                std::deque<JsonObject> outgoing;
                {
                    std::lock_guard l(s->mutex);
                    outgoing.swap(s->outgoing);
                }
                for (auto &j : outgoing) {
                    put(j, L"seq", double(++seq));
                    log("IPC send op=" + get(j, L"op") + " seq=" + std::to_string(seq), LogLevel::Debug,
                        i.id);
                    pipe.send(j);
                }
                JsonObject j;
                if (pipe.receive(j, 100)) {
                    if (get(j, L"id") != i.id)
                        throw std::runtime_error("Instance identity mismatch");
                    auto op = get(j, L"op");
                    if (op == "ready") {
                        if (j.GetNamedNumber(L"seq", 0) != 1)
                            throw std::runtime_error("Invalid ready correlation");
                        if (!ready) {
                            ready = true;
                            held = false;
                            release();
                        }
                        update(i.id, s, [&](Instance &x) {
                            x.status = "Running";
                            x.error.clear();
                            x.load_ms = now_ms() - start;
                        });
                        log("Ready elapsed_ms=" + std::to_string(now_ms() - start), LogLevel::Info, i.id);
                    } else if (op == "layout") {
                        auto layout = decode(j.GetNamedObject(L"instance"));
                        update(i.id, s, [&](Instance &x) {
                            if (layout.layout_revision != x.layout_revision)
                                return;
                            log("Layout accepted cells=" + std::to_string(layout.columns) + "x" +
                                    std::to_string(layout.rows) + " x=" + std::to_string(layout.x) +
                                    " y=" + std::to_string(layout.y) +
                                    " revision=" + std::to_string(layout.layout_revision),
                                LogLevel::Info, i.id);
                            x.x = layout.x;
                            x.y = layout.y;
                            x.width = layout.width;
                            x.height = layout.height;
                            x.columns = layout.columns;
                            x.rows = layout.rows;
                            x.monitor = layout.monitor;
                            mark_dirty();
                        });
                    } else if (op == "configuration") {
                        log("Plugin configuration changed", LogLevel::Info, i.id);
                        auto config = str(j.GetNamedObject(L"configuration"));
                        update(i.id, s, [&](Instance &x) {
                            x.config = config;
                            mark_dirty();
                        });
                    } else if (op == "unavailable") {
                        log("Desktop unavailable: " + get(j, L"error"), LogLevel::Warning, i.id);
                        if (!ready) {
                            held = false;
                            release();
                            ready = true;
                        }
                        update(i.id, s, [&](Instance &x) {
                            x.status = "Unavailable";
                            x.error = get(j, L"error");
                        });
                    } else if (op == "error")
                        throw std::runtime_error(get(j, L"error"));
                    else if (op == "log") {
                        auto level = j.GetNamedNumber(L"level", 1);
                        auto severity = level <= 0   ? LogLevel::Debug
                                        : level == 1 ? LogLevel::Info
                                        : level == 2 ? LogLevel::Warning
                                                     : LogLevel::Error;
                        log_file(root_ / L"logs" / L"manager.log",
                                 "host_pid=" + std::to_string(pi.dwProcessId) +
                                     " host_tid=" + std::to_string(uint32_t(j.GetNamedNumber(L"thread", 0))) +
                                     " " + get(j, L"text"),
                                 severity, "plugin", i.id);
                    }
                }
                if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
                    DWORD code = 0;
                    GetExitCodeProcess(process, &code);
                    char text[96];
                    sprintf_s(text, "Plugin host exited unexpectedly code=0x%08lX", code);
                    throw std::runtime_error(text);
                }
            }
            try {
                pipe.send(message("shutdown", i.id, ++seq));
                WaitForSingleObject(process, 1500);
            } catch (...) {
                log("Graceful host shutdown failed: " + error_text(), LogLevel::Warning, i.id);
            }
        } catch (...) {
            if (!s->cancel && !closing_) {
                auto error = error_text();
                update(i.id, s, [&](Instance &x) {
                    x.status = "Error";
                    x.error = error;
                    x.pid = 0;
                });
                log(error, LogLevel::Error, i.id);
            }
        }
        job.reset();
        if (process)
            WaitForSingleObject(process, 2000);
        if (held)
            release();
        log("Host session finished cancelled=" + std::to_string(bool(s->cancel)), LogLevel::Info, i.id);
        s->done = true;
    }
    void start_locked(Instance &i) {
        auto old = sessions_.find(i.id);
        if (old != sessions_.end()) {
            old->second->cancel = true;
            retired_.push_back(old->second);
            sessions_.erase(old);
            wake_slots();
        }
        i.pid = 0;
        i.error.clear();
        auto p = std::find_if(plugins_.begin(), plugins_.end(),
                              [&](Plugin const &x) { return x.id == i.plugin && x.error.empty(); });
        if (p == plugins_.end()) {
            i.status = "Error";
            i.error = "Plugin missing or incompatible";
            log(i.error + " plugin=" + i.plugin, LogLevel::Error, i.id);
            return;
        }
        auto s = std::make_shared<Session>();
        sessions_[i.id] = s;
        i.status = "Queued";
        auto copy = i;
        auto plugin = *p;
        s->worker = std::jthread([this, copy, plugin, s] { launch(copy, plugin, s); });
    }
    // Instances without a live host: never started yet, or parked in Error by a
    // plugin that was missing when they were last attempted. Both the startup scan
    // and a manual rescan retry them, so re-adding a plugin recovers its instances
    // without the user having to press retry on each one.
    void retry_stalled_locked() {
        for (auto &i : instances_)
            if (i.enabled && (i.status == "Stopped" || i.status == "Error"))
                start_locked(i);
    }
    void scan() {
        auto t = now_ms();
        auto p = discover(root_ / L"plugins");
        std::lock_guard l(mutex_);
        plugins_ = std::move(p);
        scanned_ = true;
        retry_stalled_locked();
        log("Discovery completed plugins=" + std::to_string(plugins_.size()) +
            " elapsed_ms=" + std::to_string(now_ms() - t));
        for (auto const &plugin : plugins_)
            if (!plugin.error.empty())
                log("Manifest rejected directory=" + utf8(plugin.directory.wstring()) +
                        " error=" + plugin.error,
                    LogLevel::Warning);
    }

  public:
    Engine(fs::path root, fs::path host, EngineOptions options = {})
        : root_(std::move(root)), host_(std::move(host)), store_(root_), options_(options) {
        options_.concurrency = std::max(1u, options_.concurrency);
        instances_ = store_.load();
        warning_ = store_.warning;
        log("Settings loaded instances=" + std::to_string(instances_.size()) +
            " concurrency=" + std::to_string(options_.concurrency));
        if (!warning_.empty())
            log(warning_, LogLevel::Warning);
    }
    ~Engine() {
        shutdown();
    }
    void start() {
        service_ = std::jthread([this](std::stop_token stop) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            try {
                scan();
            } catch (...) {
                std::lock_guard l(mutex_);
                warning_ = error_text();
                log("Discovery failed: " + warning_, LogLevel::Error);
            }
            while (!stop.stop_requested()) {
                std::vector<std::shared_ptr<Session>> reap;
                std::optional<std::vector<Instance>> snapshot;
                int theme = 0;
                std::string locale;
                {
                    std::lock_guard l(mutex_);
                    for (auto it = retired_.begin(); it != retired_.end();)
                        if ((*it)->done) {
                            reap.push_back(*it);
                            it = retired_.erase(it);
                        } else
                            ++it;
                    if (dirty_ && now_ms() >= save_due_) {
                        snapshot = instances_;
                        theme = store_.theme_mode;
                        locale = store_.language;
                        dirty_ = false;
                    }
                }
                // Disk flushing must never hold the mutex used by WinUI snapshots.
                if (snapshot)
                    try {
                        store_.save(*snapshot, theme, locale);
                        log("Settings saved instances=" + std::to_string(snapshot->size()));
                    } catch (...) {
                        std::lock_guard l(mutex_);
                        warning_ = error_text();
                        log("Settings save failed: " + warning_, LogLevel::Error);
                        dirty_ = true;
                        save_due_ = now_ms() + 5000;
                    }
                for (auto &s : reap)
                    if (s->worker.joinable())
                        s->worker.join();
                reap.clear();
                if (rescan_requested_.exchange(false))
                    try {
                        refresh();
                    } catch (...) {
                        std::lock_guard l(mutex_);
                        warning_ = error_text();
                        log("Rescan failed: " + warning_, LogLevel::Error);
                    }
                Sleep(50);
            }
        });
    }
    void rescan() {
        rescan_requested_ = true;
    }
    void refresh() { // caller must use a background thread
        auto p = discover(root_ / L"plugins");
        std::lock_guard l(mutex_);
        plugins_ = std::move(p);
        scanned_ = true;
        retry_stalled_locked();
        log("Rescan completed plugins=" + std::to_string(plugins_.size()));
        for (auto const &plugin : plugins_)
            if (!plugin.error.empty())
                log("Manifest rejected directory=" + utf8(plugin.directory.wstring()) +
                        " error=" + plugin.error,
                    LogLevel::Warning);
    }
    std::vector<Plugin> plugins() const {
        std::lock_guard l(mutex_);
        return plugins_;
    }
    std::vector<Instance> instances() const {
        std::lock_guard l(mutex_);
        return instances_;
    }
    bool scanned() const {
        std::lock_guard l(mutex_);
        return scanned_;
    }
    std::string warning() const {
        std::lock_guard l(mutex_);
        return warning_;
    }
    std::string add(std::string const &plugin, std::string config = "{}") {
        json(config);
        std::lock_guard l(mutex_);
        if (!store_.writable())
            throw std::runtime_error(warning_);
        auto p = std::find_if(plugins_.begin(), plugins_.end(),
                              [&](auto const &x) { return x.id == plugin && x.error.empty(); });
        if (p == plugins_.end())
            throw std::runtime_error("Plugin unavailable");
        Instance i;
        i.id = guid();
        i.plugin = plugin;
        i.width = p->width;
        i.height = p->height;
        i.columns = p->columns;
        i.rows = p->rows;
        i.x = 40 + 24 * double(instances_.size() % 12);
        i.y = i.x;
        i.config = config;
        instances_.push_back(i);
        log("Instance added plugin=" + plugin, LogLevel::Info, i.id);
        mark_dirty();
        start_locked(instances_.back());
        return i.id;
    }
    void enable(std::string const &id, bool value) {
        std::lock_guard l(mutex_);
        for (auto &i : instances_)
            if (i.id == id) {
                i.enabled = value;
                log(value ? "Enable/retry requested" : "Disable requested", LogLevel::Info, id);
                if (value)
                    start_locked(i);
                else {
                    auto it = sessions_.find(id);
                    if (it != sessions_.end()) {
                        it->second->cancel = true;
                        retired_.push_back(it->second);
                        sessions_.erase(it);
                        wake_slots();
                    }
                    i.status = "Stopped";
                    i.pid = 0;
                    i.error.clear();
                }
                mark_dirty();
            }
    }
    void remove(std::string const &id) {
        log("Remove requested", LogLevel::Info, id);
        enable(id, false);
        std::lock_guard l(mutex_);
        std::erase_if(instances_, [&](auto const &i) { return i.id == id; });
        mark_dirty();
    }
    void lock(std::string const &id, bool locked) {
        std::lock_guard l(mutex_);
        for (auto &i : instances_)
            if (i.id == id) {
                i.locked = locked;
                log("Layout lock=" + std::to_string(locked), LogLevel::Info, id);
                auto j = message("lock", id);
                put(j, L"locked", locked);
                auto s = sessions_.find(id);
                if (s != sessions_.end())
                    s->second->enqueue(j);
                mark_dirty();
            }
    }
    void resize(std::string const &id, int columns, int rows) {
        if (columns < 1 || columns > 12 || rows < 1 || rows > 12)
            throw std::runtime_error("Grid size must be from 1 to 12 cells");
        std::lock_guard l(mutex_);
        for (auto &i : instances_)
            if (i.id == id) {
                if (i.locked)
                    throw std::runtime_error("Unlock the layout before changing its size");
                i.columns = columns;
                i.rows = rows;
                log("Resize requested cells=" + std::to_string(columns) + "x" + std::to_string(rows),
                    LogLevel::Info, id);
                ++i.layout_revision;
                if (i.enabled && (i.status == "Error" || i.status == "Unavailable")) {
                    start_locked(i);
                    mark_dirty();
                    return;
                }
                auto j = message("resize", id);
                j.SetNamedValue(L"instance", encode(i));
                auto s = sessions_.find(id);
                if (s != sessions_.end() && !s->second->done)
                    s->second->enqueue(j);
                mark_dirty();
            }
    }
    void configure(std::string const &id, std::string const &configuration) {
        auto config = json(configuration);
        std::lock_guard l(mutex_);
        for (auto &i : instances_)
            if (i.id == id) {
                i.config = configuration;
                log("Configuration applied bytes=" + std::to_string(configuration.size()), LogLevel::Info,
                    id);
                auto j = message("configure", id);
                j.SetNamedValue(L"configuration", config);
                auto s = sessions_.find(id);
                if (s != sessions_.end())
                    s->second->enqueue(j);
                mark_dirty();
            }
    }
    int theme_mode() const {
        std::lock_guard l(mutex_);
        return store_.theme_mode;
    }
    std::string language() const {
        std::lock_guard l(mutex_);
        return store_.language;
    }
    void set_language(std::string const &value) {
        std::lock_guard l(mutex_);
        store_.language = normalize_language(value);
        log("Language=" + store_.language);
        for (auto const &[id, s] : sessions_) {
            auto j = message("language", id);
            put(j, L"language", store_.language);
            s->enqueue(j);
        }
        mark_dirty();
    }
    void set_theme_mode(int mode) {
        std::lock_guard l(mutex_);
        store_.theme_mode = std::clamp(mode, 0, 2);
        log("Theme mode=" + std::to_string(store_.theme_mode));
        mark_dirty();
    }
    void theme(bool dark) {
        std::lock_guard l(mutex_);
        for (auto const &[id, s] : sessions_) {
            auto j = message("theme", id);
            put(j, L"dark", dark);
            s->enqueue(j);
        }
    }
    void shutdown() {
        if (closing_.exchange(true))
            return;
        wake_slots();
        log("Engine shutdown begin");
        if (service_.joinable()) {
            service_.request_stop();
            service_.join();
        }
        std::vector<std::shared_ptr<Session>> all;
        {
            std::lock_guard l(mutex_);
            for (auto &[_, s] : sessions_) {
                s->cancel = true;
                all.push_back(s);
            }
            for (auto &s : retired_) {
                s->cancel = true;
                all.push_back(s);
            }
            sessions_.clear();
            retired_.clear();
        }
        for (auto &s : all)
            if (s->worker.joinable())
                s->worker.join();
        {
            std::lock_guard l(mutex_);
            if (dirty_)
                try {
                    store_.save(instances_);
                    log("Final settings saved instances=" + std::to_string(instances_.size()));
                } catch (...) {
                    warning_ = error_text();
                    log("Final settings save failed: " + warning_, LogLevel::Error);
                }
        }
        log("Engine shutdown complete");
    }

  private:
    std::atomic<bool> rescan_requested_ = false;
};
} // namespace ww
