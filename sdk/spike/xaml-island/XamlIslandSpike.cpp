// Technical probe: can WindowsWidget host WinUI 3 XAML content in a widget, and
// can that content be translucent on a desktop child window?
//
// It answers four questions and prints each result, then (in --probe mode) runs a
// decisive pixel experiment:
//
//   A. Does a plain Win32 process with its own message loop host a
//      DesktopWindowXamlSource? (No WinUI application, no Application::Start.)
//   B. Does the island render inside a WS_EX_LAYERED container that is a child of
//      the Explorer desktop, the way a real widget is placed?
//   C. Is the island opaque? Measured, not assumed: capture the screen region of
//      the widget while it is visible and again while it is hidden. If the two
//      images match, the desktop shows through; if they differ, the island paints
//      its own opaque background.
//   D. Does destroying and recreating the container plus island work? That is the
//      path the host takes whenever Explorer restarts.
//
// Usage:
//   XamlIslandSpike.exe                 plain top-level window, prints A/B/C
//   XamlIslandSpike.exe --acrylic       also set SystemBackdrop(DesktopAcrylicBackdrop)
//   XamlIslandSpike.exe --rebuild 3     recreate container+island N times first (D)
//   XamlIslandSpike.exe --capture a.bmp save the widget region to a BMP
//   XamlIslandSpike.exe --seconds 5     message-loop time before reporting

// windows.h first: Microsoft.UI.Interop.h and the C++/WinRT interop header both
// need HWND/HMONITOR/STDAPICALLTYPE to already be declared.
#include <windows.h>
#undef GetCurrentTime
#include <shcore.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <Microsoft.UI.Dispatching.Interop.h>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "shcore.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace {
int g_failures = 0;

void report(bool ok, std::string const &what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    std::printf("%s\n", ok ? "" : "");
    std::fflush(stdout);
    if (!ok)
        ++g_failures;
}

void note(std::string const &what) {
    std::printf("      %s\n", what.c_str());
    std::fflush(stdout);
}

// ── the Application object an island needs (Application::Start is NOT called) ──

struct ProbeApp : ApplicationT<ProbeApp, Markup::IXamlMetadataProvider> {
    ProbeApp() = default;
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
};

// ── window plumbing ───────────────────────────────────────────────────────────

constexpr wchar_t kContainerClass[] = L"XamlIslandSpike.Container";
constexpr int kWidgetWidth = 320, kWidgetHeight = 200;

HWND g_container = nullptr;
bool g_container_visible = true;

LRESULT CALLBACK container_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        auto dc = BeginPaint(h, &ps);
        RECT r{};
        GetClientRect(h, &r);
        // Deliberately nothing: if the island is opaque we will see its own
        // background; if it is transparent we will see this window's untouched
        // (and therefore desktop-composited) pixels.
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND find_desktop() {
    HWND parent = nullptr;
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            if (FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr)) {
                *reinterpret_cast<HWND *>(p) = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&parent));
    return parent;
}

HWND g_host = nullptr;

// mode 0 = top-level, 1 = child of the Explorer desktop (what a widget is),
// 2 = child of an ordinary window we own (isolates "child" from "desktop").
HWND create_container(int mode, bool layered) {
    if (mode == 1) {
        auto desktop = find_desktop();
        if (!desktop) {
            note("找不到桌面宿主窗口；退回顶层窗口模式");
            mode = 0;
        } else {
            SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(desktop));
        }
    }
    if (mode == 2) {
        g_host = CreateWindowExW(0, kContainerClass, L"spike host", WS_OVERLAPPEDWINDOW, 40, 40, 600, 420,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(g_host, SW_SHOWNOACTIVATE);
    }
    DWORD ex = WS_EX_TOOLWINDOW | (mode == 1 ? WS_EX_NOACTIVATE : 0u);
    // Embedded mode is created as a popup and reparented, like the production
    // host does: WS_CHILD needs a parent at creation time.
    DWORD style = mode == 2 ? WS_CHILD : WS_POPUP;
    HWND owner = mode == 2 ? g_host : nullptr;
    HWND h = CreateWindowExW(ex, kContainerClass, L"", style, mode == 2 ? 20 : 60, mode == 2 ? 20 : 60,
                             kWidgetWidth, kWidgetHeight, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h)
        return nullptr;
    if (mode == 1) {
        auto current = GetWindowLongPtrW(h, GWL_STYLE);
        SetWindowLongPtrW(h, GWL_STYLE, (current & ~WS_POPUP) | WS_CHILD);
        SetParent(h, find_desktop());
        SetWindowPos(h, HWND_TOP, 60, 60, kWidgetWidth, kWidgetHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(h, SW_SHOWNOACTIVATE); // without this the island is never composited
    }
    if (layered) {
        auto current = GetWindowLongPtrW(h, GWL_EXSTYLE);
        SetWindowLongPtrW(h, GWL_EXSTYLE, current | WS_EX_LAYERED);
        if (!SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA))
            note("SetLayeredWindowAttributes 失败，Win32=" + std::to_string(GetLastError()));
    }
    return h;
}

