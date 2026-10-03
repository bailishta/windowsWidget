// Month calendar widget, presented with real WinUI 3 controls.
//
// The plugin never touches WinRT: a XAML island cannot be created from a plugin
// DLL (measured - see sdk/WidgetSdk.h). Instead the plugin declares
// WIDGET_CAPABILITY_XAML, hands the host a complete XAML document through
// WidgetHostApi2::ui_render, and receives clicks by element name through
// WidgetApi2::ui_event.
//
// Memory notes, because a widget runs for days:
//  * The document is only rebuilt when something actually changed - a timer tick
//    that does not cross a date boundary, or a redraw with identical content,
//    does nothing at all. The host also drops identical documents.
//  * No window is created; only a message-only window exists, to carry the timer.
//  * The markup string is the plugin's only per-instance allocation.

#include <windows.h>
#include "../../sdk/WidgetSdk.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

// Deliberately no C++/WinRT and no windowsapp.lib: a plugin DLL that carries the
// WinRT umbrella import crashes a process in which the host has initialised WinUI
// (measured). Plugins stay plain Win32.
namespace {
std::string to_utf8(std::wstring const &text) {
    if (text.empty())
        return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(size), 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}
bool chinese_ui() {
    return PRIMARYLANGID(GetThreadUILanguage()) == LANG_CHINESE;
}
}

