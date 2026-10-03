// To-do list widget, presented with real WinUI 3 controls.
//
// Same contract as the calendar: this DLL is plain Win32 and never touches WinRT.
// It declares WIDGET_CAPABILITY_XAML, hands the host XAML through ui_render, and
// gets clicks back by element name through ui_event.
//
// Where the list lives
//   The list is business data, so it is owned by the plugin and stored in its own
//   per-instance data directory - the manager's settings file only ever holds the
//   plugin's configuration.
//   The widget cannot take keyboard focus (the container is WS_EX_NOACTIVATE), so
//   there is no way to type a new item in place. Instead the configuration is an
//   inbox: put entries in "items" from the manager's configuration box, and the
//   plugin moves them into its own list and clears the inbox again. Ticking and
//   deleting happen in the widget itself with the mouse.

#include <windows.h>
#include "../../sdk/WidgetSdk.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kTimerClass[] = L"WindowsWidget.Todo.Timer";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerPeriodMs = 60000;

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

std::string number(double value) {
    char buffer[32];
    sprintf_s(buffer, "%.0f", value);
    return buffer;
}

bool chinese_ui() {
    return PRIMARYLANGID(GetThreadUILanguage()) == LANG_CHINESE;
}

// One item per line: "1<TAB>text" when done, "0<TAB>text" otherwise. A private,
// versionless format is fine here - the file never leaves the plugin's own
// data directory, and a hand-rolled parser keeps the DLL free of C++/WinRT.
std::string encode_item(std::string const &text, bool done) {
    std::string line = done ? "1\t" : "0\t";
    for (char c : text) {
        if (c == '\n')
            line += "\\n";
        else if (c == '\r')
            line += "\\r";
        else
            line += c;
    }
    return line;
}

std::string decode_text(std::string const &line) {
    std::string out;
    out.reserve(line.size());
    for (size_t n = 0; n < line.size(); ++n) {
        if (line[n] == '\\' && n + 1 < line.size()) {
            char next = line[n + 1];
            if (next == 'n') {
                out += '\n';
                ++n;
                continue;
            }
            if (next == 'r') {
                out += '\r';
                ++n;
                continue;
            }
        }
        out += line[n];
    }
    return out;
}

struct Todo {
    struct Item {
        std::string text;
        bool done = false;
    };
    WidgetHostApi const *host = nullptr;
    WidgetHostApi2 const *ui = nullptr;
    HWND timer_window = nullptr;
    std::string data_dir, store_path, config = "{}";
    std::vector<Item> items;
    bool dark = true;
    uint32_t dpi = 96, width = 300, height = 280;
    bool dirty = false;

    void log(std::string const &text, uint32_t level = 1) {
        if (host)
            host->log(host->context, level, text.c_str());
    }

    // ── storage ───────────────────────────────────────────────────────────────

    void load() {
        items.clear();
        if (store_path.empty())
            return;
        FILE *file = nullptr;
        if (fopen_s(&file, store_path.c_str(), "rb") != 0 || !file)
            return;
        std::string content;
        char buffer[4096];
        size_t read = 0;
        while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
            content.append(buffer, read);
        fclose(file);
        size_t at = 0;
        while (at <= content.size() && !content.empty()) {
            auto end = content.find('\n', at);
            if (end == std::string::npos)
                end = content.size();
            std::string line = content.substr(at, end - at);
            if (!line.empty())
                line.erase(line.find_last_not_of("\r") + 1);
            if (line.size() > 2 && (line[0] == '0' || line[1] == '\t'))
                items.push_back({decode_text(line.substr(2)), line[0] == '1'});
            if (end >= content.size())
                break;
            at = end + 1;
        }
    }

