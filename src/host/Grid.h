#pragma once
#include "../common/Common.h"
#include <optional>
#include <limits>

namespace ww {
// Placement fails for two very different reasons: a full desktop grid or a display
// change resolves itself on a later attempt, while a grid size the plugin's own
// limits can never accept stays broken until the configuration changes. Hosts retry
// the first case and report the second, so the distinction travels with the
// exception instead of being re-derived from the message text.
struct PlacementError : std::runtime_error {
    PlacementError(bool transient, std::string const &message)
        : std::runtime_error(message), temporary(transient) {}
    bool temporary;
};
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
// Whole cells for a dragged edge, narrowed to the plugin's DIP limits first. A
// plugin may declare a minimum that needs more than the 12-cell maximum; clamping
// that minimum to the desktop limit keeps both bounds of the final std::clamp
// ordered, and lets placement report the size constraint instead of a cell count
// no grid can satisfy.
inline int clamped_cells(double pixels, LONG spacing, double minimum, double maximum, double scale) {
    if (!std::isfinite(pixels) || !std::isfinite(minimum) || !std::isfinite(maximum) || spacing < 1 ||
        !std::isfinite(scale) || scale <= 0)
        throw std::runtime_error("Invalid grid measurement");
    int lower = std::min(12, std::max(1, int(std::ceil(minimum * scale / spacing))));
    int upper = std::max(lower, std::min(12, int(std::floor(maximum * scale / spacing))));
    return std::clamp(nearest_cells(pixels, spacing), lower, upper);
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
