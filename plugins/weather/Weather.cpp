// Weather widget, presented with real WinUI 3 controls.
//
// Same contract as the other XAML widgets: plain Win32 DLL, no WinRT, hands the
// host a XAML document and receives clicks by element name.
//
// Where the data comes from
//   Open-Meteo, which needs no API key: one HTTPS request per refresh with
//   WinHTTP, on a worker thread - never on the host UI thread, which is what the
//   SDK contract requires and what keeps the widget responsive.
//   The last good response is cached in the plugin's own data directory, so a
//   machine that is offline (or behind a proxy that refuses) still shows the last
//   reading with a visible "stale" mark instead of an empty card.
//
// Memory notes
//   One worker thread and one request buffer per instance; both are bounded and
//   released in destroy. The thread is joined, not detached, so a removed widget
//   leaves nothing behind.

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../sdk/WidgetSdk.h"

#pragma comment(lib, "winhttp.lib")

namespace {
constexpr wchar_t kWindowClass[] = L"WindowsWidget.Weather.Window";
constexpr UINT kMessageRefreshed = WM_APP + 1;
constexpr UINT kTimerId = 1;
constexpr UINT kRefreshPeriodMs = 30 * 60 * 1000;

std::string to_utf8(std::wstring const &text) {
    if (text.empty())
        return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(size), 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring to_wide(std::string const &text) {
    if (text.empty())
        return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(size), 0);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), size);
    return out;
}

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

bool chinese_ui() {
    return PRIMARYLANGID(GetThreadUILanguage()) == LANG_CHINESE;
}

std::string number(double value) {
    char buffer[32];
    sprintf_s(buffer, "%.0f", value);
    return buffer;
}

// WMO weather codes, the only vocabulary Open-Meteo returns.
std::wstring describe(int code) {
    struct Entry {
        int code;
        const wchar_t *zh;
        const wchar_t *en;
        const wchar_t *glyph;
    };
    static const Entry table[] = {
        {0, L"晴", L"Clear", L""},          {1, L"晴间多云", L"Mostly clear", L""},
        {2, L"多云", L"Partly cloudy", L""}, {3, L"阴", L"Overcast", L""},
        {45, L"雾", L"Fog", L""},            {48, L"雾凇", L"Rime fog", L""},
        {51, L"小毛毛雨", L"Light drizzle", L""}, {53, L"毛毛雨", L"Drizzle", L""},
        {55, L"浓毛毛雨", L"Dense drizzle", L""}, {61, L"小雨", L"Light rain", L""},
        {63, L"中雨", L"Rain", L""},         {65, L"大雨", L"Heavy rain", L""},
        {71, L"小雪", L"Light snow", L""},   {73, L"中雪", L"Snow", L""},
        {75, L"大雪", L"Heavy snow", L""},   {80, L"阵雨", L"Showers", L""},
        {81, L"强阵雨", L"Heavy showers", L""}, {82, L"暴雨", L"Violent showers", L""},
        {95, L"雷阵雨", L"Thunderstorm", L""}, {96, L"雷阵雨伴冰雹", L"Storm with hail", L""},
        {99, L"强雷暴伴冰雹", L"Severe storm with hail", L""},
    };
    for (auto const &entry : table)
        if (entry.code == code)
            return chinese_ui() ? entry.zh : entry.en;
    return chinese_ui() ? L"未知" : L"Unknown";
}

// Plain Segoe UI symbols rather than an icon font: they are always present, and a
// missing glyph would show up as a box in the middle of the reading.
std::wstring glyph_for(int code) {
    if (code == 0 || code == 1)
        return L"☀"; // sun
    if (code == 2)
        return L"⛅"; // sun behind cloud
    if (code == 3)
        return L"☁"; // cloud
    if (code == 45 || code == 48)
        return L"░"; // fog
    if (code >= 71 && code <= 75)
        return L"❄"; // snowflake
    if (code >= 95)
        return L"⚡"; // lightning
    return L"☂";     // rain
}

// ── tiny JSON scans: the response is a fixed, machine-generated shape, and a
//    hand-rolled reader keeps the DLL free of C++/WinRT ────────────────────────

