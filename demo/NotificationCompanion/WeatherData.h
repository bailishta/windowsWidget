#pragma once
#include <windows.h>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include "FeatureSettings.h"
#pragma comment(lib,"winhttp.lib")
namespace companion {
struct WeatherReading {
    bool valid=false,has_feels=false,has_humidity=false,has_wind=false;
    double temperature=0,feels=0,humidity=0,wind=0;
    int code=-1;uint64_t generation=0;
    std::wstring city,measured,requested_city,description;
};
inline winrt::Windows::Data::Json::IJsonValue weather_field(JsonObject const& root,std::wstring const& path) {
    using namespace winrt::Windows::Data::Json;IJsonValue value=root;size_t pos=0;
    try {
        if(path.empty())return nullptr;
        while(pos<path.size()) {
            if(path[pos]==L'[') {
                auto end=path.find(L']',pos);if(end==path.npos||end==pos+1)return nullptr;
                auto index=path.substr(pos+1,end-pos-1);if(index.find_first_not_of(L"0123456789")!=index.npos)return nullptr;
                auto n=std::stoull(index);auto array=value.GetArray();if(n>=array.Size())return nullptr;value=array.GetAt(unsigned(n));pos=end+1;
            } else {
                auto end=path.find_first_of(L".[",pos);auto key=path.substr(pos,end==path.npos?path.size()-pos:end-pos);if(key.empty())return nullptr;
                value=value.GetObject().GetNamedValue(key);pos=end==path.npos?path.size():end;
            }
            if(pos<path.size()&&path[pos]==L'.'){++pos;if(pos==path.size())return nullptr;}
        }
        return value;
    }catch(...){return nullptr;}
}
inline bool weather_number(JsonObject const& root,std::wstring const& path,double& number) {
    using namespace winrt::Windows::Data::Json;auto value=weather_field(root,path);if(!value)return false;
    try {
        if(value.ValueType()==JsonValueType::Number)number=value.GetNumber();
        else if(value.ValueType()==JsonValueType::String){auto str=trim(std::wstring(value.GetString()));size_t end=0;number=std::stod(str,&end);if(end!=str.size())return false;}
        else return false;
        return std::isfinite(number);
    }catch(...){return false;}
}
inline std::wstring weather_string(JsonObject const& root,std::wstring const& path) {
    auto value=weather_field(root,path);return value&&value.ValueType()==winrt::Windows::Data::Json::JsonValueType::String?std::wstring(value.GetString()).substr(0,100):L"";
}
inline WeatherReading parse_weather(JsonObject const& root,WeatherConfig const& config,std::wstring const& city) {
    WeatherReading r;r.requested_city=city;r.city=weather_string(root,config.city);if(r.city.empty())r.city=city;
    if(!weather_number(root,config.temperature,r.temperature))throw std::runtime_error("temperature");
    r.has_feels=weather_number(root,config.feels,r.feels);r.has_humidity=weather_number(root,config.humidity,r.humidity);r.has_wind=weather_number(root,config.wind,r.wind);
    if(config.fahrenheit){r.temperature=(r.temperature-32)*5/9;if(r.has_feels)r.feels=(r.feels-32)*5/9;}
    if(std::abs(r.temperature)>200)throw std::runtime_error("temperature range");
    if(r.has_humidity&&(r.humidity<0||r.humidity>100))r.has_humidity=false;
    if(r.has_wind){if(config.wind_unit==1)r.wind*=3.6;else if(config.wind_unit==2)r.wind*=1.609344;if(r.wind<0||r.wind>1000)r.has_wind=false;}
    double code=0;if(weather_number(root,config.code,code)&&code>=0&&code<=99&&code==std::floor(code))r.code=int(code);
    r.description=weather_string(root,config.description);r.measured=weather_string(root,config.time);r.valid=true;return r;
}
inline std::wstring weather_encode(std::wstring const& value) {
    auto utf8=winrt::to_string(value);std::wstring result;constexpr wchar_t hex[]=L"0123456789ABCDEF";
    for(unsigned char c:utf8)if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')result+=wchar_t(c);else{result+=L'%';result+=hex[c>>4];result+=hex[c&15];}return result;
}
inline std::wstring weather_url(WeatherConfig const& config,std::wstring const& city) {
    auto url=config.url;auto replace=[&](std::wstring const& token,std::wstring const& value){size_t at=0;while((at=url.find(token,at))!=url.npos){url.replace(at,token.size(),value);at+=value.size();}};
    replace(L"{city}",weather_encode(city));replace(L"{lat}",std::to_wstring(config.latitude));replace(L"{lon}",std::to_wstring(config.longitude));return url;
}
class WeatherData {
    struct InternetHandle {HINTERNET value=nullptr;~InternetHandle(){if(value)WinHttpCloseHandle(value);}};
    struct State {std::mutex mutex;std::atomic_bool busy=false;bool ready=false;WeatherReading reading;std::wstring error;};
    std::shared_ptr<State> state=std::make_shared<State>();
    static std::wstring get(std::wstring const& url,std::wstring const& header=L"",std::wstring const& secret=L"") {
        URL_COMPONENTS parts{sizeof(parts)};parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=parts.dwUserNameLength=parts.dwPasswordLength=DWORD(-1);
        if(!WinHttpCrackUrl(url.c_str(),DWORD(url.size()),0,&parts)||parts.nScheme!=INTERNET_SCHEME_HTTPS||parts.dwUserNameLength||parts.dwPasswordLength)throw std::runtime_error("url");
        std::wstring host(parts.lpszHostName,parts.dwHostNameLength),path(parts.lpszUrlPath,parts.dwUrlPathLength);if(path.empty())path=L"/";if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
        InternetHandle session{WinHttpOpen(L"WindowsWidget/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0)};if(!session.value)throw std::runtime_error("session");
        WinHttpSetTimeouts(session.value,4000,4000,4000,4000);
        InternetHandle connection{WinHttpConnect(session.value,host.c_str(),parts.nPort,0)};
        InternetHandle request{connection.value?WinHttpOpenRequest(connection.value,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr};
        if(!request.value)throw std::runtime_error("request");DWORD disabled=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION;
        if(!WinHttpSetOption(request.value,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled)))throw std::runtime_error("options");
        std::wstring headers=secret.empty()?L"":header+L": "+secret+L"\r\n";
        if(!WinHttpSendRequest(request.value,headers.empty()?WINHTTP_NO_ADDITIONAL_HEADERS:headers.c_str(),DWORD(headers.size()),nullptr,0,0,0)||!WinHttpReceiveResponse(request.value,nullptr))throw std::runtime_error("request");
        DWORD status=0,size=sizeof(status);if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr)||status!=200)throw std::runtime_error("http");
        std::string body;auto deadline=GetTickCount64()+15000;
        for(;;){char buffer[8192];DWORD received=0;if(GetTickCount64()>deadline||!WinHttpReadData(request.value,buffer,sizeof(buffer),&received))throw std::runtime_error("read");if(!received)break;body.append(buffer,received);if(body.size()>256*1024)throw std::runtime_error("size");}
        return std::wstring(winrt::to_hstring(body));
    }
public:
    bool busy() const{return state->busy;}
    void refresh(std::wstring city,WeatherConfig config,uint64_t generation) {
        validate_weather_config(config);if(state->busy.exchange(true))return;auto shared=state;
        std::thread([shared,city=std::move(city),config=std::move(config),generation]{
            WeatherReading reading;reading.requested_city=city;reading.generation=generation;std::wstring error;bool initialized=false;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);initialized=true;
                if(config.custom)reading=parse_weather(JsonObject::Parse(get(weather_url(config,city),config.header_name,config.header_value)),config,city);
                else {
                    auto geo=JsonObject::Parse(get(L"https://geocoding-api.open-meteo.com/v1/search?name="+weather_encode(city)+L"&count=1&language=zh&format=json"));
                    auto matches=geo.GetNamedArray(L"results");if(!matches.Size())throw std::runtime_error("city");auto place=matches.GetObjectAt(0);
                    auto response=JsonObject::Parse(get(L"https://api.open-meteo.com/v1/forecast?latitude="+std::to_wstring(place.GetNamedNumber(L"latitude"))+L"&longitude="+std::to_wstring(place.GetNamedNumber(L"longitude"))+L"&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m&timezone=auto&forecast_days=1"));
                    WeatherConfig fields;fields.temperature=L"current.temperature_2m";fields.feels=L"current.apparent_temperature";fields.humidity=L"current.relative_humidity_2m";fields.wind=L"current.wind_speed_10m";fields.code=L"current.weather_code";fields.time=L"current.time";fields.city=L"";
                    reading=parse_weather(response,fields,city);reading.city=place.GetNamedString(L"name");
                }
            }catch(...){error=L"unavailable";}
            reading.requested_city=city;reading.generation=generation;if(initialized)winrt::uninit_apartment();
            {std::lock_guard lock(shared->mutex);shared->reading=std::move(reading);shared->error=std::move(error);shared->ready=true;}shared->busy=false;
        }).detach();
    }
    bool consume(WeatherReading& reading,std::wstring& error){std::lock_guard lock(state->mutex);if(!state->ready)return false;reading=state->reading;error=state->error;state->ready=false;return true;}
};
}
