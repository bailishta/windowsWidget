#include "../WidgetSdk.h"
#include <memory>

namespace {
struct Hello {
    HWND window = nullptr;
    bool dark = false;
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Hello*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Hello*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window = hwnd;
        }
        if (self && message == WM_PAINT) {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(hwnd, &paint);
            RECT bounds;
            GetClientRect(hwnd, &bounds);
            auto brush = CreateSolidBrush(self->dark ? RGB(32,32,35) : RGB(246,246,249));
            FillRect(dc, &bounds, brush);
            DeleteObject(brush);
            SetTextColor(dc, self->dark ? RGB(240,240,245) : RGB(25,25,30));
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, L"Hello, desktop!", -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &paint);
            return 0;
        }
        if (self && message == WM_NCDESTROY) self->window = nullptr;
        return DefWindowProcW(hwnd, message, w, l);
    }
};
HRESULT __cdecl Create(const WidgetCreateInfo* info, void** instance, HWND* window) {
    if (!info || info->size < sizeof(*info) || !instance || !window) return E_INVALIDARG;
    *instance = nullptr; *window = nullptr;
    try {
        auto hello = std::make_unique<Hello>();
        hello->dark = info->dark != 0;
        HMODULE module;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&Create), &module);
        WNDCLASSW cls{};
        cls.hInstance = module;
        cls.lpszClassName = L"WindowsWidget.Example.Hello";
        cls.lpfnWndProc = Hello::WndProc;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cls);
        auto hwnd = CreateWindowExW(0, cls.lpszClassName, L"Hello", WS_CHILD | WS_VISIBLE,
                                    0, 0, 240, 140, info->parent, nullptr, module, hello.get());
        if (!hwnd) return HRESULT_FROM_WIN32(GetLastError());
        *window = hwnd; *instance = hello.release();
        return S_OK;
    } catch (...) { return E_FAIL; }
}
}
WIDGET_EXPORT HRESULT __cdecl WidgetGetApi(uint32_t version, WidgetApi* api) {
    if (version != WIDGET_ABI_VERSION || !api || api->size < sizeof(WidgetApi)) return E_NOINTERFACE;
    *api = {
        sizeof(WidgetApi), WIDGET_ABI_VERSION, Create,
        [](void* p) { auto self = static_cast<Hello*>(p); if (self->window) DestroyWindow(self->window); delete self; },
        [](void*, const char* configuration) -> HRESULT { return configuration ? S_OK : E_INVALIDARG; },
        [](void* p, uint32_t, uint32_t, uint32_t) { InvalidateRect(static_cast<Hello*>(p)->window, nullptr, FALSE); },
        [](void* p, uint32_t dark) { auto self = static_cast<Hello*>(p); self->dark = dark != 0; InvalidateRect(self->window, nullptr, FALSE); }
    };
    return S_OK;
}
