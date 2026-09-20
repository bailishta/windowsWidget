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
    void attach(HWND window) {
        check(valid(), "Desktop not available");
        auto style = GetWindowLongPtrW(window, GWL_STYLE);
        SetWindowLongPtrW(window, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD);
        SetLastError(0);
        auto previous = SetParent(window, parent_);
        check(previous || GetLastError() == 0, "Attach desktop window");
        SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
            reserve_other_widgets(grid, parent_, window, m.dpi);
        } else {
            // Deterministic hidden-test grid; real widgets always query Explorer.
            grid.spacing = {MulDiv(80, m.dpi, 96), MulDiv(100, m.dpi, 96)};
        }
        spacing_ = grid.spacing;
        bool migrating = i.columns == 0 && i.rows == 0;
        if (migrating) {
            i.columns = nearest_cells(i.width * scale, spacing_.cx);
            i.rows = nearest_cells(i.height * scale, spacing_.cy);
            i.columns = std::max(i.columns, int(std::ceil(min_width * scale / spacing_.cx)));
            i.rows = std::max(i.rows, int(std::ceil(min_height * scale / spacing_.cy)));
        }
        auto extent = grid_extent(i.columns, i.rows, spacing_);
        int w = extent.cx, h = extent.cy;
        if (w < min_width * scale || h < min_height * scale || w > max_width * scale ||
            h > max_height * scale)
            throw std::runtime_error("This grid size is outside the plugin size limits");
        if (w > m.work.right - m.work.left || h > m.work.bottom - m.work.top)
            throw std::runtime_error("This grid size does not fit the display; choose fewer cells");
        int x = std::clamp<int>(m.work.left + int(std::lround(i.x * scale)), m.work.left, m.work.right - w);
        int y = std::clamp<int>(m.work.top + int(std::lround(i.y * scale)), m.work.top, m.work.bottom - h);
        POINT pt{x, y};
        if (!offdesktop) {
            auto aligned = aligned_position(pt, SIZE{w, h}, m.work, grid);
            if (!aligned)
                throw std::runtime_error(
                    "No free desktop grid area; free space or resize the widget and retry");
            pt = *aligned;
            x = pt.x;
            y = pt.y;
            MapWindowPoints(HWND_DESKTOP, parent_, &pt, 1);
        }
        check(SetWindowPos(window, HWND_TOP, pt.x, pt.y, w, h,
                           SWP_NOACTIVATE | (offdesktop ? 0 : SWP_SHOWWINDOW)),
              "Position widget");
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
