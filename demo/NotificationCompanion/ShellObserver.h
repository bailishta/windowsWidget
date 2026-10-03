#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <UIAutomation.h>
#include <tlhelp32.h>
#include <winrt/base.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace companion {
inline std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    return value;
}
inline std::wstring token(std::wstring value) {
    value = lower(std::move(value));
    std::erase_if(value, [](wchar_t c) { return iswspace(c) || c == L'_' || c == L'-'; });
    return value;
}
inline unsigned identity(std::wstring const& id, std::wstring const& klass) {
    auto i = token(id), c = lower(klass);
    if (i == L"notificationcentergrid" || i == L"calendarcentergrid" ||
        i == L"calendarcontrolscrollviewer" || i == L"notificationcenter" ||
        i == L"calendarview" ||
        i == L"focussessioncontrol" || c.find(L"actioncenter.notificationcenter") != c.npos ||
        c.find(L"actioncenter.clockcalendar") != c.npos || c.find(L"calendarview") != c.npos)
        return 1;
    if (i == L"controlcenterregion" || i == L"controlcenter" ||
        c.find(L"quickactions.controlcenter") != c.npos || c == L"controlcenter.controlcenter")
        return 2;
    return 0;
}
inline unsigned title_identity(std::wstring const& value) {
    auto t = token(value);
    if (t == L"notificationcenter" || t == L"notifications" || t == L"actioncenter" ||
        t == L"通知中心" || t == L"通知" || t == L"操作中心" || t == L"clockandcalendar" || t == L"日历") return 1;
    if (t == L"quicksettings" || t == L"controlcenter" || t == L"快速设置") return 2;
    return 0;
}
inline bool shell_process(std::wstring const& value) {
    return value == L"shellhost.exe" || value == L"shellexperiencehost.exe" || value == L"explorer.exe";
}
inline bool dedicated_surface(std::wstring const& value) {
    auto c = lower(value);
    return c == L"controlcenterwindow" || c == L"windows.ui.core.corewindow";
}
inline bool possible_surface(std::wstring const& value) {
    auto c = lower(value);
    return dedicated_surface(value) || c.find(L"xaml") != c.npos ||
           c.find(L"desktopwindowcontentbridge") != c.npos;
}
inline std::wstring class_name(HWND h) {
    wchar_t text[256]{}; GetClassNameW(h, text, int(std::size(text))); return text;
}
inline std::wstring process_name(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    winrt::handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process) return {};
    wchar_t path[32768]; DWORD length = DWORD(std::size(path));
    return QueryFullProcessImageNameW(process.get(), 0, path, &length) ?
        lower(std::filesystem::path(std::wstring(path, length)).filename().wstring()) : L"";
}
struct Candidate {
    HWND window = nullptr;
    DWORD pid = 0, cloak = 0;
    RECT bounds{}, native_bounds{};
    std::wstring kind = L"unknown", process, klass, evidence = L"native", geometry = L"native";
    HRESULT uia_status = S_FALSE;
    unsigned uia_nodes = 0, identity_bits = 0;
    bool open = false, cloak_known = false, recognized = false;
    uint64_t opened_at = 0;
};
// A CoreWindow / ControlCenterWindow can also host a transient message banner.
// Discovery is broad, but automatic following requires positive panel identity.
// Do not infer it from size, position, foreground activation or class alone.
inline bool eligible_panel(Candidate const& c) {
    return c.recognized && (c.kind == L"notifications" || c.kind == L"quick");
}
inline bool refresh_visibility(Candidate& c) {
    c.cloak_known = SUCCEEDED(DwmGetWindowAttribute(c.window, DWMWA_CLOAKED, &c.cloak, sizeof(c.cloak)));
    MONITORINFO mi{sizeof(mi)}; RECT overlap{};
    bool monitor = GetMonitorInfoW(MonitorFromWindow(c.window, MONITOR_DEFAULTTONEAREST), &mi);
    c.open = IsWindow(c.window) && IsWindowVisible(c.window) && !IsIconic(c.window) &&
        c.cloak_known && !c.cloak && monitor && c.bounds.right-c.bounds.left >= 160 &&
        c.bounds.bottom-c.bounds.top >= 100 && IntersectRect(&overlap, &c.bounds, &mi.rcWork);
    return c.open;
}
inline bool content_rect(RECT rect, RECT work) {
    RECT clipped{};
    return rect.right > rect.left && rect.bottom > rect.top &&
        IntersectRect(&clipped, &rect, &work) &&
        rect.right - rect.left < (work.right - work.left) * 3 / 4;
}
inline bool join_rect(RECT& joined, RECT rect, bool& has_rect) {
    if (IsRectEmpty(&rect)) return false;
    if (has_rect) UnionRect(&joined, &joined, &rect); else joined = rect;
    has_rect = true; return true;
}

