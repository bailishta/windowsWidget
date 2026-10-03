#pragma once
#include "Grid.h"
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>

namespace ww {
// Read-only Shell COM queries. No remote memory, icon movement or Explorer patches.
inline DesktopGrid read_desktop_grid(HWND desktop, HMONITOR target, RECT work, UINT /*dpi*/,
                                     winrt::com_ptr<IFolderView> *cached_view = nullptr) {
    winrt::com_ptr<IShellWindows> windows;
    winrt::check_hresult(
        CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.put())));
    VARIANT location{}, empty{};
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    long handle = 0;
    winrt::com_ptr<IDispatch> dispatch;
    winrt::check_hresult(
        windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &handle, SWFO_NEEDDISPATCH, dispatch.put()));
    check(bool(dispatch), "Desktop Shell view unavailable");
    winrt::com_ptr<IShellBrowser> browser;
    winrt::check_hresult(
        dispatch.as<IServiceProvider>()->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(browser.put())));
    winrt::com_ptr<IShellView> shell;
    winrt::check_hresult(browser->QueryActiveShellView(shell.put()));
    auto view = shell.as<IFolderView>();
    if (cached_view)
        *cached_view = view;
    HWND view_window = nullptr;
    winrt::check_hresult(shell->GetWindow(&view_window));
    check(GetAncestor(view_window, GA_ROOT) == desktop, "Desktop view changed while aligning");
    auto icons = FindWindowExW(view_window, nullptr, L"SysListView32", nullptr);
    check(icons != nullptr, "Desktop icon view unavailable");
    POINT spacing{};
    winrt::check_hresult(view->GetSpacing(&spacing));
    check(spacing.x >= 8 && spacing.y >= 8, "Invalid desktop icon grid");
    DesktopGrid result;
    result.spacing = {spacing.x, spacing.y};
    result.origin = {work.left, work.top};
    winrt::com_ptr<IEnumIDList> items;
    winrt::check_hresult(view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(items.put())));
    bool origin_found = false;
    for (;;) {
        PITEMID_CHILD item = nullptr;
        auto hr = items->Next(1, &item, nullptr);
        if (hr == S_FALSE)
            break;
        winrt::check_hresult(hr);
        POINT position{};
        hr = view->GetItemPosition(item, &position);
        CoTaskMemFree(item);
        winrt::check_hresult(hr);
        check(ClientToScreen(icons, &position), "Map desktop icon position");
        auto monitor = MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST);
        if (monitor == target && !origin_found) {
            result.origin = position;
            origin_found = true;
        }
        // Reserve the whole icon cell, including its label and surrounding space.
        result.occupied.push_back({position.x, position.y, position.x + spacing.x, position.y + spacing.y});
    }
    return result;
}
class PlacementGuard {
    Handle mutex_;

  public:
    PlacementGuard() : mutex_(CreateMutexW(nullptr, FALSE, L"Local\\WindowsWidget.DesktopPlacement")) {
        check(bool(mutex_), "Create placement mutex");
        auto result = WaitForSingleObject(mutex_, 5000);
        check(result == WAIT_OBJECT_0 || result == WAIT_ABANDONED, "Desktop placement busy; retry");
    }
    ~PlacementGuard() {
        ReleaseMutex(mutex_);
    }
};
// Widgets are top-level windows now, so sibling widgets are found by scanning
// top-level windows rather than the desktop's children. They live in other host
// processes; window properties and rectangles are readable across processes.
inline void reserve_other_widgets(DesktopGrid &grid, HWND self) {
    struct Context {
        DesktopGrid &grid;
        HWND self;
    } context{grid, self};
    EnumWindows(
        [](HWND window, LPARAM value) -> BOOL {
            auto &c = *reinterpret_cast<Context *>(value);
            if (window == c.self || !GetPropW(window, L"WindowsWidget.Placed"))
                return TRUE;
            wchar_t name[80]{};
            GetClassNameW(window, name, 80);
            if (std::wstring_view(name) == L"WindowsWidget.Container") {
                RECT r{};
                if (GetWindowRect(window, &r)) {
                    c.grid.occupied.push_back(r);
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
}
} // namespace ww
