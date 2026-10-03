#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#undef GetCurrentTime
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.Windows.Globalization.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include <cwctype>
#include <sstream>
#include <shobjidl.h>
#include "PluginProcess.h"
#include "ComponentPackages.h"
#include "IconAssets.h"
#include "ShellObserver.h"
#include "AsyncLog.h"
#include "PanelMotion.h"
#include "AcrylicBackdrop.h"
#include "WidgetLayout.h"
#include "WeatherData.h"
#include "PerformancePlot.h"
#include "NativePlugin.h"
#include "FollowSession.h"
#include "StartupSettings.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using Windows::Data::Json::JsonObject;
using Windows::Data::Json::JsonValue;
namespace fs = std::filesystem;
namespace SB = Microsoft::UI::Composition::SystemBackdrops;

namespace {
constexpr UINT tray_message = WM_APP + 40;
constexpr UINT motion_message = WM_APP + 41;
constexpr UINT theme_message = WM_APP + 42;
constexpr UINT ui_interval_ms = 16;
struct Options {
    bool self_test = false, preview = false, english = false, inspect = false;
    fs::path root, plugin_manifest;
    std::wstring channel;
    bool background=false, runtime_test=false, autostart=false, custom_data=false;
} options;
int demo_exit_code = 0;

std::wstring handle_text(HWND window) { return std::to_wstring(reinterpret_cast<uintptr_t>(window)); }
void field(JsonObject const& j, wchar_t const* key, std::wstring_view value) {
    j.SetNamedValue(key, JsonValue::CreateStringValue(value));
}
void log(std::wstring_view event, JsonObject j = {}) noexcept {
    try {
        SYSTEMTIME t{};
        GetSystemTime(&t);
        wchar_t stamp[64];
        swprintf_s(stamp, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear,t.wMonth,t.wDay,
                   t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
        field(j, L"time", stamp);
        field(j, L"event", event);
        j.SetNamedValue(L"tick_ms", JsonValue::CreateNumberValue(double(GetTickCount64())));
        field(j, L"foreground", handle_text(GetForegroundWindow()));
        if (options.root.empty()) return;
        static companion::AsyncLog writer(options.root);
        writer.write(to_string(j.Stringify()));
    } catch (...) { OutputDebugStringW(L"Companion demo log unavailable\n"); }
}
using Candidate = companion::Candidate;
JsonObject describe(Candidate const& c) {
    JsonObject j;
    field(j,L"hwnd",handle_text(c.window)); field(j,L"kind_hint",c.kind);
    field(j,L"process",c.process); field(j,L"class",c.klass);
    field(j,L"evidence",c.evidence); field(j,L"geometry_source",c.geometry);
    j.SetNamedValue(L"follow_eligible",JsonValue::CreateBooleanValue(companion::eligible_panel(c)));
    if(!companion::eligible_panel(c)) field(j,L"follow_exclusion",L"panel_identity_unconfirmed");
    j.SetNamedValue(L"process_id",JsonValue::CreateNumberValue(c.pid));
    j.SetNamedValue(L"uia_status",JsonValue::CreateNumberValue(c.uia_status));
    j.SetNamedValue(L"uia_nodes",JsonValue::CreateNumberValue(c.uia_nodes));
    j.SetNamedValue(L"identity_bits",JsonValue::CreateNumberValue(c.identity_bits));
    j.SetNamedValue(L"open", JsonValue::CreateBooleanValue(c.open));
    j.SetNamedValue(L"cloak_known", JsonValue::CreateBooleanValue(c.cloak_known));
    j.SetNamedValue(L"cloak", JsonValue::CreateNumberValue(c.cloak));
    j.SetNamedValue(L"left", JsonValue::CreateNumberValue(c.bounds.left));
    j.SetNamedValue(L"top", JsonValue::CreateNumberValue(c.bounds.top));
    j.SetNamedValue(L"right", JsonValue::CreateNumberValue(c.bounds.right));
    j.SetNamedValue(L"bottom", JsonValue::CreateNumberValue(c.bounds.bottom));
    j.SetNamedValue(L"native_left",JsonValue::CreateNumberValue(c.native_bounds.left));
    j.SetNamedValue(L"native_top",JsonValue::CreateNumberValue(c.native_bounds.top));
    j.SetNamedValue(L"native_right",JsonValue::CreateNumberValue(c.native_bounds.right));
    j.SetNamedValue(L"native_bottom",JsonValue::CreateNumberValue(c.native_bounds.bottom));
    return j;
}
// Physical screen coordinates, including negative monitor origins. No overlap fallback.
bool position(RECT work, RECT anchor, int preferred_width, int preferred_height, int gap, RECT& output,
              int minimum_width = 240, int minimum_height = 180) {
    int height = std::min(preferred_height, int(work.bottom-work.top)-gap*2);
    if (height < minimum_height) return false;
    int room_left = int(anchor.left-work.left)-gap*2;
    int room_right = int(work.right-anchor.right)-gap*2;
    bool left = room_left >= room_right;
    int width = std::min(preferred_width, left ? room_left : room_right);
    if (width < minimum_width) return false;
    int x = left ? int(anchor.left)-gap-width : int(anchor.right)+gap;
    // The calendar is anchored near the taskbar; align its bottom instead of
    // aligning with the (often transparent/empty) notification host's top.
    int y = std::clamp(int(anchor.bottom)-height, int(work.top)+gap, int(work.bottom)-gap-height);
    output = {x,y,x+width,y+height};
    return true;
}
bool shell_dark_theme() {
    DWORD light = 1, size = sizeof(light);
    // Shell and app themes can differ. Follow the current user's Shell setting;
    // this is a read-only compatibility hint, not a documented Shell contract.
    if (RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size)==ERROR_SUCCESS) return light==0;
    auto color = Windows::UI::ViewManagement::UISettings().GetColorValue(Windows::UI::ViewManagement::UIColorType::Background);
    return color.R < 128;
}
bool foreground_belongs_to(Candidate const& c, HWND foreground) {
    if (!foreground) return false;
    if (foreground==c.window || GetAncestor(foreground,GA_ROOT)==c.window || GetAncestor(foreground,GA_ROOTOWNER)==c.window) return true;
    DWORD pid=0;GetWindowThreadProcessId(foreground,&pid);
    // Keep Shell-owned context menus in the same activation chain. A normal
    // Explorer window must not be mistaken for an Explorer-hosted flyout.
    return c.process!=L"explorer.exe" && c.pid && pid==c.pid;
}

struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
    companion::ShellObserver observer;
    companion::PanelMotion motion;
    companion::MotionFrames motion_frames;
    com_ptr<companion::AcrylicBackdrop> backdrop;
    SB::SystemBackdropConfiguration backdrop_config{nullptr};
    Windows::UI::ViewManagement::UISettings ui_settings{nullptr};
    event_token color_changed{};
    Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider;
    Window window{nullptr};
    Grid root{nullptr};
    StackPanel motion_content{nullptr};
    TextBlock status{nullptr}, diagnostics{nullptr};
    HWND hwnd = nullptr, control = nullptr;
    HMONITOR preview_monitor = nullptr;
    NOTIFYICONDATAW icon{};
    bool shown = false, dark = true, closing = false;
    companion::FollowSession session;
    companion::StartupPreferences startup_preferences;
    companion::StartupRegistration startup_registration;
    ToggleSwitch auto_start_switch{nullptr},silent_start_switch{nullptr};
    bool updating_startup_controls=false;
    ToggleSwitch hide_tray_switch{nullptr};
    bool hide_tray_icon=false,tray_visible=false,updating_tray_controls=false;
    bool acrylic_attached = false, theme_override = false, vertical_motion = false;
    bool foreground_owned = false, automatic_dismissal = false;
    RECT animated_position{};
    bool animated_clipped = false;
    unsigned filter = 0, clicks = 0, passed = 0, motion_frame_requests = 0;
    unsigned fixture_step = 0;
    double fixture_started = 0;
    uint64_t started = 0;
    HWND active_shell = nullptr, dismissed_shell = nullptr;
    std::wstring active_kind, dismissed_kind;
    HMONITOR dismissed_monitor = nullptr;
    RECT last_position{};
    bool observer_stale = false;
    std::map<HWND, Candidate> logged_inventory, logged_candidates;
    bool logged_empty = false;
    struct WidgetSurface {
        Window window{nullptr}; Grid root{nullptr}; HWND hwnd=nullptr;
        com_ptr<companion::AcrylicBackdrop> backdrop;
        RECT target{}, animated{};
        bool clipped = false;
    };
    std::vector<WidgetSurface> widgets;
    std::vector<RECT> widget_positions;
    std::vector<RECT> layout_from;
    double layout_started=0;bool layout_moving=false;
    std::vector<int> widget_heights{264,280,208,208};
    std::vector<bool> widget_enabled{true,true,true,true};
    struct FreePosition {double x=0,y=0;bool saved=false;};
    std::vector<FreePosition> free_positions=std::vector<FreePosition>(4);
    RECT layout_work{},layout_reserved{};UINT layout_dpi=96;
    bool auto_arrange=true,dragging=false;
    HWND dragging_handle=nullptr;POINT dragging_start{};RECT dragging_origin{};
    struct Task {std::wstring title;bool done=false;};
    std::vector<Task> tasks;
    StackPanel task_list{nullptr};TextBlock task_count{nullptr},task_empty_message{nullptr};
    Button task_visibility_button{nullptr};bool hide_completed_tasks=false;
    companion::WeatherData weather;
    companion::WeatherReading weather_reading;
    std::wstring weather_city=L"Hong Kong";
    companion::WeatherConfig weather_config;uint64_t weather_generation=1;
    std::array<companion::QuickEntry,3> quick_entries=companion::default_quick_entries();StackPanel quick_list{nullptr};
    TextBlock weather_credit{nullptr};
    TextBlock weather_title{nullptr},weather_temp{nullptr},weather_details{nullptr},weather_status{nullptr},weather_symbol{nullptr};
    uint64_t last_weather=0,last_metrics=0;
    TextBlock cpu_caption{nullptr},memory_caption{nullptr};
    std::unique_ptr<companion::PerformancePlot> cpu_plot,memory_plot;
    ULONGLONG previous_idle=0,previous_kernel=0,previous_user=0;
    Window editor{nullptr};
    Grid task_input_row{nullptr};TextBox task_input{nullptr};TextBlock task_hint{nullptr};
    ScrollViewer task_scroll{nullptr};Button task_edit_button{nullptr},task_submit_button{nullptr},note_edit_button{nullptr};
    int task_edit_index=-1;double task_preview_height=140;
    bool inline_editing=false,inline_focus_pending=false;HWND inline_edit_handle=nullptr;
    uint64_t inline_focus_deadline=0;
    struct Probe { uint64_t due; uint64_t action; HWND shell; HWND foreground; unsigned delay; };
    std::vector<Probe> probes;
    std::vector<std::unique_ptr<companion::PluginProcess>> plugins;
    std::unique_ptr<companion::PluginChannel> child_channel;
    companion::PluginDefinition plugin_definition;
    std::unique_ptr<companion::NativePlugin> native_plugin;
    ContentControl native_body{nullptr};
    Window manager{nullptr};HWND manager_hwnd=nullptr;FrameworkElement manager_page{nullptr};
    std::vector<TextBlock> plugin_status;TextBlock manager_hint{nullptr};
    TextBox note_editor{nullptr};uint64_t note_due=0;
    std::wstring catalog_errors;bool manager_read_only=false,library_view=false;
    unsigned runtime_step=0;std::vector<DWORD> runtime_pids;bool runtime_processing=false;