    void save() {
        if (store_path.empty()) {
            log("todo: no data directory, changes are not persisted", 2);
            return;
        }
        std::string content;
        for (auto const &item : items) {
            content += encode_item(item.text, item.done);
            content += '\n';
        }
        // Same "write a temp file, then swap" idea the manager uses for its own
        // settings: a widget can be killed at any moment by the Job Object.
        auto temp = store_path + ".tmp";
        FILE *file = nullptr;
        if (fopen_s(&file, temp.c_str(), "wb") != 0 || !file) {
            log("todo: cannot write the list", 2);
            return;
        }
        fwrite(content.data(), 1, content.size(), file);
        fclose(file);
        MoveFileExW(to_wide(temp).c_str(), to_wide(store_path).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        dirty = false;
    }

    // ── configuration inbox ───────────────────────────────────────────────────

    static std::vector<std::string> strings_in(char const *text) {
        std::vector<std::string> found;
        if (!text)
            return found;
        std::string_view view(text);
        auto key = std::string_view("\"items\"");
        auto at = view.find(key);
        if (at == std::string_view::npos)
            return found;
        auto open = view.find('[', at);
        auto close = open == std::string_view::npos ? std::string_view::npos : view.find(']', open);
        if (open == std::string_view::npos || close == std::string_view::npos)
            return found;
        for (size_t n = open; n < close; ++n) {
            if (view[n] != '"')
                continue;
            auto end = view.find('"', n + 1);
            if (end == std::string_view::npos || end > close)
                break;
            found.emplace_back(view.substr(n + 1, end - n - 1));
            n = end;
        }
        return found;
    }

    void import(char const *text) {
        for (auto const &entry : strings_in(text)) {
            if (entry.empty())
                continue;
            bool known = false;
            for (auto const &item : items)
                if (item.text == entry) {
                    known = true;
                    break;
                }
            if (!known)
                items.push_back({entry, false});
        }
    }

    // ── rendering ─────────────────────────────────────────────────────────────

    std::string markup() {
        std::string bg = dark ? "#FF202023" : "#FFFAFAFA";
        std::string edge = dark ? "#FF3D3D41" : "#FFE2E2E2";
        std::string fg = dark ? "#FFF2F2F2" : "#FF1B1B1B";
        std::string dim = dark ? "#FFA6A6A6" : "#FF5F5F5F";
        const char *accent = "#FF0078D4";
        float body = std::clamp(width / 24.f, 11.f, 14.f);
        float head = std::clamp(width / 20.f, 12.f, 16.f);

        int remaining = 0;
        for (auto const &item : items)
            if (!item.done)
                ++remaining;

        std::string x;
        x.reserve(4096);
        x += "<Grid xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" "
             "xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">";
        x += "<Border Background=\"" + bg + "\" BorderBrush=\"" + edge +
             "\" BorderThickness=\"1\" CornerRadius=\"10\" Padding=\"12\">";
        x += "<Grid RowSpacing=\"8\"><Grid.RowDefinitions><RowDefinition Height=\"Auto\"/>"
             "<RowDefinition Height=\"*\"/></Grid.RowDefinitions>";

        x += "<Grid><Grid.ColumnDefinitions><ColumnDefinition Width=\"*\"/>"
             "<ColumnDefinition Width=\"Auto\"/></Grid.ColumnDefinitions>";
        x += "<TextBlock Text=\"" +
             escape(to_utf8(chinese_ui() ? L"待办" : L"To-do")) + "\" FontSize=\"" + number(head) +
             "\" FontWeight=\"SemiBold\" Foreground=\"" + fg + "\" VerticalAlignment=\"Center\"/>";
        x += "<TextBlock Grid.Column=\"1\" Text=\"" + std::to_string(remaining) + " / " +
             std::to_string(items.size()) + "\" FontSize=\"" + number(body - 1) +
             "\" Foreground=\"" + dim + "\" VerticalAlignment=\"Center\"/>";
        x += "</Grid>";

        if (items.empty()) {
            x += "<TextBlock Grid.Row=\"1\" Text=\"" +
                 escape(to_utf8(chinese_ui()
                                    ? L"清单为空。\n在管理器的插件配置里填 items"
                                    : L"Nothing here yet.\nAdd items in the plugin configuration")) +
                 "\" FontSize=\"" + number(body) +
                 "\" TextWrapping=\"Wrap\" Foreground=\"" + dim +
                 "\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\"/>";
        } else {
            // One row per item, each with a tick button and a delete button. The
            // names are positional ("t3"/"x3"), which is all the host reports back.
            x += "<ScrollViewer Grid.Row=\"1\" VerticalScrollBarVisibility=\"Auto\">";
            x += "<StackPanel Spacing=\"2\">";
            for (size_t n = 0; n < items.size(); ++n) {
                auto const &item = items[n];
                std::string index = std::to_string(n);
                x += "<Grid><Grid.ColumnDefinitions><ColumnDefinition Width=\"Auto\"/>"
                     "<ColumnDefinition Width=\"*\"/><ColumnDefinition Width=\"Auto\"/></Grid.ColumnDefinitions>";
                x += "<Button x:Name=\"t" + index + "\" Content=\"" +
                     (item.done ? "&#x2713;" : "&#x25CB;") +
                     "\" Background=\"Transparent\" BorderThickness=\"0\" Padding=\"6,0\" FontSize=\"" +
                     number(body) + "\" Foreground=\"" +
                     std::string(item.done ? dim : accent) + "\"/>";
                x += "<TextBlock Grid.Column=\"1\" Text=\"" + escape(item.text) + "\" FontSize=\"" +
                     number(body) + "\" TextTrimming=\"CharacterEllipsis\" VerticalAlignment=\"Center\" "
                     "Foreground=\"" + (item.done ? dim : fg) + "\" TextDecorations=\"" +
                     (item.done ? "Strikethrough" : "") + "\"/>";
                x += "<Button x:Name=\"x" + index +
                     "\" Grid.Column=\"2\" Content=\"&#x2715;\" Background=\"Transparent\" "
                     "BorderThickness=\"0\" Padding=\"6,0\" FontSize=\"" + number(body - 2) +
                     "\" Foreground=\"" + dim + "\"/>";
                x += "</Grid>";
            }
            x += "</StackPanel></ScrollViewer>";
        }
        x += "</Grid></Border></Grid>";
        return x;
    }

    void render() {
        if (!ui || !ui->ui_render)
            return;
        auto document = markup();
        if (!ui->ui_render(host->context, document.c_str()))
            log("todo: the host rejected the XAML document", 3);
    }

    void toggle(size_t index) {
        if (index >= items.size())
            return;
        items[index].done = !items[index].done;
        dirty = true;
        save();
        render();
    }

    void remove(size_t index) {
        if (index >= items.size())
            return;
        items.erase(items.begin() + ptrdiff_t(index));
        dirty = true;
        save();
        render();
    }

    static LRESULT CALLBACK timer_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Todo *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Todo *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && m == WM_TIMER) {
            // A compaction tick: nothing to do here normally, but it is the natural
            // place to flush a pending write after the widget has been idle.
            if (self->dirty)
                self->save();
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
        auto c = std::make_unique<Todo>();
        c->host = info->host;
        c->ui = host_v2;
        c->dark = info->dark != 0;
        c->dpi = info->dpi;
        if (host_v2->data_directory_utf8)
            c->data_dir = host_v2->data_directory_utf8;
        if (!c->data_dir.empty())
            c->store_path = c->data_dir + "\\todos.txt";
        c->load();
        c->import(info->configuration_utf8);
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&create), &module);
        WNDCLASSW wc{};
        wc.lpfnWndProc = Todo::timer_proc;
        wc.hInstance = module;
        wc.lpszClassName = kTimerClass;
        RegisterClassW(&wc);
        c->timer_window = CreateWindowExW(0, kTimerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                          module, c.get());
        if (!c->timer_window)
            return E_FAIL;
        SetTimer(c->timer_window, kTimerId, kTimerPeriodMs, nullptr);
        c->save();
        c->render();
        *out = c.release();
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

