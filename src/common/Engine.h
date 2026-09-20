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
    unsigned active_ = 0;
    bool dirty_ = false;
    uint64_t save_due_ = 0;
    std::string warning_;
    bool scanned_ = false;
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
        while (!s->cancel && !closing_) {
            {
                std::lock_guard l(slots_mutex_);
                if (active_ < options_.concurrency) {
                    ++active_;
                    return true;
                }
            }
            Sleep(20);
        }
        return false;
    }
    void release() {
        std::lock_guard l(slots_mutex_);
        --active_;
    }
    void mark_dirty() {
        dirty_ = true;
        save_due_ = now_ms() + 350;
    }
    void launch(Instance i, Plugin p, std::shared_ptr<Session> s) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        bool held = false;
        Handle process, job;
        auto start = now_ms();
        try {
            if (!(held = slot(s))) {
                s->done = true;
                return;
            }
            start = now_ms();
            update(i.id, s, [](Instance &x) {
                x.status = "Starting";
                x.error.clear();
            });
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
            log_file(root_ / L"logs" / L"manager.log",
                     i.id + " handshake " + std::to_string(now_ms() - start) + "ms");
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
                        log_file(root_ / L"logs" / L"manager.log",
                                 i.id + " ready " + std::to_string(now_ms() - start) + "ms");
                    } else if (op == "layout") {
                        auto layout = decode(j.GetNamedObject(L"instance"));
                        update(i.id, s, [&](Instance &x) {
                            if (layout.layout_revision != x.layout_revision)
                                return;
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
                        auto config = str(j.GetNamedObject(L"configuration"));
                        update(i.id, s, [&](Instance &x) {
                            x.config = config;
                            mark_dirty();
                        });
                    } else if (op == "unavailable") {
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
                    else if (op == "log")
                        log_file(root_ / L"logs" / L"manager.log", i.id + " plugin: " + get(j, L"text"));
                }
                if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
                    throw std::runtime_error("Plugin host exited unexpectedly");
            }
            try {
                pipe.send(message("shutdown", i.id, ++seq));
                WaitForSingleObject(process, 1500);
            } catch (...) {
            }
        } catch (...) {
            if (!s->cancel && !closing_) {
                auto error = error_text();
                update(i.id, s, [&](Instance &x) {
                    x.status = "Error";
                    x.error = error;
                    x.pid = 0;
                });
                log_file(root_ / L"logs" / L"manager.log", i.id + " error: " + error);
            }
        }
        job.reset();
        if (process)
            WaitForSingleObject(process, 2000);
        if (held)
            release();
        s->done = true;
    }
    void start_locked(Instance &i) {
        auto old = sessions_.find(i.id);
        if (old != sessions_.end()) {
            old->second->cancel = true;
            retired_.push_back(old->second);
            sessions_.erase(old);
        }
        i.pid = 0;
        i.error.clear();
        auto p = std::find_if(plugins_.begin(), plugins_.end(),
                              [&](Plugin const &x) { return x.id == i.plugin && x.error.empty(); });
        if (p == plugins_.end()) {
            i.status = "Error";
            i.error = "Plugin missing or incompatible";
            return;
        }
        auto s = std::make_shared<Session>();
        sessions_[i.id] = s;
        i.status = "Queued";
        auto copy = i;
        auto plugin = *p;
        s->worker = std::jthread([this, copy, plugin, s] { launch(copy, plugin, s); });
    }
    void scan() {
        auto t = now_ms();
        auto p = discover(root_ / L"plugins");
        std::lock_guard l(mutex_);
        plugins_ = std::move(p);
        scanned_ = true;
        for (auto &i : instances_)
            if (i.enabled && i.status == "Stopped")
                start_locked(i);
        log_file(root_ / L"logs" / L"manager.log", "discovery " + std::to_string(now_ms() - t) + "ms");
    }

  public:
    Engine(fs::path root, fs::path host, EngineOptions options = {})
        : root_(std::move(root)), host_(std::move(host)), store_(root_), options_(options) {
        options_.concurrency = std::max(1u, options_.concurrency);
        instances_ = store_.load();
        warning_ = store_.warning;
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
                    } catch (...) {
                        std::lock_guard l(mutex_);
                        warning_ = error_text();
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
        mark_dirty();
        start_locked(instances_.back());
        return i.id;
    }
    void enable(std::string const &id, bool value) {
        std::lock_guard l(mutex_);
        for (auto &i : instances_)
            if (i.id == id) {
                i.enabled = value;
                if (value)
                    start_locked(i);
                else {
                    auto it = sessions_.find(id);
                    if (it != sessions_.end()) {
                        it->second->cancel = true;
                        retired_.push_back(it->second);
                        sessions_.erase(it);
                    }
                    i.status = "Stopped";
                    i.pid = 0;
                    i.error.clear();
                }
                mark_dirty();
            }
    }
    void remove(std::string const &id) {
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
                } catch (...) {
                    warning_ = error_text();
                }
        }
    }

  private:
    std::atomic<bool> rescan_requested_ = false;
};
} // namespace ww
