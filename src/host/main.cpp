#include "Desktop.h"
#include "Composition.h"
#include "XamlSurface.h"
#include "../../sdk/WidgetSdk.h"
#include <windowsx.h>
#include <commctrl.h>
#include <Microsoft.UI.Dispatching.Interop.h>

using namespace ww;
namespace {
constexpr UINT inbox_message = WM_APP + 1;
struct Host {
    Pipe pipe;
    std::mutex mutex;
    std::deque<JsonObject> incoming, outgoing;
    std::jthread io;
    std::atomic<bool> disconnected = false;
    HWND control = nullptr, window = nullptr, content = nullptr;
    Desktop desktop;
    HMODULE module = nullptr;
    WidgetApi2 api{};
    WidgetHostApi2 services{};
    XamlSurface xaml;
    bool wants_xaml = false;
    void *instance = nullptr;
    Instance state;
    std::string data_dir;
    bool dark = true, offdesktop = false, quitting = false, unavailable = false, dragging = false,
         resizing = false, rebuilding = false;
    bool display_changed = false, placement_permanent = false;
    std::string last_unavailable, last_failure;
    POINT anchor{};
    RECT original{};
    UINT dpi = 96;
    HMONITOR render_monitor = nullptr;
    double minw = 140, minh = 100, maxw = 1000, maxh = 1000;
    uint64_t last_parent_check = 0;
    uint64_t last_spacing_check = 0;
    uint64_t request = 1;
    void place() {
        desktop.place(window, state, offdesktop, minw, minh, maxw, maxh);
    }
    ~Host() {
        quitting = true;
        if (io.joinable()) {
            io.request_stop();
            io.join();
        }
        destroy_plugin();
        if (window && IsWindow(window))
            DestroyWindow(window);
        if (control && IsWindow(control))
            DestroyWindow(control);
        if (module)
            FreeLibrary(module);
    }
    void send(JsonObject j) {
        std::lock_guard l(mutex);
        outgoing.push_back(j);
    }
    void event(std::string const &name) {
        send(message(name, state.id, request));
    }
    void error(std::string text, bool temporary = false) {
        // Recovery retries once per tick; an unchanged condition is reported once
        // instead of flooding the manager log with identical lines.
        auto &last = temporary ? last_unavailable : last_failure;
        if (text == last)
            return;
        last = text;
        auto j = message(temporary ? "unavailable" : "error", state.id, request);
        put(j, L"error", text);
        send(j);
    }
    // Records a placement failure with the classification that travelled with the
    // exception: only a transient one is retried by the periodic tick.
    void placement_failed(bool temporary, std::string const &failed) {
        unavailable = true;
        placement_permanent = !temporary;
        error(failed, temporary);
    }
    void mark_available() {
        unavailable = false;
        placement_permanent = false;
        last_unavailable.clear();
        last_failure.clear();
    }
    void layout_event() {
        auto j = message("layout", state.id, request);
        j.SetNamedValue(L"instance", encode(state));
        send(j);
    }
    void log_text(std::string const &text, uint32_t level) {
        auto j = message("log", state.id, request);
        put(j, L"text", text);
        put(j, L"level", double(level));
        put(j, L"thread", double(GetCurrentThreadId()));
        send(j);
    }
    void on_xaml_event(std::string const &name) {
        if (instance && api.ui_event)
            api.ui_event(instance, name.c_str());
    }
    static uint32_t __cdecl render_ui(void *context, const char *markup) {
        auto self = static_cast<Host *>(context);
        std::string failure;
        if (!self->xaml.render(markup, failure)) {
            self->log_text("Widget XAML rejected: " + failure, 3);
            return 0;
        }
        return 1;
    }
    void destroy_plugin() {
        if (instance && api.destroy) {
            api.destroy(instance);
            instance = nullptr;
            content = nullptr;
        }
        xaml.detach();
    }
    static LRESULT CALLBACK window_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Host *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Host *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self)
            try {
                return self->window_message(h, m, w, l);
            } catch (...) {
                self->error(error_text());
                ShowWindow(h, SW_HIDE);
            }
        return DefWindowProcW(h, m, w, l);
    }
    static LRESULT CALLBACK control_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Host *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Host *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self)
            try {
                if (m == inbox_message) {
                    self->drain();
                    return 0;
                }
                if (m == WM_TIMER) {
                    self->tick();
                    return 0;
                }
                if (m == WM_DISPLAYCHANGE || m == WM_SETTINGCHANGE) {
                    // Query Shell COM later, outside a sent system message.
                    self->display_changed = true;
                    return 0;
                }
            } catch (PlacementError const &e) {
                self->placement_failed(e.temporary, error_text());
                if (self->window && IsWindow(self->window))
                    ShowWindow(self->window, SW_HIDE);
            } catch (...) {
                // Not a placement problem - a plugin that rejected its configuration,
                // for example - so the widget itself is fine and must stay visible.
                // The periodic retry is deliberately not armed either: a later
                // successful placement would report the widget ready and clear the
                // error this failure is reporting.
                self->error(error_text(), true);
            }
        return DefWindowProcW(h, m, w, l);
    }
    void create_plugin() {
        // A XAML widget has no content window of its own: the host owns the island
        // and the plugin only hands over markup (see WidgetSdk.h).
        if (wants_xaml) {
            std::string failure;
            if (!xaml.attach(window, [this](std::string const &name) { on_xaml_event(name); }, failure))
                throw std::runtime_error("XAML surface unavailable: " + failure);
        }
        WidgetCreateInfo info{sizeof(info), window, dpi, dark ? 1u : 0u, state.config.c_str(),
                              reinterpret_cast<const WidgetHostApi *>(&services)};
        winrt::check_hresult(api.create(&info, &instance, &content));
        if (wants_xaml) {
            check(instance != nullptr, "XAML widget returned no instance");
            resize_content();
            return;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(content, &pid);
        check(instance && IsWindow(content) && pid == GetCurrentProcessId() && GetParent(content) == window,
              "Plugin must return a child HWND in host process");
        resize_content();
    }
    void resize_content() {
        if (!window)
            return;
        RECT r;
        GetClientRect(window, &r);
        dpi = GetDpiForWindow(window);
        render_monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        for (auto const &m : monitors())
            if (m.handle == render_monitor)
                dpi = m.dpi;
        UINT bar = MulDiv(24, dpi, 96), pad = MulDiv(5, dpi, 96);
        if (content && IsWindow(content))
            SetWindowPos(content, nullptr, pad, bar, std::max(1L, r.right - LONG(pad * 2)),
                         std::max(1L, r.bottom - LONG(bar + pad)), SWP_NOZORDER | SWP_NOACTIVATE);
        if (xaml.active())
            xaml.resize(int(std::max(1L, r.right - LONG(pad * 2))),
                        int(std::max(1L, r.bottom - LONG(bar + pad))));
        if (instance && api.layout)
            api.layout(instance, std::max(1L, r.right - LONG(pad * 2)),
                       std::max(1L, r.bottom - LONG(bar + pad)), dpi);
        int radius = MulDiv(20, dpi, 96);
        SetWindowRgn(window, CreateRoundRectRgn(0, 0, r.right + 1, r.bottom + 1, radius, radius), TRUE);
        InvalidateRect(window, nullptr, FALSE);
    }
    void create_window() {
        if (!offdesktop) {
            // Discovery and the DPI hand-off race against the same Explorer restart;
            // both failures are transient and retried by the periodic tick.
            try {
                if (!desktop.discover())
                    throw std::runtime_error(
                        "Explorer desktop unavailable; retry when desktop is ready");
                desktop.match_dpi();
            } catch (...) {
                unavailable = true;
                error(error_text(), true);
                return;
            }
        } else
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        rebuilding = true;
        struct Rebuilding {
            bool &flag;
            ~Rebuilding() {
                flag = false;
            }
        } rebuilding_guard{rebuilding};
        if (window && IsWindow(window)) {
            destroy_plugin();
            DestroyWindow(window);
        } else
            destroy_plugin();
        window = nullptr;
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WindowsWidget.Container",
                                 L"WindowsWidget", WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 260,
                                 160, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        check(window != nullptr, "Create widget container");
        try {
            initialize_widget_surface(window);
        } catch (...) {
            // Attaching to the desktop and enabling the composition surface fail
            // while Explorer restarts. Drop the half-built container so the periodic
            // recovery path retries it; keeping it would leave a hidden window that
            // no longer matches the "no container means rebuild" invariant.
            auto failed = error_text();
            DestroyWindow(window);
            window = nullptr;
            placement_failed(true, failed);
            return;
        }
        dpi = GetDpiForWindow(window);
        try {
            create_plugin();
        } catch (...) {
            // The plugin produced no usable content. Keep the container so the
            // periodic recovery does not rebuild it, and stop the placement retry:
            // without content a later successful place() would report the widget
            // ready even though nothing is behind it.
            destroy_plugin();
            unavailable = true;
            placement_permanent = true;
            if (!offdesktop)
                ShowWindow(window, SW_HIDE);
            error(error_text());
            return;
        }
        try {
            place();
        } catch (PlacementError const &e) {
            if (!e.temporary) {
                // A size the plugin's own limits can never hold stays broken on
                // every retry, so it is reported like any other load failure.
                auto failed = error_text();
                destroy_plugin();
                DestroyWindow(window);
                window = nullptr;
                placement_failed(false, failed);
                return;
            }
            // A full grid or a changed display comes right on its own. The widget is
            // already complete, so keep it and let the periodic tick retry only the
            // placement rather than rebuilding the container and reloading the
            // plugin once per second.
            unavailable = true;
            placement_permanent = false;
            if (!offdesktop)
                ShowWindow(window, SW_HIDE);
            error(error_text(), true);
            return;
        } catch (...) {
            // The desktop query and the cross-host placement mutex fail while
            // Explorer is busy or restarting; the widget is complete, so retry only
            // the placement.
            unavailable = true;
            placement_permanent = false;
            if (!offdesktop)
                ShowWindow(window, SW_HIDE);
            error(error_text(), true);
            return;
        }
        if (!offdesktop)
            ShowWindow(window, SW_SHOWNOACTIVATE);
        resize_content();
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        auto diagnostic = message("log", state.id);
        put(diagnostic, L"text",
            "composed widget surface; alpha=255; parentExStyle=" +
                std::to_string(offdesktop ? 0 : GetWindowLongPtrW(desktop.parent(), GWL_EXSTYLE)));
        send(diagnostic);
        mark_available();
        rebuilding = false;
        send(message("ready", state.id, 1));
        layout_event();
    }
    LRESULT window_message(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            auto dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            auto brush = CreateSolidBrush(dark ? RGB(32, 32, 35) : RGB(246, 246, 249));
            FillRect(dc, &r, brush);
            DeleteObject(brush);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, dark ? RGB(175, 175, 185) : RGB(100, 100, 110));
            RECT header{12, 2, r.right - 12, MulDiv(24, dpi, 96)};
            const wchar_t *header_text =
                state.locked ? localized(thread_chinese(), L"小组件 · 已锁定", L"Widget · Locked")
                             : localized(thread_chinese(), L"⠿  拖动调整位置", L"⠿  Drag to move");
            if (r.right < MulDiv(180, dpi, 96))
                header_text = state.locked ? L"▣" : L"⠿";
            DrawTextW(dc, header_text, -1, &header, DT_SINGLELINE | DT_VCENTER);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE:
            if (h == window)
                resize_content();
            return 0;
        case WM_MOVE:
            if (h == window && !rebuilding &&
                MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != render_monitor)
                resize_content();
            return 0;
        case WM_DPICHANGED:
            if (h == window) {
                resize_content();
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (!state.locked) {
                GetCursorPos(&anchor);
                GetWindowRect(h, &original);
                RECT r;
                GetClientRect(h, &r);
                resizing = GET_X_LPARAM(l) > r.right - MulDiv(14, dpi, 96) ||
                           GET_Y_LPARAM(l) > r.bottom - MulDiv(14, dpi, 96);
                dragging = true;
                SetCapture(h);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (dragging) {
                POINT p;
                GetCursorPos(&p);
                int dx = p.x - anchor.x, dy = p.y - anchor.y;
                if (resizing) {
                    auto s = dpi / 96.0;
                    int width =
                        std::clamp<int>(original.right - original.left + dx, int(minw * s), int(maxw * s));
                    int height =
                        std::clamp<int>(original.bottom - original.top + dy, int(minh * s), int(maxh * s));
                    auto cell = desktop.spacing();
                    state.columns = clamped_cells(width, cell.cx, minw, maxw, s);
                    state.rows = clamped_cells(height, cell.cy, minh, maxh, s);
                    auto extent = grid_extent(state.columns, state.rows, cell);
                    width = extent.cx;
                    height = extent.cy;
                    SetWindowPos(h, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                } else {
                    POINT pos{original.left + dx, original.top + dy};
                    if (!offdesktop)
                        MapWindowPoints(HWND_DESKTOP, desktop.parent(), &pos, 1);
                    SetWindowPos(h, nullptr, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (dragging) {
                dragging = false;
                ReleaseCapture();
                auto width = state.width, height = state.height;
                desktop.capture(h, state);
                if (!resizing) {
                    state.width = width;
                    state.height = height;
                }
                // A drop can land on a monitor with no free cell, or race an Explorer
                // restart. Both resolve on their own, so they must not escape to the
                // generic handler as a permanent failure that ends the host.
                try {
                    place();
                    resize_content();
                    layout_event();
                } catch (PlacementError const &e) {
                    placement_failed(e.temporary, error_text());
                } catch (...) {
                    placement_failed(true, error_text());
                }
            }
            return 0;
        case WM_CAPTURECHANGED:
            dragging = false;
            return 0;
        case WM_DESTROY:
            if (h == window && !quitting && !rebuilding) {
                // Explorer tore the desktop down; the plugin's content window is
                // about to go with the container. Release the plugin here, while
                // its window still exists, so the rebuild path never calls
                // destroy() on a HWND the system has already reclaimed.
                destroy_plugin();
                window = nullptr;
                content = nullptr;
                unavailable = true;
            }
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
    void drain() {
        std::deque<JsonObject> messages;
        {
            std::lock_guard l(mutex);
            messages.swap(incoming);
        }
        for (auto &j : messages) {
            check(get(j, L"id") == state.id, "Host instance mismatch");
            request = static_cast<uint64_t>(j.GetNamedNumber(L"seq", 0));
            auto op = get(j, L"op");
            if (op == "shutdown") {
                quitting = true;
                PostQuitMessage(0);
                return;
            }
            if (op == "theme") {
                dark = j.GetNamedBoolean(L"dark", true);
                xaml.theme(dark);
                if (instance && api.theme)
                    api.theme(instance, dark);
                if (window)
                    InvalidateRect(window, nullptr, FALSE);
            } else if (op == "language") {
                set_thread_language(normalize_language(get(j, L"language")));
                // v1 plugins can read GetThreadUILanguage in their UI callbacks.
                if (instance && api.theme)
                    api.theme(instance, dark);
                if (window)
                    InvalidateRect(window, nullptr, FALSE);
            } else if (op == "lock") {
                state.locked = j.GetNamedBoolean(L"locked", false);
                if (window)
                    InvalidateRect(window, nullptr, FALSE);
            } else if (op == "resize") {
                auto desired = decode(j.GetNamedObject(L"instance"));
                if (desired.layout_revision >= state.layout_revision) {
                    state.columns = desired.columns;
                    state.rows = desired.rows;
                    state.layout_revision = desired.layout_revision;
                    if (window && IsWindow(window)) {
                        place();
                        resize_content();
                        layout_event();
                        // A successful resize can recover a previously unavailable
                        // placement, but only while the plugin actually has content:
                        // otherwise this would report a widget ready with nothing in it.
                        if (unavailable && instance) {
                            mark_available();
                            send(message("ready", state.id, 1));
                        }
                    }
                    // Without a container the grid dimensions above are enough: the
                    // periodic recovery rebuilds the widget at the requested size.
                }
            } else if (op == "configure") {
                auto config = str(j.GetNamedObject(L"configuration"));
                if (instance)
                    winrt::check_hresult(api.configure(instance, config.c_str()));
                state.config = config;
            }
        }
    }
    void tick() {
        if (disconnected) {
            quitting = true;
            PostQuitMessage(1);
            return;
        }
        if (offdesktop)
            return;
        if (!dragging && now_ms() - last_spacing_check > 2000) {
            last_spacing_check = now_ms();
            if (desktop.spacing_changed())
                display_changed = true;
        }
        if (display_changed && window && IsWindow(window)) {
            display_changed = false;
            try {
                place();
                layout_event();
                if (unavailable) {
                    mark_available();
                    send(message("ready", state.id, 1));
                }
            } catch (PlacementError const &e) {
                placement_failed(e.temporary, error_text());
            } catch (...) {
                placement_failed(true, error_text());
            }
        }
        if (now_ms() - last_parent_check < 1000)
            return;
        last_parent_check = now_ms();
        if (window && IsWindow(window))
            Desktop::restack(window, desktop.parent());
        if (!desktop.valid() || !window || !IsWindow(window)) {
            if (window && IsWindow(window))
                ShowWindow(window, SW_HIDE);
            if (!unavailable) {
                unavailable = true;
                error("Explorer restarted; restoring desktop", true);
            }
            try {
                create_window();
            } catch (...) {
                // create_window reports desktop and placement failures itself and
                // returns; anything escaping it is a real host failure worth
                // surfacing as an error the user can retry explicitly.
                unavailable = true;
                if (window && IsWindow(window))
                    ShowWindow(window, SW_HIDE);
                error(error_text());
            }
        } else if (unavailable && !placement_permanent && instance) {
            // An earlier placement failed while the container stayed alive, so the
            // plugin is healthy and only the position is missing. Retry on this
            // window instead of rebuilding the plugin; a grid that has since been
            // freed or re-measured starts working again on its own.
            try {
                place();
                layout_event();
                mark_available();
                send(message("ready", state.id, 1));
            } catch (PlacementError const &e) {
                placement_failed(e.temporary, error_text());
            } catch (...) {
                placement_failed(true, error_text());
            }
        }
    }
    void run(std::wstring const &pipe_name) {
        pipe = Pipe::client(pipe_name);
        JsonObject init;
        check(pipe.receive(init, 15000), "Initialization timeout");
        check(get(init, L"op") == "initialize", "Expected initialization");
        state = decode(init.GetNamedObject(L"instance"));
        check(get(init, L"id") == state.id, "Initial identity mismatch");
        offdesktop = init.GetNamedBoolean(L"offdesktop", false);
        dark = init.GetNamedBoolean(L"dark", true);
        set_thread_language(normalize_language(get(init, L"language", default_language())));
        minw = init.GetNamedNumber(L"minWidth", 140);
        minh = init.GetNamedNumber(L"minHeight", 100);
        maxw = init.GetNamedNumber(L"maxWidth", 1000);
        maxh = init.GetNamedNumber(L"maxHeight", 1000);
        state.width = std::clamp(state.width, minw, maxw);
        state.height = std::clamp(state.height, minh, maxh);
        data_dir = get(init, L"dataRoot");
        fs::create_directories(wide(data_dir));
        services = {sizeof(services),
                    1,
                    this,
                    [](void *p, uint32_t level, const char *text) {
                        try {
                            auto self = static_cast<Host *>(p);
                            auto j = message("log", self->state.id);
                            put(j, L"text", std::string_view(text ? text : ""));
                            put(j, L"level", double(level));
                            put(j, L"thread", double(GetCurrentThreadId()));
                            self->send(j);
                        } catch (...) {
                        }
                    },
                    [](void *p, const char *config) {
                        try {
                            auto self = static_cast<Host *>(p);
                            auto value = json(config);
                            self->state.config = str(value);
                            auto j = message("configuration", self->state.id);
                            j.SetNamedValue(L"configuration", value);
                            self->send(j);
                        } catch (...) {
                        }
                    },
                    data_dir.c_str(), render_ui};
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc = window_proc;
        wc.lpszClassName = L"WindowsWidget.Container";
        RegisterClassW(&wc);
        wc.lpfnWndProc = control_proc;
        wc.lpszClassName = L"WindowsWidget.Control";
        RegisterClassW(&wc);
        control = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                                  nullptr, wc.hInstance, this);
        check(control != nullptr, "Create control window");
        SetTimer(control, 1, 250, nullptr);
        io = std::jthread([this](std::stop_token stop) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            try {
                while (!stop.stop_requested()) {
                    std::deque<JsonObject> out;
                    {
                        std::lock_guard l(mutex);
                        out.swap(outgoing);
                    }
                    for (auto const &j : out)
                        pipe.send(j);
                    JsonObject j;
                    if (pipe.receive(j, 40)) {
                        std::lock_guard l(mutex);
                        incoming.push_back(j);
                        PostMessageW(control, inbox_message, 0, 0);
                    }
                }
            } catch (...) {
                disconnected = true;
            }
        });
        try {
            // Deliberately no SetDefaultDllDirectories: restricting the process-wide
            // default search path makes WinUI's own module lookups fail later, which
            // surfaces as a fail-fast inside Microsoft.ui.xaml.dll. The flags on
            // LoadLibraryExW already give the plugin the search order it needs.
            auto dll = wide(get(init, L"dll"));
            module = LoadLibraryExW(dll.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            check(module != nullptr, "Load plugin DLL");
            auto entry = reinterpret_cast<WidgetGetApiFn>(GetProcAddress(module, "WidgetGetApi"));
            check(entry != nullptr, "WidgetGetApi export missing");
            // Seeded with the largest struct we understand; a v1 plugin overwrites
            // .size with the smaller v1 value and simply leaves the v2 fields alone.
            api.size = sizeof(api);
            api.version = 1;
            winrt::check_hresult(entry(1, reinterpret_cast<WidgetApi *>(&api)));
            check(api.version == 1 && api.size >= sizeof(WidgetApi) && api.create && api.destroy &&
                      api.configure && api.layout && api.theme,
                  "Invalid plugin ABI");
            wants_xaml = api.size >= sizeof(WidgetApi2) && (api.capabilities & WIDGET_CAPABILITY_XAML);
            if (wants_xaml)
                log_text("Widget declares XAML content; the host will own the island", 1);
            create_window();
        } catch (...) {
            error(error_text());
        }
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            // Required for a host that carries Windows App SDK UI content: this gives
            // XAML islands first refusal on keyboard accelerators and focus. It is a
            // no-op when no plugin created one.
            if (::ContentPreTranslateMessage(&msg))
                continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        destroy_plugin();
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // A bad third-party DLL must report an IPC error, never block on a system dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        int count;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        std::wstring pipe;
        for (int i = 1; i + 1 < count; ++i)
            if (std::wstring_view(args[i]) == L"--pipe")
                pipe = args[++i];
        LocalFree(args);
        if (pipe.empty())
            return 2;
        Host host;
        host.run(pipe);
        return 0;
    } catch (...) {
        OutputDebugStringW(wide(error_text()).c_str());
        return 1;
    }
}