// ── island ────────────────────────────────────────────────────────────────────

Hosting::DesktopWindowXamlSource g_island{nullptr};

void dump_children(HWND parent, const char *stage) {
    std::cout << "      [" << stage << "] 容器的子窗口：" << std::endl;
    EnumChildWindows(
        parent,
        [](HWND h, LPARAM) -> BOOL {
            wchar_t cls[128]{};
            GetClassNameW(h, cls, 128);
            RECT r{};
            GetWindowRect(h, &r);
            std::cout << "        hwnd=" << h << " class=" << winrt::to_string(cls) << " rect="
                      << r.left << "," << r.top << "," << r.right << "," << r.bottom
                      << " visible=" << int(IsWindowVisible(h)) << std::endl;
            return TRUE;
        },
        0);
    std::cout.flush();
}

bool create_island(HWND container, bool acrylic, bool solid, bool resize) {
    try {
        auto id = winrt::Microsoft::UI::GetWindowIdFromWindow(container);
        g_island = Hosting::DesktopWindowXamlSource();
        g_island.Initialize(id);
        // Two shapes: an opaque magenta field proves the island renders and shows
        // exactly where, and a translucent panel shows whether the desktop behind
        // comes through.
        const wchar_t *markup = solid ? LR"(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="#FFFF00FF">
  <TextBlock Text="SOLID" FontSize="28" Foreground="#FF101010"
             HorizontalAlignment="Center" VerticalAlignment="Center"/>
</Grid>)" : LR"(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
  <Border Background="#66FF0000" CornerRadius="16" Margin="12" BorderThickness="2" BorderBrush="#CC00FF00">
    <StackPanel VerticalAlignment="Center" HorizontalAlignment="Center" Spacing="10">
      <TextBlock Text="WinUI 3 in a widget" FontSize="20" Foreground="#FF101010"/>
      <Button Content="WinUI 3 button"/>
    </StackPanel>
  </Border>
</Grid>)";
        auto root = Markup::XamlReader::Load(hstring(markup)).as<FrameworkElement>();
        g_island.Content(root);
        if (resize && g_island.SiteBridge())
            g_island.SiteBridge().MoveAndResize(
                winrt::Windows::Graphics::RectInt32{0, 0, kWidgetWidth, kWidgetHeight});
        report(true, "island 初始化并挂上 XAML 内容");
    } catch (hresult_error const &e) {
        report(false, "island 初始化失败：" + to_string(e.message()) + "（HRESULT=0x" +
                          std::to_string(uint32_t(e.code().value)) + "）");
        g_island = nullptr;
        return false;
    }
    if (acrylic) {
        try {
            g_island.SystemBackdrop(Media::DesktopAcrylicBackdrop());
            report(true, "DesktopWindowXamlSource.SystemBackdrop(DesktopAcrylicBackdrop) 设置成功");
        } catch (hresult_error const &e) {
            report(false, "SystemBackdrop 设置失败（这本身就是结论）：" + to_string(e.message()));
        } catch (...) {
            report(false, "SystemBackdrop 不可用（编译期存在但调用失败）");
        }
    }
    return true;
}

void destroy_island() {
    if (g_island) {
        g_island.Content(nullptr);
        g_island.Close();
        g_island = nullptr;
    }
}

// ── pixels ────────────────────────────────────────────────────────────────────

struct Image {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels; // BGRA
};

Image capture_screen(RECT area) {
    Image image;
    image.width = std::max(1L, area.right - area.left);
    image.height = std::max(1L, area.bottom - area.top);
    image.pixels.assign(size_t(image.width) * size_t(image.height), 0);
    auto screen = GetDC(nullptr);
    auto memory = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), image.width, -image.height, 1, 32, BI_RGB};
    void *bits = nullptr;
    auto bitmap = CreateDIBSection(memory, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        auto previous = SelectObject(memory, bitmap);
        BitBlt(memory, 0, 0, image.width, image.height, screen, area.left, area.top, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        memcpy(image.pixels.data(), bits, image.pixels.size() * 4);
        SelectObject(memory, previous);
    }
    if (bitmap)
        DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return image;
}