struct Snapshot {
    uint64_t collected_at = 0;
    std::vector<Candidate> candidates;
    std::vector<Candidate> inventory;
    HRESULT automation_status = S_FALSE;
    unsigned roots = 0;
    uint64_t native_samples = 0, uia_passes = 0, native_duration_ms = 0;
};

// Identity survives a hidden flyout. Geometry is only reusable for the same
// process, window class and native rectangle; never carry it to a reused HWND.
inline void merge_accessibility(Candidate& current, Candidate const& cached) {
    if (current.window != cached.window || current.pid != cached.pid || current.klass != cached.klass) return;
    current.uia_status = cached.uia_status; current.uia_nodes = cached.uia_nodes;
    current.identity_bits = cached.identity_bits;
    if (cached.recognized) {
        current.kind = cached.kind; current.evidence = cached.evidence; current.recognized = true;
    }
    if (cached.geometry != L"native" && current.cloak_known && !current.cloak &&
        EqualRect(&current.native_bounds, &cached.native_bounds)) {
        current.bounds = cached.bounds; current.geometry = cached.geometry;
    }
}
inline bool same_observation(Candidate const& a, Candidate const& b) {
    // Node counts fluctuate as the taskbar updates. They do not change identity,
    // placement or visibility and must not generate a full inventory log dump.
    return a.window == b.window && a.pid == b.pid && a.kind == b.kind && a.process == b.process &&
        a.klass == b.klass && a.evidence == b.evidence && a.geometry == b.geometry &&
        a.open == b.open && a.cloak == b.cloak && a.cloak_known == b.cloak_known &&
        a.uia_status == b.uia_status && a.identity_bits == b.identity_bits && a.recognized == b.recognized &&
        EqualRect(&a.bounds, &b.bounds) && EqualRect(&a.native_bounds, &b.native_bounds);
}

