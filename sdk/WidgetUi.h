#pragma once
// Header-only drawing helpers for WindowsWidget plugins.
//
// It wraps the Direct2D/DirectWrite boilerplate every plugin would otherwise
// repeat: a software DC render target bound to the HDC the host hands you,
// the Windows 11 light/dark palette, the system accent colour, rounded cards
// and text. Depends only on the Windows SDK - no WinUI, no CRT++ glue.
//
// Usage inside a plugin's window procedure:
//
//   widgetui::Painter painter;
//   LRESULT WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
//       if (m == WM_PAINT || m == WM_PRINTCLIENT) {
//           HDC dc; PAINTSTRUCT ps{};
//           dc = (m == WM_PAINT) ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
//           RECT r{}; GetClientRect(h, &r);
//           if (painter.begin(dc, r, dpi, dark)) {
//               painter.fill_card(painter.palette().card);
//               painter.text(L"Hello", 20.f, painter.palette().primary, r, widgetui::Align::Center);
//               painter.end();
//           }
//           if (m == WM_PAINT) EndPaint(h, &ps);
//           return 0;
//       }
//       ...
//
// Paint through the HDC: the desktop is composited with WS_EX_NOREDIRECTIONBITMAP
// and a DXGI swap chain bound to an HWND is not reliable there.

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <string>

namespace widgetui {
using Microsoft::WRL::ComPtr;

enum class Align { Near, Center, Far };

struct Palette {
    D2D1_COLOR_F card;      // widget background
    D2D1_COLOR_F stroke;    // hairline border
    D2D1_COLOR_F primary;   // main text
    D2D1_COLOR_F secondary; // captions, labels
    D2D1_COLOR_F subtle;    // hover / filled cells
    D2D1_COLOR_F accent;    // system accent
    D2D1_COLOR_F on_accent; // text drawn on the accent
};

inline D2D1_COLOR_F rgba(uint32_t argb) {
    return D2D1::ColorF(float((argb >> 16) & 0xff) / 255.f, float((argb >> 8) & 0xff) / 255.f,
                        float(argb & 0xff) / 255.f, float((argb >> 24) & 0xff) / 255.f);
}

// HKCU\...\DWM\AccentColor is stored as ABGR. It is undocumented, so this falls
// back to the Windows 11 default blue rather than failing.
inline uint32_t system_accent_argb() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        auto r = value & 0xff, g = (value >> 8) & 0xff, b = (value >> 16) & 0xff;
        if (r + g + b > 40) // ignore near-black leftovers from custom accent settings
            return 0xff000000u | (r << 16) | (g << 8) | b;
    }
    return 0xffD47800u; // #0078D4
}

inline Palette palette(bool dark) {
    Palette p{};
    if (dark) {
        p.card = rgba(0xff2b2b2bu);
        p.stroke = rgba(0xff3d3d3du);
        p.primary = rgba(0xfff2f2f2u);
        p.secondary = rgba(0xffa6a6a6u);
        p.subtle = rgba(0xff3a3a3au);
        p.accent = rgba(system_accent_argb());
        p.on_accent = rgba(0xffffffffu);
    } else {
        p.card = rgba(0xfffafafau);
        p.stroke = rgba(0xffe2e2e2u);
        p.primary = rgba(0xff1b1b1bu);
        p.secondary = rgba(0xff5f5f5fu);
        p.subtle = rgba(0xffecececu);
        p.accent = rgba(system_accent_argb());
        p.on_accent = rgba(0xffffffffu);
    }
    return p;
}

class Painter {
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> text_factory_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> formats_[4];
    RECT bounds_{};
    D2D1_SIZE_F size_{};
    float dpi_ = 96.f;
    bool drawing_ = false;
    Palette palette_;

    // Cached per-invalidate paint state: a widget repaints on the same HDC size
    // most of the time, so rebuilding formats every frame is wasted work.
    struct FormatKey {
        float size = 0;
        int weight = -1;
        bool center = false;
        bool equal(const FormatKey &o) const {
            return size == o.size && weight == o.weight && center == o.center;
        }
    } key_[4];

