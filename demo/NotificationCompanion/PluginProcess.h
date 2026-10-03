#pragma once
#include <windows.h>
#include <winrt/base.h>
#include "PluginCatalog.h"

namespace companion {
// A single writer per direction. The short seqlock never waits on a plugin.
// Shared structures contain only POD; no WinRT object or pointer crosses processes.
struct PluginFrame {RECT work{},reserved{},target{};UINT dpi=96;LONG automatic=1,dark=1,phase=0;};
struct PluginWire {
    DWORD magic=0x57574731,version=1;
    volatile LONG sequence=0;
    PluginFrame frame;
    volatile LONG ready=0,dragging=0,drag_revision=0,command=0;
    RECT drag_position{};
    volatile LONG native_renders=0;
    DWORD pid=0;
    HWND panel=nullptr,control=nullptr,parent=nullptr;
    // Optional ABI 1 tail: retain the current layout during in-card keyboard editing.
    volatile LONG editing=0;
};
class PluginChannel {
    winrt::handle mapping;
public:
    PluginWire* wire=nullptr;
    PluginChannel()=default;
    PluginChannel(PluginChannel const&)=delete;
    ~PluginChannel(){if(wire)UnmapViewOfFile(wire);}
    void create(std::wstring const& name,HWND parent) {
        mapping.attach(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(PluginWire),name.c_str()));
        winrt::check_bool(bool(mapping));
        wire=static_cast<PluginWire*>(MapViewOfFile(mapping.get(),FILE_MAP_ALL_ACCESS,0,0,sizeof(PluginWire)));winrt::check_bool(wire!=nullptr);
        new(wire) PluginWire();wire->parent=parent;
    }
    void open(std::wstring const& name) {
        mapping.attach(OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str()));winrt::check_bool(bool(mapping));
        wire=static_cast<PluginWire*>(MapViewOfFile(mapping.get(),FILE_MAP_ALL_ACCESS,0,0,sizeof(PluginWire)));winrt::check_bool(wire!=nullptr);
        if(wire->magic!=0x57574731||wire->version!=1)throw std::runtime_error("Incompatible channel");
    }
    void publish(PluginFrame const& frame) {
        InterlockedIncrement(&wire->sequence);MemoryBarrier();wire->frame=frame;MemoryBarrier();InterlockedIncrement(&wire->sequence);
    }
    bool read(PluginFrame& frame) const {
        for(int attempt=0;attempt<3;++attempt) {
            LONG before=InterlockedCompareExchange(&wire->sequence,0,0);if(before&1)continue;
            MemoryBarrier();frame=wire->frame;MemoryBarrier();
            if(before==InterlockedCompareExchange(&wire->sequence,0,0))return true;
        }return false;
    }
};
struct PluginProcess {
    PluginDefinition definition;
    std::unique_ptr<PluginChannel> channel;
    winrt::handle process,job;
    DWORD pid=0;uint64_t started=0;LONG drag_revision=0;
    std::wstring state=L"已停用";
    bool enabled=true;
    HWND panel() const {
        if(!channel||!channel->wire||!InterlockedCompareExchange(&channel->wire->ready,0,0))return nullptr;
        auto h=channel->wire->panel;DWORD owner=0;GetWindowThreadProcessId(h,&owner);
        return owner==pid && channel->wire->pid==pid ? h : nullptr;
    }
    void stop() {
        if(channel&&process&&channel->wire->control) {
            DWORD owner=0;GetWindowThreadProcessId(channel->wire->control,&owner);
            if(owner==pid) {DWORD_PTR ignored=0;SendMessageTimeoutW(channel->wire->control,WM_APP+101,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,200,&ignored);}
        }
        if(job)TerminateJobObject(job.get(),0);
        if(process)WaitForSingleObject(process.get(),2000); // Release loaded EXEs/DLLs before uninstalling the package.
        job.close();process.close();channel.reset();pid=0;state=L"已停用";
    }
    void start(fs::path const& data,HWND parent,bool english,bool inspect) {
        stop();channel=std::make_unique<PluginChannel>();auto token=L"Local\\WindowsWidget.Plugin."+unique_token();channel->create(token,parent);
        job.attach(CreateJobObjectW(nullptr,nullptr));winrt::check_bool(bool(job));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        winrt::check_bool(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limit,sizeof(limit)));
        auto exe=definition.kind==L"process"?executable_plugin_entry(definition.manifest,std::wstring(definition.document.GetNamedString(L"entry")),true):executable_path();
        // All values are generated tokens or validated Windows paths (which cannot contain quotes).
        auto command=L"\""+exe.wstring()+L"\" --plugin \""+definition.manifest.wstring()+L"\" --channel \""+token+L"\" --data-dir \""+data.wstring()+L"\"";
        if(english)command+=L" --en";if(inspect)command+=L" --inspect";
        STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};
        winrt::check_bool(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup,&info));
        process.attach(info.hProcess);winrt::handle thread{info.hThread};pid=info.dwProcessId;
        if(!AssignProcessToJobObject(job.get(),process.get())){TerminateProcess(process.get(),1);winrt::throw_last_error();}
        if(ResumeThread(thread.get())==DWORD(-1)){TerminateJobObject(job.get(),1);winrt::throw_last_error();}
        started=GetTickCount64();state=L"正在启动";
    }
    bool update() {
        if(!process)return false;
        DWORD code=0;winrt::check_bool(GetExitCodeProcess(process.get(),&code));
        if(code!=STILL_ACTIVE){stop();state=L"运行已退出（"+std::to_wstring(code)+L"），可以重启";return true;}
        if(panel()){if(state!=L"运行中"){state=L"运行中";return true;}}
        else if(GetTickCount64()-started>20000){stop();state=L"启动超时，可以重启";return true;}
        return false;
    }
    void command(LONG action){if(channel)InterlockedExchange(&channel->wire->command,action);}
};
}