    const wchar_t* tr(const wchar_t* zh,const wchar_t* en) const { return options.english ? en : zh; }
    Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const& name) {return provider.GetXamlType(name);}
    Markup::IXamlType GetXamlType(hstring const& name) {return provider.GetXamlType(name);}
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() {return provider.GetXmlnsDefinitions();}

    App() {
        UnhandledException([](auto const&, UnhandledExceptionEventArgs const& e) {
            JsonObject j; field(j,L"message",e.Message()); log(L"unhandled_winui",j);
        });
    }
    TextBlock text(std::wstring_view value, double size = 14) {
        TextBlock t; t.Text(value); t.FontSize(size); t.TextWrapping(TextWrapping::Wrap); return t;
    }
    Grid icon_label(std::wstring_view title,double font_size,double icon_size,fs::path const& image) {
        Grid row;row.ColumnSpacing(10);
        ColumnDefinition picture;picture.Width({1,GridUnitType::Auto});row.ColumnDefinitions().Append(picture);
        ColumnDefinition words;words.Width({1,GridUnitType::Star});row.ColumnDefinitions().Append(words);
        if(auto visual=companion::icon_image(image,icon_size))row.Children().Append(visual);
        auto label=text(title,font_size);label.VerticalAlignment(VerticalAlignment::Center);Grid::SetColumn(label,1);row.Children().Append(label);return row;
    }
    Button button(std::wstring_view title, std::function<void()> action) {
        Button b; b.Content(box_value(hstring(title))); b.IsTabStop(coordinator());
        b.HorizontalAlignment(HorizontalAlignment::Stretch);
        // Button handles PointerPressed internally. Observe it without consuming input.
        b.AddHandler(UIElement::PointerPressedEvent(), box_value(Input::PointerEventHandler(
            [this](auto const&, auto const&) { begin_probe(); })), true);
        b.Click([this,action](auto const&,auto const&) { try {action();} catch (...) {log(L"action_error");} });
        return b;
    }
    Border card(UIElement const& child) {
        auto b=Markup::XamlReader::Load(L"<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' "
            L"CornerRadius='8' Padding='16' Background='{ThemeResource CardBackgroundFillColorDefaultBrush}' "
            L"BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}' BorderThickness='1' />").as<Border>();
        b.Child(child); return b;
    }
    #include "WidgetFeatures.inc"
    #include "FeatureEditors.inc"
    #include "PluginRuntime.inc"
    #include "ControlCenter.inc"
    #include "StartupActions.inc"
    #include "StartupTests.inc"
    #include "PluginTests.inc"
    void build_ui() {
        if(root) {
            apply_surface_theme(root,acrylic_attached,backdrop.get());
            for(auto& surface:widgets)if(surface.root)apply_surface_theme(surface.root,surface.window.SystemBackdrop()!=nullptr,surface.backdrop.get());
            return;
        }
        root=surface_root();apply_surface_theme(root,acrylic_attached,nullptr);
        size_t index=options.self_test?0:4;
#ifdef WIDGET_COMPONENT_KIND
        if(child_channel)index=WIDGET_COMPONENT_KIND;
#endif
        auto stack=index<4?plugin_body(index):custom_plugin_body();motion_content=stack;
        status=text(L"",13);diagnostics=text(L"",12);
        root.Children().Append(stack);window.Content(root);
        if(options.self_test)build_widget_surfaces();
    }
    void initialize_backdrop() {
        backdrop=make_self<companion::AcrylicBackdrop>();
        backdrop_config=backdrop->configuration;
        // This changes material policy only. It never activates the HWND or
        // transfers foreground focus from the notification center.
        backdrop_config.IsInputActive(true);
        backdrop_config.Theme(dark ? SB::SystemBackdropTheme::Dark : SB::SystemBackdropTheme::Light);
        try {
            window.DispatcherQueue().EnsureSystemDispatcherQueue();
            if(backdrop->supported) {
                window.SystemBackdrop(backdrop.as<Microsoft::UI::Xaml::Media::SystemBackdrop>());
                acrylic_attached=true;
            }
        } catch(hresult_error const& e) {
            JsonObject j;field(j,L"message",e.message());j.SetNamedValue(L"hresult",JsonValue::CreateNumberValue(e.code()));log(L"backdrop_unavailable",j);
            window.SystemBackdrop(nullptr);acrylic_attached=false;
        }
        JsonObject j;field(j,L"material",acrylic_attached ? L"desktop_acrylic_base" : L"solid_fallback");
        field(j,L"theme",dark ? L"dark" : L"light");log(L"backdrop_configured",j);
        if(root)root.Background(SolidColorBrush(acrylic_attached ? Windows::UI::Color{0,0,0,0} :
            dark ? Windows::UI::Color{255,32,32,32} : Windows::UI::Color{255,243,243,243}));
    }
    void refresh_system_theme() {
        if(!theme_override) {
            bool next=shell_dark_theme();
            if(next!=dark) { dark=next;build_ui();theme_titlebar(manager);theme_titlebar(editor);log(L"system_theme_changed"); }
        }
        // DesktopAcrylicController handles transparency/high-contrast/energy
        // fallback. Never force a tint opacity to bypass those system policies.
        if(motion.moving() && !ui_settings.AnimationsEnabled()) {
            motion.request(motion.wants_visible(),false,companion::motion_now_ms());
            motion_frame();motion_frames.active(false);
        }
        if(layout_moving && !ui_settings.AnimationsEnabled()){layout_moving=false;animated_position={};motion_frame();}
    }
    void begin_probe() {
        if(child_channel){log(L"plugin_action");return;}
        ++clicks;
        auto id = GetTickCount64(); auto foreground = GetForegroundWindow();
        JsonObject j; field(j,L"shell_hwnd",handle_text(active_shell));
        j.SetNamedValue(L"action_id",JsonValue::CreateNumberValue(double(id)));
        log(L"pointer_press",j);
        diagnostics.Text(tr(L"正在检查点击后的系统面板状态…",L"Checking flyout state after the click…"));
        for (unsigned delay : {50u,250u,1000u}) probes.push_back({id+delay,id,active_shell,foreground,delay});
    }
    void drain_probes(std::vector<Candidate> const& candidates) {
        auto now = GetTickCount64();
        for (auto it=probes.begin();it!=probes.end();) {
            if (it->due>now) {++it;continue;}
            auto candidate = std::find_if(candidates.begin(),candidates.end(),[&](auto const& c){return c.window==it->shell;});
            JsonObject j;
            j.SetNamedValue(L"action_id",JsonValue::CreateNumberValue(double(it->action)));
            j.SetNamedValue(L"delay_ms",JsonValue::CreateNumberValue(it->delay));
            j.SetNamedValue(L"elapsed_ms",JsonValue::CreateNumberValue(double(now-it->action)));
            field(j,L"shell_hwnd",handle_text(it->shell)); field(j,L"foreground_before",handle_text(it->foreground));
            j.SetNamedValue(L"foreground_same",JsonValue::CreateBooleanValue(GetForegroundWindow()==it->foreground));
            j.SetNamedValue(L"shell_observed",JsonValue::CreateBooleanValue(candidate!=candidates.end()));
            j.SetNamedValue(L"shell_open",JsonValue::CreateBooleanValue(candidate!=candidates.end() && candidate->open));
            log(L"click_probe",j);
            if (it->delay==1000 && diagnostics) diagnostics.Text(
                !it->shell ? tr(L"手动预览：本次没有关联的系统面板。",L"Manual preview: no associated system flyout.") :
                candidate!=candidates.end() && candidate->open ? tr(L"一秒后仍观察到系统面板打开。",L"Flyout still observed open after one second.") :
                tr(L"系统面板已关闭或未能继续识别，请查看日志。",L"Flyout closed or no longer identified. Check the log."));
            it=probes.erase(it);
        }
    }
    void finish_hide() {
        motion_frames.active(false);
        layout_moving=false;
        for(auto& surface:widgets) {
            ShowWindow(surface.hwnd,SW_HIDE);
            SetWindowPos(surface.hwnd,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            if(surface.clipped)restore_card_region(surface.hwnd);
            surface.clipped=false;surface.animated={};
        }
        if(shown) {
            ShowWindow(surface_handle(0),SW_HIDE);
            SetWindowPos(surface_handle(0),HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            shown=false;log(L"panel_hidden");
        }
        if(animated_clipped)restore_card_region(surface_handle(0));
        animated_clipped=false;
        animated_position={};
    }
    void animate_surface(HWND handle,RECT target,RECT& prior,bool& clipped,double openness,size_t index) {
        if(!handle||!IsWindow(handle))return;
        if(coordinator() && motion.phase==companion::MotionPhase::Visible && index<plugins.size() && plugins[index]->channel && InterlockedCompareExchange(&plugins[index]->channel->wire->dragging,0,0))return;
        if(IsRectEmpty(&target)) {
            if(IsWindowVisible(handle))ShowWindow(handle,SW_HIDE);
            if(clipped)restore_card_region(handle);
            clipped=false;prior={};return;
        }
        RECT slot=target;
        if(layout_moving && index<layout_from.size() && !IsRectEmpty(&layout_from[index])) {
            double t=companion::fluent_ease((companion::motion_now_ms()-layout_started)/companion::PanelMotion::enter_ms,true);
            int x=int(std::lround(layout_from[index].left+(target.left-layout_from[index].left)*t));
            int y=int(std::lround(layout_from[index].top+(target.top-layout_from[index].top)*t));
            OffsetRect(&slot,x-slot.left,y-slot.top);
        }
        auto translated=companion::translated_panel(slot,openness,vertical_motion);
        bool clipping=motion.moving() || layout_moving;
        // Settled Acrylic windows must not receive repeated region / position
        // changes. Restore the resting card region once, even if the final
        // rounded pixel position was already reached on the previous frame.
        if(clipping || clipped) {
            if(clipping) {
                UINT dpi=GetDpiForWindow(handle);if(!dpi)dpi=96;
                auto region=CreateRoundRectRgn(0,0,target.right-target.left+1,target.bottom-target.top+1,MulDiv(16,dpi,96),MulDiv(16,dpi,96));
                auto clip=companion::motion_clip(slot,translated);
                auto viewport=CreateRectRgn(clip.left,clip.top,clip.right,clip.bottom);
                if(!region || !viewport){if(region)DeleteObject(region);if(viewport)DeleteObject(viewport);throw hresult_error(E_OUTOFMEMORY);}
                CombineRgn(region,region,viewport,RGN_AND);DeleteObject(viewport);
                if(!IsRectEmpty(&layout_reserved)) {
                    RECT reserved=layout_reserved;InflateRect(&reserved,MulDiv(8,layout_dpi,96),MulDiv(8,layout_dpi,96));
                    auto exclude=CreateRectRgn(reserved.left-translated.left,reserved.top-translated.top,reserved.right-translated.left,reserved.bottom-translated.top);
                    if(exclude){CombineRgn(region,region,exclude,RGN_DIFF);DeleteObject(exclude);}
                }
                if(!SetWindowRgn(handle,region,FALSE)){DeleteObject(region);throw hresult_error(E_FAIL);}
            } else restore_card_region(handle);
            clipped=clipping;
        }
        if(!EqualRect(&translated,&prior)) {
            check_bool(SetWindowPos(handle,HWND_TOPMOST,translated.left,translated.top,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOSENDCHANGING));
            prior=translated;
        }
        if(shown && !options.self_test && !IsWindowVisible(handle))ShowWindow(handle,SW_SHOWNOACTIVATE);
    }
    void motion_frame() {
        motion_frames.consumed();if(closing)return;
        auto now=companion::motion_now_ms();bool finished=motion.advance(now);
        if(motion.phase==companion::MotionPhase::Hidden){if(finished)log(L"motion_exit_completed");finish_hide();return;}
        if(layout_moving && (!ui_settings.AnimationsEnabled() || now-layout_started>=companion::PanelMotion::enter_ms))layout_moving=false;
        double openness=motion.value(now);
        animate_surface(surface_handle(0),last_position,animated_position,animated_clipped,openness,0);
        for(size_t i=0;i<widgets.size();++i) {
            auto& surface=widgets[i];animate_surface(surface.hwnd,surface.target,surface.animated,surface.clipped,openness,i+1);
        }
        motion_frames.active(motion.moving()||layout_moving);
        if(finished){JsonObject j;j.SetNamedValue(L"generation",JsonValue::CreateNumberValue(double(motion.generation)));log(L"motion_enter_completed",j);}
    }
    void hide(bool immediate=false) {
        bool animate=!immediate && !options.self_test && ui_settings && ui_settings.AnimationsEnabled();
        if(!motion.request(false,animate,companion::motion_now_ms())) return;
        if(coordinator())service_plugins();
        if(motion.moving()) {
            JsonObject j;j.SetNamedValue(L"duration_ms",JsonValue::CreateNumberValue(motion.duration));
            log(L"motion_exit_started",j);root.IsHitTestVisible(false);for(auto& surface:widgets)if(surface.root)surface.root.IsHitTestVisible(false);motion_frames.active(true);
        } else finish_hide();
    }
    void set_status(wchar_t const* value) {
        if (status.Text() != value) status.Text(value);
    }
    bool is_dismissed(Candidate const& c) const {
        return dismissed_shell && (c.window==dismissed_shell ||
            (!dismissed_kind.empty() && (dismissed_kind==L"unknown" || c.kind==dismissed_kind) &&
             MonitorFromWindow(c.window,MONITOR_DEFAULTTONEAREST)==dismissed_monitor));
    }
    void clear_dismissal() { dismissed_shell=nullptr;dismissed_kind.clear();dismissed_monitor=nullptr;automatic_dismissal=false; }
    void mark_dismissal(bool automatic) {
        dismissed_shell=active_shell;dismissed_kind=active_kind;
        dismissed_monitor=active_shell ? MonitorFromWindow(active_shell,MONITOR_DEFAULTTONEAREST) : nullptr;
        automatic_dismissal=automatic;
    }
    void dismiss_following() {
        mark_dismissal(false);
        session.preview=false;hide();
    }
    void show(RECT bounds, bool synthetic) {
        if (options.self_test) return;
        if(motion.phase==companion::MotionPhase::Visible) vertical_motion=!synthetic && active_kind==L"quick";
        bool moved = !EqualRect(&last_position,&bounds);
        if (!motion.wants_visible() || moved) {
            bool opening = !motion.wants_visible();
            auto foreground = GetForegroundWindow();
            bool was_hidden=motion.phase==companion::MotionPhase::Hidden;
            last_position=bounds;
            // Size changes happen only when the target layout changes. Frames
            // below translate an existing surface with SWP_NOSIZE.
            if(surface_handle(0) && (moved || !shown) && !IsRectEmpty(&bounds)) {
                auto current=layout_moving?animated_position:companion::translated_panel(bounds,motion.value(companion::motion_now_ms()),vertical_motion);
                if (!SetWindowPos(surface_handle(0),HWND_TOPMOST,current.left,current.top,bounds.right-bounds.left,
                                  bounds.bottom-bounds.top,SWP_NOACTIVATE)) {log(L"set_position_failed");hide(true);return;}
            }
            if (opening) {
                if(was_hidden) vertical_motion=!synthetic && active_kind==L"quick";
                motion.request(true,ui_settings.AnimationsEnabled(),companion::motion_now_ms());
                root.IsHitTestVisible(true);for(auto& surface:widgets)if(surface.root)surface.root.IsHitTestVisible(true);motion_frame();
                shown=true;motion_frame();
                motion_frames.active(motion.moving()||layout_moving);
                JsonObject j;field(j,L"axis",vertical_motion ? L"vertical" : L"horizontal");
                j.SetNamedValue(L"duration_ms",JsonValue::CreateNumberValue(motion.duration));
                j.SetNamedValue(L"reversed",JsonValue::CreateBooleanValue(!was_hidden));log(L"motion_enter_started",j);
            } else {
                animated_position={};motion_frame();
            }
            JsonObject j; field(j,L"source",synthetic ? L"manual_preview" : L"shell_candidate");
            j.SetNamedValue(L"left",JsonValue::CreateNumberValue(bounds.left));
            j.SetNamedValue(L"top",JsonValue::CreateNumberValue(bounds.top));
            j.SetNamedValue(L"right",JsonValue::CreateNumberValue(bounds.right));
            j.SetNamedValue(L"bottom",JsonValue::CreateNumberValue(bounds.bottom));
            j.SetNamedValue(L"foreground_same",JsonValue::CreateBooleanValue(foreground==GetForegroundWindow()));
            if (opening && !synthetic) {
                auto snapshot = observer.latest();
                auto candidate = std::find_if(snapshot->candidates.begin(),snapshot->candidates.end(),[this](auto const& c){return c.window==active_shell;});
                if (candidate!=snapshot->candidates.end() && candidate->opened_at)
                    j.SetNamedValue(L"observed_to_show_ms",JsonValue::CreateNumberValue(double(GetTickCount64()-candidate->opened_at)));
                j.SetNamedValue(L"native_sample_ms",JsonValue::CreateNumberValue(double(snapshot->native_duration_ms)));
            }
            log(opening ? L"panel_shown" : L"panel_repositioned",j);
        }
    }
    void poll() {
        if(child_channel){plugin_tick();return;}
        service_plugins();
        if(options.runtime_test){
            if(!runtime_processing){runtime_processing=true;try{runtime_test_tick();}catch(...){runtime_processing=false;throw;}runtime_processing=false;}
            return;
        }
        for(auto& surface:widgets)if(surface.window && surface.window.SystemBackdrop() && surface.backdrop && FAILED(surface.backdrop->last_error)) {
            surface.window.SystemBackdrop(nullptr);apply_surface_theme(surface.root,false,surface.backdrop.get());log(L"widget_backdrop_unavailable");
        }
        if(acrylic_attached && backdrop && FAILED(backdrop->last_error)) {
            JsonObject j;j.SetNamedValue(L"hresult",JsonValue::CreateNumberValue(backdrop->last_error));
            log(L"backdrop_unavailable",j);acrylic_attached=false;window.SystemBackdrop(nullptr);
            root.Background(SolidColorBrush(dark ? Windows::UI::Color{255,32,32,32} : Windows::UI::Color{255,243,243,243}));
        }
        if(options.self_test && fixture_step && fixture_step<5) {
            test_motion_cycle(); return;
        }
        auto snapshot = observer.latest();
        bool fresh = snapshot->collected_at && GetTickCount64()-snapshot->collected_at < 2500;
        auto candidates = fresh ? snapshot->candidates : std::vector<Candidate>{};
        for (auto& c:candidates) companion::refresh_visibility(c);
        auto log_changes = [](auto const& observations, auto& prior, wchar_t const* event) {
            std::set<HWND> present;
            for (auto const& c : observations) {
                present.insert(c.window);
                auto previous = prior.find(c.window);
                if (previous == prior.end() || !companion::same_observation(c, previous->second)) {
                    log(event, describe(c)); prior[c.window] = c;
                }
            }
            std::erase_if(prior, [&](auto const& entry) { return !present.contains(entry.first); });
        };
        log_changes(snapshot->inventory, logged_inventory, L"shell_inventory");
        if (observer_stale == fresh) {
            observer_stale=!fresh;JsonObject j;
            j.SetNamedValue(L"enumerated_roots",JsonValue::CreateNumberValue(snapshot->roots));
            j.SetNamedValue(L"uia_status",JsonValue::CreateNumberValue(snapshot->automation_status));
            log(fresh ? L"observer_ready" : L"observer_pending",j);
        }
        log_changes(candidates, logged_candidates, L"candidate_state");
        if (candidates.empty() && !logged_empty) {
            JsonObject j;j.SetNamedValue(L"enumerated_roots",JsonValue::CreateNumberValue(snapshot->roots));
            j.SetNamedValue(L"uia_status",JsonValue::CreateNumberValue(snapshot->automation_status));log(L"no_candidate",j);
        }
        logged_empty = candidates.empty();
        drain_probes(candidates); if(!coordinator())poll_features();
        if(dragging)return;
        auto foreground = GetForegroundWindow();
        if (dismissed_shell && fresh && std::none_of(candidates.begin(),candidates.end(),[this](auto const& c){return is_dismissed(c) && c.open;})) clear_dismissal();
        if(automatic_dismissal && std::any_of(candidates.begin(),candidates.end(),[&](auto const& c){
            return is_dismissed(c) && c.open && foreground_belongs_to(c,foreground);
        })) clear_dismissal();
        auto current=std::find_if(candidates.begin(),candidates.end(),[this](auto const& c){return c.window==active_shell && c.open;});
        // Cloaking can occur after the Shell's exit animation. Begin our exit
        // when its previously observed activation chain is lost, and prevent
        // the still-uncloaked native host from immediately reopening the panel.
        bool editing_card=plugin_inline_editing();
        if(!editor && !editing_card && !dismissed_shell && foreground_owned && current!=candidates.end() && !foreground_belongs_to(*current,foreground)) {
            mark_dismissal(true);log(L"shell_exit_early");
        }
        auto available=candidates;
        for (auto& c:available) if (is_dismissed(c)) c.open=false;
        // A real flyout takes over the preview's position. Preview never blocks following.
        auto selected = companion::select(available,filter,nullptr,active_shell,GetAncestor(foreground,GA_ROOT));
        if(editing_card && shown && !IsRectEmpty(&last_position)) {
            // Keep the existing reserved Shell area and card positions while typing.
            // Activating an input may dismiss the system flyout; it must not move the editor.
            show(last_position,false);
        } else if (selected) {
            if(session.adopt_panel())log(L"preview_handed_to_shell");
            if(active_shell!=selected->window)foreground_owned=false;
            foreground_owned|=foreground_belongs_to(*selected,foreground);
            active_shell=selected->window;active_kind=selected->kind;
            auto monitor=MonitorFromWindow(active_shell,MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(monitor,&mi);
            UINT dpi=GetDpiForWindow(active_shell); if (!dpi) dpi=96;
            RECT bounds{};
            RECT anchor=companion::related_bounds(candidates,*selected);
            for(auto const& c:candidates)if(c.open && companion::eligible_panel(c) && MonitorFromWindow(c.window,MONITOR_DEFAULTTONEAREST)==monitor)
                UnionRect(&anchor,&anchor,&c.bounds);
            if (layout_widgets(mi.rcWork,anchor,dpi,bounds)) {
                set_status(selected->kind==L"unknown" ? tr(L"跟随 Shell 面板 · 类型待确认",L"Following Shell flyout · type unconfirmed") :
                            selected->kind==L"notifications" ? tr(L"跟随通知中心",L"Following notifications") : tr(L"跟随快速设置",L"Following quick settings"));
                show(bounds,false);
            } else { set_status(tr(L"当前屏幕没有足够并排空间",L"Not enough room beside the flyout"));hide(true); }
        } else if (session.standalone(editor!=nullptr || editing_card)) {
            active_shell=nullptr;foreground_owned=false;
            MONITORINFO mi{sizeof(mi)};
            auto monitor=preview_monitor;
            if (!monitor || !GetMonitorInfoW(monitor,&mi)) {
                POINT point{};GetCursorPos(&point);monitor=MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST);
                preview_monitor=monitor;
            }
            GetMonitorInfoW(monitor,&mi);
            UINT dpi=96, dpi_y=96;
            if (FAILED(GetDpiForMonitor(monitor,MDT_EFFECTIVE_DPI,&dpi,&dpi_y))) dpi=96;
            RECT anchor{};RECT bounds{};
            if(layout_widgets(mi.rcWork,anchor,dpi,bounds)) {
                set_status(fresh ? tr(L"手动预览 · 等待系统面板，打开后自动并排",L"Preview · will move beside the system flyout") :
                            tr(L"手动预览 · 系统面板观察器正在检测",L"Preview · detecting system flyouts"));show(bounds,true);
            } else hide(true);
        } else { active_shell=nullptr;foreground_owned=false;hide(!fresh); }
        if (options.self_test && GetTickCount64()-started>1800) {
            if (IsWindowVisible(hwnd)) throw hresult_error(E_FAIL,L"Self-test must stay hidden");
            expect(fresh && snapshot->native_samples >= 10,L"Native observer remains live with a delayed UIA worker");
            expect(snapshot->uia_passes > 0 && snapshot->native_samples > snapshot->uia_passes,L"Native sampling proceeds independently of slow accessibility");
            expect(GetTickCount64()-snapshot->collected_at < 1000,L"Slow accessibility does not stale the native snapshot");
            expect(motion_frame_requests>0,L"Frame broker posts to the UI thread while a hidden test stays hidden");
            expect(!acrylic_attached || window.SystemBackdrop()!=nullptr,L"WinUI owns the Acrylic backdrop lifecycle");
            JsonObject j; j.SetNamedValue(L"assertions",JsonValue::CreateNumberValue(passed));
            j.SetNamedValue(L"native_samples",JsonValue::CreateNumberValue(double(snapshot->native_samples)));
            j.SetNamedValue(L"uia_passes",JsonValue::CreateNumberValue(double(snapshot->uia_passes)));
            j.SetNamedValue(L"native_sample_age_ms",JsonValue::CreateNumberValue(double(GetTickCount64()-snapshot->collected_at)));
            j.SetNamedValue(L"uia_diagnostic_delay_ms",JsonValue::CreateNumberValue(400));
            j.SetNamedValue(L"motion_frame_requests",JsonValue::CreateNumberValue(motion_frame_requests));
            j.SetNamedValue(L"acrylic_attached",JsonValue::CreateBooleanValue(acrylic_attached));
            j.SetNamedValue(L"acrylic_connected",JsonValue::CreateBooleanValue(backdrop && backdrop->connected));
            j.SetNamedValue(L"pending_policy_calls",JsonValue::CreateNumberValue(backdrop ? backdrop->pending_policy_calls : 0));
            j.SetNamedValue(L"backdrop_connections",JsonValue::CreateNumberValue(backdrop ? backdrop->connections : 0));
            log(L"self_test_pass",j);shutdown();
        }
    }
    void expect(bool condition,wchar_t const* message) {
        if (!condition) throw hresult_error(E_FAIL,message);
        ++passed;
    }
    void test_motion_cycle() {
        auto now=companion::motion_now_ms();
        if(IsWindowVisible(hwnd)) throw hresult_error(E_FAIL,L"Native animation fixture must remain hidden");
        if(fixture_step==1 && now-fixture_started>40) {
            double progress=motion.value(now);
            expect(motion.request(false,true,now) && std::abs(motion.value(now)-progress)<0.000001,L"Native entrance reverses into closing without a jump");
            motion_frames.active(true);
            fixture_step=2;fixture_started=now;
        } else if(fixture_step==2 && now-fixture_started>10) {
            double progress=motion.value(now);
            expect(motion.request(true,true,now) && std::abs(motion.value(now)-progress)<0.000001,L"Native closing reverses into opening without a jump");
            motion_frames.active(true);
            fixture_step=3;
        } else if(fixture_step==3 && motion.phase==companion::MotionPhase::Visible) {
            RECT actual{};GetWindowRect(hwnd,&actual);
            expect(actual.left==last_position.left && actual.top==last_position.top,L"Frame broker settles the actual HWND at its target");
            for(auto const& surface:widgets) {
                RECT bounds{};GetWindowRect(surface.hwnd,&bounds);
                expect(bounds.left==surface.target.left && bounds.top==surface.target.top && !IsWindowVisible(surface.hwnd),L"Shared motion settles every independent widget while remaining hidden");
            }
            expect(motion_frame_requests>=3,L"Frame broker drives multiple native frames on the UI thread");
            expect(full_card_region(hwnd),L"Entrance restores the full shadowless card region on completion");
            motion.request(false,true,now);motion_frames.active(true);fixture_step=4;
        } else if(fixture_step==4 && motion.phase==companion::MotionPhase::Hidden) {
            expect(!IsWindowVisible(hwnd),L"Completed native animation remained hidden throughout the test");
            expect(full_card_region(hwnd),L"Exit restores the full shadowless card region after hiding");
            fixture_step=5;last_position={};log(L"native_motion_fixture_pass");
        }
    }
    void toggle_preview() {
        if (!motion.wants_visible()) {
            session.preview=true;
            POINT point{};GetCursorPos(&point);preview_monitor=MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST);
            clear_dismissal();
        } else dismiss_following();
        log(session.preview ? L"preview_enabled" : L"preview_disabled");poll();
    }
    void self_test() {
        test_startup_settings();
        test_plugin_catalog();
        expect(!IsWindowVisible(hwnd),L"Window hidden");
        expect((GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0,L"No activate style");
        expect((GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)!=0,L"Tool window style");
        expect(SendMessageW(hwnd,WM_MOUSEACTIVATE,0,MAKELPARAM(HTCLIENT,WM_LBUTTONDOWN))==MA_NOACTIVATE,L"Mouse activation policy");
        for(auto const& surface:widgets) {
            expect(!IsWindowVisible(surface.hwnd),L"Every widget starts hidden");
            expect((GetWindowLongPtrW(surface.hwnd,GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0,L"Every widget shares the no-activate policy");
        }
        auto check_layout=[this](RECT work,RECT reserved,UINT dpi) {
            std::vector<int> heights;for(int height:widget_heights)heights.push_back(MulDiv(height,dpi,96));
            int gap=MulDiv(8,dpi,96);auto slots=companion::arrange_widgets(work,reserved,MulDiv(320,dpi,96),heights,gap);
            for(size_t i=0;i<slots.size();++i) {
                expect(!IsRectEmpty(&slots[i]) && companion::allowed_slot(slots[i],work,reserved,gap),L"Packed widget stays in workspace and avoids Shell");
                for(size_t j=0;j<i;++j)expect(!companion::overlaps(slots[i],slots[j],gap),L"Automatic arrangement keeps spacing between every pair");
            }
            expect(slots[0].right==reserved.left-gap,L"First widget is nearest the Shell side");
            expect(slots[0].bottom==std::min(int(work.bottom)-gap,int(reserved.bottom)),L"First widget is anchored at the bottom");
            auto dragged=companion::constrain_widget(reserved,slots[0],work,reserved,gap);
            expect(companion::allowed_slot(dragged,work,reserved,gap),L"Free dragging cannot cover Shell");
        };
        check_layout({0,0,1920,1032},{1572,8,1908,1020},96);
        check_layout({-1920,-100,0,932},{-350,-92,-12,920},96);
        check_layout({0,0,1920,1032},{1500,8,1908,1020},144);
        auto tight=companion::arrange_widgets({0,0,500,800},{100,0,500,800},320,{264,280},8);
        expect(IsRectEmpty(&tight[0]) && IsRectEmpty(&tight[1]),L"Insufficient space never forces overlapping cards");
        auto above=companion::arrange_widgets({0,0,900,900},{550,650,890,890},320,{264,280,208,208},8);
        expect(std::any_of(above.begin(),above.end(),[](RECT slot){return slot.left>=550 && slot.bottom<650;}),L"Space above a short Shell flyout remains available");
        expect(auto_arrange && tasks.empty(),L"Fresh installs auto arrange without fabricated tasks");
        tasks.push_back({L"保存测试 · 中文 / English",true});auto_arrange=false;
        free_positions[1]={0.3,0.7,true};widget_enabled[2]=false;save_state();
        tasks.clear();auto_arrange=true;free_positions[1]={};widget_enabled[2]=true;load_state();
        expect(tasks.size()==1 && tasks[0].title==L"保存测试 · 中文 / English" && tasks[0].done,L"Atomic config restores Unicode tasks and completion");
        expect(!auto_arrange && free_positions[1].saved && std::abs(free_positions[1].x-0.3)<0.000001 && !widget_enabled[2],L"Config restores free positions, layout mode and widget visibility");
        tasks.clear();auto_arrange=true;free_positions[1]={};widget_enabled[2]=true;save_state();render_tasks();
        RECT output{};
        expect(position({0,0,1920,1040},{1520,12,1908,1000},360,700,12,output) && output.right<1520,L"Left placement");
        expect(position({-1920,0,0,1040},{-400,12,-12,1000},360,700,12,output) && output.left<0,L"Negative monitor origin");
        expect(position({0,0,1920,1040},{12,12,400,1000},360,700,12,output) && output.left>400,L"Right placement");
        expect(!position({0,0,500,1040},{100,12,490,1000},360,700,12,output),L"Narrow monitor refused");
        expect(position({0,0,1920,500},{1520,200,1908,480},360,700,12,output) && output.bottom<=488,L"Height clamps inside work area");
        expect(root && weather_temp && task_list && widgets.size()==3,L"Four real WinUI widget surfaces constructed without clock, calendar or focus");
        dark=false;build_ui();expect(root.RequestedTheme()==ElementTheme::Light,L"Light theme");
        dark=true;build_ui();expect(root.RequestedTheme()==ElementTheme::Dark,L"Dark theme");
        test_task_editor();
        test_feature_settings();
        expect(probes.empty(),L"No simulated user clicks");
        auto padding=root.Padding();
        expect(padding.Left==16 && padding.Top==16 && padding.Right==16 && padding.Bottom==16,L"Padding on all four sides");
        auto radius=card(text(L"Test")).CornerRadius();
        expect(radius.TopLeft==8 && radius.TopRight==8 && radius.BottomRight==8 && radius.BottomLeft==8,L"Round all four card corners");
        expect(companion::identity(L"NotificationCenterGrid",L"Grid")==1,L"Notification structure");
        expect(companion::identity(L"CalendarView",L"CalendarView")==1,L"Calendar structure");
        expect(companion::identity(L"ControlCenterRegion",L"Grid")==2,L"Quick settings structure");
        expect(companion::identity(L"RootGrid",L"Grid")==0,L"Unrelated XAML is not a flyout");
        expect(companion::title_identity(L"通知中心")==1 && companion::title_identity(L"Notification Center")==1,L"Chinese and English identity");
        expect(!companion::shell_process(L"CompanionDemo.exe"),L"Third-party window rejected");
        expect(!companion::content_rect({0,0,1920,1040},{0,0,1920,1040}),L"Transparent full-screen root is not a content rectangle");
        std::vector<Candidate> sample(2);
        sample[0].window=hwnd;sample[0].kind=L"notifications";sample[0].open=true;sample[0].recognized=true;
        sample[1].window=control;sample[1].kind=L"quick";sample[1].open=true;sample[1].recognized=true;
        expect(companion::select(sample,1,nullptr,nullptr,control)==&sample[0],L"Notification filter");
        expect(companion::select(sample,2,nullptr,nullptr,hwnd)==&sample[1],L"Quick settings filter");
        expect(companion::select(sample,0,nullptr,hwnd,control)==&sample[1],L"Foreground candidate takes priority");
        // Replay the user's open/close sequence, including a preview started
        // before Shell opened. Layout preference persistence must not pin it.
        companion::FollowSession replay;
        companion::PanelMotion replay_motion;
        expect(!replay.standalone(false),L"Normal startup waits for a real system panel");
        replay.preview=true;
        expect(replay.standalone(false),L"Explicit preview can show without Shell");
        for(auto kind:{L"notifications",L"quick"}) {
            auto panel=sample[0];panel.kind=kind;panel.open=true;
            std::vector<Candidate> sequence{panel};
            expect(companion::select(sequence,0,nullptr,nullptr,panel.window)!=nullptr,L"Identified control or notification center begins following");
            replay.adopt_panel();replay_motion.request(true,true,0);
            expect(!replay.preview && !replay.standalone(false),L"A real system panel consumes preview instead of preserving fallback");
            expect(!replay.adopt_panel(),L"Repeated observations do not restore preview");
            sequence[0].open=false;
            expect(!companion::select(sequence,0,nullptr,panel.window,nullptr) && !replay.standalone(false),L"Closing either center leaves no standalone display request");
            replay_motion.request(false,true,100);
            expect(replay_motion.phase==companion::MotionPhase::Exiting,L"Closing Shell starts the shared exit animation");
            replay_motion.advance(1000);
            expect(replay_motion.phase==companion::MotionPhase::Hidden,L"All widgets finish hiding after Shell closes");
            sequence[0].open=true;
            expect(companion::select(sequence,0,nullptr,nullptr,panel.window)!=nullptr,L"Reopening either center can show widgets again");
            replay_motion.request(true,true,1100);replay_motion.advance(2000);
            expect(replay_motion.phase==companion::MotionPhase::Visible && !replay.preview,L"Reopening remains in follow mode");
            replay_motion.request(false,false,2100);
        }
        expect(replay.standalone(true) && !replay.preview,L"An editor only temporarily retains cards without enabling preview");
        expect(!replay.standalone(false),L"Closing the editor returns to automatic following");
        // Replay the user's banner identity and geometry without showing or
        // sending a notification. Both Shell surfaces use CoreWindow here.
        Candidate banner;banner.window=reinterpret_cast<HWND>(uintptr_t(460680));banner.pid=23816;
        banner.process=L"shellexperiencehost.exe";banner.klass=L"Windows.UI.Core.CoreWindow";
        banner.bounds=banner.native_bounds={1524,776,1920,1032};banner.open=true;banner.cloak_known=true;
        banner.uia_status=S_OK;banner.uia_nodes=17;
        expect(!companion::eligible_panel(banner),L"User screenshot banner is excluded despite the shared Shell window class");
        std::vector<Candidate> banner_only{banner};
        expect(!companion::select(banner_only,0,nullptr,banner.window,banner.window),L"A message banner alone never triggers the default follow mode");
        expect(!companion::select(banner_only,1,nullptr,nullptr,banner.window) && !companion::select(banner_only,2,nullptr,nullptr,banner.window),L"Message banners are excluded in both type filters");
        auto oversized_banner=banner;oversized_banner.bounds={1560,0,1920,1032};
        expect(!companion::matches_filter(oversized_banner,0),L"Large or full-height messages cannot bypass positive panel identity");
        oversized_banner.klass=L"ControlCenterWindow";oversized_banner.process=L"shellhost.exe";
        expect(!companion::matches_filter(oversized_banner,0),L"ControlCenterWindow class alone cannot trigger automatic following either");
        std::vector<Candidate> simultaneous{banner,sample[0],sample[1]};
        expect(companion::select(simultaneous,0,nullptr,banner.window,banner.window)==&simultaneous[1],L"A foreground or previous banner cannot replace a real notification center");
        simultaneous[1].open=false;
        expect(companion::select(simultaneous,0,nullptr,banner.window,banner.window)==&simultaneous[2],L"Quick settings can follow while a message banner is visible");
        simultaneous[2].open=false;
        expect(!companion::select(simultaneous,0,nullptr,banner.window,banner.window),L"Closing the real panel leaves no message-triggered follow surface");
        auto unconfirmed=banner;unconfirmed.kind=L"notifications";
        expect(!companion::matches_filter(unconfirmed,0),L"An unconfirmed kind hint is not sufficient for automatic following");
        unconfirmed.recognized=true;unconfirmed.kind=L"unknown";
        expect(!companion::matches_filter(unconfirmed,0),L"Inconsistent unknown classification is excluded");
        expect(companion::identity(L"ExpandCollapseButton",L"Button")==0 && companion::identity(L"ToastView",L"Windows.UI.Shell.ActionCenter.NotificationView")==0,L"Notification leaf controls do not establish notification-center identity");
        RECT observed{870,265,1204,1019};
        expect(position({0,0,1224,1032},observed,360,700,12,output) &&
               output.left==498 && output.right==858 && output.top==319 && output.bottom==1019,L"Screenshot regression: beside and bottom-aligned with calendar");
        expect(position({0,0,1920,1032},{1560,0,1920,1032},360,700,12,output) &&
               output.top==320 && output.bottom==1020,L"Empty notification region does not top-align the companion");
        expect(position({0,0,2880,1548},{2340,0,2880,1548},540,1050,18,output,360,270) &&
               output.top==480 && output.bottom==1530,L"Bottom placement at 150 percent DPI");
        expect(position({-3840,-2160,0,-96},{-720,-2160,0,-96},720,1400,24,output,480,360) &&
               output.bottom==-120 && output.top==-1520,L"Bottom placement at 200 percent DPI with negative monitor origin");
        sample[0].bounds={870,265,1204,495};sample[1].kind=L"notifications";sample[1].bounds={870,509,1204,1019};
        auto joined=companion::related_bounds(sample,sample[0]);
        expect(joined.top==265 && joined.bottom==1019 && joined.left==870,L"Notification and calendar bounds join");
        dismissed_shell=hwnd;dismissed_kind=L"notifications";dismissed_monitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);
        expect(is_dismissed(sample[0]) && is_dismissed(sample[1]),L"Dismiss notification and calendar together");clear_dismissal();
        Candidate cached;
        cached.window=hwnd;cached.pid=GetCurrentProcessId();cached.klass=L"TestShell";
        cached.recognized=true;cached.kind=L"notifications";cached.geometry=L"uia.content_union";
        cached.native_bounds={1560,0,1920,1032};cached.bounds={1580,265,1900,1019};
        Candidate current;current.window=hwnd;current.pid=cached.pid;current.klass=cached.klass;
        current.native_bounds=cached.native_bounds;current.bounds=current.native_bounds;current.cloak_known=true;
        auto merged=current;companion::merge_accessibility(merged,cached);
        expect(merged.recognized && EqualRect(&merged.bounds,&cached.bounds),L"Reuse validated accessibility identity and geometry");
        merged=current;merged.cloak=2;companion::merge_accessibility(merged,cached);
        expect(merged.recognized && merged.geometry==L"native",L"Hidden flyout retains identity without cached visible geometry");
        merged=current;++merged.pid;companion::merge_accessibility(merged,cached);
        expect(!merged.recognized && merged.geometry==L"native",L"Reused HWND from another process rejects cached identity");
        merged=current;merged.native_bounds.left-=100;companion::merge_accessibility(merged,cached);
        expect(merged.recognized && merged.geometry==L"native",L"Moved Shell window rejects obsolete cached geometry");
        auto changed=current;++changed.uia_nodes;
        expect(companion::same_observation(current,changed),L"Accessibility node-count churn does not spam logs");
        changed.open=!current.open;
        expect(!companion::same_observation(current,changed),L"Visibility changes still emit diagnostics");
        changed=current;changed.recognized=!current.recognized;
        expect(!companion::same_observation(current,changed),L"Panel eligibility changes still emit diagnostics");
        expect(Hosting::ElementCompositionPreview::GetElementVisual(motion_content)!=nullptr,L"Content has a compositor visual for native-frame-independent animation");
        expect(backdrop_config && backdrop_config.IsInputActive(),L"Nonactivating window uses active material policy without foreground activation");
        expect(!acrylic_attached || root.Background().as<SolidColorBrush>().Color().A==0,L"Acrylic is exposed through a transparent XAML root");
        expect(acrylic_attached ? backdrop->supported : root.Background().as<SolidColorBrush>().Color().A==255,L"Material uses supported system Acrylic or opaque fallback");
        auto saved_theme=backdrop_config.Theme();auto pending_calls=backdrop->pending_policy_calls;
        backdrop->apply_policy(nullptr);
        expect(backdrop->pending_policy_calls==pending_calls+1 && backdrop_config.Theme()==saved_theme && backdrop_config.IsInputActive(),L"Missing initial backdrop policy preserves safe defaults instead of dereferencing null");
        auto saved_root=root;
        for(unsigned i=0;i<6;++i) {dark=!dark;build_ui();}
        expect(root==saved_root && window.Content()==saved_root,L"Repeated theme changes keep the connected XAML root alive");
        expect(backdrop_config.Theme()==(dark ? SB::SystemBackdropTheme::Dark : SB::SystemBackdropTheme::Light),L"Repeated theme changes update the backdrop configuration");
        expect(companion::fluent_ease(0,true)==0 && companion::fluent_ease(1,true)==1,L"Entrance easing endpoints");
        expect(companion::fluent_ease(0,false)==0 && companion::fluent_ease(1,false)==1,L"Exit easing endpoints");
        expect(companion::fluent_ease(0.5,true)>0.85 && companion::fluent_ease(0.5,false)<0.15,L"Fluent entrance decelerates and exit accelerates");
        companion::PanelMotion transition;
        expect(transition.request(true,true,0) && transition.phase==companion::MotionPhase::Entering,L"Hidden panel begins entrance");
        auto generation=transition.generation;
        expect(!transition.request(true,true,40) && transition.generation==generation,L"Repeated observations do not restart entrance");
        double before=transition.value(125);
        expect(transition.request(false,true,125) && std::abs(transition.value(125)-before)<0.000001,L"Mid-entrance closing preserves current progress");
        before=transition.value(140);
        expect(transition.request(true,true,140) && std::abs(transition.value(140)-before)<0.000001,L"Mid-exit reopening reverses continuously");
        expect(transition.advance(1000) && transition.phase==companion::MotionPhase::Visible,L"Entrance completion reaches visible state");
        expect(transition.request(false,true,1100) && !transition.request(false,true,1110),L"Repeated close does not restart exit");
        expect(transition.advance(1400) && transition.phase==companion::MotionPhase::Hidden,L"Exit completion reaches hidden state");
        expect(transition.request(true,false,1500) && transition.phase==companion::MotionPhase::Visible,L"Animations disabled displays immediately");
        expect(transition.request(false,false,1501) && transition.phase==companion::MotionPhase::Hidden,L"Animations disabled hides immediately");
        transition.request(true,true,1600);
        expect(transition.request(true,false,1610) && transition.phase==companion::MotionPhase::Visible,L"Turning off effects mid-entrance settles immediately");
        transition.request(false,true,1700);
        expect(transition.request(false,false,1710) && transition.phase==companion::MotionPhase::Hidden,L"Turning off effects mid-exit hides immediately");
        RECT slot{-720,320,-360,1020};
        auto shifted=companion::translated_panel(slot,0,false);
        auto clip=companion::motion_clip(slot,shifted);
        expect(IsRectEmpty(&clip),L"Closed horizontal motion has an empty visible region");
        shifted=companion::translated_panel(slot,0.5,false);clip=companion::motion_clip(slot,shifted);
        expect(clip.left==0 && clip.right==180 && clip.bottom==700,L"Horizontal motion is clipped to its own final slot on a negative monitor");
        shifted=companion::translated_panel(slot,1,false);clip=companion::motion_clip(slot,shifted);
        expect(clip.right==360 && clip.bottom==700,L"Open motion exposes the whole panel");
        shifted=companion::translated_panel(slot,0.5,true);clip=companion::motion_clip(slot,shifted);
        expect(clip.right==360 && clip.bottom==350,L"Quick-settings vertical motion stays within its own slot");
        auto shell_owned=sample[0];shell_owned.pid=GetCurrentProcessId();shell_owned.process=L"shellhost.exe";
        expect(foreground_belongs_to(shell_owned,control),L"Shell-owned context menus retain the activation chain");
        shell_owned.process=L"explorer.exe";
        expect(!foreground_belongs_to(shell_owned,control),L"Another Explorer window is not an Explorer-hosted flyout");
        test_stationary_surfaces();
        last_position={0,0,320,264};
        for(size_t i=0;i<widgets.size();++i)widgets[i].target={LONG(400+i*340),200,LONG(720+i*340),LONG(200+widget_heights[i+1])};
        motion.request(true,true,companion::motion_now_ms());motion_frame();
        expect(!IsWindowVisible(hwnd) && motion.moving(),L"Native motion operations never show the hidden fixture");
        auto native_region=CreateRectRgn(0,0,0,0);
        int region_type=GetWindowRgn(hwnd,native_region);DeleteObject(native_region);
        expect(region_type!=ERROR,L"Moving window has a real clipping region");
        fixture_step=1;fixture_started=companion::motion_now_ms();motion_frames.active(true);
        log(L"self_test_started");
    }
    static LRESULT CALLBACK panel_proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR p) {
        auto self=reinterpret_cast<App*>(p);
        try {
            if(m==WM_ENTERSIZEMOVE){self->dragging=true;self->layout_moving=false;}
            if(m==WM_MOVING && !self->auto_arrange) {
                auto requested=reinterpret_cast<RECT*>(l);size_t i=self->surface_index(h);
                if(i<self->widget_positions.size())*requested=companion::constrain_widget(*requested,self->widget_positions[i],self->layout_work,self->layout_reserved,MulDiv(8,self->layout_dpi,96));
                return TRUE;
            }
            if(m==WM_EXITSIZEMOVE) {
                self->dragging=false;size_t i=self->surface_index(h);RECT actual{};GetWindowRect(h,&actual);
                if(i<self->widget_positions.size()) {
                    self->remember_position(i,actual);self->widget_positions[i]=actual;
                    if(i==0){self->last_position=actual;self->animated_position=actual;}
                    else {self->widgets[i-1].target=actual;self->widgets[i-1].animated=actual;}
                    self->save_state();self->poll();
                }
            }
        }catch(...){log(L"widget_drag_failed");self->dragging=false;}
        if(m==WM_NCHITTEST && self->motion.phase==companion::MotionPhase::Exiting)return HTTRANSPARENT;
        if(m==WM_MOUSEACTIVATE) {
            if(self->inline_editing&&h==self->inline_edit_handle)return MA_ACTIVATE;
            log(L"mouse_activate_blocked");return MA_NOACTIVATE;
        }
        if(m==WM_ACTIVATE) {
            if(LOWORD(w)==WA_INACTIVE&&self->inline_editing&&h==self->inline_edit_handle&&!self->inline_focus_pending)
                PostMessageW(self->control,WM_APP+106,0,0);
            else if(LOWORD(w)!=WA_INACTIVE&&!self->inline_editing)log(L"unexpected_panel_activation");
        }
        if(m==WM_CLOSE) {self->dismiss_following();return 0;}
        if(m==WM_NCDESTROY) RemoveWindowSubclass(h,panel_proc,1);
        return DefSubclassProc(h,m,w,l);
    }
    static LRESULT CALLBACK control_proc(HWND h,UINT m,WPARAM w,LPARAM l) {
        auto self=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(m==WM_NCCREATE) {self=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self) try {
            if(m==WM_APP+106) {if(self->inline_editing&&GetForegroundWindow()!=self->inline_edit_handle)self->finish_inline_edit();return 0;}
            if(m==WM_APP+101 && self->child_channel){self->shutdown();return 0;}
            if(m==WM_APP+100){self->show_manager();return 0;}
            if(m==motion_message) {++self->motion_frame_requests;self->motion_frame();return 0;}
            if(m==theme_message) {self->refresh_system_theme();return 0;}
            if(m==WM_TIMER) {self->poll();return 0;}
            if(m==WM_HOTKEY) {if(w==2)self->shutdown();else self->toggle_preview();return 0;}
            if(m==tray_message) {
                if(l==WM_LBUTTONUP) self->show_manager();
                if(l==WM_RBUTTONUP) self->tray_menu();
                return 0;
            }
            if(m==WM_SETTINGCHANGE) {self->refresh_system_theme();self->poll();return 0;}
            if(m==WM_DISPLAYCHANGE) {self->hide(true);self->last_position={};self->poll();return 0;}
            if(m==RegisterWindowMessageW(L"TaskbarCreated") && self->coordinator()) {
                self->tray_visible=false;self->apply_tray_visibility();return 0;
            }
        } catch(hresult_error const& e) {JsonObject j;field(j,L"message",e.message());log(L"poll_failed",j);if(options.self_test||options.runtime_test){demo_exit_code=1;self->shutdown();}else self->hide(true);}
        catch (...) {log(L"poll_failed");if(options.self_test||options.runtime_test){demo_exit_code=1;self->shutdown();}else self->hide(true);}
        return DefWindowProcW(h,m,w,l);
    }
    void tray_menu() {
        auto menu=CreatePopupMenu();if(!menu)return;
        AppendMenuW(menu,MF_STRING,1,tr(L"打开控制中心",L"Open control center"));
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,3,tr(L"退出",L"Quit"));
        POINT point{};GetCursorPos(&point);SetForegroundWindow(control);
        auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,control,nullptr);
        DestroyMenu(menu);PostMessageW(control,WM_NULL,0,0);
        if(command==1)show_manager();else if(command==3)shutdown();
    }
    void shutdown() {
        if(closing)return;closing=true;observer.stop();motion_frames.stop();log(L"shutdown");
        if(ui_settings && color_changed.value)ui_settings.ColorValuesChanged(color_changed);
        if(coordinator()){save_manager();for(auto& p:plugins)p->stop();if(manager){manager.Close();manager=nullptr;}}else {if(native_plugin)native_plugin.reset();save_state();if(note_editor)save_note();}
        if(editor){close_editor();editor=nullptr;}
        for(auto& surface:widgets)if(surface.window){RemoveWindowSubclass(surface.hwnd,panel_proc,1);surface.window.SystemBackdrop(nullptr);surface.window.Close();}
        widgets.clear();
        if(window)window.SystemBackdrop(nullptr);
        if(control){KillTimer(control,1);UnregisterHotKey(control,1);UnregisterHotKey(control,2);if(tray_visible)Shell_NotifyIconW(NIM_DELETE,&icon);tray_visible=false;}
        if(hwnd)RemoveWindowSubclass(hwnd,panel_proc,1);
        if(window)window.Close();
        if(control){DestroyWindow(control);control=nullptr;}
        Exit();
    }
    void OnLaunched(LaunchActivatedEventArgs const&) {
        try {
            started=GetTickCount64();session.preview=options.preview;dark=shell_dark_theme();
            if(coordinator()&&!options.runtime_test) {
                startup_preferences.load(options.root);
                if(startup_preferences.read_only){catalog_errors+=tr(L"启动设置损坏或版本不兼容，原文件已保留。\n",L"Startup settings are damaged or incompatible; the original file was preserved.\n");log(L"startup_preferences_invalid");}
            }
            if(!options.channel.empty()){child_channel=std::make_unique<companion::PluginChannel>();child_channel->open(options.channel);plugin_definition=companion::read_plugin(options.plugin_manifest);widget_heights={plugin_definition.height};free_positions.resize(1);widget_enabled.resize(1);}
            load_state();
            ui_settings=Windows::UI::ViewManagement::UISettings();
            Resources().MergedDictionaries().Append(XamlControlsResources());
            window=Window();window.Title(child_channel?L"WindowsWidget · "+plugin_definition.name:L"WindowsWidget · Coordinator");
            companion::set_window_icon(window,child_channel?companion::component_window_icon(plugin_definition):companion::bundled_icon(L"app",true));
            check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
            auto presenter=window.AppWindow().Presenter().as<Microsoft::UI::Windowing::OverlappedPresenter>();
            presenter.SetBorderAndTitleBar(false,false);presenter.IsResizable(false);presenter.IsMaximizable(false);presenter.IsMinimizable(false);
            SetWindowLongPtrW(hwnd,GWL_STYLE,(GetWindowLongPtrW(hwnd,GWL_STYLE)|WS_POPUP)&~(WS_CAPTION|WS_THICKFRAME|WS_BORDER|WS_DLGFRAME));
            SetWindowLongPtrW(hwnd,GWL_EXSTYLE,(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW)&~WS_EX_APPWINDOW);
            if(options.inspect)SetWindowLongPtrW(hwnd,GWL_EXSTYLE,(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)|WS_EX_APPWINDOW)&~(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW));
            SetWindowSubclass(hwnd,panel_proc,1,reinterpret_cast<DWORD_PTR>(this));
            SetWindowPos(hwnd,nullptr,0,0,320,264,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
            build_ui();initialize_backdrop();
            configure_card_shape(hwnd);
            WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=control_proc;wc.lpszClassName=child_channel?L"WindowsWidget.Plugin.Control":L"WindowsWidget.CompanionDemo.Control";
            RegisterClassW(&wc);
            control=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"",WS_POPUP,0,0,0,0,nullptr,nullptr,wc.hInstance,this);
            check_bool(control!=nullptr);
            if(child_channel) {
                if(plugin_definition.kind==L"native"){native_plugin=std::make_unique<companion::NativePlugin>();native_plugin->start(plugin_definition,options.root,hwnd,native_body,dark);}
                auto* wire=child_channel->wire;wire->pid=GetCurrentProcessId();wire->panel=hwnd;wire->control=control;
                SetWindowPos(hwnd,nullptr,0,0,320,plugin_definition.height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                restore_card_region(hwnd);
                InterlockedExchange(&wire->ready,1);SetTimer(control,1,32,nullptr);log(L"plugin_ready");return;
            }
            if(coordinator())initialize_plugins();
            motion_frames.start(control,motion_message);
            color_changed=ui_settings.ColorValuesChanged([target=control](auto const&,auto const&){
                PostMessageW(target,theme_message,0,0);
            });
            if(!options.self_test&&!options.runtime_test) {
                icon.cbSize=sizeof(icon);icon.hWnd=control;icon.uID=1;icon.uFlags=NIF_ICON|NIF_TIP|NIF_MESSAGE;
                icon.hIcon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));icon.uCallbackMessage=tray_message;
                wcscpy_s(icon.szTip,L"随行组件 · 控制中心");
                apply_tray_visibility();
                if(!RegisterHotKey(control,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'W'))log(L"preview_hotkey_unavailable");
                if(!RegisterHotKey(control,2,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'Q'))log(L"quit_hotkey_unavailable");
            }
            SetTimer(control,1,ui_interval_ms,nullptr);
            observer.start(options.self_test ? 400 : 0);
            JsonObject startup;field(startup,L"build",L"inline-editing-1");
            startup.SetNamedValue(L"autostart_launch",JsonValue::CreateBooleanValue(options.autostart));
            startup.SetNamedValue(L"silent_start",JsonValue::CreateBooleanValue(startup_preferences.silent));
            startup.SetNamedValue(L"hide_tray_icon",JsonValue::CreateBooleanValue(hide_tray_icon));
            startup.SetNamedValue(L"preview",JsonValue::CreateBooleanValue(session.preview));
            field(startup,L"follow_policy",L"confirmed_panels_only");
            startup.SetNamedValue(L"enter_ms",JsonValue::CreateNumberValue(companion::PanelMotion::enter_ms));
            startup.SetNamedValue(L"exit_ms",JsonValue::CreateNumberValue(companion::PanelMotion::exit_ms));
            startup.SetNamedValue(L"animations_enabled",JsonValue::CreateBooleanValue(ui_settings.AnimationsEnabled()));
            startup.SetNamedValue(L"ui_interval_ms",JsonValue::CreateNumberValue(ui_interval_ms));
            startup.SetNamedValue(L"native_interval_ms",JsonValue::CreateNumberValue(companion::ShellObserver::native_interval_ms));
            field(startup,L"alignment",L"bottom");
            field(startup,L"panel_hwnd",handle_text(hwnd));field(startup,L"control_hwnd",handle_text(control));log(L"startup",startup);
            if(options.self_test)self_test();else {poll();if(companion::manager_on_launch(options.autostart,options.background,options.runtime_test,startup_preferences.silent))show_manager();}
        } catch(hresult_error const& e) {
            demo_exit_code=1;JsonObject j;field(j,L"message",e.message());j.SetNamedValue(L"hresult",JsonValue::CreateNumberValue(e.code()));log(L"startup_failed",j);shutdown();
        } catch(...) {demo_exit_code=1;log(L"startup_failed");shutdown();}
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    try {
        init_apartment(apartment_type::single_threaded);
        PWSTR local=nullptr;check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local));
        options.root=fs::path(local)/L"WindowsWidget"/L"CompanionDemo";CoTaskMemFree(local);
        int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);
        for(int i=1;i<count;++i) {
            std::wstring_view arg=args[i];
            if(arg==L"--self-test")options.self_test=true;
            else if(arg==L"--preview")options.preview=true;
            else if(arg==L"--inspect"){options.inspect=true;options.preview=true;}
            else if(arg==L"--plugin"&&i+1<count)options.plugin_manifest=fs::absolute(args[++i]);
            else if(arg==L"--channel"&&i+1<count)options.channel=args[++i];
            else if(arg==L"--background")options.background=true;
            else if(arg==L"--autostart")options.autostart=true;
            else if(arg==L"--runtime-test"){options.runtime_test=true;options.background=true;}
            else if(arg==L"--en")options.english=true;
            else if(arg==L"--data-dir"&&i+1<count){options.root=fs::absolute(args[++i]);options.custom_data=true;}
            else {LocalFree(args);return 2;}
        }
        LocalFree(args);