    IDWriteTextFormat *format(float size, DWRITE_FONT_WEIGHT weight, Align align) {
        FormatKey wanted{size, int(weight), align == Align::Center};
        for (int n = 0; n < 4; ++n)
            if (key_[n].size > 0 && key_[n].equal(wanted))
                return formats_[n].Get();
        for (int n = 0; n < 4; ++n) {
            if (key_[n].size > 0)
                continue;
            // Segoe UI Variable is the Windows 11 UI face; Segoe UI is the fallback
            // on systems that do not ship it.
            if (FAILED(text_factory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight,
                                                       DWRITE_FONT_STYLE_NORMAL,
                                                       DWRITE_FONT_STRETCH_NORMAL, size, L"", &formats_[n])))
                if (FAILED(text_factory_->CreateTextFormat(L"Segoe UI", nullptr, weight,
                                                           DWRITE_FONT_STYLE_NORMAL,
                                                           DWRITE_FONT_STRETCH_NORMAL, size, L"",
                                                           &formats_[n])))
                    return nullptr;
            formats_[n]->SetTextAlignment(align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                              : align == Align::Far  ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                                     : DWRITE_TEXT_ALIGNMENT_LEADING);
            formats_[n]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            formats_[n]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            key_[n] = wanted;
            return formats_[n].Get();
        }
        return nullptr;
    }

    void release_paint_state() {
        brush_.Reset();
        target_.Reset();
        for (auto &f : formats_)
            f.Reset();
        for (auto &k : key_)
            k = FormatKey{};
        drawing_ = false;
    }

  public:
    ~Painter() {
        release_paint_state();
    }

    // Drop cached device-dependent resources (call from WM_SIZE and WM_DPICHANGED).
    void invalidate() {
        release_paint_state();
    }

    bool begin(HDC dc, RECT client, uint32_t dpi, bool dark) {
        if (!factory_) {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())))
                return false;
        }
        if (!text_factory_) {
            if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                           reinterpret_cast<IUnknown **>(text_factory_.GetAddressOf()))))
                return false;
        }
        if (!target_) {
            // Software rendering: this DC belongs to a cross-process layered desktop
            // surface, where a DXGI swap chain is not reliable.
            auto properties =
                D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                               D2D1_ALPHA_MODE_IGNORE));
            if (FAILED(factory_->CreateDCRenderTarget(&properties, target_.GetAddressOf())))
                return false;
        }
        if (!brush_) {
            if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush_.GetAddressOf())))
                return false;
        }
        dpi_ = float(dpi ? dpi : 96);
        bounds_ = client;
        target_->SetDpi(dpi_, dpi_);
        if (FAILED(target_->BindDC(dc, &bounds_)))
            return false;
        size_ = target_->GetSize();
        palette_ = palette(dark);
        target_->BeginDraw();
        target_->Clear(palette_.card);
        drawing_ = true;
        return true;
    }

    bool drawing() const {
        return drawing_;
    }

    void end() {
        if (!drawing_)
            return;
        drawing_ = false;
        auto result = target_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET)
            release_paint_state();
    }

    Palette const &palette() const {
        return palette_;
    }

    // Logical size in DIPs (already divided by the render target DPI).
    D2D1_SIZE_F size() const {
        return size_;
    }

    float dip(float pixels) const {
        return pixels * 96.f / dpi_;
    }

    void fill_rect(D2D1_RECT_F r, D2D1_COLOR_F color) {
        if (!drawing_)
            return;
        brush_->SetColor(color);
        target_->FillRectangle(&r, brush_.Get());
    }

    void fill_rounded(D2D1_RECT_F r, float radius, D2D1_COLOR_F color) {
        if (!drawing_)
            return;
        brush_->SetColor(color);
        target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
    }

    void stroke_rounded(D2D1_RECT_F r, float radius, D2D1_COLOR_F color, float width = 1.f) {
        if (!drawing_)
            return;
        brush_->SetColor(color);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get(), width);
    }

    void text(std::wstring const &value, float size, D2D1_COLOR_F color, D2D1_RECT_F box,
              Align align = Align::Near, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
        if (!drawing_ || value.empty())
            return;
        auto *f = format(size, weight, align);
        if (!f)
            return;
        brush_->SetColor(color);
        target_->DrawTextW(value.c_str(), UINT32(value.size()), f, box, brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // Segoe Fluent Icons sits on the same baseline as Segoe UI, so glyphs can be
    // mixed into text runs by codepoint (16/20/24/32 are the recommended sizes).
    void glyph(wchar_t codepoint, float size, D2D1_COLOR_F color, D2D1_RECT_F box,
               Align align = Align::Center) {
        text(std::wstring(1, codepoint), size, color, box, align, DWRITE_FONT_WEIGHT_NORMAL);
    }

    // The widget's own background: a rounded card with a hairline border.
    void card(D2D1_RECT_F r, float radius = 10.f, bool with_border = true) {
        fill_rounded(r, radius, palette_.card);
        if (with_border)
            stroke_rounded(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f),
                           radius, palette_.stroke);
    }
};

// Formats a float without trailing zeros ("21" not "21.0"), for temperatures etc.
inline std::wstring number(double value, int decimals = 0) {
    wchar_t buffer[32];
    swprintf_s(buffer, L"%.*f", decimals, value);
    return buffer;
}

} // namespace widgetui
