#pragma once
#include "../common/Common.h"
#include <optional>
#include <limits>

namespace ww {
struct DesktopGrid {
    POINT origin{};
    SIZE spacing{80, 100};
    std::vector<RECT> occupied;
};
inline SIZE grid_extent(int columns, int rows, SIZE spacing) {
    if (columns < 1 || columns > 12 || rows < 1 || rows > 12 || spacing.cx < 1 || spacing.cy < 1 ||
        spacing.cx > 4096 || spacing.cy > 4096)
        throw std::runtime_error("Invalid grid size or desktop spacing");
    return {columns * spacing.cx, rows * spacing.cy};
}
inline int nearest_cells(double pixels, LONG spacing) {
    if (!std::isfinite(pixels) || spacing < 1)
        throw std::runtime_error("Invalid grid measurement");
    return std::clamp(int(std::lround(pixels / spacing)), 1, 12);
}
inline bool overlaps(RECT a, RECT b) {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}
// Coordinates are screen pixels, including negative virtual-desktop positions.
// Stable row/column traversal breaks ties without moving Explorer's icons.
inline std::optional<POINT> aligned_position(POINT desired, SIZE size, RECT work, DesktopGrid const &grid) {
    if (grid.spacing.cx <= 0 || grid.spacing.cy <= 0 || size.cx <= 0 || size.cy <= 0)
        return std::nullopt;
    auto first_x = grid.origin.x +
                   LONG(std::ceil(double(work.left - grid.origin.x) / grid.spacing.cx)) * grid.spacing.cx;
    auto first_y =
        grid.origin.y + LONG(std::ceil(double(work.top - grid.origin.y) / grid.spacing.cy)) * grid.spacing.cy;
    std::optional<POINT> result;
    double best = std::numeric_limits<double>::max();
    for (LONG y = first_y; y <= work.bottom - size.cy; y += grid.spacing.cy)
        for (LONG x = first_x; x <= work.right - size.cx; x += grid.spacing.cx) {
            RECT candidate{x, y, x + size.cx, y + size.cy};
            if (std::any_of(grid.occupied.begin(), grid.occupied.end(),
                            [&](auto r) { return overlaps(candidate, r); }))
                continue;
            double dx = double(x) - desired.x, dy = double(y) - desired.y;
            double distance = dx * dx + dy * dy;
            if (distance < best) {
                best = distance;
                result = POINT{x, y};
            }
        }
    return result;
}
} // namespace ww
