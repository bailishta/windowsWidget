#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include "../../sdk/WidgetSdk.h"

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Data::Json;
using namespace std::chrono_literals;
namespace {
struct Device {std::wstring id,name;int battery=-1;};
std::string escape(std::wstring const& value) {
    std::string result;
    for(char c:to_string(value))switch(c){case '&':result+="&amp;";break;case '<':result+="&lt;";break;case '>':result+="&gt;";break;case '"':result+="&quot;";break;case '\'':result+="&apos;";break;default:result+=c;}
    return result;
}
template<typename Operation> auto finish(Operation const& op,std::atomic_bool const& stop) {
    auto deadline=GetTickCount64()+3000;
    while(op.wait_for(50ms)==Windows::Foundation::AsyncStatus::Started) {
        if(stop||GetTickCount64()>deadline){op.Cancel();throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));}
    }
    return op.get();
}
std::wstring property_string(DeviceInformation const& info,wchar_t const* key) {
    try{auto object=info.Properties().TryLookup(key);if(auto p=object.try_as<Windows::Foundation::IPropertyValue>()) {
        if(p.Type()==Windows::Foundation::PropertyType::Guid)return std::wstring(to_hstring(p.GetGuid()));
        if(p.Type()==Windows::Foundation::PropertyType::String)return std::wstring(p.GetString());
    }}catch(...){}return {};
}
std::wstring normalized(std::wstring value){std::transform(value.begin(),value.end(),value.begin(),towlower);return value;}
// Windows driver battery property, queried as an optional compatibility path.
// GATT Battery Service remains the documented BLE fallback. Never infer a percentage.
constexpr DEVPROPKEY battery_key={{0x104ea319,0x6ee2,0x4701,{0xbd,0x47,0x8d,0xdb,0xf4,0x25,0xbb,0xe5}},2};
struct DriverBattery {std::wstring container,instance;int percent;};
std::vector<DriverBattery> driver_batteries() {
    std::vector<DriverBattery> result;
    HDEVINFO set=SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_ALLCLASSES|DIGCF_PRESENT);
    if(set==INVALID_HANDLE_VALUE)return result;
    for(DWORD i=0;;++i) {
        SP_DEVINFO_DATA dev{sizeof(dev)};if(!SetupDiEnumDeviceInfo(set,i,&dev))break;
        wchar_t identity[4096];if(!SetupDiGetDeviceInstanceIdW(set,&dev,identity,4096,nullptr))continue;
        std::wstring id=normalized(identity);if(!id.starts_with(L"bthenum\\")&&!id.starts_with(L"bthledevice\\"))continue;
        DEVPROPTYPE type=0;BYTE value=255;
        if(!SetupDiGetDevicePropertyW(set,&dev,&battery_key,&type,&value,sizeof(value),nullptr,0)||type!=DEVPROP_TYPE_BYTE||value>100)continue;
        GUID container{};std::wstring group;
        if(SetupDiGetDevicePropertyW(set,&dev,&DEVPKEY_Device_ContainerId,&type,reinterpret_cast<BYTE*>(&container),sizeof(container),nullptr,0)&&type==DEVPROP_TYPE_GUID)group=normalized(std::wstring(to_hstring(container)));
        result.push_back({group,id,value});
    }
    SetupDiDestroyDeviceInfoList(set);return result;
}
struct Plugin {
    WidgetHostApi2 const* host=nullptr;
    HWND timer=nullptr;
    std::thread worker;
    std::atomic_bool stopping=false;
    std::mutex lock;
    std::condition_variable wake;
    std::array<Device,4> slots{};
    unsigned connected=0,period=30;
    bool dirty=true,refresh_requested=false,scanning=true;
    std::wstring status=L"正在检测已连接的设备…";
    std::filesystem::path directory;