void __cdecl destroy(void *p) {
    auto c = static_cast<Todo *>(p);
    if (c->dirty)
        c->save();
    if (c->timer_window && IsWindow(c->timer_window)) {
        KillTimer(c->timer_window, kTimerId);
        DestroyWindow(c->timer_window);
    }
    delete c;
}

HRESULT __cdecl configure(void *p, const char *text) {
    try {
        auto c = static_cast<Todo *>(p);
        // The configuration is an inbox: anything new in "items" moves into the
        // plugin's own store, and the inbox is emptied again so the manager's
        // settings file does not carry business data long term.
        c->import(text);
        c->save();
        c->render();
        return S_OK;
    } catch (...) {
        return E_INVALIDARG;
    }
}

void __cdecl layout(void *p, uint32_t width, uint32_t height, uint32_t dpi) {
    auto c = static_cast<Todo *>(p);
    uint32_t previous = uint32_t(c->width) ^ (uint32_t(c->height) << 16);
    uint32_t current = width ^ (height << 16);
    c->width = width;
    c->height = height;
    c->dpi = dpi;
    if (previous != current)
        c->render();
}

void __cdecl theme(void *p, uint32_t dark) {
    auto c = static_cast<Todo *>(p);
    if (c->dark == (dark != 0))
        return;
    c->dark = dark != 0;
    c->render();
}

void __cdecl ui_event(void *p, const char *element) {
    auto c = static_cast<Todo *>(p);
    std::string_view name(element ? element : "");
    if (name.size() < 2)
        return;
    size_t index = 0;
    for (size_t n = 1; n < name.size(); ++n) {
        if (name[n] < '0' || name[n] > '9')
            return;
        index = index * 10 + size_t(name[n] - '0');
    }
    if (name[0] == 't')
        c->toggle(index);
    else if (name[0] == 'x')
        c->remove(index);
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