// Read-only accessibility inspection. No invocation, focus setting, or input injection.
// UIA calls live on a separate MTA thread so a Shell provider cannot stall WinUI.
class ShellObserver {
    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        std::atomic_bool stop = false;
        std::shared_ptr<Snapshot const> latest = std::make_shared<Snapshot>();
        std::map<HWND, Candidate> accessibility;
        HRESULT automation_status = S_FALSE;
        uint64_t uia_passes = 0;
    };
    std::shared_ptr<State> state = std::make_shared<State>();

    static std::vector<HWND> roots(std::set<HWND>& remembered) {
        std::set<HWND> handles;
        auto add = [&handles](HWND h) {
            if (h && IsWindow(h)) {
                handles.insert(GetAncestor(h, GA_ROOT));
                handles.insert(GetAncestor(h, GA_ROOTOWNER));
            }
        };
        EnumWindows([](HWND h, LPARAM p) -> BOOL {
            static_cast<std::set<HWND>*>(reinterpret_cast<void*>(p))->insert(h); return TRUE;
        }, reinterpret_cast<LPARAM>(&handles));
        // Enumerate Shell GUI threads too, including windows not covered by EnumWindows.
        struct ThreadRoster { uint64_t refreshed = 0; std::vector<DWORD> threads; };
        static thread_local ThreadRoster roster;
        if (!roster.refreshed || GetTickCount64()-roster.refreshed > 2000) {
            roster.refreshed=GetTickCount64();roster.threads.clear();
            HANDLE raw=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS|TH32CS_SNAPTHREAD,0);
            winrt::handle snapshot{raw==INVALID_HANDLE_VALUE ? nullptr : raw};
            if (snapshot) {
                std::set<DWORD> processes;
                DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);
                PROCESSENTRY32W process{};process.dwSize=sizeof(process);
                if (Process32FirstW(snapshot.get(),&process)) do {
                    DWORD other=0;
                    if (shell_process(lower(process.szExeFile)) && ProcessIdToSessionId(process.th32ProcessID,&other) && other==session)
                        processes.insert(process.th32ProcessID);
                } while (Process32NextW(snapshot.get(),&process));
                THREADENTRY32 thread{};thread.dwSize=sizeof(thread);
                if (Thread32First(snapshot.get(),&thread)) do {
                    if (processes.contains(thread.th32OwnerProcessID)) roster.threads.push_back(thread.th32ThreadID);
                } while (Thread32Next(snapshot.get(),&thread));
            }
        }
        for (auto thread : roster.threads) EnumThreadWindows(thread,[](HWND h,LPARAM p)->BOOL {
            static_cast<std::set<HWND>*>(reinterpret_cast<void*>(p))->insert(h);return TRUE;
        },reinterpret_cast<LPARAM>(&handles));
        // EnumWindows does not promise enumeration of non-desktop app windows.
        // Complement it with top-level class search and the foreground root.
        for (auto klass : {L"ControlCenterWindow", L"Windows.UI.Core.CoreWindow",
                           L"XamlExplorerHostIslandWindow", L"Windows.UI.Composition.DesktopWindowContentBridge",
                           L"Microsoft.UI.Composition.DesktopWindowContentBridge"}) {
            HWND after = nullptr;
            std::set<HWND> visited;
            for (unsigned i = 0; i < 256; ++i) {
                HWND next = FindWindowExW(nullptr, after, klass, nullptr);
                if (!next || !visited.insert(next).second) break;
                add(next); after = next;
            }
        }
        add(GetForegroundWindow());
        std::erase_if(remembered,[](HWND h){return !IsWindow(h);});
        for (HWND h:remembered) add(h);
        handles.erase(nullptr);
        return {handles.begin(), handles.end()};
    }
    static std::wstring bstr_text(BSTR value) { return value ? std::wstring(value, SysStringLen(value)) : L""; }

    static void inspect(IUIAutomation* automation, Candidate& c) {
        // A popup may be a visual child of a small taskbar/native host.
        if (!automation || !IsWindowVisible(c.window) || IsIconic(c.window) || !c.cloak_known || c.cloak) return;
        winrt::com_ptr<IUIAutomationElement> root;
        c.uia_status = automation->ElementFromHandle(c.window, root.put());
        if (FAILED(c.uia_status) || !root) return;
        BSTR name = nullptr;
        if (SUCCEEDED(root->get_CurrentName(&name))) {
            unsigned hint = title_identity(bstr_text(name)); SysFreeString(name);
            if (hint) { c.kind = hint == 1 ? L"notifications" : L"quick"; c.evidence = L"uia.root_name"; c.recognized = true; }
        }
        winrt::com_ptr<IUIAutomationCacheRequest> cache;
        winrt::com_ptr<IUIAutomationCondition> condition;
        c.uia_status = automation->CreateCacheRequest(cache.put());
        if (FAILED(c.uia_status)) return;
        for (auto prop : {UIA_AutomationIdPropertyId, UIA_ClassNamePropertyId,
                          UIA_BoundingRectanglePropertyId, UIA_IsOffscreenPropertyId}) cache->AddProperty(prop);
        cache->put_TreeScope(TreeScope_Element);
        automation->CreateTrueCondition(condition.put());
        if (!condition) return;
        cache->put_TreeFilter(condition.get());
        winrt::com_ptr<IUIAutomationElementArray> elements;
        c.uia_status = root->FindAllBuildCache(TreeScope_Subtree, condition.get(), cache.get(), elements.put());
        if (FAILED(c.uia_status) || !elements) return;
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(MonitorFromWindow(c.window, MONITOR_DEFAULTTONEAREST), &mi)) return;
        int count = 0; elements->get_Length(&count);c.uia_nodes=unsigned(std::max(0,count));
        unsigned detected = 0;
        bool volume = false, brightness = false;
        RECT panels{}, content{}; bool has_panels = false, has_content = false;
        for (int i = 0; i < std::min(count, 2048); ++i) {
            winrt::com_ptr<IUIAutomationElement> element;
            if (FAILED(elements->GetElement(i, element.put()))) continue;
            BOOL offscreen = TRUE;
            if (FAILED(element->get_CachedIsOffscreen(&offscreen)) || offscreen) continue;
            RECT rect{};
            if (FAILED(element->get_CachedBoundingRectangle(&rect)) || IsRectEmpty(&rect)) continue;
            BSTR id = nullptr, klass = nullptr;
            element->get_CachedAutomationId(&id); element->get_CachedClassName(&klass);
            auto identifier=token(bstr_text(id));
            unsigned hint = identity(identifier, bstr_text(klass));
            volume |= identifier==L"volumeslider";
            brightness |= identifier==L"brightnessslider";
            SysFreeString(id); SysFreeString(klass);
            detected |= hint;
            if (content_rect(rect, mi.rcWork)) {
                join_rect(content, rect, has_content);
                if (hint && rect.right - rect.left >= 160 && rect.bottom - rect.top >= 80)
                    join_rect(panels, rect, has_panels);
            }
        }
        if (volume && brightness) detected|=2;
        c.identity_bits=detected;
        if (detected == 1 || detected == 2) {
            c.kind = detected == 1 ? L"notifications" : L"quick";
            c.evidence = L"uia.structure";
        }
        c.recognized = c.kind != L"unknown";
        bool transparent_host = !content_rect(c.native_bounds, mi.rcWork) ||
            (c.native_bounds.bottom-c.native_bounds.top > (mi.rcWork.bottom-mi.rcWork.top)*95/100 &&
             c.native_bounds.top <= mi.rcWork.top+40);
        if (has_content && c.recognized && transparent_host) {
            // Transparent full-screen hosts are not the visible flyout bounds.
            UINT dpi = GetDpiForWindow(c.window); if (!dpi) dpi = 96;
            InflateRect(&content, MulDiv(20, dpi, 96), MulDiv(20, dpi, 96));
            IntersectRect(&c.bounds, &content, &mi.rcWork); c.geometry = L"uia.content_union";
        } else if (has_panels && !content_rect(c.native_bounds, mi.rcWork)) {
            c.bounds = panels; c.geometry = L"uia.panels";
        }
        refresh_visibility(c);
    }
    static std::vector<Candidate> discover(std::set<HWND>& remembered, unsigned& root_count) {
        std::vector<Candidate> discovered;
        auto all = roots(remembered); root_count = unsigned(all.size());
        std::map<DWORD, std::wstring> processes;
        for (HWND h : all) {
            Candidate c; c.window = h; c.klass = class_name(h);
            GetWindowThreadProcessId(h, &c.pid);
            auto [it, inserted] = processes.try_emplace(c.pid);
            if (inserted) it->second = process_name(h);
            c.process = it->second;
            if (!shell_process(c.process)) continue;
            remembered.insert(h);
            // A foreground Explorer/taskbar is not itself a candidate flyout.
            if (c.process == L"explorer.exe" && !possible_surface(c.klass) &&
                c.klass != L"Shell_TrayWnd" && c.klass != L"Shell_SecondaryTrayWnd") continue;
            wchar_t title[512]{}; GetWindowTextW(h, title, int(std::size(title)));
            auto hint = title_identity(title);
            if (hint) { c.kind = hint == 1 ? L"notifications" : L"quick"; c.evidence = L"native.title"; c.recognized = true; }
            discovered.push_back(c);
        }
        return discovered;
    }
