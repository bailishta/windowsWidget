#include "../src/common/Engine.h"
#include "../src/common/BundledClock.h"
#include "../src/host/Composition.h"
#include "../src/host/Desktop.h"
#include "../sdk/WidgetSdk.h"
#include <iostream>
#include <set>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
using namespace ww;
namespace {
int passed = 0;
void require(bool ok, const char *name) {
    if (!ok)
        throw std::runtime_error(name);
    ++passed;
    std::cout << "PASS " << name << std::endl;
}
template <class F> void wait_for(F predicate, DWORD timeout = 20000) {
    auto start = now_ms();
    while (!predicate()) {
        if (now_ms() - start > timeout)
            throw std::runtime_error("Wait condition timed out");
        Sleep(20);
    }
}
fs::path fixture(std::wstring const &name) {
    auto dir = executable_dir() / L"test-results" / (name + L"-" + wide(guid()));
    auto plugin = dir / L"plugins" / L"test";
    fs::create_directories(plugin);
    fs::copy_file(executable_dir() / L"TestWidget.dll", plugin / L"TestWidget.dll");
    atomic_write(
        plugin / L"widget.json",
        R"({"id":"test.widget","name":"Test","version":"1.0","sdk":1,"architecture":"x64","entry":"TestWidget.dll","width":240,"height":140})");
    return dir;
}
std::string status_of(Engine &e, std::string const &id) {
    for (auto const &i : e.instances())
        if (i.id == id) {
            return i.status;
        }
    return "Removed";
}
void storage_test() {
    auto dir = fixture(L"store");
    Store store(dir);
    require(store.load().empty(), "new store is empty");
    Instance a;
    a.id = guid();
    a.plugin = "missing.widget";
    a.config = R"({"hour24":false})";
    a.x = -25;
    a.locked = true;
    store.theme_mode = 2;
    store.save({a});
    a.x = 150;
    store.save({a});
    Store restored(dir);
    auto list = restored.load();
    require(list.size() == 1 && list[0].x == 150 && list[0].locked && restored.theme_mode == 2,
            "layout, config, theme round trip");
    std::ofstream(dir / L"settings.json", std::ios::trunc) << "broken";
    Store recovered(dir);
    auto backup = recovered.load();
    require(backup.size() == 1 && backup[0].x == -25 && !recovered.warning.empty(),
            "corrupt settings recover previous backup");
    recovered.save(backup);
    require(Store(dir).load().size() == 1, "recovered settings remain writable");
    atomic_write(dir / L"settings.json", R"({"version":99,"instances":[]})");
    Store future(dir);
    future.load();
    bool rejected = false;
    try {
        future.save({});
    } catch (...) {
        rejected = true;
    }
    require(rejected && read_file(dir / L"settings.json").find("99") != std::string::npos,
            "newer settings are not overwritten");
    auto bad = fixture(L"corrupt");
    atomic_write(bad / L"settings.json", "broken");
    Store no_backup(bad);
    no_backup.load();
    require(!no_backup.writable(), "unrecoverable settings preserved read-only");
}
void discovery_test() {
    auto dir = fixture(L"discovery");
    auto plugins = discover(dir / L"plugins");
    require(plugins.size() == 1 && plugins[0].error.empty(), "valid manifest discovered without loading DLL");
    auto manifest = dir / L"plugins" / L"test" / L"widget.json";
    auto j = json(read_file(manifest));
    put(j, L"architecture", "arm64");
    atomic_write(manifest, str(j));
    require(!discover(dir / L"plugins")[0].error.empty(), "incompatible architecture rejected");
    put(j, L"architecture", "x64");
    put(j, L"sdk", 99.0);
    atomic_write(manifest, str(j));
    require(!discover(dir / L"plugins")[0].error.empty(), "incompatible SDK rejected");
    put(j, L"sdk", 1.0);
    put(j, L"entry", "../escape.dll");
    atomic_write(manifest, str(j));
    require(!discover(dir / L"plugins")[0].error.empty(), "escaping DLL path rejected");
    atomic_write(manifest, "{bad");
    require(!discover(dir / L"plugins")[0].error.empty(), "malformed manifest reported");
}
void grid_test() {
    auto cells = grid_extent(3, 2, {93, 103});
    require(cells.cx == 279 && cells.cy == 206,
            "3 by 2 uses current icon cell width and height independently");
    cells = grid_extent(1, 2, {93, 103});
    require(cells.cx == 93 && cells.cy == 206, "1 by 2 retains a single column");
    cells = grid_extent(1, 1, {93, 103});
    require(cells.cx == 93 && cells.cy == 103, "1 by 1 is one rectangular icon cell");
    require(nearest_cells(279, 93) == 3 && nearest_cells(60, 93) == 1,
            "legacy pixel dimensions migrate to nearest whole cell");
    DesktopGrid grid{{10, 20}, {100, 100}, {}};
    auto result = aligned_position({156, 162}, {180, 130}, {0, 0, 800, 600}, grid);
    require(result && result->x == 110 && result->y == 120, "snap chooses nearest grid point");
    grid.occupied.push_back({100, 100, 300, 260});
    result = aligned_position({156, 162}, {180, 130}, {0, 0, 800, 600}, grid);
    require(result && !overlaps({result->x, result->y, result->x + 180, result->y + 130}, grid.occupied[0]),
            "snap avoids icon and widget rectangles");
    grid.occupied.push_back({result->x, result->y, result->x + 180, result->y + 130});
    auto next = aligned_position({156, 162}, {180, 130}, {0, 0, 800, 600}, grid);
    require(next && !overlaps({next->x, next->y, next->x + 180, next->y + 130}, grid.occupied[1]),
            "next placement respects the previous widget reservation");
    grid = {{-1910, -1060}, {150, 150}, {}};
    result = aligned_position({-2000, -1200}, {420, 270}, {-1920, -1080, 0, 0}, grid);
    require(result && result->x == -1910 && result->y == -1060,
            "negative monitor coordinates and scaled grid stay visible");
    grid.occupied = {{-1920, -1080, 0, 0}};
    require(!aligned_position({-100, -100}, {420, 270}, {-1920, -1080, 0, 0}, grid),
            "full desktop reports no space instead of overlapping icons");
    grid.occupied.clear();
    require(!aligned_position({0, 0}, {2000, 1200}, {-1920, -1080, 0, 0}, grid),
            "oversized widget has no offscreen grid candidate");
}
void grid_resize_test() {
    auto root = fixture(L"grid-resize");
    auto manifest_file = root / L"plugins" / L"test" / L"widget.json";
    auto manifest = json(read_file(manifest_file));
    put(manifest, L"minWidth", 32.0);
    put(manifest, L"minHeight", 32.0);
    atomic_write(manifest_file, str(manifest));
    EngineOptions options;
    options.offdesktop = true;
    Engine e(root, executable_dir() / L"WidgetHost.exe", options);
    e.start();
    wait_for([&] { return e.scanned(); });
    auto id = e.add("test.widget");
    wait_for([&] {
        auto items = e.instances();
        return items[0].status == "Running" && items[0].columns > 0;
    });
    auto original = e.instances()[0];
    require(original.columns == 3 && original.rows == 1,
            "legacy DIP instance converts to grid on first host layout");
    e.resize(id, 1, 1);
    wait_for([&] {
        auto i = e.instances()[0];
        return i.width == 80 && i.height == 100 && i.status == "Running";
    });
    require(true, "live 1 by 1 resize reaches the actual hidden host");
    e.resize(id, 3, 2);
    e.resize(id, 1, 2);
    wait_for([&] {
        auto i = e.instances()[0];
        return i.columns == 1 && i.rows == 2 && i.width == 80 && i.height == 200;
    });
    require(e.instances()[0].pid == original.pid, "latest resize wins without restarting the plugin process");
    bool rejected = false;
    try {
        e.resize(id, 0, 2);
    } catch (...) {
        rejected = true;
    }
    require(rejected && e.instances()[0].columns == 1, "invalid grid input leaves current size intact");
    e.lock(id, true);
    rejected = false;
    try {
        e.resize(id, 3, 2);
    } catch (...) {
        rejected = true;
    }
    require(rejected, "layout lock also prevents size edits");
    e.lock(id, false);
    e.enable(id, false);
    e.resize(id, 3, 2);
    require(e.instances()[0].status == "Stopped",
            "size can be saved for a stopped instance without starting it");
    e.shutdown();
    auto saved = Store(root).load();
    require(saved.size() == 1 && saved[0].columns == 3 && saved[0].rows == 2,
            "grid dimensions persist with the original instance");
    Engine restored(root, executable_dir() / L"WidgetHost.exe", options);
    restored.start();
    wait_for([&] { return restored.scanned(); });
    restored.enable(id, true);
    wait_for([&] {
        auto i = restored.instances()[0];
        return i.status == "Running" && i.width == 240 && i.height == 200;
    });
    require(true, "restart computes pixels from saved cell counts rather than stale DIP size");
    restored.shutdown();
    auto encoded = encode(saved[0]);
    put(encoded, L"gridColumns", 1.5);
    rejected = false;
    try {
        decode(encoded);
    } catch (...) {
        rejected = true;
    }
    require(rejected, "fractional persisted grid dimensions are rejected");
}
void language_test() {
    require(normalize_language("zh-TW") == "zh-CN" && normalize_language("en-GB") == "en-US" &&
                normalize_language("fr-FR") == "en-US",
            "only Chinese and English can be selected");
    auto root = fixture(L"language");
    EngineOptions options;
    options.offdesktop = true;
    Engine e(root, executable_dir() / L"WidgetHost.exe", options);
    e.set_language("en-US");
    e.start();
    wait_for([&] { return e.scanned(); });
    auto id = e.add("test.widget");
    auto log = root / L"logs" / L"manager.log";
    wait_for([&] {
        return status_of(e, id) == "Running" &&
               read_file(log).find("test UI language=1033") != std::string::npos;
    });
    require(true, "host create callback receives English UI language");
    e.set_language("zh-CN");
    wait_for([&] { return read_file(log).find("test UI language=2052") != std::string::npos; });
    require(true, "running host receives Chinese language change over IPC");
    e.shutdown();
    Store restored(root);
    auto instances = restored.load();
    require(restored.language == "zh-CN" && instances.size() == 1,
            "language and existing instance persist together");
}
void diagnose_grid() {
    Desktop desktop;
    check(desktop.discover(), "Find desktop for read-only grid query");
    desktop.match_dpi();
    for (auto const &monitor : monitors()) {
        auto grid = read_desktop_grid(desktop.parent(), monitor.handle, monitor.work, monitor.dpi);
        std::cout << "GRID dpi=" << monitor.dpi << " spacing=" << grid.spacing.cx << "," << grid.spacing.cy
                  << " origin=" << grid.origin.x << "," << grid.origin.y << " icons=" << grid.occupied.size()
                  << std::endl;
        auto extent = grid_extent(3, 2, grid.spacing);
        std::cout << "SIZE 3x2=" << extent.cx << "x" << extent.cy << " pixels\n";
        auto point =
            aligned_position({monitor.work.left + 40, monitor.work.top + 40}, extent, monitor.work, grid);
        if (point)
            std::cout << "CANDIDATE " << point->x << "," << point->y << " (not applied)\n";
    }
}
void bundled_clock_test() {
    auto root = fixture(L"bundled-clock");
    auto source = root / L"bundled", destination = root / L"installed";
    fs::create_directories(source);
    fs::create_directories(destination);
    atomic_write(source / L"ClockWidget.dll", "new clock bytes");
    auto manifest = json(
        R"({"id":"org.windowswidget.clock","version":"1.0.1","bundledRevision":1,"entry":"ClockWidget.dll"})");
    atomic_write(source / L"widget.json", str(manifest));
    atomic_write(destination / L"widget.json",
                 R"({"id":"org.windowswidget.clock","version":"1.0.0","entry":"ClockWidget.dll"})");
    atomic_write(destination / L"ClockWidget.dll", "old clock bytes");
    atomic_write(destination / L"user-data.json", R"({"keep":true})");
    require(install_bundled_clock(source, destination), "legacy bundled clock upgrades");
    auto installed = json(read_file(destination / L"widget.json"));
    require(get(installed, L"entry") == "ClockWidget.r1.dll" &&
                read_file(destination / L"ClockWidget.r1.dll") == "new clock bytes",
            "upgrade publishes manifest pointing to complete versioned DLL");
    require(read_file(destination / L"ClockWidget.dll") == "old clock bytes" &&
                get(json(read_file(destination / L"widget.json.bak")), L"version") == "1.0.0" &&
                read_file(destination / L"user-data.json") == R"({"keep":true})",
            "upgrade retains old DLL, backup manifest and user data");
    require(!install_bundled_clock(source, destination), "same bundled revision is not reinstalled");
    put(installed, L"bundledRevision", 2.0);
    atomic_write(destination / L"widget.json", str(installed));
    require(!install_bundled_clock(source, destination), "newer bundled clock is not downgraded");
    put(installed, L"id", "third.party.clock");
    put(installed, L"bundledRevision", 0.0);
    atomic_write(destination / L"widget.json", str(installed));
    require(!install_bundled_clock(source, destination), "bundled upgrade preserves a different plugin");
}
void clock_render_test() {
    // Hidden local windows only: never attach to or capture the user's desktop.
    struct Resources {
        HWND parent = nullptr, surface = nullptr;
        HMODULE module = nullptr;
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ old = nullptr;
        WidgetApi api{sizeof(WidgetApi)};
        void *instance = nullptr;
        ~Resources() {
            if (instance)
                api.destroy(instance);
            if (parent)
                DestroyWindow(parent);
            if (old)
                SelectObject(dc, old);
            if (bitmap)
                DeleteObject(bitmap);
            if (dc)
                DeleteDC(dc);
            if (module)
                FreeLibrary(module);
        }
    } r;
    r.parent = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"STATIC", L"Hidden composition fixture", WS_POPUP,
                               0, 0, 560, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(r.parent != nullptr, "Create hidden parent");
    r.surface = CreateWindowExW(0, L"STATIC", L"Hidden widget surface", WS_CHILD, 0, 0, 560, 320, r.parent,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    check(r.surface != nullptr, "Create hidden surface");
    try {
        initialize_widget_surface(r.surface);
    } catch (...) {
        std::cerr << "LAYER diagnostic valid=" << IsWindow(r.surface) << " exstyle=" << std::hex
                  << GetWindowLongPtrW(r.surface, GWL_EXSTYLE) << std::dec << std::endl;
        throw;
    }
    BYTE alpha = 0;
    DWORD flags = 0;
    require(GetLayeredWindowAttributes(r.surface, nullptr, &alpha, &flags) && alpha == 255 &&
                (flags & LWA_ALPHA) && (GetWindowLongPtrW(r.surface, GWL_EXSTYLE) & WS_EX_LAYERED) &&
                !(GetWindowLongPtrW(r.surface, GWL_EXSTYLE) & WS_EX_TOPMOST),
            "widget owns an opaque layer beneath a no-redirection parent");
    r.module = LoadLibraryW((executable_dir() / L"ClockWidget.dll").c_str());
    check(r.module != nullptr, "Load real clock renderer");
    auto get_api = reinterpret_cast<WidgetGetApiFn>(GetProcAddress(r.module, "WidgetGetApi"));
    check(get_api != nullptr, "Clock API export");
    winrt::check_hresult(get_api(WIDGET_ABI_VERSION, &r.api));
    std::string errors;
    WidgetHostApi host{sizeof(WidgetHostApi),
                       WIDGET_ABI_VERSION,
                       &errors,
                       [](void *context, uint32_t level, const char *text) {
                           if (level >= 3)
                               *static_cast<std::string *>(context) += text;
                       },
                       [](void *, const char *) {},
                       ""};
    WidgetCreateInfo info{sizeof(WidgetCreateInfo), r.surface, 96, 1, "{}", &host};
    HWND content = nullptr;
    winrt::check_hresult(r.api.create(&info, &r.instance, &content));
    constexpr int width = 560, height = 320;
    r.dc = CreateCompatibleDC(nullptr);
    check(r.dc != nullptr, "Create offscreen DC");
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void *bits = nullptr;
    r.bitmap = CreateDIBSection(r.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    check(r.bitmap != nullptr, "Create pixel buffer");
    r.old = SelectObject(r.dc, r.bitmap);
    auto pixels = static_cast<uint32_t *>(bits);
    for (auto language : {"en-US", "zh-CN"}) {
        set_thread_language(language);
        for (uint32_t dpi : {96u, 144u, 192u}) {
            for (bool compact : {false, true}) {
                auto w = int((compact ? 64 : 280) * dpi / 96), h = int((compact ? 52 : 160) * dpi / 96);
                SetWindowPos(content, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                r.api.layout(r.instance, w, h, dpi);
                for (uint32_t dark : {1u, 0u}) {
                    r.api.theme(r.instance, dark);
                    winrt::check_hresult(
                        r.api.configure(r.instance, dark ? "{\"hour24\":true}" : "{\"hour24\":false}"));
                    std::fill_n(pixels, width * height, 0x00ff00ffu);
                    SendMessageW(content, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(r.dc), PRF_CLIENT);
                    GdiFlush();
                    int background = 0, text = 0;
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x) {
                            auto p = pixels[y * width + x];
                            auto red = (p >> 16) & 255, green = (p >> 8) & 255, blue = p & 255;
                            if (dark ? (red < 50 && green < 50 && blue < 50)
                                     : (red > 230 && green > 230 && blue > 230))
                                ++background;
                            if (dark ? (red > 180 && green > 180 && blue > 180)
                                     : (red < 100 && green < 100 && blue < 100))
                                ++text;
                        }
                    std::cout << "RENDER language=" << language << " dpi=" << dpi << " dark=" << dark
                              << " background=" << background << " text=" << text << std::endl;
                    require(errors.empty() && background > w * h * 0.7 && text > (compact ? 15 : 100),
                            "clock paints real background and time text at requested theme and DPI");
                }
            }
        }
    }
    require(!IsWindowVisible(r.parent), "render regression never shows a desktop window");
}
uint64_t concurrency_test(unsigned count) {
    auto root = fixture(L"concurrency" + std::to_wstring(count));
    EngineOptions options;
    options.concurrency = count;
    options.offdesktop = true;
    Engine engine(root, executable_dir() / L"WidgetHost.exe", options);
    engine.start();
    wait_for([&] { return engine.scanned(); });
    auto start = now_ms();
    for (int n = 0; n < 8; ++n)
        engine.add("test.widget", R"({"delayMs":700})");
    unsigned peak = 0;
    bool progressive = false;
    wait_for([&] {
        auto list = engine.instances();
        unsigned starting = 0, running = 0;
        for (auto const &i : list) {
            if (i.status == "Error")
                throw std::runtime_error(i.error);
            if (i.status == "Starting")
                ++starting;
            if (i.status == "Running")
                ++running;
        }
        peak = std::max(peak, starting);
        if (running > 0 && running < 8)
            progressive = true;
        return running == 8;
    });
    auto elapsed = now_ms() - start;
    require(peak <= count, "bounded startup concurrency");
    if (count == 4)
        require(peak >= 2, "multiple hosts initialize concurrently");
    require(progressive, "widgets become ready independently");
    std::set<DWORD> ids;
    for (auto const &i : engine.instances())
        ids.insert(i.pid);
    require(ids.size() == 8, "each widget instance owns a different process");
    std::vector<Handle> handles;
    for (auto id : ids)
        handles.emplace_back(OpenProcess(SYNCHRONIZE, FALSE, id));
    engine.shutdown();
    for (auto const &h : handles)
        if (h)
            require(WaitForSingleObject(h, 2000) == WAIT_OBJECT_0, "shutdown reclaims child process");
    auto restored = Store(root).load();
    require(restored.size() == 8, "multiple instances persisted");
    std::cout << "BENCH concurrency=" << count << " instances=8 delay=700ms total=" << elapsed
              << "ms peak=" << peak << std::endl;
    return elapsed;
}
void failure_test() {
    auto root = fixture(L"failures");
    EngineOptions options;
    options.concurrency = 2;
    options.startup_timeout = 1500;
    options.offdesktop = true;
    Engine engine(root, executable_dir() / L"WidgetHost.exe", options);
    engine.start();
    wait_for([&] { return engine.scanned(); });
    auto crashed = engine.add("test.widget", R"({"mode":"crash"})");
    auto hung = engine.add("test.widget", R"({"mode":"hang"})");
    auto healthy = engine.add("test.widget");
    wait_for(
        [&] {
            return status_of(engine, crashed) == "Error" && status_of(engine, hung) == "Error" &&
                   status_of(engine, healthy) == "Running";
        },
        10000);
    require(true, "crash and timeout release slots without affecting healthy host");
    auto removing = engine.add("test.widget", R"({"delayMs":1200})");
    wait_for([&] { return status_of(engine, removing) == "Starting"; });
    engine.remove(removing);
    Sleep(1600);
    require(status_of(engine, removing) == "Removed", "remove during initialization ignores late ready");
    engine.enable(healthy, false);
    require(status_of(engine, healthy) == "Stopped", "disable stops instance");
    engine.enable(healthy, true);
    wait_for([&] { return status_of(engine, healthy) == "Running"; });
    engine.lock(healthy, true);
    engine.configure(healthy, R"({"setting":42})");
    engine.set_theme_mode(2);
    engine.theme(true);
    Sleep(500);
    engine.shutdown();
    auto saved = Store(root).load();
    bool found = false;
    for (auto const &i : saved)
        if (i.id == healthy)
            found = i.locked && i.config.find("42") != std::string::npos;
    require(found, "lock and plugin configuration persist");
    auto missing = root / L"plugins" / L"test" / L"TestWidget.dll";
    fs::rename(missing, missing.wstring() + L".disabled");
    Engine restored(root, executable_dir() / L"WidgetHost.exe", options);
    restored.start();
    wait_for([&] { return restored.scanned(); });
    require(restored.instances().size() == 3 && status_of(restored, healthy) == "Error",
            "missing plugin preserves restored instances");
    restored.shutdown();
}
void bad_dll_test() {
    auto root = fixture(L"bad-dll");
    atomic_write(root / L"plugins" / L"test" / L"TestWidget.dll", "not a PE file");
    EngineOptions o;
    o.offdesktop = true;
    Engine e(root, executable_dir() / L"WidgetHost.exe", o);
    e.start();
    wait_for([&] { return e.scanned(); });
    auto start = now_ms();
    auto id = e.add("test.widget");
    wait_for([&] { return status_of(e, id) == "Error"; });
    require(true, "invalid DLL isolated in host");
    require(now_ms() - start < 3000, "invalid DLL fails promptly without a blocking system dialog");
    require(e.instances()[0].error.find("Load plugin DLL") != std::string::npos,
            "invalid DLL returns a loader error rather than initialization timeout");
    e.shutdown();
}
void abi_test() {
    auto root = fixture(L"bad-abi");
    fs::copy_file(executable_dir() / L"BadAbiWidget.dll", root / L"plugins" / L"test" / L"TestWidget.dll",
                  fs::copy_options::overwrite_existing);
    EngineOptions o;
    o.offdesktop = true;
    Engine e(root, executable_dir() / L"WidgetHost.exe", o);
    e.start();
    wait_for([&] { return e.scanned(); });
    auto id = e.add("test.widget");
    wait_for([&] { return status_of(e, id) == "Error"; });
    require(true, "DLL rejecting SDK ABI fails only that instance");
    e.shutdown();
}
void interrupted_write_test() {
    auto root = fixture(L"interrupted");
    Store s(root);
    Instance i;
    i.id = guid();
    i.plugin = "test.widget";
    s.save({i});
    std::ofstream(root / L"settings.json.tmp") << "partial write";
    Store read(root);
    require(read.load().size() == 1, "interrupted temp write leaves committed settings intact");
}
int orphan_controller(fs::path root) {
    EngineOptions o;
    o.offdesktop = true;
    Engine e(root, executable_dir() / L"WidgetHost.exe", o);
    e.start();
    wait_for([&] { return e.scanned(); });
    auto id = e.add("test.widget");
    wait_for([&] { return status_of(e, id) == "Running" && fs::exists(root / L"settings.json"); });
    auto list = e.instances();
    JsonObject marker;
    put(marker, L"pid", double(list[0].pid));
    atomic_write(root / L"child.json", str(marker));
    Sleep(30000);
    return 0;
}
void orphan_test() {
    auto root = fixture(L"orphan");
    auto exe = executable_dir() / L"WidgetTests.exe";
    std::wstring command = L"\"" + exe.wstring() + L"\" --orphan-controller \"" + root.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         executable_dir().c_str(), &si, &pi),
          "Start test controller");
    Handle controller(pi.hProcess), thread(pi.hThread);
    try {
        wait_for([&] { return fs::exists(root / L"child.json"); }, 10000);
        auto marker = json(read_file(root / L"child.json"));
        Handle child(OpenProcess(SYNCHRONIZE, FALSE, DWORD(marker.GetNamedNumber(L"pid"))));
        require(bool(child), "abnormal-exit fixture has a live host");
        TerminateProcess(controller, 55);
        WaitForSingleObject(controller, 2000);
        require(WaitForSingleObject(child, 3000) == WAIT_OBJECT_0,
                "Job Object reclaims host when manager crashes");
        EngineOptions o;
        o.offdesktop = true;
        Engine restored(root, executable_dir() / L"WidgetHost.exe", o);
        restored.start();
        wait_for([&] {
            auto v = restored.instances();
            return v.size() == 1 && v[0].status == "Running";
        });
        require(true, "committed layout restores after abnormal manager exit");
        restored.shutdown();
    } catch (...) {
        TerminateProcess(controller, 56);
        throw;
    }
}
void desktop_test() {
    auto root = fixture(L"desktop");
    Engine e(root, executable_dir() / L"WidgetHost.exe");
    e.start();
    wait_for([&] { return e.scanned(); });
    auto id = e.add("test.widget");
    wait_for([&] {
        auto state = status_of(e, id);
        if (state == "Error" || state == "Unavailable")
            throw std::runtime_error(e.instances()[0].error);
        return state == "Running";
    });
    struct Find {
        DWORD pid;
        HWND widget;
    };
    Find find{e.instances()[0].pid, nullptr};
    EnumChildWindows(
        GetDesktopWindow(),
        [](HWND w, LPARAM p) -> BOOL {
            auto &f = *reinterpret_cast<Find *>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            wchar_t name[128];
            GetClassNameW(w, name, 128);
            if (pid == f.pid && std::wstring_view(name) == L"WindowsWidget.Container") {
                f.widget = w;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    require(find.widget && IsWindowVisible(find.widget), "desktop widget exists and is visible");
    auto style = GetWindowLongPtrW(find.widget, GWL_STYLE);
    auto ex = GetWindowLongPtrW(find.widget, GWL_EXSTYLE);
    require((style & WS_CHILD) && !(style & WS_POPUP), "widget is embedded as a desktop child");
    require((ex & WS_EX_TOOLWINDOW) && (ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_TOPMOST),
            "widget has desktop tool/no-activate style and is not topmost");
    wchar_t name[128];
    GetClassNameW(GetParent(find.widget), name, 128);
    require(std::wstring_view(name) == L"Progman" || std::wstring_view(name) == L"WorkerW",
            "desktop parent is an Explorer desktop host");
    e.shutdown();
}
void describe_window(HWND window) {
    wchar_t name[128]{};
    GetClassNameW(window, name, 128);
    RECT rect{};
    GetWindowRect(window, &rect);
    DWORD process = 0, cloaked = 0;
    GetWindowThreadProcessId(window, &process);
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    std::cout << "hwnd=" << reinterpret_cast<uintptr_t>(window) << " class=" << utf8(name)
              << " parent=" << reinterpret_cast<uintptr_t>(GetParent(window)) << " pid=" << process
              << " style=0x" << std::hex << GetWindowLongPtrW(window, GWL_STYLE) << " exstyle=0x"
              << GetWindowLongPtrW(window, GWL_EXSTYLE) << std::dec << " visible=" << IsWindowVisible(window)
              << " cloaked=" << cloaked << " rect=" << rect.left << "," << rect.top << "," << rect.right
              << "," << rect.bottom << "\n";
}
void diagnose_desktop() {
    EnumWindows(
        [](HWND parent, LPARAM) -> BOOL {
            wchar_t name[128]{};
            GetClassNameW(parent, name, 128);
            if (std::wstring_view(name) != L"Progman" && std::wstring_view(name) != L"WorkerW")
                return TRUE;
            describe_window(parent);
            EnumChildWindows(
                parent,
                [](HWND child, LPARAM) -> BOOL {
                    wchar_t cls[128]{};
                    GetClassNameW(child, cls, 128);
                    std::wstring_view name(cls);
                    if (name == L"SHELLDLL_DefView" || name == L"SysListView32" || name == L"WorkerW" ||
                        name.starts_with(L"WindowsWidget."))
                        describe_window(child);
                    return TRUE;
                },
                0);
            return TRUE;
        },
        0);
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    try {
        winrt::init_apartment(argc == 2 && std::wstring_view(argv[1]) == L"--diagnose-grid"
                                  ? winrt::apartment_type::single_threaded
                                  : winrt::apartment_type::multi_threaded);
        if (argc == 2 && std::wstring_view(argv[1]) == L"--diagnose-desktop") {
            diagnose_desktop();
            return 0;
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--diagnose-grid") {
            diagnose_grid();
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--orphan-controller")
            return orphan_controller(argv[2]);
        if (argc == 2 && std::wstring_view(argv[1]) == L"--desktop") {
            desktop_test();
            return 0;
        }
        storage_test();
        interrupted_write_test();
        discovery_test();
        grid_test();
        grid_resize_test();
        language_test();
        bundled_clock_test();
        clock_render_test();
        auto serial = concurrency_test(1);
        auto parallel = concurrency_test(4);
        require(parallel < serial * 0.7, "parallel loading materially faster than serial baseline");
        failure_test();
        bad_dll_test();
        abi_test();
        orphan_test();
        std::cout << "RESULT " << passed << " assertions passed\n";
        return 0;
    } catch (...) {
        std::cerr << "FAIL " << error_text() << "\n";
        return 1;
    }
}
