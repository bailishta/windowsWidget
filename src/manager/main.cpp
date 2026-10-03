#include "../common/Engine.h"
#include "../common/BundledClock.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.Windows.Globalization.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <microsoft.ui.xaml.window.h>
#include <dwmapi.h>
#include <source_location>
#pragma comment(lib, "dwmapi.lib")
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace ww;
namespace {
fs::path data_root;
bool smoke = false;
bool validate_ui = false;
int smoke_theme = 0;
int exit_code = 0;
void ui_log(std::string_view text, LogLevel level = LogLevel::Info, std::string_view instance = {}) noexcept {
    try {
        log_file(data_root / L"logs" / L"ui.log", text, level, "manager-ui", instance);
    } catch (...) {
        OutputDebugStringA("WindowsWidget: UI diagnostic unavailable\n");
    }
}
struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
    App() {
        UnhandledException([](auto const &, UnhandledExceptionEventArgs const &event) {
            try {
                char code[32];
                sprintf_s(code, "0x%08X", unsigned(event.Exception().value));
                ui_log("Unhandled WinUI exception HRESULT=" + std::string(code) + " " +
                           to_string(event.Message()),
                       LogLevel::Fatal);
                logger().flush(std::chrono::seconds(2));
            } catch (...) {
                OutputDebugStringA("WindowsWidget: unhandled WinUI error\n");
            }
            // Leave Handled unchanged: fatal UI state is not safe to continue.
        });
    }
    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata;
    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const &type) {
        return metadata.GetXamlType(type);
    }
    Markup::IXamlType GetXamlType(hstring const &name) {
        return metadata.GetXamlType(name);
    }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return metadata.GetXmlnsDefinitions();
    }
    Window window{nullptr};
    FrameworkElement root{nullptr};
    DispatcherTimer timer{nullptr};
    std::unique_ptr<Engine> engine;
    HWND hwnd = nullptr, tray = nullptr;
    NOTIFYICONDATAW icon{};
    Handle single;
    std::string selected, instance_signature, plugin_signature;
    std::string size_signature;
    bool updating = false, quitting = false, last_dark = true;
    uint64_t launched = 0;
    bool recorded_state = false;
    bool chinese = true;
    const wchar_t *tr(const wchar_t *zh, const wchar_t *en) const {
        return localized(chinese, zh, en);
    }
    std::string plugin_name(Plugin const &plugin) const {
        return plugin.id == "org.windowswidget.clock" ? utf8(tr(L"时钟", L"Clock")) : plugin.name;
    }
    void apply_language() {
        auto language = engine->language();
        chinese = language == "zh-CN";
        set_thread_language(language);
        winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(
            to_hstring(language));
        root.Language(to_hstring(language));
        window.Title(tr(L"WindowsWidget · 桌面小组件", L"WindowsWidget · Desktop widgets"));
        struct Label {
            const wchar_t *name, *zh, *en;
        };
        for (auto const &label :
             {Label{L"Heading", L"桌面小组件", L"Desktop widgets"},
              Label{L"Subtitle", L"让常用信息，留在桌面。", L"Keep useful information on your desktop."},
              Label{L"SettingsHeading", L"组件设置", L"Widget settings"},
              Label{L"ConfigHeading", L"插件配置（JSON）", L"Plugin configuration (JSON)"},
              Label{L"SizeHeading", L"大小（桌面图标格）", L"Size (desktop icon cells)"},
              Label{L"SizeHint", L"1 格包含一个桌面图标及其周围留白。",
                    L"One cell includes a desktop icon and its surrounding space."},
              Label{L"TrayHint", L"关闭此窗口后，小组件仍在运行。\n可从系统托盘重新打开或退出。",
                    L"Widgets keep running when this window closes.\nOpen or quit from the system tray."}})
            control<TextBlock>(label.name).Text(tr(label.zh, label.en));
        for (auto const &label :
             {Label{L"AddButton", L"添加到桌面", L"Add to desktop"},
              Label{L"RefreshButton", L"重新扫描", L"Rescan"},
              Label{L"FolderButton", L"插件目录", L"Plugin folder"},
              Label{L"EnableButton", L"启用", L"Enable"}, Label{L"RetryButton", L"重试", L"Retry"},
              Label{L"RemoveButton", L"移除", L"Remove"},
              Label{L"SaveConfigButton", L"应用配置", L"Apply configuration"}})
            control<Button>(label.name).Content(box_value(tr(label.zh, label.en)));
        control<Button>(L"ApplySizeButton").Content(box_value(tr(L"应用大小", L"Apply size")));
        control<Button>(L"LogsButton").Content(box_value(tr(L"打开日志", L"Open logs")));
        control<ComboBox>(L"SizePreset").PlaceholderText(tr(L"常用大小", L"Common sizes"));
        control<NumberBox>(L"ColumnsBox").Header(box_value(tr(L"宽（格）", L"Width (cells)")));
        control<NumberBox>(L"RowsBox").Header(box_value(tr(L"高（格）", L"Height (cells)")));
        auto theme = control<ComboBox>(L"ThemePicker");
        theme.Items().GetAt(0).as<ComboBoxItem>().Content(box_value(tr(L"跟随系统", L"System theme")));
        theme.Items().GetAt(1).as<ComboBoxItem>().Content(box_value(tr(L"浅色", L"Light")));
        theme.Items().GetAt(2).as<ComboBoxItem>().Content(box_value(tr(L"深色", L"Dark")));
        auto lock = control<ToggleSwitch>(L"LockSwitch");
        lock.Header(box_value(tr(L"布局锁定", L"Lock layout")));
        lock.OffContent(box_value(tr(L"可拖动与调整尺寸", L"Move and resize")));
        lock.OnContent(box_value(tr(L"固定位置和尺寸", L"Position and size locked")));
        for (auto const &label : {Label{L"ThemePicker", L"外观主题", L"Appearance"},
                                  Label{L"PluginPicker", L"小组件插件", L"Widget plugin"},
                                  Label{L"InstanceList", L"运行实例列表", L"Widget instances"},
                                  Label{L"ConfigEditor", L"插件配置 JSON", L"Plugin configuration JSON"}})
            Automation::AutomationProperties::SetName(root.FindName(label.name).as<DependencyObject>(),
                                                      tr(label.zh, label.en));
        control<ComboBox>(L"PluginPicker").PlaceholderText(tr(L"正在发现小组件…", L"Discovering widgets…"));
        if (selected.empty())
            control<TextBlock>(L"DetailLabel")
                .Text(tr(L"添加或选择一个小组件。", L"Add or select a widget."));
        wcscpy_s(icon.szTip, tr(L"WindowsWidget · 桌面小组件", L"WindowsWidget · Desktop widgets"));
        if (tray)
            Shell_NotifyIconW(NIM_MODIFY, &icon);
        plugin_signature = "!";
        instance_signature = "!";
        refresh();
    }

    template <class T> T control(wchar_t const *name) {
        return root.FindName(name).as<T>();
    }
    template <class F> void action(F f, std::source_location location = std::source_location::current()) {
        try {
            ui_log("UI action source_line=" + std::to_string(location.line()), LogLevel::Debug, selected);
            f();
        } catch (...) {
            auto error = error_text();
            ui_log("UI action failed source_line=" + std::to_string(location.line()) + " " + error,
                   LogLevel::Error, selected);
            control<TextBlock>(L"ErrorLabel").Text(to_hstring(error));
        }
    }
    void install_clock() {
        auto source = executable_dir() / L"plugins" / L"clock";
        auto destination = data_root / L"plugins" / L"clock";
        if (install_bundled_clock(source, destination))
            log_file(data_root / L"logs" / L"manager.log",
                     "Installed bundled clock revision 3 (desktop cell sizes and compact clock)");
    }
    static LRESULT CALLBACK tray_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<App *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            if (m == RegisterWindowMessageW(L"TaskbarCreated")) {
                Shell_NotifyIconW(NIM_ADD, &self->icon);
                return 0;
            }
            if (m == WM_APP + 1) {
                if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == NIN_SELECT) {
                    ui_log("Opened from tray");
                    ShowWindow(self->hwnd, SW_SHOW);
                    SetForegroundWindow(self->hwnd);
                } else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
                    auto menu = CreatePopupMenu();
                    AppendMenuW(menu, MF_STRING, 1, self->tr(L"打开小组件管理器", L"Open widget manager"));
                    AppendMenuW(menu, MF_STRING, 2,
                                self->tr(L"退出并关闭所有小组件", L"Quit and close all widgets"));
                    POINT p;
                    GetCursorPos(&p);
                    SetForegroundWindow(h);
                    auto id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, h, nullptr);
                    DestroyMenu(menu);
                    if (id == 1) {
                        ui_log("Opened from tray menu");
                        ShowWindow(self->hwnd, SW_SHOW);
                        SetForegroundWindow(self->hwnd);
                    }
                    if (id == 2)
                        self->shutdown();
                }
                return 0;
            }
            if (m == WM_QUERYENDSESSION)
                return TRUE;
            if (m == WM_ENDSESSION && w) {
                self->shutdown();
                return 0;
            }
        }
        return DefWindowProcW(h, m, w, l);
    }
    void create_tray() {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = tray_proc;
        wc.lpszClassName = L"WindowsWidget.Tray";
        RegisterClassW(&wc);
        tray = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                               nullptr, wc.hInstance, this);
        icon.cbSize = sizeof(icon);
        icon.hWnd = tray;
        icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon.uCallbackMessage = WM_APP + 1;
        icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(icon.szTip, tr(L"WindowsWidget · 桌面小组件", L"WindowsWidget · Desktop widgets"));
        Shell_NotifyIconW(NIM_ADD, &icon);
    }
    void shutdown() {
        if (quitting)
            return;
        quitting = true;
        ui_log("Manager shutdown requested");
        if (timer)
            timer.Stop();
        Shell_NotifyIconW(NIM_DELETE, &icon);
        if (engine)
            engine->shutdown();
        if (window)
            window.Close();
        ui_log("Manager shutdown complete");
        logger().flush();
        Exit();
    }
    void OnLaunched(LaunchActivatedEventArgs const &) {
        try {
            single.reset(CreateMutexW(
                nullptr, FALSE, smoke ? L"Local\\WindowsWidget.Smoke" : L"Local\\WindowsWidget.Manager"));
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                ui_log("Another manager is running; activating existing instance");
                auto existing = FindWindowW(L"WindowsWidget.Tray", nullptr);
                if (existing)
                    PostMessageW(existing, WM_APP + 1, 0, WM_LBUTTONUP);
                Exit();
                return;
            }
            fs::create_directories(data_root / L"plugins");
            ui_log("Manager launching build=" __DATE__ " " __TIME__);
            install_clock();
            EngineOptions options;
            options.offdesktop = validate_ui;
            engine = std::make_unique<Engine>(data_root, executable_dir() / L"WidgetHost.exe", options);
            chinese = engine->language() == "zh-CN";
            set_thread_language(engine->language());
            winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(
                to_hstring(engine->language()));
            engine->start();
            Resources().MergedDictionaries().Append(XamlControlsResources());
            window = Window();
            window.Title(L"WindowsWidget · 桌面小组件");
            winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
            root =
                Markup::XamlReader::Load(to_hstring(read_file(executable_dir() / L"ui" / L"MainWindow.xaml")))
                    .as<FrameworkElement>();
            window.Content(root);
            try {
                window.SystemBackdrop(Media::MicaBackdrop());
            } catch (...) {
                ui_log("Mica backdrop unavailable: " + error_text(), LogLevel::Warning);
            }
            auto dpi = GetDpiForWindow(hwnd);
            MONITORINFO screen{sizeof(screen)};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &screen);
            SetWindowPos(hwnd, nullptr, 0, 0,
                         std::min(MulDiv(1080, dpi, 96), int(screen.rcWork.right - screen.rcWork.left)),
                         std::min(MulDiv(820, dpi, 96), int(screen.rcWork.bottom - screen.rcWork.top)),
                         SWP_NOMOVE | SWP_NOZORDER);
            window.Closed([this](auto &&, WindowEventArgs const &e) {
                if (!quitting) {
                    ui_log("Window closed; continuing in tray");
                    e.Handled(true);
                    ShowWindow(hwnd, SW_HIDE);
                }
            });
            if (!validate_ui)
                create_tray();
            control<Button>(L"AddButton").Click([this](auto &&, auto &&) {
                action([&] {
                    auto combo = control<ComboBox>(L"PluginPicker");
                    if (combo.SelectedIndex() >= 0) {
                        auto item = combo.SelectedItem().as<ComboBoxItem>();
                        engine->add(to_string(unbox_value<hstring>(item.Tag())));
                    }
                });
            });
            control<Button>(L"RefreshButton").Click([this](auto &&, auto &&) { engine->rescan(); });
            control<Button>(L"LogsButton").Click([this](auto &&, auto &&) {
                action([&] {
                    auto path = data_root / L"logs";
                    fs::create_directories(path);
                    ui_log("Open logs requested");
                    auto result = reinterpret_cast<INT_PTR>(
                        ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                    if (result <= 32)
                        throw std::runtime_error("Open logs folder failed code=" + std::to_string(result));
                });
            });
            control<Button>(L"FolderButton").Click([](auto &&, auto &&) {
                ShellExecuteW(nullptr, L"open", (data_root / L"plugins").c_str(), nullptr, nullptr,
                              SW_SHOWNORMAL);
            });
            control<ListView>(L"InstanceList").SelectionChanged([this](auto &&, auto &&) {
                if (!updating)
                    selection();
            });
            control<Button>(L"EnableButton").Click([this](auto &&, auto &&) {
                action([&] {
                    for (auto const &i : engine->instances())
                        if (i.id == selected)
                            engine->enable(selected, !i.enabled);
                });
            });
            control<Button>(L"RetryButton").Click([this](auto &&, auto &&) {
                action([&] {
                    if (!selected.empty())
                        engine->enable(selected, true);
                });
            });
            control<Button>(L"RemoveButton").Click([this](auto &&, auto &&) {
                action([&] {
                    engine->remove(selected);
                    selected.clear();
                });
            });
            control<ToggleSwitch>(L"LockSwitch").Toggled([this](auto &&, auto &&) {
                if (!updating && !selected.empty())
                    engine->lock(selected, control<ToggleSwitch>(L"LockSwitch").IsOn());
            });
            control<Button>(L"SaveConfigButton").Click([this](auto &&, auto &&) {
                action([&] {
                    if (!selected.empty()) {
                        engine->configure(selected, to_string(control<TextBox>(L"ConfigEditor").Text()));
                        control<TextBlock>(L"ErrorLabel").Text(tr(L"配置已应用", L"Configuration applied"));
                    }
                });
            });
            control<ComboBox>(L"SizePreset").SelectionChanged([this](auto &&, auto &&) {
                auto preset = control<ComboBox>(L"SizePreset").SelectedItem();
                if (!updating && preset) {
                    auto value = to_string(unbox_value<hstring>(preset.as<ComboBoxItem>().Tag()));
                    auto separator = value.find(',');
                    control<NumberBox>(L"ColumnsBox").Value(std::stoi(value.substr(0, separator)));
                    control<NumberBox>(L"RowsBox").Value(std::stoi(value.substr(separator + 1)));
                }
            });
            control<Button>(L"ApplySizeButton").Click([this](auto &&, auto &&) {
                action([&] {
                    auto columns = control<NumberBox>(L"ColumnsBox").Value();
                    auto rows = control<NumberBox>(L"RowsBox").Value();
                    if (!std::isfinite(columns) || !std::isfinite(rows) || columns < 1 || columns > 12 ||
                        rows < 1 || rows > 12 || columns != std::floor(columns) || rows != std::floor(rows))
                        throw std::runtime_error(
                            utf8(tr(L"宽高必须为 1 到 12 的整数格。",
                                    L"Width and height must be whole cells from 1 to 12.")));
                    engine->resize(selected, int(columns), int(rows));
                });
            });
            control<ComboBox>(L"ThemePicker").SelectedIndex(engine->theme_mode());
            control<ComboBox>(L"LanguagePicker").SelectedIndex(chinese ? 0 : 1);
            control<ComboBox>(L"LanguagePicker").SelectionChanged([this](auto &&, auto &&) {
                action([&] {
                    engine->set_language(control<ComboBox>(L"LanguagePicker").SelectedIndex() == 0 ? "zh-CN"
                                                                                                   : "en-US");
                    apply_language();
                });
            });
            apply_language();
            control<ComboBox>(L"ThemePicker").SelectionChanged([this](auto &&, auto &&) {
                engine->set_theme_mode(control<ComboBox>(L"ThemePicker").SelectedIndex());
                apply_theme();
            });
            if (smoke)
                control<ComboBox>(L"ThemePicker").SelectedIndex(smoke_theme);
            last_dark = !system_dark();
            apply_theme();
            if (validate_ui) {
                // Build and inspect real WinUI controls without activating a window.
                for (auto locale : {"en-US", "zh-CN"}) {
                    engine->set_language(locale);
                    apply_language();
                    check(control<TextBlock>(L"Heading").Text() == tr(L"桌面小组件", L"Desktop widgets"),
                          "Localized WinUI heading");
                    check(unbox_value<hstring>(control<Button>(L"AddButton").Content()) ==
                              tr(L"添加到桌面", L"Add to desktop"),
                          "Localized WinUI action");
                    check(control<ComboBox>(L"LanguagePicker").Items().Size() == 2,
                          "Exactly two UI languages");
                    control<ComboBox>(L"SizePreset").SelectedIndex(1);
                    check(control<NumberBox>(L"ColumnsBox").Value() == 1 &&
                              control<NumberBox>(L"RowsBox").Value() == 2,
                          "Grid size preset selects 1 by 2");
                    control<ComboBox>(L"SizePreset").SelectedIndex(4);
                    check(control<NumberBox>(L"ColumnsBox").Value() == 3 &&
                              control<NumberBox>(L"RowsBox").Value() == 2,
                          "Grid size preset selects 3 by 2");
                    check(!IsWindowVisible(hwnd), "UI validation window stays hidden");
                }
                check(unbox_value<hstring>(control<Button>(L"LogsButton").Content()) ==
                          tr(L"打开日志", L"Open logs"),
                      "Localized log folder action");
                action([] { throw std::runtime_error("diagnostic validation error"); });
                check(control<TextBlock>(L"ErrorLabel").Text() == L"diagnostic validation error",
                      "UI error remains visible while being logged");
                ui_log("PASS hidden WinUI English/Chinese controls, size presets and diagnostic error handling");
                shutdown();
                return;
            }
            timer = DispatcherTimer();
            timer.Interval(std::chrono::milliseconds(250));
            timer.Tick([this](auto &&, auto &&) {
                action([&] {
                    refresh();
                    apply_theme();
                    if (smoke && now_ms() - launched > 12000)
                        shutdown();
                });
            });
            control<TextBox>(L"ConfigEditor").IsEnabled(false);
            control<ComboBox>(L"ThemePicker").Focus(FocusState::Programmatic);
            launched = now_ms();
            timer.Start();
            window.Activate();
            ui_log("WinUI window ready");
        } catch (...) {
            auto err = error_text();
            exit_code = 1;
            ui_log("Manager startup failed: " + err, LogLevel::Fatal);
            if (!smoke)
                MessageBoxW(nullptr, wide(err).c_str(),
                            tr(L"WindowsWidget 启动失败", L"WindowsWidget failed to start"), MB_ICONERROR);
            shutdown();
        }
    }
    void apply_theme() {
        int mode = control<ComboBox>(L"ThemePicker").SelectedIndex();
        bool dark = mode == 2 || (mode == 0 && system_dark());
        root.RequestedTheme(dark ? ElementTheme::Dark : ElementTheme::Light);
        BOOL native_dark = dark;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &native_dark, sizeof(native_dark));
        if (dark != last_dark) {
            ui_log(dark ? "Effective theme=dark" : "Effective theme=light");
            last_dark = dark;
            engine->theme(dark);
        }
    }
    std::wstring status(Instance const &i) {
        if (i.status == "Running")
            return tr(L"运行中", L"Running");
        if (i.status == "Queued")
            return tr(L"等待加载", L"Queued");
        if (i.status == "Starting")
            return tr(L"正在加载", L"Loading");
        if (i.status == "Stopped")
            return tr(L"已停用", L"Stopped");
        if (i.status == "Unavailable")
            return tr(L"桌面暂不可用", L"Desktop unavailable");
        return tr(L"加载失败", L"Failed to load");
    }
    void selection() {
        auto list = control<ListView>(L"InstanceList");
        if (!list.SelectedItem())
            return;
        auto item = list.SelectedItem().as<ListViewItem>();
        auto id = to_string(unbox_value<hstring>(item.Tag()));
        bool changed = id != selected;
        selected = id;
        for (auto const &i : engine->instances())
            if (i.id == selected) {
                updating = true;
                control<TextBlock>(L"DetailLabel")
                    .Text(to_hstring(
                        i.plugin + "\n" + i.id.substr(0, 8) + " · PID " + std::to_string(i.pid) + "\n" +
                        (i.columns ? std::to_string(i.columns) + " × " + std::to_string(i.rows) +
                                         utf8(tr(L" 格", L" cells"))
                                   : utf8(tr(L"启动后换算为图标格", L"Convert to icon cells on startup")))));
                auto size = i.id + ":" + std::to_string(i.columns) + ":" + std::to_string(i.rows);
                if (size != size_signature) {
                    size_signature = size;
                    control<NumberBox>(L"ColumnsBox").Value(i.columns ? i.columns : 3);
                    control<NumberBox>(L"RowsBox").Value(i.rows ? i.rows : 2);
                    control<ComboBox>(L"SizePreset").SelectedIndex(-1);
                }
                control<Button>(L"EnableButton")
                    .Content(box_value(i.enabled ? tr(L"停用", L"Disable") : tr(L"启用", L"Enable")));
                control<ToggleSwitch>(L"LockSwitch").IsOn(i.locked);
                if (changed) {
                    control<TextBox>(L"ConfigEditor").Text(to_hstring(i.config));
                    control<ScrollViewer>(L"DetailScroll").ChangeView(nullptr, 0.0, nullptr, true);
                }
                control<TextBlock>(L"ErrorLabel").Text(to_hstring(i.error));
                updating = false;
            }
    }
    void refresh() {
        if (!recorded_state) {
            recorded_state = true;
            wchar_t title[256]{};
            GetWindowTextW(hwnd, title, 256);
            log_file(data_root / L"logs" / L"ui.log",
                     "UI tick hwnd=" + std::to_string(reinterpret_cast<uintptr_t>(hwnd)) +
                         " visible=" + std::to_string(IsWindowVisible(hwnd)) + " title=" + utf8(title));
        }
        auto plugins = engine->plugins();
        std::string ps;
        for (auto const &p : plugins)
            ps += p.id + p.error;
        auto picker = control<ComboBox>(L"PluginPicker");
        if (ps != plugin_signature) {
            plugin_signature = ps;
            picker.Items().Clear();
            for (auto const &p : plugins) {
                ComboBoxItem item;
                item.Content(
                    box_value(to_hstring(plugin_name(p) + (p.error.empty() ? "" : " · " + p.error))));
                item.Tag(box_value(to_hstring(p.id)));
                item.IsEnabled(p.error.empty());
                picker.Items().Append(item);
            }
            for (uint32_t n = 0; n < picker.Items().Size(); ++n)
                if (picker.Items().GetAt(n).as<ComboBoxItem>().IsEnabled()) {
                    picker.SelectedIndex(n);
                    break;
                }
        }
        control<Button>(L"AddButton").IsEnabled(picker.SelectedIndex() >= 0);
        if (engine->scanned() && plugins.empty())
            picker.PlaceholderText(
                tr(L"暂无插件，请打开插件目录添加", L"No plugins. Open the plugin folder to add one."));
        auto instances = engine->instances();
        std::string signature;
        for (auto const &i : instances)
            signature += i.id + i.status + i.error + std::to_string(i.locked) + std::to_string(i.enabled) +
                         ":" + std::to_string(i.columns) + ":" + std::to_string(i.rows);
        if (signature != instance_signature) {
            instance_signature = signature;
            updating = true;
            auto view = control<ListView>(L"InstanceList");
            view.Items().Clear();
            int index = 0, restore = -1;
            for (auto const &i : instances) {
                ListViewItem item;
                item.Tag(box_value(to_hstring(i.id)));
                item.HorizontalContentAlignment(HorizontalAlignment::Stretch);
                StackPanel panel;
                panel.Spacing(6);
                panel.Margin(Thickness{6, 12, 6, 12});
                TextBlock title;
                auto p = std::find_if(plugins.begin(), plugins.end(),
                                      [&](auto const &x) { return x.id == i.plugin; });
                title.Text(to_hstring(p == plugins.end() ? i.plugin : plugin_name(*p)));
                title.FontSize(18);
                TextBlock detail;
                detail.Text(status(i) + L" · " + wide(i.id.substr(0, 8)) +
                            (i.load_ms ? L" · " + std::to_wstring(i.load_ms) + L" ms" : L""));
                detail.Opacity(0.65);
                panel.Children().Append(title);
                panel.Children().Append(detail);
                item.Content(panel);
                view.Items().Append(item);
                if (i.id == selected)
                    restore = index;
                ++index;
            }
            if (restore < 0 && !instances.empty())
                restore = 0;
            view.SelectedIndex(restore);
            updating = false;
            selection();
        }
        control<TextBlock>(L"CountLabel")
            .Text(std::wstring(tr(L"运行实例 · ", L"Widget instances · ")) +
                  std::to_wstring(instances.size()));
        auto warn = engine->warning();
        if (logger().failed() > 0)
            warn = utf8(tr(L"日志写入失败，请检查日志目录权限或磁盘空间。",
                           L"Logging failed. Check log folder permissions and disk space.")) +
                   (warn.empty() ? "" : " " + warn);
        else if (logger().dropped() > 0)
            warn =
                utf8(tr(L"日志量过大，部分记录已丢弃。", L"Log queue overflow: some records were dropped.")) +
                (warn.empty() ? "" : " " + warn);
        control<TextBlock>(L"Footer").Text(warn.empty()
                                               ? tr(L"桌面网格自动对齐 · 自动保存布局",
                                                    L"Desktop grid alignment · Layout saved automatically")
                                               : to_hstring(warn));
        bool have = !selected.empty();
        for (auto name : {L"EnableButton", L"RetryButton", L"RemoveButton", L"SaveConfigButton"})
            control<Button>(name).IsEnabled(have);
        control<ToggleSwitch>(L"LockSwitch").IsEnabled(have);
        control<TextBox>(L"ConfigEditor").IsEnabled(have);
        bool editable_size = false;
        for (auto const &i : instances)
            if (i.id == selected)
                editable_size = !i.locked;
        control<ComboBox>(L"SizePreset").IsEnabled(editable_size);
        control<NumberBox>(L"ColumnsBox").IsEnabled(editable_size);
        control<NumberBox>(L"RowsBox").IsEnabled(editable_size);
        control<Button>(L"ApplySizeButton").IsEnabled(editable_size);
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        init_apartment(apartment_type::single_threaded);
        data_root = default_root();
        int count;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        for (int n = 1; n < count; ++n) {
            std::wstring_view arg = args[n];
            if (arg == L"--data-dir" && n + 1 < count) {
                std::wstring value = args[++n];
                if (value.empty())
                    throw std::runtime_error("--data-dir needs a directory path");
                auto candidate = fs::absolute(value);
                std::error_code probe;
                if (fs::exists(candidate, probe) && !fs::is_directory(candidate, probe))
                    throw std::runtime_error("--data-dir must name a directory, not an existing file");
                if (candidate == candidate.root_path())
                    throw std::runtime_error("--data-dir must not be a drive root");
                data_root = candidate;
            }
            else if (arg == L"--smoke")
                smoke = true;
            else if (arg == L"--validate-ui") {
                validate_ui = true;
                smoke = true;
            } else if (arg == L"--dark")
                smoke_theme = 2;
            else if (arg == L"--light")
                smoke_theme = 1;
            else if (arg == L"--log-level" && n + 1 < count) {
                std::wstring_view level(args[++n]);
                if (level == L"debug")
                    logger().minimum(LogLevel::Debug);
                else if (level == L"info")
                    logger().minimum(LogLevel::Info);
                else if (level == L"warning")
                    logger().minimum(LogLevel::Warning);
                else if (level == L"error")
                    logger().minimum(LogLevel::Error);
                else
                    throw std::runtime_error("Expected --log-level debug|info|warning|error");
            }
        }
        LocalFree(args);
        if (validate_ui && data_root == default_root())
            throw std::runtime_error("UI validation requires an isolated --data-dir");
        ui_log("Process start executable=" + utf8(executable_dir().wstring()) +
               " data_root=" + utf8(data_root.wstring()) + " validation=" + std::to_string(validate_ui));
        Application::Start([](auto &&) { make<App>(); });
        ui_log("Process exit code=" + std::to_string(exit_code));
        logger().flush();
        return exit_code;
    } catch (...) {
        auto error = error_text();
        ui_log("Entry point failed: " + error, LogLevel::Fatal);
        OutputDebugStringW(wide(error).c_str());
        logger().flush();
        return 1;
    }
}