public:
    ShellObserver() = default;
    ShellObserver(ShellObserver const&) = delete;
    ~ShellObserver() { stop(); }
    static constexpr unsigned native_interval_ms = 32;
    void start(unsigned diagnostic_uia_delay_ms = 0) {
        auto shared = state;
        // Native visibility never waits for an accessibility provider. Discovery
        // is infrequent; known Shell windows are sampled independently at 32ms.
        std::thread([shared] {
            std::set<HWND> remembered;
            std::vector<Candidate> known;
            std::map<HWND, Candidate> previous;
            uint64_t discovered_at = 0, samples = 0;
            unsigned root_count = 0;
            while (!shared->stop.load()) {
                auto begin = GetTickCount64();
                try {
                    if (!discovered_at || begin - discovered_at >= 500) {
                        known = discover(remembered, root_count); discovered_at = begin;
                    }
                    auto snapshot = std::make_shared<Snapshot>();
                    std::map<HWND, Candidate> accessibility;
                    {
                        std::lock_guard lock(shared->mutex);
                        accessibility = shared->accessibility;
                        snapshot->automation_status = shared->automation_status;
                        snapshot->uia_passes = shared->uia_passes;
                    }
                    for (auto c : known) {
                        DWORD pid = 0; GetWindowThreadProcessId(c.window, &pid);
                        if (!IsWindow(c.window) || pid != c.pid || class_name(c.window) != c.klass) continue;
                        if (FAILED(DwmGetWindowAttribute(c.window, DWMWA_EXTENDED_FRAME_BOUNDS,
                            &c.native_bounds, sizeof(c.native_bounds))) && !GetWindowRect(c.window, &c.native_bounds)) continue;
                        c.bounds = c.native_bounds; refresh_visibility(c);
                        if (auto it = accessibility.find(c.window); it != accessibility.end()) merge_accessibility(c, it->second);
                        refresh_visibility(c);
                        auto old = previous.find(c.window);
                        if (c.open) c.opened_at = old != previous.end() && old->second.open && old->second.pid == c.pid ?
                            old->second.opened_at : GetTickCount64();
                        snapshot->inventory.push_back(c);
                        if (eligible_panel(c)) snapshot->candidates.push_back(c);
                    }
                    previous.clear();
                    for (auto const& c : snapshot->inventory) previous[c.window] = c;
                    snapshot->roots = root_count; snapshot->native_samples = ++samples;
                    snapshot->collected_at = GetTickCount64();
                    snapshot->native_duration_ms = snapshot->collected_at - begin;
                    std::lock_guard lock(shared->mutex); shared->latest = std::move(snapshot);
                } catch (...) { /* Keep the previous snapshot; the UI detects staleness. */ }
                auto elapsed = GetTickCount64() - begin;
                std::unique_lock lock(shared->mutex);
                shared->wake.wait_for(lock, std::chrono::milliseconds(elapsed < native_interval_ms ? native_interval_ms - elapsed : 1),
                    [&] { return shared->stop.load(); });
            }
        }).detach();
        std::thread([shared, diagnostic_uia_delay_ms] {
            HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            {
                winrt::com_ptr<IUIAutomation> automation;
                HRESULT status = FAILED(apartment) ? apartment :
                    CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(automation.put()));
                if (automation) {
                    auto timeouts = automation.try_as<IUIAutomation2>();
                    if (timeouts) { timeouts->put_ConnectionTimeout(200); timeouts->put_TransactionTimeout(400); }
                }
                { std::lock_guard lock(shared->mutex); shared->automation_status = status; }
                std::map<HWND, uint64_t> checked;
                while (!shared->stop.load()) {
                    try {
                        if (diagnostic_uia_delay_ms) {
                            std::unique_lock lock(shared->mutex);
                            shared->wake.wait_for(lock, std::chrono::milliseconds(diagnostic_uia_delay_ms), [&] { return shared->stop.load(); });
                        }
                        if (shared->stop.load()) break;
                        std::shared_ptr<Snapshot const> snapshot;
                        { std::lock_guard lock(shared->mutex); snapshot = shared->latest; }
                        for (auto c : snapshot->inventory) {
                            if (shared->stop.load()) break;
                            auto now = GetTickCount64();
                            // Wait for the visual opening motion to settle before
                            // reading geometry, without delaying native showing.
                            if (c.open && c.recognized && now - c.opened_at < 200) continue;
                            bool taskbar = c.klass == L"Shell_TrayWnd" || c.klass == L"Shell_SecondaryTrayWnd";
                            if (!c.open && !taskbar) continue;
                            if (checked.contains(c.window) && now - checked[c.window] < 1000) continue;
                            checked[c.window] = now;
                            Candidate inspected = c;
                            inspected.bounds = c.native_bounds; inspected.geometry = L"native";
                            inspect(automation.get(), inspected);
                            if (FAILED(inspected.uia_status)) {
                                // A temporary provider failure does not discard
                                // previously known identity or valid geometry.
                                auto failed = inspected.uia_status; inspected = c; inspected.uia_status = failed;
                            }
                            std::lock_guard lock(shared->mutex);
                            shared->accessibility[c.window] = std::move(inspected);
                        }
                        std::unique_lock lock(shared->mutex);
                        ++shared->uia_passes;
                        std::erase_if(shared->accessibility, [](auto const& entry) { return !IsWindow(entry.first); });
                        shared->wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return shared->stop.load(); });
                    } catch (...) {
                        std::unique_lock lock(shared->mutex);
                        shared->wake.wait_for(lock, std::chrono::milliseconds(500), [&] { return shared->stop.load(); });
                    }
                }
            }
            if (SUCCEEDED(apartment)) CoUninitialize();
        }).detach();
    }
    std::shared_ptr<Snapshot const> latest() const { std::lock_guard lock(state->mutex); return state->latest; }
    void stop() { state->stop = true; state->wake.notify_all(); }
};

