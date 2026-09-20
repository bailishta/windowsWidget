#include "../../src/common/Common.h"
#include "../../src/common/Language.h"
#include "../../sdk/WidgetSdk.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using namespace ww;
namespace {
struct Clock {
    HWND window = nullptr;
    uint32_t render_dpi = 96;
    bool dark = true, hour24 = true;
    WidgetHostApi const *host = nullptr;
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> text_factory;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextFormat> large, date_format;
    uint64_t last_minute = 0;
    static uint64_t minute_key(SYSTEMTIME const &t) {
        return ((((uint64_t(t.wYear) * 13 + t.wMonth) * 32 + t.wDay) * 24 + t.wHour) * 60 + t.wMinute);
    }
    bool format_hour24 = true, format_chinese = true;
    float format_width = 0;
    void configure(const char *s) {
        auto j = json(s);
        hour24 = j.GetNamedBoolean(L"hour24", true);
        if (window)
            InvalidateRect(window, nullptr, FALSE);
    }
    void release_target() {
        brush.Reset();
        target.Reset();
    }
    void paint(HDC dc) {
        try {
            RECT r;
            GetClientRect(window, &r);
            if (!target) {
                // Paint through the redirected HDC. A DXGI HWND swap chain does
                // not reliably participate in a cross-process layered desktop.
                auto properties = D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                winrt::check_hresult(factory->CreateDCRenderTarget(&properties, &target));
                winrt::check_hresult(target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush));
            }
            winrt::check_hresult(target->BindDC(dc, &r));
            float dpi = float(render_dpi);
            target->SetDpi(dpi, dpi);
            auto size = target->GetSize();
            target->BeginDraw();
            target->Clear(dark ? D2D1::ColorF(0.125f, 0.125f, 0.137f) : D2D1::ColorF(0.965f, 0.965f, 0.977f));
            SYSTEMTIME t;
            GetLocalTime(&t);
            last_minute = minute_key(t);
            wchar_t time[40], date[100];
            if (hour24)
                swprintf_s(time, L"%02u:%02u", t.wHour, t.wMinute);
            else
                swprintf_s(time, L"%u:%02u %s", t.wHour % 12 ? t.wHour % 12 : 12, t.wMinute,
                           t.wHour >= 12 ? L"PM" : L"AM");
            GetDateFormatEx(thread_locale(), DATE_LONGDATE, &t, nullptr, date, 100, nullptr);
            if (!large || !date_format || format_width != size.width || format_hour24 != hour24 ||
                format_chinese != thread_chinese()) {
                large.Reset();
                date_format.Reset();
                winrt::check_hresult(text_factory->CreateTextFormat(
                    L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    std::clamp(size.width / (hour24 ? 5.2f : 7.4f), 12.f, 64.f), thread_locale(), &large));
                winrt::check_hresult(text_factory->CreateTextFormat(
                    L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, 12, thread_locale(), &date_format));
                large->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                large->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                date_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                format_width = size.width;
                format_hour24 = hour24;
                format_chinese = thread_chinese();
            }
            brush->SetColor(dark ? D2D1::ColorF(0.97f, 0.97f, 1) : D2D1::ColorF(0.1f, 0.1f, 0.14f));
            bool show_date = size.width >= 150 && size.height >= 100;
            target->DrawTextW(
                time, UINT32(wcslen(time)), large.Get(),
                D2D1::RectF(3, 0, size.width - 3, show_date ? size.height * 0.73f : size.height),
                brush.Get());
            brush->SetColor(dark ? D2D1::ColorF(0.64f, 0.68f, 0.77f) : D2D1::ColorF(0.38f, 0.41f, 0.48f));
            if (show_date)
                target->DrawTextW(date, UINT32(wcslen(date)), date_format.Get(),
                                  D2D1::RectF(6, size.height * 0.70f, size.width - 6, size.height),
                                  brush.Get());
            auto result = target->EndDraw();
            if (result == D2DERR_RECREATE_TARGET)
                release_target();
            else
                winrt::check_hresult(result);
        } catch (...) {
            auto text = "Clock rendering failed: " + error_text();
            host->log(host->context, 3, text.c_str());
            release_target();
        }
    }
    void draw() {
        PAINTSTRUCT ps;
        auto dc = BeginPaint(window, &ps);
        paint(dc);
        EndPaint(window, &ps);
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Clock *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Clock *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->window = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            if (m == WM_PRINTCLIENT) {
                self->paint(reinterpret_cast<HDC>(w));
                return 0;
            }
            if (m == WM_PAINT) {
                self->draw();
                return 0;
            }
            if (m == WM_ERASEBKGND)
                return 1;
            if (m == WM_TIMER) {
                SYSTEMTIME now{};
                GetLocalTime(&now);
                if (self->last_minute != minute_key(now))
                    InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (m == WM_TIMECHANGE) {
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (m == WM_SIZE) {
                self->release_target();
                return 0;
            }
            if (m == WM_CONTEXTMENU) {
                auto menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING | (self->hour24 ? MF_CHECKED : 0), 1,
                            localized(thread_chinese(), L"24 小时制", L"24-hour time"));
                POINT point;
                GetCursorPos(&point);
                int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, h, nullptr);
                DestroyMenu(menu);
                if (id == 1) {
                    self->hour24 = !self->hour24;
                    JsonObject j;
                    put(j, L"hour24", self->hour24);
                    auto s = str(j);
                    self->host->configuration_changed(self->host->context, s.c_str());
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
        }
        return DefWindowProcW(h, m, w, l);
    }
};
HRESULT __cdecl create(const WidgetCreateInfo *info, void **out, HWND *content) {
    if (!info || !out || !content)
        return E_INVALIDARG;
    *out = nullptr;
    *content = nullptr;
    try {
        auto c = std::make_unique<Clock>();
        c->host = info->host;
        c->render_dpi = info->dpi;
        c->dark = info->dark != 0;
        c->configure(info->configuration_utf8);
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, c->factory.GetAddressOf()));
        winrt::check_hresult(
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown **>(c->text_factory.GetAddressOf())));
        HMODULE module;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&create), &module);
        WNDCLASSW wc{};
        wc.lpfnWndProc = Clock::proc;
        wc.hInstance = module;
        wc.lpszClassName = L"WindowsWidget.Clock";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        auto h =
            CreateWindowExW(0, wc.lpszClassName, localized(thread_chinese(), L"时钟", L"Clock"),
                            WS_CHILD | WS_VISIBLE, 0, 0, 260, 160, info->parent, nullptr, module, c.get());
        check(h != nullptr, "Clock window");
        SetTimer(h, 1, 1000, nullptr);
        *content = h;
        *out = c.release();
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}
} // namespace
WIDGET_EXPORT HRESULT __cdecl WidgetGetApi(uint32_t version, WidgetApi *api) {
    if (version != 1 || !api || api->size < sizeof(WidgetApi))
        return E_NOINTERFACE;
    *api = {sizeof(WidgetApi),
            1,
            create,
            [](void *p) {
                auto c = static_cast<Clock *>(p);
                if (IsWindow(c->window))
                    DestroyWindow(c->window);
                delete c;
            },
            [](void *p, const char *j) -> HRESULT {
                try {
                    static_cast<Clock *>(p)->configure(j);
                    return S_OK;
                } catch (...) {
                    return E_INVALIDARG;
                }
            },
            [](void *p, uint32_t, uint32_t, uint32_t dpi) {
                auto c = static_cast<Clock *>(p);
                c->render_dpi = dpi;
                c->release_target();
                InvalidateRect(c->window, nullptr, FALSE);
            },
            [](void *p, uint32_t dark) {
                auto c = static_cast<Clock *>(p);
                c->dark = dark != 0;
                SetWindowTextW(c->window, localized(thread_chinese(), L"时钟", L"Clock"));
                InvalidateRect(c->window, nullptr, FALSE);
            }};
    return S_OK;
}
