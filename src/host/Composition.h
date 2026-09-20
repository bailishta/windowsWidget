#pragma once
#include "../common/Common.h"

namespace ww {
// Win11's raised desktop uses WS_EX_NOREDIRECTIONBITMAP on Progman. A normal
// child consumes/clips desktop space, but its GDI pixels have no parent bitmap
// to be composed from. Give the widget its own redirected, fully opaque layer.
// This also redirects ordinary child HWND painting without changing the SDK.
inline void initialize_widget_surface(HWND window) {
    auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_LAYERED);
    // Zero is a valid previous style; a WM_STYLECHANGED handler can also leave
    // a stale last-error value. Verify the resulting flag before initializing.
    check((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED) != 0,
          "Enable widget composition surface");
    check(SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA), "Initialize opaque widget layer");
}
} // namespace ww
