#pragma once
#include "Common.h"
#include "Language.h"
namespace ww {
struct Plugin {
    std::string id, name, error;
    fs::path directory, dll;
    double width = 260, height = 160, min_width = 140, min_height = 100, max_width = 1000, max_height = 1000;
    int columns = 0, rows = 0;
};
struct Instance {
    std::string id, plugin, monitor, config = "{}", status = "Stopped", error;
    double x = 40, y = 40, width = 260, height = 160;
    int columns = 0, rows = 0; // Zero pair migrates legacy DIP sizes on first placement.
    uint32_t layout_revision = 0;
    bool enabled = true, locked = false;
    DWORD pid = 0;
    uint64_t load_ms = 0;
};
inline int grid_count(JsonObject const &j, wchar_t const *key) {
    double value = j.GetNamedNumber(key, 0);
    // Zero is the sentinel for "omitted", not a usable size: a manifest without
    // grid cells is migrated from its legacy DIP dimensions on first placement.
    if (!std::isfinite(value) || value != std::floor(value) || value < 0 || value > 12)
        throw std::runtime_error("Grid size must be a whole number from 1 to 12, or 0 when omitted");
    return int(value);
}
inline JsonObject encode(Instance const &i) {
    JsonObject j;
    put(j, L"id", i.id);
    put(j, L"plugin", i.plugin);
    put(j, L"monitor", i.monitor);
    put(j, L"x", i.x);
    put(j, L"y", i.y);
    put(j, L"width", i.width);
    put(j, L"height", i.height);
    put(j, L"gridColumns", double(i.columns));
    put(j, L"gridRows", double(i.rows));
    put(j, L"layoutRevision", double(i.layout_revision));
    put(j, L"enabled", i.enabled);
    put(j, L"locked", i.locked);
    j.SetNamedValue(L"configuration", json(i.config));
    return j;
}
inline Instance decode(JsonObject const &j) {
    Instance i;
    i.id = get(j, L"id");
    i.plugin = get(j, L"plugin");
    i.monitor = get(j, L"monitor");
    i.x = j.GetNamedNumber(L"x", 40);
    i.y = j.GetNamedNumber(L"y", 40);
    i.width = j.GetNamedNumber(L"width", 260);
    i.height = j.GetNamedNumber(L"height", 160);
    i.columns = grid_count(j, L"gridColumns");
    i.rows = grid_count(j, L"gridRows");
    auto revision = j.GetNamedNumber(L"layoutRevision", 0);
    if ((i.columns == 0) != (i.rows == 0) || !std::isfinite(revision) || revision < 0 ||
        revision > UINT32_MAX || revision != std::floor(revision))
        throw std::runtime_error("Invalid grid layout");
    i.layout_revision = uint32_t(revision);
    i.enabled = j.GetNamedBoolean(L"enabled", true);
    i.locked = j.GetNamedBoolean(L"locked", false);
    i.config = str(j.GetNamedObject(L"configuration", JsonObject()));
    if (i.id.empty() || i.plugin.empty() || i.width < 1 || i.height < 1 || !std::isfinite(i.x) ||
        !std::isfinite(i.y) || std::abs(i.x) > 1000000 || std::abs(i.y) > 1000000 || i.width > 10000 ||
        i.height > 10000)
        throw std::runtime_error("Invalid instance layout");
    return i;
}
inline std::vector<Plugin> discover(fs::path const &root) {
    std::vector<Plugin> result;
    if (!fs::exists(root))
        return result;
    for (auto const &e : fs::directory_iterator(root)) {
        if (!e.is_directory() || !fs::exists(e.path() / L"widget.json"))
            continue;
        Plugin p;
        p.directory = e.path();
        p.name = utf8(e.path().filename().wstring());
        try {
            auto j = json(read_file(e.path() / L"widget.json"));
            p.id = get(j, L"id");
            p.name = get(j, L"name", p.name);
            if (p.id.empty() || get(j, L"version").empty())
                throw std::runtime_error("Missing id/version");
            if (j.GetNamedNumber(L"sdk", 0) != 1)
                throw std::runtime_error("Unsupported SDK version");
            if (get(j, L"architecture") != "x64")
                throw std::runtime_error("Plugin must be x64");
            fs::path dll = wide(get(j, L"entry"));
            if (dll.empty() || dll.is_absolute())
                throw std::runtime_error("Invalid entry path");
            for (auto const &part : dll)
                if (part == L"..")
                    throw std::runtime_error("Entry escapes plugin directory");
            p.dll = fs::weakly_canonical(e.path() / dll);
            auto base = fs::weakly_canonical(e.path());
            auto rel = p.dll.lexically_relative(base);
            if (rel.empty() || *rel.begin() == L"..")
                throw std::runtime_error("DLL outside plugin directory");
            p.width = j.GetNamedNumber(L"width", 260);
            p.height = j.GetNamedNumber(L"height", 160);
            p.columns = grid_count(j, L"gridColumns");
            p.rows = grid_count(j, L"gridRows");
            if ((p.columns == 0) != (p.rows == 0))
                throw std::runtime_error("Both grid dimensions are required");
            p.min_width = j.GetNamedNumber(L"minWidth", 140);
            p.min_height = j.GetNamedNumber(L"minHeight", 100);
            p.max_width = j.GetNamedNumber(L"maxWidth", 1000);
            p.max_height = j.GetNamedNumber(L"maxHeight", 1000);
            if (p.min_width < 32 || p.min_height < 32 || p.max_width > 4096 || p.max_height > 4096 ||
                p.width < p.min_width || p.width > p.max_width || p.height < p.min_height ||
                p.height > p.max_height)
                throw std::runtime_error("Invalid size constraints");
            if (!fs::exists(p.dll))
                throw std::runtime_error("DLL missing");
            for (auto const &old : result)
                if (old.id == p.id)
                    throw std::runtime_error("Duplicate plugin id");
        } catch (...) {
            p.error = error_text();
        }
        result.push_back(std::move(p));
    }
    return result;
}
class Store {
    fs::path file_;
    bool writable_ = true;
    std::vector<Instance> parse(fs::path const &p) {
        auto j = json(read_file(p));
        auto v = j.GetNamedNumber(L"version", 0);
        if (v != 1)
            throw std::runtime_error("Unsupported settings version");
        theme_mode = std::clamp(int(j.GetNamedNumber(L"themeMode", 0)), 0, 2);
        language = normalize_language(get(j, L"language", default_language()));
        std::vector<Instance> r;
        for (auto const &x : j.GetNamedArray(L"instances")) {
            auto i = decode(x.GetObject());
            for (auto const &old : r)
                if (old.id == i.id)
                    throw std::runtime_error("Duplicate instance id");
            r.push_back(i);
        }
        return r;
    }

