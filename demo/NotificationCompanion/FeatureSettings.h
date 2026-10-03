#pragma once
#include <wincrypt.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <vector>
#include "PluginCatalog.h"
#pragma comment(lib,"crypt32.lib")

namespace companion {
inline std::wstring trim(std::wstring value) {
    auto first=value.find_first_not_of(L" \t\r\n"),last=value.find_last_not_of(L" \t\r\n");
    return first==value.npos?L"":value.substr(first,last-first+1);
}
struct QuickEntry {std::wstring name,target,arguments;};
inline std::array<QuickEntry,3> default_quick_entries() {
    return {{{L"文件资源管理器",L"explorer.exe",L""},{L"Windows 设置",L"ms-settings:",L""},{L"截图工具",L"ms-screenclip:",L""}}};
}
inline bool valid_quick_entry(QuickEntry const& entry) {
    return entry.name.size()<=40&&entry.target.size()<=2048&&entry.arguments.size()<=2048&&
        (entry.target.empty()||!entry.name.empty())&&entry.target.find_first_of(L"\r\n\"\0",0,4)==entry.target.npos&&entry.arguments.find_first_of(L"\r\n\0",0,3)==entry.arguments.npos;
}
inline std::wstring secret_encrypt(std::wstring const& value) {
    if(value.empty())return L"";
    auto utf8=winrt::to_string(value);DATA_BLOB plain{DWORD(utf8.size()),reinterpret_cast<BYTE*>(utf8.data())},encrypted{};
    winrt::check_bool(CryptProtectData(&plain,L"WindowsWidget weather API",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted));
    DWORD length=0;CryptBinaryToStringW(encrypted.pbData,encrypted.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&length);
    std::wstring encoded(length,L'\0');BOOL success=CryptBinaryToStringW(encrypted.pbData,encrypted.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,encoded.data(),&length);LocalFree(encrypted.pbData);winrt::check_bool(success);encoded.resize(length);return encoded;
}
inline std::wstring secret_decrypt(std::wstring const& encoded) {
    if(encoded.empty())return L"";DWORD size=0;
    winrt::check_bool(CryptStringToBinaryW(encoded.c_str(),DWORD(encoded.size()),CRYPT_STRING_BASE64,nullptr,&size,nullptr,nullptr));
    std::vector<BYTE> bytes(size);winrt::check_bool(CryptStringToBinaryW(encoded.c_str(),DWORD(encoded.size()),CRYPT_STRING_BASE64,bytes.data(),&size,nullptr,nullptr));
    DATA_BLOB input{size,bytes.data()},output{};winrt::check_bool(CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output));
    std::string value(reinterpret_cast<char*>(output.pbData),output.cbData);LocalFree(output.pbData);return std::wstring(winrt::to_hstring(value));
}
struct WeatherConfig {
    bool custom=false;
    unsigned interval_minutes=30;
    std::wstring url,header_name=L"Authorization",header_value;
    std::wstring temperature=L"temperature",feels=L"feels_like",humidity=L"humidity",wind=L"wind",code=L"code",description=L"description",time=L"time",city=L"city";
    bool fahrenheit=false;
    unsigned wind_unit=0; // km/h, m/s, mph
    double latitude=0,longitude=0;
};
inline void validate_weather_config(WeatherConfig const& c) {
    if(c.interval_minutes<5||c.interval_minutes>1440)throw std::runtime_error("更新间隔必须为 5–1440 分钟");
    if(!std::isfinite(c.latitude)||!std::isfinite(c.longitude)||std::abs(c.latitude)>90||std::abs(c.longitude)>180)throw std::runtime_error("经纬度超出范围");
    if(c.wind_unit>2)throw std::runtime_error("风速单位无效");
    if(!c.custom)return;
    if(c.url.size()>4096||!c.url.starts_with(L"https://")||c.url.find_first_of(L"\r\n\t \"#")!=c.url.npos)throw std::runtime_error("API 地址需要完整 HTTPS URL（可包含 {city}、{lat}、{lon}）");
    auto host_end=c.url.find_first_of(L"/?",8);auto host=c.url.substr(8,host_end==c.url.npos?c.url.size()-8:host_end-8);
    if(host.empty()||host.find(L'@')!=host.npos)throw std::runtime_error("API 地址不能包含用户名或密码");
    if(c.temperature.empty())throw std::runtime_error("请填写温度字段路径");
    for(auto const* path:{&c.temperature,&c.feels,&c.humidity,&c.wind,&c.code,&c.description,&c.time,&c.city})if(path->size()>200)throw std::runtime_error("字段路径最多 200 字符");
    if(c.header_value.size()>2048||c.header_value.find_first_of(L"\r\n")!=c.header_value.npos)throw std::runtime_error("认证值不能包含换行，最多 2048 字符");
    if(!c.header_value.empty()&&(c.header_name.empty()||c.header_name.size()>80||!std::all_of(c.header_name.begin(),c.header_name.end(),[](wchar_t ch){return (ch>=L'a'&&ch<=L'z')||(ch>=L'A'&&ch<=L'Z')||(ch>=L'0'&&ch<=L'9')||ch==L'-';})))throw std::runtime_error("请输入有效的认证请求头名称");
}
inline JsonObject weather_config_json(WeatherConfig const& c) {
    JsonObject j;auto s=[&](wchar_t const* key,std::wstring const& value){j.SetNamedValue(key,JsonValue::CreateStringValue(value));};
    j.SetNamedValue(L"custom",JsonValue::CreateBooleanValue(c.custom));j.SetNamedValue(L"interval_minutes",JsonValue::CreateNumberValue(c.interval_minutes));
    s(L"url",c.url);s(L"header_name",c.header_name);s(L"header_secret",secret_encrypt(c.header_value));
    s(L"temperature",c.temperature);s(L"feels",c.feels);s(L"humidity",c.humidity);s(L"wind",c.wind);s(L"code",c.code);s(L"description",c.description);s(L"time",c.time);s(L"city",c.city);
    j.SetNamedValue(L"fahrenheit",JsonValue::CreateBooleanValue(c.fahrenheit));j.SetNamedValue(L"wind_unit",JsonValue::CreateNumberValue(c.wind_unit));
    j.SetNamedValue(L"latitude",JsonValue::CreateNumberValue(c.latitude));j.SetNamedValue(L"longitude",JsonValue::CreateNumberValue(c.longitude));return j;
}
inline WeatherConfig weather_config_from_json(JsonObject const& j) {
    WeatherConfig c;c.custom=j.GetNamedBoolean(L"custom",false);double minutes=j.GetNamedNumber(L"interval_minutes",30),unit=j.GetNamedNumber(L"wind_unit",0);
    if(!std::isfinite(minutes)||minutes<5||minutes>1440||minutes!=std::floor(minutes)||!std::isfinite(unit)||unit<0||unit>2||unit!=std::floor(unit))throw std::runtime_error("天气设置中的更新间隔或单位无效");
    c.interval_minutes=unsigned(minutes);c.wind_unit=unsigned(unit);
    c.url=j.GetNamedString(L"url",L"");c.header_name=j.GetNamedString(L"header_name",L"Authorization");c.header_value=secret_decrypt(std::wstring(j.GetNamedString(L"header_secret",L"")));
    c.temperature=j.GetNamedString(L"temperature",L"temperature");c.feels=j.GetNamedString(L"feels",L"feels_like");c.humidity=j.GetNamedString(L"humidity",L"humidity");c.wind=j.GetNamedString(L"wind",L"wind");
    c.code=j.GetNamedString(L"code",L"code");c.description=j.GetNamedString(L"description",L"description");c.time=j.GetNamedString(L"time",L"time");c.city=j.GetNamedString(L"city",L"city");
    c.fahrenheit=j.GetNamedBoolean(L"fahrenheit",false);c.latitude=j.GetNamedNumber(L"latitude",0);c.longitude=j.GetNamedNumber(L"longitude",0);validate_weather_config(c);return c;
}
inline bool weather_due(uint64_t now,uint64_t last,WeatherConfig const& c){return !last||now-last>=uint64_t(c.interval_minutes)*60000;}
}
