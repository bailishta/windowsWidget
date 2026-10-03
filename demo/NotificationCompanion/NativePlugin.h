#pragma once
#include "../../sdk/WidgetSdk.h"

namespace companion {
// This bridge knows only the public SDK. All device/data/UI logic stays in the loaded mod.
// It is instantiated inside a dedicated PluginProcess, never inside the manager.
class NativePlugin {
    HMODULE module=nullptr;
    void* instance=nullptr;
    WidgetApi2 api{};
    WidgetHostApi2 host{};
    HWND content=nullptr;
    DWORD ui_thread=GetCurrentThreadId();
    std::string data,configuration;
    winrt::Microsoft::UI::Xaml::Controls::ContentControl body{nullptr};
    uint32_t prior_width=0,prior_height=0,prior_dpi=0,prior_dark=2;
    unsigned renders=0;
    void wire_buttons(winrt::Microsoft::UI::Xaml::DependencyObject const& node) {
        using namespace winrt::Microsoft::UI::Xaml;
        if(auto button=node.try_as<Controls::Button>()) {
            auto name=winrt::to_string(button.Name());
            if(!name.empty())button.Click([this,name](auto const&,auto const&){event(name.c_str());});
        }
        int count=Media::VisualTreeHelper::GetChildrenCount(node);
        for(int i=0;i<count;++i)wire_buttons(Media::VisualTreeHelper::GetChild(node,i));
    }
    static uint32_t __cdecl render(void* context,const char* value) noexcept {
        auto self=static_cast<NativePlugin*>(context);
        if(!value||GetCurrentThreadId()!=self->ui_thread)return 0;
        try {
            if(strnlen_s(value,128*1024)>=128*1024)return 0;
            auto tree=winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(winrt::to_hstring(value));
            auto element=tree.as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
            self->body.Content(element);
            // Templates (for example Button) are materialized when Loaded runs.
            element.Loaded([self](auto const& sender,auto const&){self->wire_buttons(sender.template as<winrt::Microsoft::UI::Xaml::DependencyObject>());});
            ++self->renders;return 1;
        }catch(...){OutputDebugStringW(L"Native widget XAML render failed\n");return 0;}
    }
    static void __cdecl logging(void*,uint32_t,const char* message) noexcept {
        try{if(message)OutputDebugStringW(winrt::to_hstring(message).c_str());}catch(...){}
    }
    static void __cdecl changed(void* context,const char* json) noexcept {
        auto self=static_cast<NativePlugin*>(context);
        try{auto value=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(json));write_json_atomic(fs::path(std::wstring(winrt::to_hstring(self->data)))/L"configuration.json",value);}catch(...){}
    }
public:
    NativePlugin()=default;
    unsigned render_count() const {return renders;}
    NativePlugin(NativePlugin const&)=delete;
    ~NativePlugin(){stop();}
    void start(PluginDefinition const& definition,fs::path const& directory,HWND parent,
        winrt::Microsoft::UI::Xaml::Controls::ContentControl const& target,bool dark) {
        body=target;data=winrt::to_string(directory.wstring());
        auto settings=directory/L"configuration.json";
        configuration=fs::exists(settings)?read_plugin_file(settings):winrt::to_string(definition.document.GetNamedObject(L"configuration",winrt::Windows::Data::Json::JsonObject()).Stringify());
        auto entry=native_plugin_entry(definition.manifest,std::wstring(definition.document.GetNamedString(L"entry")));
        module=LoadLibraryExW(entry.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);winrt::check_bool(module!=nullptr);
        auto get=reinterpret_cast<WidgetGetApiFn>(GetProcAddress(module,"WidgetGetApi"));
        if(!get)throw std::runtime_error("组件缺少 WidgetGetApi 导出");
        api.size=sizeof(api);api.version=WIDGET_ABI_VERSION;
        winrt::check_hresult(get(WIDGET_ABI_VERSION,reinterpret_cast<WidgetApi*>(&api)));
        if(api.version!=WIDGET_ABI_VERSION||api.size<sizeof(WidgetApi)||api.size>sizeof(api)||!api.create||!api.destroy)throw std::runtime_error("组件 SDK 接口不兼容");
        if(api.size<sizeof(WidgetApi2)){api.capabilities=0;api.ui_event=nullptr;}
        host={sizeof(host),WIDGET_ABI_VERSION,this,logging,changed,data.c_str(),render};
        WidgetCreateInfo info{sizeof(info),parent,GetDpiForWindow(parent),dark?1u:0u,configuration.c_str(),reinterpret_cast<WidgetHostApi*>(&host)};
        winrt::check_hresult(api.create(&info,&instance,&content));
        if(!instance)throw std::runtime_error("组件没有返回实例");
        if(!(api.capabilities&WIDGET_CAPABILITY_XAML)) {
            DWORD owner=0;GetWindowThreadProcessId(content,&owner);
            if(!content||owner!=GetCurrentProcessId()||GetParent(content)!=parent)throw std::runtime_error("原生内容必须是组件进程内的子窗口");
            ShowWindow(content,SW_SHOWNOACTIVATE);
        }
    }
    void event(char const* name) {if(instance&&api.ui_event)api.ui_event(instance,name);}
    void update(HWND parent,bool dark) {
        if(!instance)return;RECT area{};GetClientRect(parent,&area);UINT dpi=GetDpiForWindow(parent);
        auto left=MulDiv(16,dpi,96),top=MulDiv(56,dpi,96);
        auto width=uint32_t(std::max(1L,area.right-left*2)),height=uint32_t(std::max(1L,area.bottom-top-left));
        if(dpi!=prior_dpi||width!=prior_width||height!=prior_height){prior_width=width;prior_height=height;prior_dpi=dpi;if(api.layout)api.layout(instance,width,height,dpi);if(content)SetWindowPos(content,HWND_TOP,left,top,width,height,SWP_NOACTIVATE);}
        if(prior_dark!=uint32_t(dark)){prior_dark=dark;if(api.theme)api.theme(instance,dark?1:0);}
    }
    void stop() noexcept {
        if(instance){auto value=instance;instance=nullptr;api.destroy(value);}
        if(body)body.Content(nullptr);
        if(module){FreeLibrary(module);module=nullptr;}
    }
};
}