  public:
    explicit Store(fs::path root) : file_(root / L"settings.json") {}
    int theme_mode = 0;
    std::string language = default_language();
    std::string warning;
    bool writable() const {
        return writable_;
    }
    std::vector<Instance> load() {
        if (!fs::exists(file_))
            return {};
        try {
            auto raw = json(read_file(file_));
            if (raw.GetNamedNumber(L"version", 0) > 1) {
                writable_ = false;
                warning = "Newer settings format; saving disabled";
                return {};
            }
            return parse(file_);
        } catch (...) {
            warning = "Settings invalid: " + error_text();
            auto backup = file_;
            backup += L".bak";
            try {
                auto r = parse(backup);
                fs::copy_file(file_, file_.wstring() + L".corrupt-" +
                                         wide(guid())); // keep evidence; repair before next backup rotation
                atomic_write(file_, read_file(backup), false);
                warning += "; recovered backup";
                return r;
            } catch (...) {
                writable_ = false;
                warning += "; no valid backup, saving disabled";
                return {};
            }
        }
    }
    void save(std::vector<Instance> const &list) {
        save(list, theme_mode);
    }
    void save(std::vector<Instance> const &list, int theme) {
        save(list, theme, language);
    }
    void save(std::vector<Instance> const &list, int theme, std::string const &locale) {
        if (!writable_)
            throw std::runtime_error(warning);
        JsonObject j;
        put(j, L"version", 1.0);
        put(j, L"themeMode", double(theme));
        put(j, L"language", normalize_language(locale));
        JsonArray a;
        for (auto const &i : list)
            a.Append(encode(i));
        j.SetNamedValue(L"instances", a);
        atomic_write(file_, str(j));
    }
};
} // namespace ww