inline bool matches_filter(Candidate const& c, unsigned filter) {
    return eligible_panel(c) && (filter == 0 || (filter == 1 && c.kind == L"notifications") || (filter == 2 && c.kind == L"quick"));
}
inline Candidate const* select(std::vector<Candidate> const& candidates, unsigned filter, HWND dismissed,
                               HWND previous, HWND foreground) {
    Candidate const* selected = nullptr; int best = -1;
    for (auto const& c : candidates) {
        if (!c.open || c.window == dismissed || !matches_filter(c, filter)) continue;
        int score = c.window == foreground ? 4 : c.window == previous ? 3 : c.recognized ? 2 : 1;
        if (score > best) { selected = &c; best = score; }
    }
    return selected;
}
inline RECT related_bounds(std::vector<Candidate> const& candidates, Candidate const& selected) {
    RECT bounds = selected.bounds;
    for (auto const& c : candidates) {
        if (!c.open || !eligible_panel(c) || c.window == selected.window || c.kind != selected.kind || !eligible_panel(selected)) continue;
        if (MonitorFromWindow(c.window, MONITOR_DEFAULTTONEAREST) != MonitorFromWindow(selected.window, MONITOR_DEFAULTTONEAREST)) continue;
        // Notification/calendar surfaces can be separate native windows.
        if (std::abs(c.bounds.left - selected.bounds.left) < 80 && std::abs(c.bounds.right - selected.bounds.right) < 80)
            UnionRect(&bounds, &bounds, &c.bounds);
    }
    return bounds;
}
} // namespace companion
