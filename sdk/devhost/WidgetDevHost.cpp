// Standalone development host for WindowsWidget plugins.
//
// It loads a plugin exactly the way WidgetHost.exe does - the same manifest rules,
// the same composition surface, the same create/layout/theme/configure order - but
// inside an ordinary window, with the plugin's host->log output on screen and on
// stdout. Nothing here touches Explorer, the desktop grid, the placement mutex or
// the manager's IPC, so a plugin can be iterated on without running the manager.
//
// Two ways to use it:
//   WidgetDevHost.exe <插件目录>                     interactive preview
//   WidgetDevHost.exe <插件目录> --shot out.bmp      offscreen render, then exit
//
// The offscreen path is scriptable and is what the repository's own test suite
// uses, so a plugin's rendering can be checked without a desktop session.

#include "../../src/common/Common.h"
#include "../../src/common/Model.h"
#include "../../src/common/Language.h"
#include "../../src/host/Composition.h"
#include "../../src/host/XamlSurface.h"
#include "../../sdk/WidgetSdk.h"
#include <commctrl.h>
#include <Microsoft.UI.Dispatching.Interop.h>
#include <winrt/base.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Content.h>
#include <cstdio>
#include <iostream>

using namespace ww;

namespace {
constexpr wchar_t kMainClass[] = L"WindowsWidget.DevHost";
constexpr wchar_t kContainerClass[] = L"WindowsWidget.DevHost.Container";
constexpr int kPreviewInset = 16;
constexpr size_t kLogLimit = 256 * 1024;

enum : int {
    idPreset = 1001,
    idApplySize,
    idTheme,
    idLanguage,
    idRebuild,
    idShot,
    idRescan,
    idApplyConfig,
    idConfig,
    idLog,
};

struct Preset {
    const wchar_t *label;
    int columns, rows;
};
const Preset kPresets[] = {
    {L"1 × 1", 1, 1}, {L"1 × 2", 1, 2}, {L"2 × 1", 2, 1},
    {L"2 × 2", 2, 2}, {L"3 × 2", 3, 2}, {L"3 × 3", 3, 3},
    {L"4 × 2", 4, 2}, {L"6 × 4", 6, 4},
};

struct Dev {
    fs::path plugins_root, data_root, shot;
    std::string wanted_id, config = "{}", data_dir;
    bool dark = true, chinese = true, layered = true, quiet = false, own_window = true;
    bool rebuilding = false, loaded = false;
    int seconds = 0; // 0 = stay open until the window is closed
    HWND main{}, container{}, content{}, log{}, config_edit{}, info{};
    HMODULE module{};
    WidgetApi2 api{};
    WidgetHostApi2 services{};
    XamlSurface xaml;
    bool wants_xaml = false;
    void *instance{};
    Plugin plugin;
    Instance state;
    uint32_t dpi = 96;
    int columns = 3, rows = 2;
    SIZE cell{80, 100};
};
Dev g;

// ── diagnostics ───────────────────────────────────────────────────────────────

void append_log(std::wstring const &line) {
    if (!g.log)
        return;
    auto length = GetWindowTextLengthW(g.log);
    if (size_t(length) * sizeof(wchar_t) > kLogLimit) {
        SendMessageW(g.log, EM_SETSEL, 0, length / 4);
        SendMessageW(g.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
    length = GetWindowTextLengthW(g.log);
    SendMessageW(g.log, EM_SETSEL, length, length);
    SendMessageW(g.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void log_line(std::string const &text, LogLevel level = LogLevel::Info) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char stamp[32];
    sprintf_s(stamp, "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    std::wstring line =
        wide(stamp) + L"[" + wide(log_level_name(level)) + L"] " + wide(text) + L"\r\n";
    append_log(line);
    if (!g.quiet)
        std::cout << stamp << "[" << log_level_name(level) << "] " << text << std::endl;
}

void log_plugin(void *, uint32_t level, const char *text) {
    auto severity = level <= 0   ? LogLevel::Debug
                    : level == 1 ? LogLevel::Info
                    : level == 2 ? LogLevel::Warning
                                 : LogLevel::Error;
    log_line("plugin: " + std::string(text ? text : ""), severity);
}

void log_configuration(void *, const char *configuration) {
    try {
        auto value = json(configuration ? configuration : "{}");
        g.config = str(value);
        if (g.config_edit)
            SetWindowTextW(g.config_edit, wide(g.config).c_str());
        log_line("插件回传 configuration_changed，已写回配置框：" + g.config);
    } catch (...) {
        log_line("插件回传了无效的配置 JSON：" + error_text(), LogLevel::Error);
    }
}

// ── plugin lifecycle ──────────────────────────────────────────────────────────

void destroy_plugin() {
    if (g.instance && g.api.destroy) {
        g.api.destroy(g.instance);
        g.instance = nullptr;
        g.content = nullptr;
        log_line("destroy 已调用");
    }
    g.xaml.detach();
}

void on_xaml_event(std::string const &name) {
    if (g.instance && g.api.ui_event)
        g.api.ui_event(g.instance, name.c_str());
}

uint32_t __cdecl render_ui(void *context, const char *markup) {
    auto self = static_cast<Dev *>(context);
    std::string failure;
    if (!self->xaml.render(markup, failure)) {
        log_line("XAML 被拒绝：" + failure, LogLevel::Error);
        return 0;
    }
    log_line("宿主已用 XAML 呈现组件界面");
    return 1;
}

SIZE preview_extent() {
    // Scale the cell the same way the off-desktop host does, so the preview keeps
    // its grid proportions under per-monitor DPI.
    return SIZE{g.columns * MulDiv(g.cell.cx, int(g.dpi), 96),
                g.rows * MulDiv(g.cell.cy, int(g.dpi), 96)};
}

void layout_plugin() {
    if (!g.content || !IsWindow(g.content))
        return;
    RECT r{};
    GetClientRect(g.container, &r);
    // Same band arithmetic as the production host so the plugin sees the same
    // content rectangle it will get on the desktop.
    UINT bar = MulDiv(24, g.dpi, 96), pad = MulDiv(5, g.dpi, 96);
    int width = std::max(1L, r.right - LONG(pad * 2));
    int height = std::max(1L, r.bottom - LONG(bar + pad));
    SetWindowPos(g.content, nullptr, pad, bar, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g.xaml.active())
        g.xaml.resize(width, height);
    if (g.api.layout)
        g.api.layout(g.instance, uint32_t(width), uint32_t(height), g.dpi);
    InvalidateRect(g.container, nullptr, FALSE);
}

void notify_theme() {
    g.xaml.theme(g.dark);
    if (g.instance && g.api.theme)
        g.api.theme(g.instance, g.dark ? 1u : 0u);
    InvalidateRect(g.container, nullptr, FALSE);
}

bool create_plugin() {
    if (!g.module || !g.api.create)
        return false;
    RECT r{};
    GetClientRect(g.container, &r);
    SetThreadUILanguage(g.chinese ? MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)
                                 : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    if (g.wants_xaml) {
        std::string failure;
        if (!g.xaml.attach(g.container, [](std::string const &name) { on_xaml_event(name); }, failure)) {
            log_line("XAML 面不可用：" + failure, LogLevel::Error);
            return false;
        }
    }
    WidgetCreateInfo info{sizeof(info),
                          g.container,
                          g.dpi,
                          g.dark ? 1u : 0u,
                          g.config.c_str(),
                          reinterpret_cast<const WidgetHostApi *>(&g.services)};
    auto hr = g.api.create(&info, &g.instance, &g.content);
    if (FAILED(hr)) {
        log_line("create 返回失败 HRESULT=0x" + std::to_string(uint32_t(hr)) + "（" + error_text() + "）",
                 LogLevel::Error);
        return false;
    }
    if (g.wants_xaml) {
        if (!g.instance) {
            log_line("create 返回成功但没有实例句柄", LogLevel::Error);
            return false;
        }
        layout_plugin();
        notify_theme();
        return true;
    }
    if (!g.instance || !g.content || !IsWindow(g.content)) {
        log_line("create 返回成功，但没有给出有效的子窗口句柄", LogLevel::Error);
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(g.content, &pid);
    if (pid != GetCurrentProcessId() || GetParent(g.content) != g.container) {
        log_line("create 返回的窗口必须是本进程内、以传入 HWND 为父窗口的 WS_CHILD 子窗口",
                 LogLevel::Error);
        return false;
    }
    log_line("create 成功：content=" + std::to_string(reinterpret_cast<uintptr_t>(g.content)));
    layout_plugin();
    notify_theme();
    return true;
}

bool ensure_container() {
    RECT r{};
    if (g.container && IsWindow(g.container))
        GetWindowRect(g.container, &r);
    if (g.container && IsWindow(g.container))
        DestroyWindow(g.container);
    g.container = nullptr;
    auto extent = preview_extent();
    // Child coordinates are relative to the parent's client area, not the screen.
    g.container = CreateWindowExW(WS_EX_TOOLWINDOW, kContainerClass, L"", WS_CHILD | WS_VISIBLE,
                                  kPreviewInset, kPreviewInset + 34, extent.cx, extent.cy,
                                  g.main, nullptr, GetModuleHandleW(nullptr), &g);
    if (!g.container) {
        log_line("无法创建预览容器窗口", LogLevel::Error);
        return false;
    }
    if (g.layered)
        try {
            initialize_widget_surface(g.container);
        } catch (...) {
            log_line("启用合成表面失败（可用 --no-layered 跳过）：" + error_text(), LogLevel::Warning);
        }
    return true;
}

bool rebuild(bool reload_dll) {
    g.rebuilding = true;
    destroy_plugin();
    if (reload_dll && g.module) {
        FreeLibrary(g.module);
        g.module = nullptr;
    }
    if (!ensure_container()) {
        g.rebuilding = false;
        return false;
    }
    if (reload_dll) {
        // No SetDefaultDllDirectories here: restricting the process-wide default
        // search path breaks WinUI's own module/resource lookups later on.
        g.module = LoadLibraryExW(g.plugin.dll.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!g.module) {
            log_line("加载 DLL 失败（Win32=" + std::to_string(GetLastError()) + "）：" +
                         utf8(g.plugin.dll.wstring()),
                     LogLevel::Error);
            g.rebuilding = false;
            return false;
        }
        auto entry = reinterpret_cast<WidgetGetApiFn>(GetProcAddress(g.module, "WidgetGetApi"));
        if (!entry) {
            log_line("DLL 没有导出 WidgetGetApi", LogLevel::Error);
            g.rebuilding = false;
            return false;
        }
        g.api = WidgetApi2{};
        g.api.size = sizeof(g.api);
        g.api.version = WIDGET_ABI_VERSION;
        // Seeded with the largest struct we understand; a v1 plugin overwrites
        // .size with the smaller value and leaves the v2 fields untouched.
        auto hr = entry(WIDGET_ABI_VERSION, reinterpret_cast<WidgetApi *>(&g.api));
        if (FAILED(hr)) {
            log_line("WidgetGetApi 拒绝了 ABI 版本 " + std::to_string(WIDGET_ABI_VERSION) +
                         "（HRESULT=0x" + std::to_string(uint32_t(hr)) + "）",
                     LogLevel::Error);
            g.rebuilding = false;
            return false;
        }
        if (g.api.version != WIDGET_ABI_VERSION || g.api.size < sizeof(WidgetApi) || !g.api.create ||
            !g.api.destroy || !g.api.configure || !g.api.layout || !g.api.theme) {
            log_line("插件回填的 WidgetApi 不完整：version=" + std::to_string(g.api.version) +
                         " size=" + std::to_string(g.api.size),
                     LogLevel::Error);
            g.rebuilding = false;
            return false;
        }
        g.wants_xaml =
            g.api.size >= sizeof(WidgetApi2) && (g.api.capabilities & WIDGET_CAPABILITY_XAML) != 0;
        log_line("ABI 协商完成：version=" + std::to_string(g.api.version) +
                 " size=" + std::to_string(g.api.size) +
                 (g.wants_xaml ? "，组件声明 XAML 界面" : ""));
    }
    g.rebuilding = false;
    return create_plugin();
}

bool select_plugin() {
    auto found = discover(g.plugins_root);
    if (found.empty()) {
        log_line("在 " + utf8(g.plugins_root.wstring()) +
                     " 下没有找到任何含 widget.json 的插件目录",
                 LogLevel::Error);
        return false;
    }
    log_line("发现 " + std::to_string(found.size()) + " 个插件目录：");
    for (auto const &p : found)
        log_line("  · " + p.id + "（" + p.name + "）" + (p.error.empty() ? " 可用" : " 不可用：" + p.error),
                 p.error.empty() ? LogLevel::Info : LogLevel::Warning);
    Plugin const *picked = nullptr;
    for (auto const &p : found) {
        if (!g.wanted_id.empty() && p.id == g.wanted_id) {
            picked = &p;
            break;
        }
        if (g.wanted_id.empty() && p.error.empty() && !picked)
            picked = &p;
    }
    if (!picked) {
        log_line(g.wanted_id.empty() ? "没有可用的插件；上面已列出每个目录被拒绝的原因"
                                     : "没有找到 id 为 " + g.wanted_id + " 的插件",
                 LogLevel::Error);
        return false;
    }
    g.plugin = *picked;
    g.columns = g.plugin.columns > 0 ? g.plugin.columns : 3;
    g.rows = g.plugin.rows > 0 ? g.plugin.rows : 2;
    g.state.id = "dev-host";
    g.state.plugin = g.plugin.id;
    g.state.columns = g.columns;
    g.state.rows = g.rows;
    log_line("已选择 " + g.plugin.id + "，DLL=" + utf8(g.plugin.dll.wstring()));
    return true;
}

// ── offscreen rendering ───────────────────────────────────────────────────────

bool write_bmp(fs::path const &file, int width, int height, void const *pixels) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = width;
    ih.biHeight = height; // positive: BMP rows are stored bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    auto stride = size_t(width) * 4;
    ih.biSizeImage = DWORD(stride * size_t(height));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    std::ofstream out(file, std::ios::binary);
    if (!out)
        return false;
    out.write(reinterpret_cast<char const *>(&fh), sizeof(fh));
    out.write(reinterpret_cast<char const *>(&ih), sizeof(ih));
    auto source = static_cast<char const *>(pixels);
    for (int y = height - 1; y >= 0; --y)
        out.write(source + size_t(y) * stride, stride);
    return bool(out);
}

bool save_shot() {
    if (!g.content || !IsWindow(g.content)) {
        log_line("没有可渲染的插件窗口", LogLevel::Error);
        return false;
    }
    RECT r{};
    GetClientRect(g.content, &r);
    int width = r.right, height = r.bottom;
    if (width <= 0 || height <= 0) {
        log_line("插件窗口尺寸为空：" + std::to_string(width) + "x" + std::to_string(height),
                 LogLevel::Error);
        return false;
    }
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        log_line("无法创建离屏位图", LogLevel::Error);
        return false;
    }
    // Prefill with the container colour so GDI output that leaves pixels untouched
    // is still visible in the saved image.
    auto pixels = static_cast<uint32_t *>(bits);
    std::fill_n(pixels, size_t(width) * size_t(height), g.dark ? 0x00202023u : 0x00f9f6f6u);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    SendMessageW(g.content, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    GdiFlush();
    SelectObject(memory, previous);
    auto total = size_t(width) * size_t(height);
    bool blank = std::all_of(pixels, pixels + total, [&](uint32_t p) { return p == pixels[0]; });
    if (blank)
        log_line("截图整幅同色：插件很可能没有处理 WM_PRINTCLIENT，"
                 "DefWindowProc 不会为自定义内容做离屏绘制（时钟插件可用作参考）",
                 LogLevel::Warning);
    auto ok = write_bmp(g.shot, width, height, bits);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!ok) {
        log_line("写入截图失败：" + utf8(g.shot.wstring()), LogLevel::Error);
        return false;
    }
    log_line("已保存截图 " + utf8(g.shot.wstring()) + "（" + std::to_string(width) + "×" +
             std::to_string(height) + "，32bpp BMP）");
    return true;
}

// ── window plumbing ───────────────────────────────────────────────────────────

LRESULT CALLBACK container_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        auto dc = BeginPaint(h, &ps);
        RECT r{};
        GetClientRect(h, &r);
        auto brush = CreateSolidBrush(g.dark ? RGB(32, 32, 35) : RGB(246, 246, 249));
        FillRect(dc, &r, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g.dark ? RGB(175, 175, 185) : RGB(100, 100, 110));
        RECT header{12, 2, r.right - 12, MulDiv(24, g.dpi, 96)};
        DrawTextW(dc, L"⠿  预览容器（与桌面上的拖动栏区域一致）", -1, &header,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void apply_size() {
    g.state.columns = g.columns;
    g.state.rows = g.rows;
    ensure_container();
    layout_plugin();
    log_line("已应用大小 " + std::to_string(g.columns) + " × " + std::to_string(g.rows) + " 格（" +
             std::to_string(g.columns * g.cell.cx) + "×" + std::to_string(g.rows * g.cell.cy) + " 像素）");
}

void apply_config() {
    if (!g.config_edit)
        return;
    auto length = GetWindowTextLengthW(g.config_edit);
    std::wstring text(size_t(length) + 1, L'\0');
    GetWindowTextW(g.config_edit, text.data(), length + 1);
    text.resize(size_t(length));
    try {
        auto value = json(utf8(text));
        g.config = str(value);
    } catch (...) {
        log_line("配置框里的内容不是合法 JSON：" + error_text(), LogLevel::Error);
        return;
    }
    if (g.instance && g.api.configure) {
        auto hr = g.api.configure(g.instance, g.config.c_str());
        if (FAILED(hr)) {
            log_line("configure 返回失败 HRESULT=0x" + std::to_string(uint32_t(hr)), LogLevel::Error);
            return;
        }
    }
    log_line("已应用配置 " + g.config);
}

void command(int id) {
    switch (id) {
    case idApplySize: {
        auto combo = GetDlgItem(g.main, idPreset);
        auto index = int(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        if (index >= 0 && index < int(std::size(kPresets))) {
            g.columns = kPresets[index].columns;
            g.rows = kPresets[index].rows;
        }
        apply_size();
        break;
    }
    case idTheme:
        g.dark = !g.dark;
        SetWindowTextW(GetDlgItem(g.main, idTheme), g.dark ? L"深色" : L"浅色");
        notify_theme();
        layout_plugin();
        log_line(std::string("主题切换为 ") + (g.dark ? "深色" : "浅色"));
        break;
    case idLanguage:
        g.chinese = !g.chinese;
        SetWindowTextW(GetDlgItem(g.main, idLanguage), g.chinese ? L"中文" : L"English");
        log_line(std::string("线程 UI 语言切换为 ") + (g.chinese ? "zh-CN" : "en-US") +
                 "（宿主按契约会再调用一次 theme 让插件刷新）");
        notify_theme();
        break;
    case idRebuild:
        log_line("重建：销毁插件与容器，再重新加载（与 Explorer 重启时的恢复路径一致）");
        rebuild(true);
        break;
    case idShot:
        if (g.shot.empty())
            g.shot = executable_dir() / L"widget-shot.bmp"; // toolbar use writes next to the tool
        save_shot();
        break;
    case idRescan:
        log_line("重新扫描插件目录");
        select_plugin();
        rebuild(true);
        break;
    case idApplyConfig:
        apply_config();
        break;
    default:
        break;
    }
}

void layout_controls(HWND h) {
    RECT r{};
    GetClientRect(h, &r);
    int margin = 12, toolbar = 34, config = 92;
    int width = r.right - margin * 2;
    int y = margin;
    int x = margin;
    auto place = [&](HWND control, int w) {
        SetWindowPos(control, nullptr, x, y, w, toolbar, SWP_NOZORDER | SWP_NOACTIVATE);
        x += w + 8;
    };
    place(GetDlgItem(h, idPreset), 110);
    place(GetDlgItem(h, idApplySize), 92);
    place(GetDlgItem(h, idTheme), 72);
    place(GetDlgItem(h, idLanguage), 88);
    place(GetDlgItem(h, idRebuild), 72);
    place(GetDlgItem(h, idShot), 72);
    place(GetDlgItem(h, idRescan), 96);
    place(GetDlgItem(h, idApplyConfig), 92);
    y += toolbar + 8;
    int bottom = int(r.bottom);
    int log_height = std::max(140, (bottom - y - config - margin) / 3);
    int preview_height = std::max(0, bottom - y - config - margin - log_height - 8);
    if (g.container && IsWindow(g.container))
        SetWindowPos(g.container, nullptr, margin + kPreviewInset, y + kPreviewInset, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    y += preview_height + 8;
    SetWindowPos(g.config_edit, nullptr, margin, y, width, config, SWP_NOZORDER | SWP_NOACTIVATE);
    y += config + 8;
    SetWindowPos(g.log, nullptr, margin, y, width, std::max(0, bottom - y - margin),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        g.dpi = GetDpiForWindow(h);
        layout_controls(h);
        return 0;
    case WM_COMMAND:
        if (HIWORD(w) == BN_CLICKED || HIWORD(w) == CBN_SELCHANGE)
            command(LOWORD(w));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, m, w, l);
    }
}

void register_classes() {
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = container_proc;
    wc.lpszClassName = kContainerClass;
    RegisterClassW(&wc);
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = kMainClass;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
}

HWND create_control(int id, wchar_t const *cls, wchar_t const *text, DWORD style) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.main,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

void build_ui() {
    g.main = CreateWindowExW(0, kMainClass, L"WindowsWidget 插件开发宿主",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 880, 720,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g.main)
        return;
    auto combo = create_control(idPreset, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL);
    for (auto const &preset : kPresets)
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.label));
    SendMessageW(combo, CB_SETCURSEL, 4, 0); // 3 × 2
    create_control(idApplySize, L"BUTTON", L"应用大小", BS_PUSHBUTTON);
    create_control(idTheme, L"BUTTON", g.dark ? L"深色" : L"浅色", BS_PUSHBUTTON);
    create_control(idLanguage, L"BUTTON", g.chinese ? L"中文" : L"English", BS_PUSHBUTTON);
    create_control(idRebuild, L"BUTTON", L"重建", BS_PUSHBUTTON);
    create_control(idShot, L"BUTTON", L"截图", BS_PUSHBUTTON);
    create_control(idRescan, L"BUTTON", L"重新扫描", BS_PUSHBUTTON);
    create_control(idApplyConfig, L"BUTTON", L"应用配置", BS_PUSHBUTTON);
    g.config_edit = create_control(idConfig, L"EDIT", L"{}",
                                   WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN);
    g.log = create_control(idLog, L"EDIT", L"",
                           WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL |
                               ES_READONLY);
    SendMessageW(g.log, EM_SETLIMITTEXT, DWORD(kLogLimit), 0);
    layout_controls(g.main);
}

// ── command line ──────────────────────────────────────────────────────────────

bool attach_console() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return false;
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    return true;
}

// Process-wide XAML start-up, done by the host before any plugin loads. Creating
// the Application object from inside a plugin DLL was measured to fail-fast inside
// Microsoft.ui.xaml.dll's activation factory, while the same code in an .exe works.
struct DevApp : winrt::Microsoft::UI::Xaml::ApplicationT<DevApp,
                    winrt::Microsoft::UI::Xaml::Markup::IXamlMetadataProvider> {
    winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata;
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(
        winrt::Windows::UI::Xaml::Interop::TypeName const &type) {
        return metadata.GetXamlType(type);
    }
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(winrt::hstring const &name) {
        return metadata.GetXamlType(name);
    }
    winrt::com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return metadata.GetXmlnsDefinitions();
    }
};

winrt::Microsoft::UI::Xaml::Application g_xaml_app{nullptr};

void start_xaml() {
    try {
        if (!winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread()) {
            static auto controller =
                winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();
            (void)controller;
        }
        if (!g_xaml_app)
            g_xaml_app = winrt::make<DevApp>();
        log_line("宿主已初始化 XAML 运行时");
    } catch (...) {
        log_line("宿主初始化 XAML 失败：" + error_text(), LogLevel::Warning);
    }
}

// Diagnostic: create the island from the host process (exe) rather than from the
// plugin DLL. Measured behaviour decides where island creation has to live.
bool host_island_probe() {
    try {
        auto id = winrt::Microsoft::UI::GetWindowIdFromWindow(g.container);
        static winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource island{nullptr};
        island = winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource();
        log_line("宿主：island 已构造");
        island.Initialize(id);
        log_line("宿主：island Initialize 成功");
        winrt::Microsoft::UI::Xaml::Controls::Grid root;
        winrt::Microsoft::UI::Xaml::Controls::TextBlock text;
        text.Text(L"HOST ISLAND OK");
        root.Children().Append(text);
        island.Content(root);
        log_line("宿主：island 内容已挂上");
        return true;
    } catch (winrt::hresult_error const &e) {
        log_line("宿主：island 失败 " + winrt::to_string(e.message()), LogLevel::Error);
    } catch (...) {
        log_line("宿主：island 失败 " + error_text(), LogLevel::Error);
    }
    return false;
}

} // namespace

// Outside the anonymous namespace: the CRT needs external linkage to find it.
int wmain(int argc, wchar_t **argv) {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        bool have_root = false;
        for (int n = 1; n < argc; ++n) {
            std::wstring_view arg = argv[n];
            auto next = [&]() -> std::wstring {
                return n + 1 < argc ? std::wstring(argv[++n]) : std::wstring();
            };
            if (arg == L"--id")
                g.wanted_id = utf8(next());
            else if (arg == L"--shot")
                g.shot = fs::absolute(next());
            else if (arg == L"--config")
                g.config = utf8(next());
            else if (arg == L"--data")
                g.data_root = fs::absolute(next());
            else if (arg == L"--seconds")
                g.seconds = _wtoi(next().c_str());
            else if (arg == L"--dark")
                g.dark = true;
            else if (arg == L"--light")
                g.dark = false;
            else if (arg == L"--zh")
                g.chinese = true;
            else if (arg == L"--en")
                g.chinese = false;
            else if (arg == L"--no-layered")
                g.layered = false;
            else if (arg == L"--quiet")
                g.quiet = true;
            else if (arg == L"--help" || arg == L"-h") {
                std::cout << "WidgetDevHost.exe <插件目录> [选项]\n"
                             "  --id <插件id>      选择插件，默认取第一个可用的\n"
                             "  --shot <文件.bmp>  离屏渲染后写出 BMP 并退出（不显示窗口）\n"
                             "  --config <json>    传给插件的初始配置，默认 {}\n"
                             "  --data <目录>      插件的实例数据目录，默认 <exe>/devdata\n"
                             "  --dark | --light   起始主题，默认深色\n"
                             "  --zh | --en        起始线程 UI 语言，默认中文\n"
                             "  --no-layered       不启用与生产一致的 WS_EX_LAYERED 合成表面\n"
                             "  --quiet            不向控制台输出，只写界面日志\n"
                          << std::endl;
                return 0;
            } else if (!arg.empty() && arg[0] != L'-') {
                g.plugins_root = fs::absolute(std::wstring(arg));
                have_root = true;
            }
        }
        if (!have_root) {
            std::cerr << "需要一个插件目录作为参数；用 --help 查看用法。" << std::endl;
            return 2;
        }
        // Attaching a console is only about double-click launches; when stdout is
        // redirected to a pipe or a file the writes work regardless.
        attach_console();
        start_xaml();
        register_classes();
        build_ui();
        if (!g.main) {
            std::cerr << "无法创建开发宿主窗口" << std::endl;
            return 1;
        }
        if (g.data_root.empty())
            g.data_root = executable_dir() / L"devdata";
        log_line("开发宿主启动：插件目录=" + utf8(g.plugins_root.wstring()));
        if (GetEnvironmentVariableW(L"WW_HOST_ISLAND", nullptr, 0) > 0) {
            ensure_container();
            host_island_probe();
            destroy_plugin();
            return 0;
        }
        if (!select_plugin()) {
            if (g.shot.empty())
                ShowWindow(g.main, SW_SHOW);
            MSG wait;
            while (GetMessageW(&wait, nullptr, 0, 0) > 0)
                DispatchMessageW(&wait);
            return 1;
        }
        g.data_dir = utf8((g.data_root / wide(g.plugin.id)).wstring());
        fs::create_directories(wide(g.data_dir));
        g.services = WidgetHostApi2{sizeof(g.services), WIDGET_ABI_VERSION, &g, log_plugin,
                                    log_configuration, g.data_dir.c_str(), render_ui};
        SetWindowTextW(g.config_edit, wide(g.config).c_str());
        if (!ensure_container() || !rebuild(true)) {
            log_line("插件未能初始化，界面保留以便排查", LogLevel::Error);
            if (g.shot.empty())
                ShowWindow(g.main, SW_SHOW);
        }
        if (!g.shot.empty()) {
            auto ok = save_shot();
            destroy_plugin();
            return ok ? 0 : 1;
        }
        ShowWindow(g.main, SW_SHOW);
        log_line("就绪：用工具栏切换主题/语言/大小，或在配置框里改 JSON 后点“应用配置”。");
        if (g.seconds > 0) {
            auto until = GetTickCount64() + uint64_t(g.seconds) * 1000;
            MSG msg;
            while (GetTickCount64() < until) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (::ContentPreTranslateMessage(&msg))
                        continue;
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(10);
            }
            destroy_plugin();
            return 0;
        }
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            // Gives XAML islands first refusal on accelerators and focus, exactly as
            // the production host does.
            if (::ContentPreTranslateMessage(&msg))
                continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        destroy_plugin();
        if (g.module)
            FreeLibrary(g.module);
        return 0;
    } catch (...) {
        std::cerr << "开发宿主失败：" << error_text() << std::endl;
        return 1;
    }
}
