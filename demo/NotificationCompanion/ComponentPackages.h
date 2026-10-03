#pragma once
#include "PluginCatalog.h"

namespace companion {
// Commit complete packages atomically. A failed import never leaves a half-installed mod.
inline PluginDefinition install_component(fs::path const& manifest,fs::path const& root) {
    auto definition=read_plugin(manifest);
    fs::create_directories(root/L"plugins");
    auto destination=fs::absolute(root/L"plugins"/definition.id);
    if(fs::exists(destination))throw std::runtime_error("组件目录已存在，请重新扫描或使用不同的 id");
    auto staging=fs::absolute(root/L"plugins"/(L".install-"+unique_token()));
    auto package_root=fs::canonical(root/L"plugins");
    if(staging.parent_path()!=package_root || destination.parent_path()!=package_root)
        throw std::runtime_error("组件安装目录不能通过链接跳转");
    std::vector<std::pair<fs::path,fs::path>> files;
    bool executable=definition.kind==L"native"||definition.kind==L"process";
    if(executable) {
        auto source=fs::canonical(manifest.parent_path());uintmax_t bytes=0;
        if(GetFileAttributesW(manifest.parent_path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("组件包不能包含链接或目录联接");
        for(auto const& item:fs::recursive_directory_iterator(source)) {
            if(GetFileAttributesW(item.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT)
                throw std::runtime_error("组件包不能包含链接或目录联接");
            if(item.is_regular_file()){bytes+=item.file_size();files.push_back({item.path(),item.path().lexically_relative(source)});}
            // A standalone process may carry the Windows App SDK runtime.
            auto byte_limit=definition.kind==L"process"?256u*1024*1024:50u*1024*1024;
            auto file_limit=definition.kind==L"process"?2048u:256u;
            if(bytes>byte_limit||files.size()>file_limit)throw std::runtime_error("组件包文件总大小或数量超过限制");
        }
    } else if(!definition.icon.empty()) {
        files.push_back({definition.icon,fs::path(std::wstring(definition.document.GetNamedString(L"icon")))});
    }
    try {
        fs::create_directories(staging);
        for(auto const& pair:files){fs::create_directories((staging/pair.second).parent_path());fs::copy_file(pair.first,staging/pair.second);}
        write_json_atomic(staging/L"widget.json",definition.document);
        read_plugin(staging/L"widget.json");
        fs::rename(staging,destination);
    }catch(...) {
        // Both the absolute path and its parent were verified above, before recursive cleanup.
        std::error_code ignored;fs::remove_all(staging,ignored);throw;
    }
    return read_plugin(destination/L"widget.json");
}
inline fs::path archive_component(PluginDefinition const& definition,fs::path const& root) {
    auto source=fs::absolute(definition.manifest.parent_path());
    auto packages=fs::canonical(root/L"plugins");
    if(source.parent_path()!=packages||fs::canonical(source)!=source||
       (GetFileAttributesW(source.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("只能卸载已安装目录中的组件包");
    auto archive_root=fs::absolute(root/L"removed-plugins");fs::create_directories(archive_root);
    if(fs::canonical(archive_root)!=archive_root)throw std::runtime_error("组件归档目录不能通过链接跳转");
    auto target=archive_root/(definition.id+L"-"+unique_token());
    // Paths are absolute, checked children of the selected data directory. Keep an undoable package backup.
    fs::rename(source,target);return target;
}
}
