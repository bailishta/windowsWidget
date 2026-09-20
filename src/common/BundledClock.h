#pragma once
#include "Common.h"

namespace ww {
// Only the application's own clock is upgraded. Third-party directories and
// per-instance settings are untouched. Publish the manifest last, so it never
// points at a partly copied DLL; retain the old DLL and manifest backup.
inline bool install_bundled_clock(fs::path const &source, fs::path const &destination) {
    if (!fs::exists(source / L"widget.json"))
        return false;
    auto manifest = json(read_file(source / L"widget.json"));
    if (get(manifest, L"id") != "org.windowswidget.clock")
        throw std::runtime_error("Invalid bundled clock id");
    auto revision = manifest.GetNamedNumber(L"bundledRevision", 0);
    if (revision < 1 || revision > 1000000 || revision != std::floor(revision))
        throw std::runtime_error("Invalid bundled clock revision");
    auto installed = destination / L"widget.json";
    if (fs::exists(installed)) {
        auto previous = json(read_file(installed));
        if (get(previous, L"id") != get(manifest, L"id") ||
            previous.GetNamedNumber(L"bundledRevision", 0) >= revision)
            return false;
    }
    auto entry = fs::path(wide(get(manifest, L"entry")));
    if (entry.has_parent_path() || entry.empty())
        throw std::runtime_error("Invalid bundled clock entry");
    fs::create_directories(destination);
    auto name = "ClockWidget.r" + std::to_string(int(revision)) + ".dll";
    auto final = destination / wide(name), temporary = destination / wide(name + ".tmp");
    fs::copy_file(source / entry, temporary, fs::copy_options::overwrite_existing);
    check(MoveFileExW(temporary.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
          "Publish bundled clock DLL");
    put(manifest, L"entry", name);
    atomic_write(installed, str(manifest));
    return true;
}
} // namespace ww
