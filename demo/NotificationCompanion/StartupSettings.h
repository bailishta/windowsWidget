#pragma once
#include <windows.h>
#include <string_view>
#include <winrt/base.h>
#include "PluginCatalog.h"

namespace companion {
inline bool manager_on_launch(bool logon, bool background, bool testing, bool silent) {
    return !testing && !background && (!logon || !silent);
}
inline bool reopen_existing_manager(bool logon, bool background) { return !logon && !background; }

// Quote a Windows argument, including trailing backslashes. No shell is used.
inline std::wstring startup_argument(std::wstring_view value) {
    std::wstring result=L"\"";size_t slashes=0;
    for(auto ch:value) {
        if(ch==L'\\') {++slashes;continue;}
        result.append(ch==L'"'?slashes*2+1:slashes,L'\\');slashes=0;result+=ch;
    }
    result.append(slashes*2,L'\\');result+=L'"';return result;
}
inline std::wstring startup_command(fs::path const& executable, fs::path const& custom_data={}, bool english=false) {
    if(!executable.is_absolute()) throw winrt::hresult_invalid_argument(L"启动程序必须使用完整路径。");
    auto command=startup_argument(executable.wstring())+L" --autostart";
    if(!custom_data.empty())command+=L" --data-dir "+startup_argument(custom_data.wstring());
    if(english)command+=L" --en";
    if(command.size()>260)throw winrt::hresult_invalid_argument(L"程序或数据目录路径过长，无法设置开机自启动。");
    return command;
}

class StartupRegistration {
    struct Key {HKEY value=nullptr;~Key(){if(value)RegCloseKey(value);}};
    HKEY hive;std::wstring path;DWORD creation;
    static void checked(LSTATUS status){if(status!=ERROR_SUCCESS)winrt::throw_hresult(HRESULT_FROM_WIN32(status));}
public:
    static constexpr wchar_t value_name[]=L"WindowsWidget.Companion";
    // Test callers use a separate volatile key, never the user's real Run key.
    explicit StartupRegistration(HKEY root=HKEY_CURRENT_USER,
        std::wstring subkey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        DWORD create_option=REG_OPTION_NON_VOLATILE):hive(root),path(std::move(subkey)),creation(create_option){}
    std::wstring command() const {
        Key key;auto result=RegOpenKeyExW(hive,path.c_str(),0,KEY_QUERY_VALUE,&key.value);
        if(result==ERROR_FILE_NOT_FOUND)return {};checked(result);
        DWORD size=0;result=RegGetValueW(key.value,nullptr,value_name,RRF_RT_REG_SZ,nullptr,nullptr,&size);
        if(result==ERROR_FILE_NOT_FOUND)return {};checked(result);
        if(size>65536||size%sizeof(wchar_t))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        std::wstring value(size/sizeof(wchar_t),L'\0');
        checked(RegGetValueW(key.value,nullptr,value_name,RRF_RT_REG_SZ,nullptr,value.data(),&size));
        while(!value.empty()&&value.back()==L'\0')value.pop_back();return value;
    }
    bool enabled() const {return !command().empty();}
    void set(bool enabled,std::wstring const& command={}) const {
        if(enabled) {
            if(command.empty()||command.size()>260)throw winrt::hresult_invalid_argument();
            Key key;checked(RegCreateKeyExW(hive,path.c_str(),0,nullptr,creation,KEY_SET_VALUE,nullptr,&key.value,nullptr));
            checked(RegSetValueExW(key.value,value_name,0,REG_SZ,reinterpret_cast<BYTE const*>(command.c_str()),DWORD((command.size()+1)*sizeof(wchar_t))));
        } else {
            Key key;auto result=RegOpenKeyExW(hive,path.c_str(),0,KEY_SET_VALUE,&key.value);
            if(result==ERROR_FILE_NOT_FOUND)return;checked(result);
            result=RegDeleteValueW(key.value,value_name);if(result!=ERROR_FILE_NOT_FOUND)checked(result);
        }
    }
};

class StartupPreferences {
    fs::path file;JsonObject document{nullptr};
public:
    bool silent=true,read_only=false;
    void load(fs::path const& root) {
        file=root/L"startup.json";silent=true;read_only=false;document=JsonObject();
        try {
            if(!fs::exists(file))return;
            auto loaded=JsonObject::Parse(winrt::to_hstring(read_plugin_file(file)));
            if(loaded.GetNamedNumber(L"schema",0)!=1)throw winrt::hresult_invalid_argument();
            silent=loaded.GetNamedBoolean(L"silent",true);document=loaded;
        } catch(...) {read_only=true;}
    }
    void set_silent(bool value) {
        if(read_only||file.empty())throw winrt::hresult_error(E_ACCESSDENIED,L"启动设置损坏或版本不兼容，原文件已保留。");
        auto next=JsonObject::Parse(document.Stringify());
        next.SetNamedValue(L"schema",JsonValue::CreateNumberValue(1));
        next.SetNamedValue(L"silent",JsonValue::CreateBooleanValue(value));
        write_json_atomic(file,next);document=next;silent=value;
    }
};
} // namespace companion
