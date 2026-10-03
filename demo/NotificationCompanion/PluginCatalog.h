#pragma once
#include <winrt/Windows.Data.Json.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <string_view>

namespace companion {
namespace fs = std::filesystem;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonValue;
struct PluginDefinition {
    std::wstring id, name, description, kind, error;
    fs::path manifest;
    fs::path icon;
    JsonObject document;
    int height=240;
};
inline fs::path executable_plugin_entry(fs::path const& manifest, std::wstring const& value,bool process=false) {
    fs::path relative(value);
    if(value.empty()||relative.is_absolute()||relative.has_root_name()||relative.extension()!=(process?L".exe":L".dll")||value.find(L':')!=value.npos)
        throw std::runtime_error("组件 entry 必须是包内 DLL 或 EXE 的相对路径，且与 kind 对应");
    for(auto const& part:relative)if(part==L".."||part==L".")throw std::runtime_error("组件 entry 不能越出组件目录");
    auto folder=fs::weakly_canonical(manifest.parent_path()), entry=fs::weakly_canonical(folder/relative);
    auto remainder=entry.lexically_relative(folder);
    if(remainder.empty()||remainder.is_absolute()||*remainder.begin()==L".."||!fs::is_regular_file(entry))
        throw std::runtime_error("组件入口不存在或位于组件目录之外");
    return entry;
}
inline fs::path native_plugin_entry(fs::path const& manifest,std::wstring const& value) {return executable_plugin_entry(manifest,value);}
inline std::string read_plugin_file(fs::path const& file) {
    if(fs::file_size(file)>64*1024)throw std::runtime_error("组件描述文件不能超过 64 KB");
    std::ifstream stream(file,std::ios::binary);
    std::string value((std::istreambuf_iterator<char>(stream)),{});
    if(value.starts_with("\xef\xbb\xbf"))value.erase(0,3);
    return value;
}
inline fs::path plugin_icon_path(fs::path const& manifest,std::wstring const& value) {
    fs::path relative(value);
    if(value.empty()||relative.is_absolute()||relative.has_root_name()||relative.extension()!=L".png"||value.find(L':')!=value.npos)
        throw std::runtime_error("组件 icon 必须是包内 PNG 的相对路径");
    for(auto const& part:relative)if(part==L".."||part==L".")throw std::runtime_error("组件 icon 不能越出组件目录");
    auto folder=fs::weakly_canonical(manifest.parent_path()),image=fs::weakly_canonical(folder/relative);
    auto remainder=image.lexically_relative(folder);
    if(remainder.empty()||remainder.is_absolute()||*remainder.begin()==L".."||!fs::is_regular_file(image)||fs::file_size(image)>4*1024*1024)
        throw std::runtime_error("组件图标不存在、越界或超过 4 MiB");
    std::ifstream stream(image,std::ios::binary);char signature[8]{};stream.read(signature,8);
    if(std::string_view(signature,8)!=std::string_view("\x89PNG\r\n\x1a\n",8))throw std::runtime_error("组件图标必须是 PNG 文件");
    return image;
}
inline bool valid_plugin_id(std::wstring const& id) {
    auto stem=id.substr(0,id.find(L'.'));
    bool reserved=stem==L"con"||stem==L"prn"||stem==L"aux"||stem==L"nul"||
        (stem.size()==4&&(stem.starts_with(L"com")||stem.starts_with(L"lpt"))&&stem.back()>=L'1'&&stem.back()<=L'9');
    return !reserved && !id.empty() && id.size()<=64 && id.front()!=L'.' && id.back()!=L'.' &&
        std::all_of(id.begin(),id.end(),[](wchar_t c){return (c>=L'a'&&c<=L'z')||(c>=L'0'&&c<=L'9')||c==L'-'||c==L'.';});
}
inline bool safe_link(std::wstring const& link) {
    return link.size()<=2048 && link.starts_with(L"https://") && link.size()>8 &&
        link.find_first_of(L"\r\n\t \"")==link.npos;
}
inline PluginDefinition read_plugin(fs::path const& manifest) {
    PluginDefinition p;p.manifest=manifest;
    p.document=JsonObject::Parse(winrt::to_hstring(read_plugin_file(manifest)));
    auto const& j=p.document;
    if(j.GetNamedNumber(L"schema",0)!=1)throw std::runtime_error("组件描述文件 schema 必须为 1");
    p.id=j.GetNamedString(L"id");p.name=j.GetNamedString(L"name");p.kind=j.GetNamedString(L"kind");
    p.description=j.GetNamedString(L"description",L"");
    j.GetNamedBoolean(L"has_settings",true);
    if(j.HasKey(L"icon"))p.icon=plugin_icon_path(manifest,std::wstring(j.GetNamedString(L"icon")));
    if(!valid_plugin_id(p.id))throw std::runtime_error("id 仅允许小写字母、数字、点和横线，最多 64 字符");
    if(p.name.empty()||p.name.size()>80||p.description.size()>300||j.GetNamedString(L"settings_label",L"").size()>60)throw std::runtime_error("组件名称或描述长度不合法");
    double height=j.GetNamedNumber(L"height",240);
    if(!std::isfinite(height)||height<180||height>600)throw std::runtime_error("height 必须在 180 到 600 之间");
    p.height=int(height);
    bool custom_kind=p.kind==L"text"||p.kind==L"note"||p.kind==L"checklist"||p.kind==L"links"||p.kind==L"native"||p.kind==L"process";
    if(!custom_kind)throw std::runtime_error("不支持的组件 kind");
    if(j.HasKey(L"script")||j.HasKey(L"dll")||(p.kind!=L"native"&&p.kind!=L"process"&&j.HasKey(L"entry")))throw std::runtime_error("只有 native / process 组件可以声明 entry");
    if(p.kind==L"native"||p.kind==L"process") {
        if(j.GetNamedNumber(L"abi",0)!=1)throw std::runtime_error("原生组件 abi 必须为 1（支持扩展 XAML 接口）");
        executable_plugin_entry(manifest,std::wstring(j.GetNamedString(L"entry")),p.kind==L"process");
    }
    if(j.GetNamedString(L"text",L"").size()>4000)throw std::runtime_error("text 最多 4000 字符");
    auto items=j.GetNamedArray(L"items",JsonArray());
    if(items.Size()>30)throw std::runtime_error("items 最多 30 项");
    for(auto const& item:items) {
        if(p.kind==L"links") {
            auto link=item.GetObject();auto title=link.GetNamedString(L"title");
            if(title.empty()||title.size()>100||!safe_link(std::wstring(link.GetNamedString(L"url"))))
                throw std::runtime_error("链接需要 title 和完整的 HTTPS url");
        } else if(p.kind==L"checklist") {
            auto title=item.GetString();if(title.empty()||title.size()>200)throw std::runtime_error("清单条目长度不合法");
        }
    }
    return p;
}
inline void write_json_atomic(fs::path const& file,JsonObject const& j) {
    fs::create_directories(file.parent_path());auto temp=file;temp+=L".tmp";
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<winrt::to_string(j.Stringify());out.flush();if(!out)throw std::runtime_error("无法保存文件");}
    winrt::check_bool(MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH));
}
inline fs::path executable_path() {
    wchar_t path[32768]{};winrt::check_bool(GetModuleFileNameW(nullptr,path,32768)!=0);return path;
}
inline std::wstring unique_token() {
    GUID guid;winrt::check_hresult(CoCreateGuid(&guid));wchar_t text[40];StringFromGUID2(guid,text,40);return text;
}
}
