#pragma once
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include "PluginCatalog.h"
#include <string_view>

namespace companion {
inline fs::path bundled_icon(std::wstring_view key, bool windows_icon=false) {
    return executable_path().parent_path()/L"icons"/(std::wstring(key)+(windows_icon?L".ico":L".png"));
}
inline std::wstring component_icon_key(PluginDefinition const& definition) {
    if(definition.id==L"mod.bluetooth-battery")return L"bluetooth-battery";
    if(definition.kind==L"native")return L"app";
    return definition.kind;
}
inline fs::path component_icon(PluginDefinition const& definition) {
    if(!definition.icon.empty())return definition.icon;
    auto path=bundled_icon(component_icon_key(definition));
    return fs::exists(path)?path:bundled_icon(L"app");
}
inline fs::path component_window_icon(PluginDefinition const& definition) {
    auto path=bundled_icon(component_icon_key(definition),true);
    return fs::exists(path)?path:bundled_icon(L"app",true);
}
inline winrt::Windows::Foundation::Uri image_file_uri(fs::path const& path) {
    auto utf8=winrt::to_string(fs::absolute(path).generic_wstring());
    std::string uri="file:///";
    constexpr char digits[]="0123456789ABCDEF";
    for(unsigned char c:utf8) {
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='/'||c==':'||c=='-'||c=='_'||c=='.'||c=='~')uri+=char(c);
        else {uri+='%';uri+=digits[c>>4];uri+=digits[c&15];}
    }
    return winrt::Windows::Foundation::Uri(winrt::to_hstring(uri));
}
inline winrt::Microsoft::UI::Xaml::Controls::Image icon_image(fs::path const& path,double size) {
    using namespace winrt::Microsoft::UI::Xaml;
    Controls::Image result{nullptr};
    if(!fs::exists(path))return result;
    result=Controls::Image();result.Width(size);result.Height(size);
    result.Stretch(Media::Stretch::Uniform);result.IsHitTestVisible(false);
    result.Source(Media::Imaging::BitmapImage(image_file_uri(path)));
    Automation::AutomationProperties::SetAccessibilityView(result,Automation::Peers::AccessibilityView::Raw);
    return result;
}
inline void set_window_icon(winrt::Microsoft::UI::Xaml::Window const& window,fs::path const& path) noexcept {
    try{if(window&&fs::exists(path))window.AppWindow().SetIcon(path.wstring());}catch(...){}
}
}