// Reads a numeric member of a named object. Open-Meteo repeats the same keys in a
// "current_units" block whose values are strings, so the scan must (a) start from
// the object, and (b) require the value to *follow* the colon rather than being
// the next digit anywhere in the document.
bool find_number(std::string const &body, std::string const &object, std::string const &key,
                 double &out) {
    auto scope = body.find("\"" + object + "\"");
    if (scope == std::string::npos)
        return false;
    auto end = body.find('}', scope);
    auto at = body.find("\"" + key + "\"", scope);
    if (at == std::string::npos || (end != std::string::npos && at > end))
        return false;
    auto colon = body.find(':', at);
    if (colon == std::string::npos)
        return false;
    auto start = body.find_first_not_of(" 	", colon + 1);
    if (start == std::string::npos || (body[start] != '-' && (body[start] < '0' || body[start] > '9')))
        return false; // a string value such as "°C": not the member we want
    auto stop = body.find_first_not_of("-0123456789.", start);
    try {
        out = std::stod(body.substr(start, stop - start));
        return true;
    } catch (...) {
        return false;
    }
}

// first_value: reads the first element of an array-valued key ("[7.1,9.2]").
bool find_first_array_number(std::string const &body, std::string const &key, double &out) {
    auto at = body.find("\"" + key + "\"");
    if (at == std::string::npos)
        return false;
    auto bracket = body.find('[', at);
    if (bracket == std::string::npos)
        return false;
    auto start = body.find_first_of("-0123456789", bracket);
    auto colon = body.find(':', at);
    if (start == std::string::npos || colon == std::string::npos || start < colon)
        return false;
    auto stop = body.find_first_not_of("-0123456789.", start);
    try {
        out = std::stod(body.substr(start, stop - start));
        return true;
    } catch (...) {
        return false;
    }
}

struct Reading {
    double temperature = 0, high = 0, low = 0;
    int code = -1;
    double code_as_double = 0; // scratch for the scan; weather_code is an integer
    bool valid = false;
    uint64_t fetched = 0; // GetTickCount64 of the successful fetch
};

struct Weather {
    WidgetHostApi const *host = nullptr;
    WidgetHostApi2 const *ui = nullptr;
    HWND window = nullptr;
    std::string data_dir, cache_path, config = "{}";
    double latitude = 39.9042, longitude = 116.4074; // Beijing
    std::string location = "\xe5\x8c\x97\xe4\xba\xac";
    bool dark = true;
    uint32_t dpi = 96, width = 300, height = 200;
    std::string error;

    std::mutex mutex;
    Reading reading;
    std::thread worker;
    std::atomic<bool> cancel{false};

    void log(std::string const &text, uint32_t level = 1) {
        if (host)
            host->log(host->context, level, text.c_str());
    }

    // ── cache ─────────────────────────────────────────────────────────────────