namespace {
constexpr wchar_t kTimerClass[] = L"WindowsWidget.Calendar.Timer";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerPeriodMs = 30000; // only to notice a date rollover

int days_in_month(int year, int month) {
    static const int table[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        return 29;
    return table[std::clamp(month, 1, 12) - 1];
}

int weekday_of(int year, int month, int day) { // 0 = Sunday
    SYSTEMTIME st{};
    st.wYear = WORD(year);
    st.wMonth = WORD(month);
    st.wDay = WORD(day);
    FILETIME ft{};
    SYSTEMTIME out{};
    if (!SystemTimeToFileTime(&st, &ft) || !FileTimeToSystemTime(&ft, &out))
        return 0;
    return out.wDayOfWeek;
}

std::string number(double value, int decimals = 0) {
    char buffer[32];
    sprintf_s(buffer, "%.*f", decimals, value);
    return buffer;
}

// XAML is a text format; the widget only ever emits digits, letters and CJK
// month names, but escape anyway so a plugin config cannot break the document.
std::string escape(std::string const &text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

std::wstring weekday_label(int index) {
    static const wchar_t *zh[] = {L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
    static const wchar_t *en[] = {L"Su", L"Mo", L"Tu", L"We", L"Th", L"Fr", L"Sa"};
    return chinese_ui() ? zh[std::clamp(index, 0, 6)] : en[std::clamp(index, 0, 6)];
}

std::wstring month_title(int year, int month) {
    wchar_t buffer[40];
    if (chinese_ui()) {
        swprintf_s(buffer, L"%d 年 %d 月", year, month);
    } else {
        static const wchar_t *names[] = {L"January", L"February", L"March",     L"April",
                                         L"May",     L"June",     L"July",      L"August",
                                         L"September", L"October", L"November", L"December"};
        swprintf_s(buffer, L"%s %d", names[std::clamp(month, 1, 12) - 1], year);
    }
    return buffer;
}

struct Calendar {
    WidgetHostApi const *host = nullptr;
    WidgetHostApi2 const *ui = nullptr; // non-null once the host confirmed ABI v2
    HWND timer_window = nullptr;
    std::string config = "{}";
    int year = 0, month = 0; // 0 means "follow the system clock"
    int first_day = 1;
    bool dark = true;
    uint32_t dpi = 96, width = 320, height = 300;
    uint64_t last_day_key = 0;

    void log(std::string const &text, uint32_t level = 1) {
        if (host)
            host->log(host->context, level, text.c_str());
    }

    void shown(SYSTEMTIME const &now, int &out_year, int &out_month) const {
        out_year = year ? year : now.wYear;
        out_month = month ? month : now.wMonth;
    }

    std::string markup() {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        int y = 0, m = 0;
        shown(now, y, m);

        std::string bg = dark ? "#FF202023" : "#FFFAFAFA";
        std::string edge = dark ? "#FF3D3D41" : "#FFE2E2E2";
        std::string fg = dark ? "#FFF2F2F2" : "#FF1B1B1B";
        std::string dim = dark ? "#FFA6A6A6" : "#FF5F5F5F";
        const char *accent = "#FF0078D4";
        const char *on_accent = "#FFFFFFFF";

        float cell = std::clamp(width / 9.5f, 20.f, 34.f);
        float day_font = std::clamp(width / 18.f, 10.f, 15.f);
        float head_font = std::clamp(width / 24.f, 11.f, 14.f);
        bool compact = width < 170 || height < 130;

        std::string x;
        x.reserve(6144);
        x += "<Grid xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" "
             "xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">";
        x += "<Border Background=\"" + bg + "\" BorderBrush=\"" + edge +
             "\" BorderThickness=\"1\" CornerRadius=\"10\" Padding=\"10\">";
        x += "<Grid RowSpacing=\"6\"><Grid.RowDefinitions><RowDefinition Height=\"Auto\"/>"
             "<RowDefinition Height=\"Auto\"/><RowDefinition Height=\"*\"/></Grid.RowDefinitions>";

        // Header: chevrons either side of the month, centred on its own column.
        x += "<Grid><Grid.ColumnDefinitions><ColumnDefinition Width=\"Auto\"/>"
             "<ColumnDefinition Width=\"*\"/><ColumnDefinition Width=\"Auto\"/></Grid.ColumnDefinitions>";
        x += "<Button x:Name=\"prev\" Grid.Column=\"0\" Content=\"&#x2039;\" Background=\"Transparent\" "
             "BorderThickness=\"0\" Padding=\"8,2\" Foreground=\"" + dim + "\"/>";
        x += "<TextBlock x:Name=\"title\" Grid.Column=\"1\" HorizontalAlignment=\"Center\" "
             "VerticalAlignment=\"Center\" FontSize=\"" + number(head_font) + "\" FontWeight=\"SemiBold\" "
             "TextTrimming=\"CharacterEllipsis\" Foreground=\"" + fg + "\" Text=\"" +
             escape(to_utf8(month_title(y, m))) + "\"/>";
        x += "<Button x:Name=\"next\" Grid.Column=\"2\" Content=\"&#x203A;\" Background=\"Transparent\" "
             "BorderThickness=\"0\" Padding=\"8,2\" Foreground=\"" + dim + "\"/>";
        x += "</Grid>";

        if (!compact) {
            x += "<Grid Grid.Row=\"1\"><Grid.ColumnDefinitions>";
            for (int n = 0; n < 7; ++n)
                x += "<ColumnDefinition Width=\"*\"/>";
            x += "</Grid.ColumnDefinitions>";
            for (int n = 0; n < 7; ++n) {
                int weekday = (first_day + n) % 7;
                bool weekend = weekday == 0 || weekday == 6;
                x += "<TextBlock Grid.Column=\"" + std::to_string(n) + "\" Text=\"" +
                     escape(to_utf8(weekday_label(weekday))) + "\" FontSize=\"" + number(head_font - 1.f) +
                     "\" HorizontalAlignment=\"Center\" Foreground=\"" + (weekend ? accent : dim) + "\"/>";
            }
            x += "</Grid>";

            int offset = (weekday_of(y, m, 1) - first_day + 7) % 7;
            int count = days_in_month(y, m);
            int rows = (offset + count + 6) / 7;
            x += "<Grid Grid.Row=\"2\"><Grid.RowDefinitions>";
            for (int r = 0; r < rows; ++r)
                x += "<RowDefinition Height=\"*\"/>";
            x += "</Grid.RowDefinitions><Grid.ColumnDefinitions>";
            for (int c = 0; c < 7; ++c)
                x += "<ColumnDefinition Width=\"*\"/>";
            x += "</Grid.ColumnDefinitions>";
            for (int day = 1; day <= count; ++day) {
                int index = offset + day - 1;
                int row = index / 7, column = index % 7;
                bool is_today = y == now.wYear && m == now.wMonth && day == now.wDay;
                int weekday = (first_day + column) % 7;
                bool weekend = weekday == 0 || weekday == 6;
                std::string place = "Grid.Row=\"" + std::to_string(row) + "\" Grid.Column=\"" +
                                    std::to_string(column) + "\"";
                std::string text = std::to_string(day);
                if (is_today) {
                    x += "<Border " + place + " Width=\"" + number(cell) + "\" Height=\"" + number(cell) +
                         "\" CornerRadius=\"" + number(cell / 2.f) + "\" Background=\"" + accent +
                         "\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\">"
                         "<TextBlock Text=\"" + text + "\" FontSize=\"" + number(day_font) +
                         "\" FontWeight=\"SemiBold\" Foreground=\"" + on_accent +
                         "\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\"/></Border>";
                } else {
                    x += "<TextBlock " + place + " Text=\"" + text + "\" FontSize=\"" + number(day_font) +
                         "\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\" Foreground=\"" +
                         (weekend ? accent : fg) + "\"/>";
                }
            }
            x += "</Grid>";
        }
        x += "</Grid></Border></Grid>";
        return x;
    }

    void render() {
        if (!ui || !ui->ui_render)
            return;
        auto document = markup();
        if (!ui->ui_render(host->context, document.c_str()))
            log("calendar: the host rejected the XAML document", 3);
    }

    void configure(char const *text) {
        if (!text)
            return;
        // A minimal scan instead of a JSON library, so the plugin stays free of
        // C++/WinRT: the only configuration key is FirstDayOfWeek, 0..6.
        std::string_view text_view(text);
        const char key[] = {'"', 'f', 'i', 'r', 's', 't', 'D', 'a', 'y', 'O', 'f', 'W', 'e', 'e', 'k', '"', 0};
        auto at = text_view.find(key);
        if (at == std::string_view::npos)
            return;
        auto colon = text_view.find(':', at);
        if (colon == std::string_view::npos)
            return;
        auto value = text_view.find_first_of("0123456789", colon);
        if (value == std::string_view::npos)
            return;
        int parsed = int(text_view[value] - '0');
        if (parsed >= 0 && parsed <= 6)
            first_day = parsed;
    }

    void step(int delta) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        int y = 0, m = 0;
        shown(now, y, m);
        m += delta;
        while (m < 1) {
            m += 12;
            --y;
        }
        while (m > 12) {
            m -= 12;
            ++y;
        }
        year = y;
        month = m;
        render();
    }

    void back_to_today() {
        year = month = 0;
        render();
    }

    static LRESULT CALLBACK timer_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Calendar *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Calendar *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && (m == WM_TIMER || m == WM_TIMECHANGE)) {
            // Repaint only when the date actually rolled over; this runs for days.
            SYSTEMTIME now{};
            GetLocalTime(&now);
            auto key = (uint64_t(now.wYear) * 13 + now.wMonth) * 32 + now.wDay;
            if (m == WM_TIMECHANGE || key != self->last_day_key) {
                self->last_day_key = key;
                self->render();
            }
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

HRESULT __cdecl create(const WidgetCreateInfo *info, void **out, HWND *content) {
    if (!info || !out || !content)
        return E_INVALIDARG;
    *out = nullptr;
    *content = nullptr;
    auto host_v2 = widget_host_v2(info->host);
    if (!host_v2 || !host_v2->ui_render)
        return E_NOINTERFACE; // the host cannot present XAML content
    try {
        auto c = std::make_unique<Calendar>();
        c->host = info->host;
        c->ui = host_v2;
        c->dark = info->dark != 0;
        c->dpi = info->dpi;
        c->configure(info->configuration_utf8);
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&create), &module);
        WNDCLASSW wc{};
        wc.lpfnWndProc = Calendar::timer_proc;
        wc.hInstance = module;
        wc.lpszClassName = kTimerClass;
        RegisterClassW(&wc);
        // Message-only: no painting surface, no desktop presence, just a timer queue.
        c->timer_window = CreateWindowExW(0, kTimerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                          module, c.get());
        if (!c->timer_window)
            return E_FAIL;
        SetTimer(c->timer_window, kTimerId, kTimerPeriodMs, nullptr);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        c->last_day_key = (uint64_t(now.wYear) * 13 + now.wMonth) * 32 + now.wDay;
        c->render();
        *out = c.release();
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

void __cdecl destroy(void *p) {
    auto c = static_cast<Calendar *>(p);
    if (c->timer_window && IsWindow(c->timer_window)) {
        KillTimer(c->timer_window, kTimerId);
        DestroyWindow(c->timer_window);
    }
    delete c;
}

HRESULT __cdecl configure(void *p, const char *text) {
    try {
        auto c = static_cast<Calendar *>(p);
        c->configure(text);
        c->render();
        return S_OK;
    } catch (...) {
        return E_INVALIDARG;
    }
}

void __cdecl layout(void *p, uint32_t width, uint32_t height, uint32_t dpi) {
    auto c = static_cast<Calendar *>(p);
    // Only re-render when the text metrics actually change; the host drops
    // identical documents anyway, but this keeps the string off the heap.
    uint32_t previous = uint32_t(c->width) ^ (uint32_t(c->height) << 16);
    uint32_t current = width ^ (height << 16);
    c->width = width;
    c->height = height;
    c->dpi = dpi;
    if (previous != current)
        c->render();
}

void __cdecl theme(void *p, uint32_t dark) {
    auto c = static_cast<Calendar *>(p);
    if (c->dark == (dark != 0))
        return;
    c->dark = dark != 0;
    c->render();
}

void __cdecl ui_event(void *p, const char *element) {
    auto c = static_cast<Calendar *>(p);
    std::string_view name(element ? element : "");
    if (name == "prev")
        c->step(-1);
    else if (name == "next")
        c->step(1);
    else if (name == "title")
        c->back_to_today();
}
} // namespace

WIDGET_EXPORT HRESULT __cdecl WidgetGetApi(uint32_t version, WidgetApi *api) {
    if (version != WIDGET_ABI_VERSION || !api || api->size < sizeof(WidgetApi))
        return E_NOINTERFACE;
    // Read the host's seeded size before overwriting it: only a host that offered
    // room for the v2 fields may receive them.
    bool extended = api->size >= sizeof(WidgetApi2);
    *api = {sizeof(WidgetApi), WIDGET_ABI_VERSION, create, destroy, configure, layout, theme};
    if (extended) {
        auto *v2 = reinterpret_cast<WidgetApi2 *>(api);
        v2->capabilities = WIDGET_CAPABILITY_XAML;
        v2->ui_event = ui_event;
        v2->size = sizeof(WidgetApi2);
    }
    return S_OK;
}