Image capture_window(HWND h, bool full_content) {
    RECT r{};
    GetWindowRect(h, &r);
    Image image;
    image.width = std::max(1L, r.right - r.left);
    image.height = std::max(1L, r.bottom - r.top);
    image.pixels.assign(size_t(image.width) * size_t(image.height), 0xff000000u);
    auto screen = GetDC(nullptr);
    auto memory = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), image.width, -image.height, 1, 32, BI_RGB};
    void *bits = nullptr;
    auto bitmap = CreateDIBSection(memory, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        auto previous = SelectObject(memory, bitmap);
        PrintWindow(h, memory, full_content ? 2u : 0u); // 2 = PW_RENDERFULLCONTENT
        GdiFlush();
        memcpy(image.pixels.data(), bits, image.pixels.size() * 4);
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return image;
}

bool write_bmp(std::string const &file, Image const &image) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = image.width;
    ih.biHeight = image.height;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    auto stride = size_t(image.width) * 4;
    ih.biSizeImage = DWORD(stride * size_t(image.height));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    std::ofstream out(file, std::ios::binary);
    if (!out)
        return false;
    out.write(reinterpret_cast<char const *>(&fh), sizeof(fh));
    out.write(reinterpret_cast<char const *>(&ih), sizeof(ih));
    for (int y = image.height - 1; y >= 0; --y)
        out.write(reinterpret_cast<char const *>(image.pixels.data() + size_t(y) * image.width), stride);
    return bool(out);
}

double mean_difference(Image const &a, Image const &b) {
    if (a.pixels.size() != b.pixels.size() || a.pixels.empty())
        return -1.0;
    double sum = 0;
    for (size_t n = 0; n < a.pixels.size(); ++n) {
        auto p = a.pixels[n], q = b.pixels[n];
        sum += std::abs(int(p & 0xff) - int(q & 0xff)) + std::abs(int((p >> 8) & 0xff) - int((q >> 8) & 0xff)) +
               std::abs(int((p >> 16) & 0xff) - int((q >> 16) & 0xff));
    }
    return sum / double(a.pixels.size());
}