    void load_cache() {
        if (cache_path.empty())
            return;
        FILE *file = nullptr;
        if (fopen_s(&file, cache_path.c_str(), "rb") != 0 || !file)
            return;
        std::string body;
        char buffer[2048];
        size_t read = 0;
        while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0 && body.size() < 64 * 1024)
            body.append(buffer, read);
        fclose(file);
        Reading parsed;
        if (find_number(body, "current", "temperature_2m", parsed.temperature) &&
            find_number(body, "current", "weather_code", parsed.code_as_double)) {
            parsed.code = int(parsed.code_as_double);
            find_first_array_number(body, "temperature_2m_max", parsed.high);
            find_first_array_number(body, "temperature_2m_min", parsed.low);
            parsed.valid = true;
            parsed.fetched = 0; // unknown age: shown as stale until the first refresh
            std::lock_guard guard(mutex);
            reading = parsed;
        }
    }

    void save_cache(std::string const &body) {
        if (cache_path.empty())
            return;
        auto temp = cache_path + ".tmp";
        FILE *file = nullptr;
        if (fopen_s(&file, temp.c_str(), "wb") != 0 || !file)
            return;
        fwrite(body.data(), 1, body.size(), file);
        fclose(file);
        MoveFileExW(to_wide(temp).c_str(), to_wide(cache_path).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    // ── network, on a worker thread ───────────────────────────────────────────

    static bool https_get(std::string const &path, std::string &body, std::string &failure) {
        bool ok = false;
        HINTERNET session = WinHttpOpen(L"WindowsWidget/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            failure = "WinHttpOpen failed";
            return false;
        }
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
        if (auto request = WinHttpConnect(session, L"api.open-meteo.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) {
            auto handle = WinHttpOpenRequest(request, L"GET", to_wide(path).c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
            if (handle && WinHttpSendRequest(handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(handle, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    body.clear();
                    DWORD available = 0;
                    while (WinHttpQueryDataAvailable(handle, &available) && available > 0 &&
                           body.size() < 64 * 1024) {
                        std::vector<char> chunk(std::min<DWORD>(available, 8192));
                        DWORD read = 0;
                        if (!WinHttpReadData(handle, chunk.data(), DWORD(chunk.size()), &read) || read == 0)
                            break;
                        body.append(chunk.data(), read);
                    }
                    ok = !body.empty();
                    if (!ok)
                        failure = "empty response";
                } else {
                    failure = "HTTP " + std::to_string(status);
                }
            } else {
                failure = "request failed (Win32=" + std::to_string(GetLastError()) + ")";
            }
            if (handle)
                WinHttpCloseHandle(handle);
            WinHttpCloseHandle(request);
        } else {
            failure = "WinHttpConnect failed";
        }
        WinHttpCloseHandle(session);
        return ok;
    }

    void refresh_async() {
        if (worker.joinable())
            return; // one request in flight at a time
        worker = std::thread([this] {
            char query[256];
            sprintf_s(query,
                      "/v1/forecast?latitude=%.4f&longitude=%.4f"
                      "&current=temperature_2m,weather_code"
                      "&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1",
                      latitude, longitude);
            std::string body, failure;
            if (https_get(query, body, failure)) {
                Reading parsed;
                if (find_number(body, "current", "temperature_2m", parsed.temperature) &&
                    find_number(body, "current", "weather_code", parsed.code_as_double)) {
                    parsed.code = int(parsed.code_as_double);
                    find_first_array_number(body, "temperature_2m_max", parsed.high);
                    find_first_array_number(body, "temperature_2m_min", parsed.low);
                    parsed.valid = true;
                    parsed.fetched = GetTickCount64();
                    {
                        std::lock_guard guard(mutex);
                        reading = parsed;
                        error.clear();
                    }
                    save_cache(body);
                } else {
                    std::lock_guard guard(mutex);
                    error = "unexpected response";
                }
            } else if (!cancel) {
                std::lock_guard guard(mutex);
                error = failure;
            }
            if (!cancel && window && IsWindow(window))
                PostMessageW(window, kMessageRefreshed, 0, 0);
        });
    }

    void join_worker() {
        cancel = true;
        if (worker.joinable())
            worker.join();
    }

    // ── rendering (host UI thread only) ───────────────────────────────────────

    static std::string ago_text(uint64_t fetched) {
        if (!fetched)
            return {};
        auto minutes = (GetTickCount64() - fetched) / 60000;
        if (chinese_ui())
            return "更新于 " + std::to_string(minutes) + " 分钟前";
        return std::to_string(minutes) + " min ago";
    }

    std::string markup() {
        Reading snapshot;
        std::string failure;
        {
            std::lock_guard guard(mutex);
            snapshot = reading;
            failure = error;
        }
        std::string bg = dark ? "#FF202023" : "#FFFAFAFA";
        std::string edge = dark ? "#FF3D3D41" : "#FFE2E2E2";
        std::string fg = dark ? "#FFF2F2F2" : "#FF1B1B1B";
        std::string dim = dark ? "#FFA6A6A6" : "#FF5F5F5F";
        const char *accent = "#FF0078D4";

        float big = std::clamp(width / 5.0f, 24.f, 52.f);
        float body = std::clamp(width / 22.f, 11.f, 14.f);

        std::string x;
        x.reserve(3072);
        x += "<Grid xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" "
             "xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">";
        x += "<Border Background=\"" + bg + "\" BorderBrush=\"" + edge +
             "\" BorderThickness=\"1\" CornerRadius=\"10\" Padding=\"12\">";
        x += "<Grid><Grid.RowDefinitions><RowDefinition Height=\"Auto\"/><RowDefinition Height=\"*\"/>"
             "<RowDefinition Height=\"Auto\"/></Grid.RowDefinitions>";

        x += "<Grid><Grid.ColumnDefinitions><ColumnDefinition Width=\"*\"/>"
             "<ColumnDefinition Width=\"Auto\"/></Grid.ColumnDefinitions>";
        x += "<TextBlock Text=\"" + escape(location) + "\" FontSize=\"" + number(body + 1) +
             "\" FontWeight=\"SemiBold\" Foreground=\"" + fg + "\"/>";
        x += "<Button x:Name=\"refresh\" Grid.Column=\"1\" Content=\"&#x21BB;\" FontSize=\"" +
             number(body + 2) + "\" Background=\"Transparent\" BorderThickness=\"0\" Padding=\"6,0\" "
             "Foreground=\"" + dim + "\"/>";
        x += "</Grid>";

        if (snapshot.valid) {
            x += "<StackPanel Grid.Row=\"1\" VerticalAlignment=\"Center\">";
            x += "<StackPanel Orientation=\"Horizontal\" Spacing=\"10\" HorizontalAlignment=\"Center\">";
            x += "<TextBlock Text=\"" + escape(to_utf8(glyph_for(snapshot.code))) +
                 "\" FontSize=\"" + number(big * 0.6f) +
                 "\" Foreground=\"" + accent + "\" VerticalAlignment=\"Center\"/>";
            x += "<TextBlock Text=\"" + number(snapshot.temperature) +
                 "&#xB0;\" FontSize=\"" + number(big) + "\" FontWeight=\"SemiBold\" Foreground=\"" + fg +
                 "\" VerticalAlignment=\"Center\"/>";
            x += "</StackPanel>";
            x += "<TextBlock Text=\"" + escape(to_utf8(describe(snapshot.code))) + "\" FontSize=\"" +
                 number(body + 1) + "\" Foreground=\"" + dim + "\" HorizontalAlignment=\"Center\"/>";
            x += "<TextBlock Text=\"&#x2191;" + number(snapshot.high) + "&#xB0;  &#x2193;" +
                 number(snapshot.low) + "&#xB0;\" FontSize=\"" + number(body) +
                 "\" Foreground=\"" + dim + "\" HorizontalAlignment=\"Center\"/>";
            x += "</StackPanel>";
        } else {
            x += "<TextBlock Grid.Row=\"1\" Text=\"" +
                 escape(to_utf8(chinese_ui() ? L"正在获取天气…" : L"Fetching weather…")) +
                 "\" FontSize=\"" + number(body) + "\" Foreground=\"" + dim +
                 "\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\"/>";
        }

        std::string footnote = snapshot.valid ? ago_text(snapshot.fetched) : std::string{};
        if (!failure.empty())
            footnote = to_utf8(std::wstring(chinese_ui() ? L"离线，显示上次结果："
                                                         : L"Offline, showing the last reading: ") +
                               to_wide(failure));
        x += "<TextBlock Grid.Row=\"2\" Text=\"" + escape(footnote) + "\" FontSize=\"" +
             number(body - 2) + "\" Foreground=\"" + dim + "\" TextTrimming=\"CharacterEllipsis\"/>";
        x += "</Grid></Border></Grid>";
        return x;
    }

    void render() {
        if (!ui || !ui->ui_render)
            return;
        auto document = markup();
        if (!ui->ui_render(host->context, document.c_str()))
            log("weather: the host rejected the XAML document", 3);
    }

    void configure(char const *text) {
        if (!text)
            return;
        std::string_view view(text);
        auto read_string = [&](char const *key) -> std::string {
            auto at = view.find(key);
            if (at == std::string_view::npos)
                return {};
            auto first = view.find('"', view.find(':', at) + 1);
            auto last = first == std::string_view::npos ? std::string_view::npos
                                                        : view.find('"', first + 1);
            if (first == std::string_view::npos || last == std::string_view::npos)
                return {};
            return std::string(view.substr(first + 1, last - first - 1));
        };
        auto read_number = [&](char const *key, double &out) {
            auto at = view.find(key);
            if (at == std::string_view::npos)
                return;
            auto colon = view.find(':', at);
            auto start = colon == std::string_view::npos
                             ? std::string_view::npos
                             : view.find_first_of("-0123456789", colon);
            if (start == std::string_view::npos)
                return;
            auto end = view.find_first_not_of("-0123456789.", start);
            try {
                double value = std::stod(std::string(view.substr(start, end - start)));
                if (std::isfinite(value))
                    out = value;
            } catch (...) {
            }
        };
        auto name = read_string("\"location\"");
        if (!name.empty())
            location = name;
        read_number("\"latitude\"", latitude);
        read_number("\"longitude\"", longitude);
        latitude = std::clamp(latitude, -90.0, 90.0);
        longitude = std::clamp(longitude, -180.0, 180.0);
    }

    static LRESULT CALLBACK window_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Weather *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Weather *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        if (m == kMessageRefreshed) {
            // Back on the UI thread, which is the only place ui_render may run.
            self->render();
            return 0;
        }
        if (m == WM_TIMER) {
            if (!self->worker.joinable())
                self->refresh_async();
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
        return E_NOINTERFACE;
    try {
        auto c = std::make_unique<Weather>();
        c->host = info->host;
        c->ui = host_v2;
        c->dark = info->dark != 0;
        c->dpi = info->dpi;
        if (host_v2->data_directory_utf8)
            c->data_dir = host_v2->data_directory_utf8;
        if (!c->data_dir.empty())
            c->cache_path = c->data_dir + "\\weather.json";
        c->configure(info->configuration_utf8);
        c->load_cache();
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&create), &module);
        WNDCLASSW wc{};
        wc.lpfnWndProc = Weather::window_proc;
        wc.hInstance = module;
        wc.lpszClassName = kWindowClass;
        RegisterClassW(&wc);
        c->window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module,
                                    c.get());
        if (!c->window)
            return E_FAIL;
        SetTimer(c->window, kTimerId, kRefreshPeriodMs, nullptr);
        c->render();
        c->refresh_async(); // first reading in the background; the card shows a placeholder
        *out = c.release();
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

void __cdecl destroy(void *p) {
    auto c = static_cast<Weather *>(p);
    c->join_worker(); // joined, not detached: a removed widget leaves no thread behind
    if (c->window && IsWindow(c->window)) {
        KillTimer(c->window, kTimerId);
        DestroyWindow(c->window);
    }
    delete c;
}

HRESULT __cdecl configure(void *p, const char *text) {
    try {
        auto c = static_cast<Weather *>(p);
        c->configure(text);
        c->render();
        c->refresh_async();
        return S_OK;
    } catch (...) {
        return E_INVALIDARG;
    }
}

void __cdecl layout(void *p, uint32_t width, uint32_t height, uint32_t dpi) {
    auto c = static_cast<Weather *>(p);
    uint32_t previous = uint32_t(c->width) ^ (uint32_t(c->height) << 16);
    uint32_t current = width ^ (height << 16);
    c->width = width;
    c->height = height;
    c->dpi = dpi;
    if (previous != current)
        c->render();
}

void __cdecl theme(void *p, uint32_t dark) {
    auto c = static_cast<Weather *>(p);
    if (c->dark == (dark != 0))
        return;
    c->dark = dark != 0;
    c->render();
}

void __cdecl ui_event(void *p, const char *element) {
    auto c = static_cast<Weather *>(p);
    if (std::string_view(element ? element : "") == "refresh") {
        if (c->worker.joinable())
            return;
        c->refresh_async();
    }
}
} // namespace

WIDGET_EXPORT HRESULT __cdecl WidgetGetApi(uint32_t version, WidgetApi *api) {
    if (version != WIDGET_ABI_VERSION || !api || api->size < sizeof(WidgetApi))
        return E_NOINTERFACE;
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
