#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include <chrono>

// Runs only the requested test executable as a debuggee. Exceptions are recorded
// before Windows can display a crash dialog; no other process is attached.
static void describe_exception(HANDLE process, DWORD thread_id, EXCEPTION_RECORD const& exception, DWORD64 main_base) {
    std::wcerr << L"Exception 0x" << std::hex << exception.ExceptionCode
               << L" at 0x" << reinterpret_cast<uintptr_t>(exception.ExceptionAddress);
    if (exception.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && exception.NumberParameters >= 2)
        std::wcerr << L" operation=" << exception.ExceptionInformation[0]
                   << L" address=0x" << exception.ExceptionInformation[1];
    std::wcerr << L"\n";
    if (reinterpret_cast<DWORD64>(exception.ExceptionAddress) >= main_base)
        std::wcerr << L"Main image relative address (if within EXE): 0x" << std::hex
                   << reinterpret_cast<DWORD64>(exception.ExceptionAddress) - main_base << L"\n";
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread_id);
    if (!thread) return;
    CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context)) {
        STACKFRAME64 frame{};
        frame.AddrPC = { context.Rip, AddrModeFlat };
        frame.AddrStack = { context.Rsp, AddrModeFlat };
        frame.AddrFrame = { context.Rbp, AddrModeFlat };
        for (unsigned i = 0; i < 24; ++i) {
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 1024]{};
            auto symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 1024;
            DWORD64 displacement = 0;
            IMAGEHLP_MODULE64 module{}; module.SizeOfStruct = sizeof(module);
            SymGetModuleInfo64(process, frame.AddrPC.Offset, &module);
            std::cerr << "  0x" << std::hex << frame.AddrPC.Offset << " " << module.ModuleName;
            if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
                std::cerr << "!" << symbol->Name << "+0x" << displacement;
            else std::cerr << " symbol-error=" << std::dec << GetLastError();
            IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD line_displacement = 0;
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line))
                std::cerr << " " << line.FileName << ":" << std::dec << line.LineNumber;
            std::cerr << "\n";
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                std::cerr << "StackWalk error=" << GetLastError() << "\n"; break;
            }
            if (!frame.AddrPC.Offset) break;
        }
    }
    CloseHandle(thread);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\"";
    bool runtime_test = false;
    for (int i = 2; i < argc; ++i) {
        command += L" \"" + std::wstring(argv[i]) + L"\"";
        runtime_test |= std::wstring(argv[i]) == L"--runtime-test";
    }
    STARTUPINFOW startup{ sizeof(startup) }; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE,
        DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
        std::cerr << "CreateProcess failed: " << GetLastError() << "\n"; return 2;
    }
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitializeW(child.hProcess, std::filesystem::path(argv[1]).parent_path().c_str(), FALSE))
        std::cerr << "SymInitialize failed: " << GetLastError() << "\n";
    auto start = std::chrono::steady_clock::now();
    // Multi-process fixtures allow 120 seconds internally and include several
    // cold WinUI launches. Keep the debugger watchdog outside that deadline.
    auto timeout = std::chrono::seconds(runtime_test ? 150 : 20);
    DWORD result = 2; bool terminated = false; DWORD64 main_base = 0;
    for (;;) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 100)) {
            if (!terminated && std::chrono::steady_clock::now() - start > timeout) {
                std::cerr << "Hidden test timeout\n"; TerminateProcess(child.hProcess, 3); terminated = true;
            }
            continue;
        }
        DWORD continuation = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            main_base = reinterpret_cast<DWORD64>(event.u.CreateProcessInfo.lpBaseOfImage);
            if (!SymLoadModuleExW(child.hProcess, event.u.CreateProcessInfo.hFile, argv[1], nullptr,
                reinterpret_cast<DWORD64>(event.u.CreateProcessInfo.lpBaseOfImage), 0, nullptr, 0))
                std::cerr << "SymLoad main image: " << std::dec << GetLastError() << "\n";
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            SymLoadModuleExW(child.hProcess, event.u.LoadDll.hFile, nullptr, nullptr,
                reinterpret_cast<DWORD64>(event.u.LoadDll.lpBaseOfDll), 0, nullptr, 0);
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        } else if (event.dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT) {
            SymUnloadModule64(child.hProcess, reinterpret_cast<DWORD64>(event.u.UnloadDll.lpBaseOfDll));
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            auto const& exception = event.u.Exception;
            if (exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT) continuation = DBG_EXCEPTION_NOT_HANDLED;
            if (!exception.dwFirstChance) {
                describe_exception(child.hProcess, event.dwThreadId, exception.ExceptionRecord, main_base);
                result = exception.ExceptionRecord.ExceptionCode;
                TerminateProcess(child.hProcess, result); terminated = true;
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            result = event.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation); break;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation);
    }
    SymCleanup(child.hProcess); CloseHandle(child.hThread); CloseHandle(child.hProcess);
    return static_cast<int>(result);
}