#ifdef WIDGET_COMPONENT_KIND
        // This standalone module only renders its own component. The coordinator never supplies a renderer kind.
        if(options.channel.empty() || options.self_test || options.runtime_test)return 2;
#endif
        if(!options.channel.empty() && options.plugin_manifest.empty())return 2;
        if((options.self_test||options.runtime_test) && options.root.filename()==L"CompanionDemo") return 2;
        // Tests use unique data directories and remain hidden; normal demo is one instance per session.
        winrt::handle singleton;
        if (!options.self_test && !options.runtime_test && options.channel.empty()) {
            HANDLE handle=CreateMutexW(nullptr,FALSE,L"Local\\WindowsWidget.CompanionDemo.Singleton");
            DWORD error=GetLastError();singleton.attach(handle);
            if (!singleton) return 1;
            if (error==ERROR_ALREADY_EXISTS){
                if(companion::reopen_existing_manager(options.autostart,options.background))
                    if(auto existing=FindWindowW(L"WindowsWidget.CompanionDemo.Control",L""))PostMessageW(existing,WM_APP+100,0,0);
                return 0;
            }
        }
        Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(options.english ? L"en-US" : L"zh-CN");
        Application::Start([](auto&&){make<App>();});
        return demo_exit_code;
    } catch(hresult_error const& e) {
        JsonObject j;field(j,L"message",e.message());j.SetNamedValue(L"hresult",JsonValue::CreateNumberValue(e.code()));
        log(L"entry_failed",j);return 1;
    } catch(...) {log(L"entry_failed");return 1;}
}