void pump(int milliseconds) {
    auto until = GetTickCount64() + uint64_t(milliseconds);
    MSG msg;
    while (GetTickCount64() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (!::ContentPreTranslateMessage(&msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        Sleep(10);
    }
}

int run(int argc, wchar_t **argv) {
    int mode = 0;
    bool acrylic = false, solid = false, resize = true, layered = true,
         measure = false;
    int rebuilds = 0, seconds = 3;
    std::string capture;
    for (int n = 1; n < argc; ++n) {
        std::wstring_view arg = argv[n];
        auto next = [&]() { return n + 1 < argc ? argv[++n] : L""; };
        if (arg == L"--desktop")
            mode = 1;
        else if (arg == L"--child")
            mode = 2;
        else if (arg == L"--no-layered")
            layered = false;
        else if (arg == L"--measure")
            measure = true;
        else if (arg == L"--acrylic")
            acrylic = true;
        else if (arg == L"--solid")
            solid = true;
        else if (arg == L"--no-resize")
            resize = false;
        else if (arg == L"--rebuild")
            rebuilds = _wtoi(next());
        else if (arg == L"--seconds")
            seconds = _wtoi(next());
        else if (arg == L"--capture")
            capture = winrt::to_string(hstring(next()));
    }

    std::cout << "=== XAML Island probe (mode=" << mode << " layered=" << int(layered)
              << " acrylic=" << int(acrylic) << " rebuilds=" << rebuilds << " measure="
              << int(measure) << ") ===" << std::endl;

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();
    report(queue != nullptr, "DispatcherQueueController::CreateOnCurrentThread");

    // The island needs an Application object; Application::Start is deliberately
    // NOT called, matching how a Win32 host is documented to use islands.
    try {
        static ProbeApp app;
        report(true, "创建 Application 派生对象（未调用 Application::Start）");
    } catch (hresult_error const &e) {
        report(false, "创建 Application 对象失败：" + to_string(e.message()));
        return 1;
    }

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = container_proc;
    wc.lpszClassName = kContainerClass;
    RegisterClassW(&wc);

    g_container = create_container(mode, layered);
    if (!g_container) {
        report(false, "创建组件容器窗口失败");
        return 1;
    }
    report(true, mode == 1   ? "容器已 SetParent 到 Explorer 桌面"
                       : mode == 2 ? "容器是自有顶层窗口的子窗口"
                                   : "容器是顶层窗口");
    note(std::string("layered=") + (layered ? "yes" : "no"));
    if (!create_island(g_container, acrylic, solid, resize)) {
        DestroyWindow(g_container);
        return 1;
    }
    dump_children(g_container, "island 创建后");
    pump(1200); // let XAML lay out and paint once
    dump_children(g_container, "布局后");

    for (int n = 0; n < rebuilds; ++n) {
        destroy_island();
        DestroyWindow(g_container);
        g_container = create_container(mode, layered);
        if (!g_container || !create_island(g_container, acrylic, solid, resize)) {
            report(false, "第 " + std::to_string(n + 1) + " 次重建失败");
            return 1;
        }
        pump(600);
        report(true, "重建 " + std::to_string(n + 1) + "/" + std::to_string(rebuilds) +
                         " 成功（容器与 island 一起重建）");
    }

    pump(seconds * 1000);

    if (!measure) {
        if (seconds > 0) {
            pump(seconds * 1000);
        } else {
            std::cout << "      窗口保持打开，关闭它即可退出（或用 --seconds N 自动退出）。" << std::endl;
            MSG idle;
            while (GetMessageW(&idle, nullptr, 0, 0) > 0) {
                if (!::ContentPreTranslateMessage(&idle)) {
                    TranslateMessage(&idle);
                    DispatchMessageW(&idle);
                }
            }
        }
        destroy_island();
        std::cout << "=== 失败项：" << g_failures << " ===" << std::endl;
        return g_failures == 0 ? 0 : 1;
    }
    RECT area{};
    GetWindowRect(g_container, &area);
    auto visible = capture_screen(area);
    if (!capture.empty()) {
        report(write_bmp(capture, visible), "写出屏幕截图 " + capture);
        auto rendered = capture_window(g_container, true);
        auto rendered_file = capture;
        auto dot2 = rendered_file.rfind('.');
        rendered_file = (dot2 == std::string::npos ? rendered_file : rendered_file.substr(0, dot2)) + "-window.bmp";
        write_bmp(rendered_file, rendered);
        std::cout << "      已写出 PrintWindow 渲染 " << rendered_file << " (" << rendered.width
                  << " x " << rendered.height << ")" << std::endl;
    }
    // Hide the widget and capture the same screen region again: what remains is
    // exactly what the widget covers.
    ShowWindow(g_container, SW_HIDE);
    g_container_visible = false;
    pump(700);
    auto hidden = capture_screen(area);
    if (!capture.empty()) {
        auto hidden_file = capture;
        auto dot = hidden_file.rfind('.');
        hidden_file = (dot == std::string::npos ? hidden_file : hidden_file.substr(0, dot)) + "-hidden.bmp";
        write_bmp(hidden_file, hidden);
        std::cout << "      已写出隐藏态截图 " << hidden_file << " (" << hidden.width << " x "
                  << hidden.height << ")" << std::endl;
    }
    ShowWindow(g_container, SW_SHOW);
    g_container_visible = true;
    pump(400);

    auto difference = mean_difference(visible, hidden);
    std::printf("      可见/隐藏 同一屏幕区域的平均通道差 = %.2f（0 表示完全一致，即桌面透出）\n",
                difference);
    std::fflush(stdout);
    if (difference >= 0 && difference < 3.0)
        report(true, "island 是透明的：隐藏组件后该区域画面几乎不变");
    else
        report(false, "island 不透明：它用自己的背景盖住了桌面（差异 " +
                          std::to_string(int(difference)) + "）");

    destroy_island();
    DestroyWindow(g_container);
    std::printf("=== 失败项：%d ===\n", g_failures);
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        return run(argc, argv);
    } catch (hresult_error const &e) {
        std::printf("[FAIL] 未捕获的 WinRT 异常：%s (0x%08X)\n", winrt::to_string(e.message()).c_str(),
                    uint32_t(e.code().value));
        return 1;
    } catch (...) {
        std::printf("[FAIL] 未捕获的异常\n");
        return 1;
    }
}