    static LRESULT CALLBACK proc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
        auto self=reinterpret_cast<Plugin*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Plugin*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(message==WM_TIMER&&self){self->render();return 0;}
        return DefWindowProcW(hwnd,message,w,l);
    }
    void render() {
        std::array<Device,4> rows;unsigned count;std::wstring message;
        {std::lock_guard guard(lock);if(!dirty)return;dirty=false;rows=slots;count=connected;message=status;}
        std::string x="<StackPanel xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' Spacing='8'>";
        for(size_t i=0;i<rows.size();++i) {
            auto const& row=rows[i];bool empty=row.id.empty();
            x+="<Border CornerRadius='6' Padding='10,8' Background='{ThemeResource CardBackgroundFillColorDefaultBrush}' BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}' BorderThickness='1'><StackPanel Spacing='4'>";
            x+="<TextBlock Text='"+escape(empty?L"设备位 "+std::to_wstring(i+1)+L" · 未连接设备":row.name)+"' FontSize='13' TextTrimming='CharacterEllipsis'/>";
            x+="<Grid><TextBlock Text='"+escape(empty?L"等待连接":row.battery<0?L"设备未提供电量":L"已连接")+"' FontSize='11' Opacity='.65'/><TextBlock HorizontalAlignment='Right' Text='"+(row.battery<0?"—":std::to_string(row.battery)+"%")+"' FontSize='12'/></Grid>";
            x+="<ProgressBar Maximum='100' Value='"+std::to_string(std::max(0,row.battery))+"' Height='3' Opacity='"+(row.battery<0?".3":"1")+"'/></StackPanel></Border>";
        }
        if(count>4)message+=L" · 另有 "+std::to_wstring(count-4)+L" 个已连接设备";
        x+="<TextBlock Text='"+escape(message)+"' FontSize='11' Opacity='.65' TextWrapping='Wrap'/><Button x:Name='refresh' Content='刷新设备' HorizontalAlignment='Stretch'/></StackPanel>";
        if(!host->ui_render(host->context,x.c_str()))host->log(host->context,2,"Bluetooth battery UI could not be rendered");
    }
    std::vector<Device> scan() {
        auto driver=driver_batteries();std::vector<Device> result;std::vector<std::wstring> keys;
        auto props=single_threaded_vector<hstring>({L"System.Devices.Aep.ContainerId",L"System.Devices.ContainerId",L"System.Devices.Aep.DeviceAddress",L"System.Devices.BatteryLife"});
        for(bool le:{true,false}) {
            if(stopping)break;
            auto selector=le?BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected):BluetoothDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected);
            auto devices=finish(DeviceInformation::FindAllAsync(selector,props,DeviceInformationKind::AssociationEndpoint),stopping);
            for(auto const& info:devices) {
                if(stopping)break;
                auto container=normalized(property_string(info,L"System.Devices.Aep.ContainerId"));if(container.empty())container=normalized(property_string(info,L"System.Devices.ContainerId"));
                auto address=normalized(property_string(info,L"System.Devices.Aep.DeviceAddress"));address.erase(std::remove(address.begin(),address.end(),L':'),address.end());
                auto key=container.empty()?(address.empty()?std::wstring(info.Id()):address):container;
                if(std::find(keys.begin(),keys.end(),key)!=keys.end())continue;
                Device row{key,std::wstring(info.Name()),-1};if(row.name.empty())row.name=L"蓝牙设备";
                try{auto value=info.Properties().TryLookup(L"System.Devices.BatteryLife");if(value){auto percent=value.as<Windows::Foundation::IPropertyValue>().GetUInt8();if(percent<=100)row.battery=percent;}}catch(...){}
                for(auto const& reading:driver)if((!container.empty()&&reading.container==container)||(!address.empty()&&reading.instance.find(address)!=std::wstring::npos)){row.battery=reading.percent;break;}
                if(le&&row.battery<0&&result.size()<4)try {
                    auto device=finish(BluetoothLEDevice::FromIdAsync(info.Id()),stopping);
                    if(device&&device.ConnectionStatus()==BluetoothConnectionStatus::Connected) {
                        auto services=finish(device.GetGattServicesForUuidAsync(GattServiceUuids::Battery(),BluetoothCacheMode::Cached),stopping);
                        if(services.Status()==GattCommunicationStatus::Success)for(auto service:services.Services()) {
                            auto values=finish(service.GetCharacteristicsForUuidAsync(GattCharacteristicUuids::BatteryLevel(),BluetoothCacheMode::Cached),stopping);
                            if(values.Status()==GattCommunicationStatus::Success)for(auto characteristic:values.Characteristics()) {
                                auto reading=finish(characteristic.ReadValueAsync(BluetoothCacheMode::Uncached),stopping);
                                if(reading.Status()==GattCommunicationStatus::Success&&reading.Value().Length()) {
                                    auto reader=Windows::Storage::Streams::DataReader::FromBuffer(reading.Value());auto percent=reader.ReadByte();if(percent<=100)row.battery=percent;
                                }
                            }service.Close();if(row.battery>=0)break;
                        }
                    }if(device)device.Close();
                }catch(...){} // A connected device without accessible Battery Service still occupies its slot.
                keys.push_back(key);result.push_back(std::move(row));
            }
        }
        return result;
    }
    void run() noexcept {
        try {
            init_apartment(apartment_type::multi_threaded);
            while(!stopping) {
                std::vector<Device> devices;std::wstring error;
                try{devices=scan();}catch(hresult_error const& e){error=L"检测失败："+std::wstring(e.message());}catch(...){error=L"设备检测暂不可用";}
                if(stopping)break;
                std::sort(devices.begin(),devices.end(),[](auto const& a,auto const& b){return a.id<b.id;});
                {
                    std::lock_guard guard(lock);connected=unsigned(devices.size());
                    // Keep existing devices in their slots; remove stale disconnected devices.
                    for(auto& slot:slots){auto found=std::find_if(devices.begin(),devices.end(),[&](auto const& d){return d.id==slot.id;});if(found==devices.end())slot={};else {slot=*found;devices.erase(found);}}
                    for(auto const& d:devices){auto empty=std::find_if(slots.begin(),slots.end(),[](auto const& s){return s.id.empty();});if(empty==slots.end())break;*empty=d;}
                    status=error.empty()?(connected?L"已连接 "+std::to_wstring(connected)+L" 个设备 · 每 "+std::to_wstring(period)+L" 秒更新":L"没有已连接的蓝牙设备 · 保留四个设备位"):error;
                    dirty=true;scanning=false;
                    // A local diagnostic snapshot helps verify actual readings, never fabricated demo data.
                    JsonObject snapshot;JsonArray entries;
                    for(auto const& row:slots){JsonObject j;j.SetNamedValue(L"name",JsonValue::CreateStringValue(row.name));j.SetNamedValue(L"connected",JsonValue::CreateBooleanValue(!row.id.empty()));j.SetNamedValue(L"battery",row.battery<0?JsonValue::CreateNullValue():JsonValue::CreateNumberValue(row.battery));entries.Append(j);}
                    snapshot.SetNamedValue(L"slots",entries);snapshot.SetNamedValue(L"connected_count",JsonValue::CreateNumberValue(connected));snapshot.SetNamedValue(L"status",JsonValue::CreateStringValue(status));
                    {std::ofstream out(directory/L"last-reading.json",std::ios::binary|std::ios::trunc);out<<to_string(snapshot.Stringify());}
                }
                std::unique_lock guard(lock);wake.wait_for(guard,std::chrono::seconds(period),[&]{return stopping||refresh_requested;});refresh_requested=false;
            }
            uninit_apartment();
        }catch(...){std::lock_guard guard(lock);status=L"蓝牙服务不可用";dirty=true;}
    }
};
HRESULT __cdecl create(WidgetCreateInfo const* info,void** instance,HWND* content) {
    try {
        auto host=widget_host_v2(info->host);if(!host||!host->ui_render)return E_NOINTERFACE;
        auto value=std::make_unique<Plugin>();value->host=host;value->directory=std::filesystem::path(std::wstring(to_hstring(host->data_directory_utf8)));std::filesystem::create_directories(value->directory);
        try{auto config=JsonObject::Parse(to_hstring(info->configuration_utf8));value->period=unsigned(std::clamp(config.GetNamedNumber(L"refresh_seconds",30),10.0,600.0));}catch(...){}
        HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&Plugin::proc),&module);
        WNDCLASSW wc{};wc.hInstance=module;wc.lpfnWndProc=Plugin::proc;wc.lpszClassName=L"WindowsWidget.Mod.BluetoothBattery";RegisterClassW(&wc);
        value->timer=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,module,value.get());if(!value->timer)return HRESULT_FROM_WIN32(GetLastError());
        SetTimer(value->timer,1,250,nullptr);value->render();auto ptr=value.get();value->worker=std::thread([ptr]{ptr->run();});*content=nullptr;*instance=value.release();return S_OK;
    }catch(hresult_error const& e){return e.code();}catch(...){return E_FAIL;}
}
void __cdecl destroy(void* instance) {
    auto self=static_cast<Plugin*>(instance);KillTimer(self->timer,1);self->stopping=true;self->wake.notify_all();if(self->worker.joinable())self->worker.join();DestroyWindow(self->timer);delete self;
}
HRESULT __cdecl configure(void*,char const*){return S_OK;}
void __cdecl layout(void*,uint32_t,uint32_t,uint32_t){}
void __cdecl theme(void*,uint32_t){} // XAML uses inherited ThemeResource brushes.
void __cdecl ui_event(void* instance,char const* name) {
    if(std::string_view(name?name:"")!="refresh"&&std::string_view(name?name:"")!="settings")return;
    auto self=static_cast<Plugin*>(instance);{std::lock_guard guard(self->lock);self->refresh_requested=true;self->status=L"正在刷新设备…";self->dirty=true;}self->wake.notify_all();self->render();
}
}
WIDGET_EXPORT HRESULT __cdecl WidgetGetApi(uint32_t version,WidgetApi* api) {
    if(version!=WIDGET_ABI_VERSION||!api||api->size<sizeof(WidgetApi2))return E_NOINTERFACE;
    *reinterpret_cast<WidgetApi2*>(api)={sizeof(WidgetApi2),WIDGET_ABI_VERSION,create,destroy,configure,layout,theme,WIDGET_CAPABILITY_XAML,ui_event};return S_OK;
}
