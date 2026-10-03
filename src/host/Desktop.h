#pragma once
#include "../common/Model.h"
#include <shellscalingapi.h>
#include "DesktopGrid.h"

namespace ww {
struct Monitor {
    HMONITOR handle;
    RECT work;
    std::string id;
    UINT dpi = 96;
};
inline std::vector<Monitor> monitors() {
    std::vector<Monitor> r;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM l) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(h, &info);
            DISPLAY_DEVICEW d{};
            d.cb = sizeof(d);
            std::wstring id = info.szDevice;
            if (EnumDisplayDevicesW(info.szDevice, 0, &d, EDD_GET_DEVICE_INTERFACE_NAME) && d.DeviceID[0])
                id = d.DeviceID;
            UINT x = 96, y = 96;
            GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &x, &y);
            reinterpret_cast<std::vector<Monitor> *>(l)->push_back({h, info.rcWork, utf8(id), x});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&r));
    return r;
}
class Desktop {
    HWND parent_ = nullptr;
    SIZE spacing_{80, 100};
    winrt::com_ptr<IFolderView> view_;

  public:
    bool spacing_changed() const {
        POINT current{};
        return view_ && SUCCEEDED(view_->GetSpacing(&current)) && current.x >= 8 && current.y >= 8 &&
               (current.x != spacing_.cx || current.y != spacing_.cy);
    }
    SIZE spacing() const {
        return spacing_;
    }
    HWND parent() const {
        return parent_;
    }
    bool valid() const {
        return parent_ && IsWindow(parent_) && FindWindowExW(parent_, nullptr, L"SHELLDLL_DefView", nullptr);
    }
    bool discover() {
        parent_ = nullptr;
        EnumWindows(
            [](HWND h, LPARAM p) -> BOOL {
                if (FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr)) {
                    *reinterpret_cast<HWND *>(p) = h;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&parent_));
        return valid();
    }
    void match_dpi() {
        check(valid(), "Desktop not available");
        auto c = GetWindowDpiAwarenessContext(parent_);
        check(SetThreadDpiAwarenessContext(c) != nullptr, "Desktop DPI context");
    }
    static bool is_desktop_window(HWND window) {
        wchar_t name[32]{};
        GetClassNameW(window, name, 32);
        std::wstring_view cls(name);
        return cls == L"Progman" || cls == L"WorkerW";
    }
    // Widgets are top-level windows, not desktop children: a XAML island refuses
    // to initialise when its parent chain leaves this process (measured:
    // E_ACCESSDENIED), and Windows 11's system backdrops only apply to top-level
    // windows anyway. The desktop window is still used - but only to read the
    // icon grid the widgets align to.
    //
    // Z-order policy. A widget is a top-level window now, so it has to be put
    // back into the desktop layer whenever the z-order changes.
    //
    // SetWindowPos(W, X) places W directly *below* X, so inserting the widget
    // below whatever currently sits directly above the desktop leaves it exactly
    // where a desktop child would have been: above the wallpaper, below every
    // application window. Raising it to the top (the obvious alternative) makes it
    // float over the user's folders, which is never what a desktop widget wants.
    static void restack(HWND window, HWND desktop) {
        HWND anchor = desktop && IsWindow(desktop) ? GetWindow(desktop, GW_HWNDPREV) : nullptr;
        SetWindowPos(window, anchor ? anchor : HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void place(HWND window, Instance &i, bool offdesktop = false, double min_width = 1, double min_height = 1,
               double max_width = 4096, double max_height = 4096) {
        auto list = monitors();
        check(!list.empty(), "No display");
        auto it = std::find_if(list.begin(), list.end(), [&](auto const &m) { return m.id == i.monitor; });
        if (it == list.end())
            it = std::find_if(list.begin(), list.end(),
                              [](auto const &m) { return m.work.left == 0 && m.work.top == 0; });
        if (it == list.end())
            it = list.begin();
        auto const &m = *it;
        i.monitor = m.id;
        double scale = m.dpi / 96.0;
        std::unique_ptr<PlacementGuard> placement;
        DesktopGrid grid;
        if (!offdesktop) {
            placement = std::make_unique<PlacementGuard>();
            grid = read_desktop_grid(parent_, m.handle, m.work, m.dpi, &view_);
            reserve_other_widgets(grid, window);
        } else {
            // Deterministic hidden-test grid; real widgets always query Explorer.
            grid.spacing = {MulDiv(80, m.dpi, 96), MulDiv(100, m.dpi, 96)};
        }
        spacing_ = grid.spacing;
        bool migrating = i.columns == 0 && i.rows == 0;
        if (migrating) {
            i.columns = nearest_cells(i.width * scale, spacing_.cx);
            i.rows = nearest_cells(i.height * scale, spacing_.cy);
            // The plugin minimum may need more cells than the desktop maximum. Clamp
            // here so the range check below stays in charge and reports it as a
            // permanent failure, instead of handing grid_extent a cell count it
            // rejects with an error no retry can ever resolve.
            i.columns =
                std::min(12, std::max(i.columns, int(std::ceil(min_width * scale / spacing_.cx))));
            i.rows = std::min(12, std::max(i.rows, int(std::ceil(min_height * scale / spacing_.cy))));
        }
        auto extent = grid_extent(i.columns, i.rows, spacing_);
        int w = extent.cx, h = extent.cy;
        if (w < min_width * scale || h < min_height * scale || w > max_width * scale ||
            h > max_height * scale)
            throw PlacementError(false, "This grid size is outside the plugin size limits");
        if (w > m.work.right - m.work.left || h > m.work.bottom - m.work.top)
            throw PlacementError(true, "This grid size does not fit the display; choose fewer cells");
        int x = std::clamp<int>(m.work.left + int(std::lround(i.x * scale)), m.work.left, m.work.right - w);
        int y = std::clamp<int>(m.work.top + int(std::lround(i.y * scale)), m.work.top, m.work.bottom - h);
        POINT pt{x, y};
        if (!offdesktop) {
            auto aligned = aligned_position(pt, SIZE{w, h}, m.work, grid);
            if (!aligned)
                throw PlacementError(
                    true, "No free desktop grid area; free space or resize the widget and retry");
            pt = *aligned;
            x = pt.x;
            y = pt.y;
        }
        // Screen coordinates throughout: the widget is a top-level window now, so
        // there is no parent client area to map into.
        check(SetWindowPos(window, HWND_TOP, pt.x, pt.y, w, h,
                           SWP_NOACTIVATE | (offdesktop ? 0 : SWP_SHOWWINDOW)),
              "Position widget");
        if (!offdesktop)
            restack(window, parent_);
        if (!offdesktop)
            check(SetPropW(window, L"WindowsWidget.Placed", reinterpret_cast<HANDLE>(1)),
                  "Reserve widget grid area");
        i.x = (x - m.work.left) / scale;
        i.y = (y - m.work.top) / scale;
        i.width = w / scale;
        i.height = h / scale;
    }
    void capture(HWND window, Instance &i) {
        RECT r;
        GetWindowRect(window, &r);
        auto h = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        for (auto const &m : monitors())
            if (m.handle == h) {
                double scale = m.dpi / 96.0;
                i.monitor = m.id;
                i.x = (r.left - m.work.left) / scale;
                i.y = (r.top - m.work.top) / scale;
                i.width = (r.right - r.left) / scale;
                i.height = (r.bottom - r.top) / scale;
                break;
            }
    }
};
} // namespace ww
