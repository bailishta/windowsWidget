#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <thread>
#include <atomic>
#include <functional>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <memory>
#include <utility>
#include "Logger.h"

namespace ww {
namespace fs = std::filesystem;
using namespace winrt::Windows::Data::Json;
inline std::wstring wide(std::string_view s) {
    return std::wstring(winrt::to_hstring(s));
}
inline std::string utf8(std::wstring_view s) {
    return winrt::to_string(s);
}
inline std::string str(JsonObject const &j) {
    return winrt::to_string(j.Stringify());
}
inline JsonObject json(std::string_view s) {
    return JsonObject::Parse(winrt::to_hstring(s));
}
inline void put(JsonObject const &j, wchar_t const *k, std::string_view v) {
    j.SetNamedValue(k, JsonValue::CreateStringValue(winrt::to_hstring(v)));
}
inline void put(JsonObject const &j, wchar_t const *k, const char *v) {
    put(j, k, std::string_view(v));
}
inline void put(JsonObject const &j, wchar_t const *k, double v) {
    j.SetNamedValue(k, JsonValue::CreateNumberValue(v));
}
inline void put(JsonObject const &j, wchar_t const *k, bool v) {
    j.SetNamedValue(k, JsonValue::CreateBooleanValue(v));
}
inline std::string get(JsonObject const &j, wchar_t const *k, std::string_view fallback = {}) {
    return winrt::to_string(j.GetNamedString(k, winrt::to_hstring(fallback)));
}
inline std::string error_text() {
    try {
        throw;
    } catch (winrt::hresult_error const &e) {
        char code[32];
        sprintf_s(code, "HRESULT=0x%08X ", static_cast<unsigned>(e.code().value));
        return std::string(code) + winrt::to_string(e.message());
    } catch (std::exception const &e) {
        return e.what();
    } catch (...) {
        return "Unknown failure";
    }
}
inline void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(std::string(message) + " (Win32=" + std::to_string(GetLastError()) + ")");
}
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    ~Handle() {
        reset();
    }
    Handle(Handle const &) = delete;
    Handle &operator=(Handle const &) = delete;
    Handle(Handle &&x) noexcept : h(std::exchange(x.h, nullptr)) {}
    Handle &operator=(Handle &&x) noexcept {
        reset(std::exchange(x.h, nullptr));
        return *this;
    }
    void reset(HANDLE x = nullptr) {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        h = x;
    }
    operator HANDLE() const {
        return h;
    }
    explicit operator bool() const {
        return h && h != INVALID_HANDLE_VALUE;
    }
};
inline std::string guid() {
    GUID g;
    winrt::check_hresult(CoCreateGuid(&g));
    wchar_t b[40];
    StringFromGUID2(g, b, 40);
    return utf8(b).substr(1, 36);
}
inline fs::path executable_dir() {
    wchar_t b[32768];
    auto n = GetModuleFileNameW(nullptr, b, 32768);
    check(n && n < 32768, "Executable path");
    return fs::path(std::wstring(b, n)).parent_path();
}
inline fs::path default_root() {
    PWSTR p = nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p));
    fs::path r(p);
    CoTaskMemFree(p);
    return r / L"WindowsWidget";
}
inline std::string read_file(fs::path const &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot read " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}
inline void atomic_write(fs::path const &p, std::string const &text, bool backup = true) {
    fs::create_directories(p.parent_path());
    auto temp = p;
    temp += L".tmp";
    Handle f(
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(bool(f), "Create settings temp");
    DWORD written = 0;
    check(WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
              written == text.size(),
          "Write settings");
    check(FlushFileBuffers(f), "Flush settings");
    f.reset();
    if (fs::exists(p)) {
        auto bak = p;
        bak += L".bak";
        check(ReplaceFileW(p.c_str(), temp.c_str(), backup ? bak.c_str() : nullptr, 0, nullptr, nullptr),
              "Replace settings");
    } else
        check(MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_WRITE_THROUGH), "Commit settings");
}
inline bool system_dark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return !light;
}
inline uint64_t now_ms() {
    return GetTickCount64();
}
inline JsonObject message(std::string const &op, std::string const &id, uint64_t seq = 0) {
    JsonObject j;
    put(j, L"protocol", 1.0);
    put(j, L"op", op);
    put(j, L"id", id);
    put(j, L"seq", double(seq));
    return j;
}

// Message-mode named pipe, overlapped I/O with bounded waits and cancellation.
class Pipe {
    Handle handle_;
    std::vector<char> buffer_ = std::vector<char>(1024 * 1024);
    bool io(bool write, void *data, DWORD size, DWORD &count, DWORD timeout) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED ov{};
        ov.hEvent = event;
        BOOL done =
            write ? WriteFile(handle_, data, size, &count, &ov) : ReadFile(handle_, data, size, &count, &ov);
        if (done)
            return true;
        auto err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            SetLastError(err);
            check(false, "Pipe I/O");
        }
        DWORD wait = WaitForSingleObject(event, timeout);
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(handle_, &ov);
            if (GetOverlappedResult(handle_, &ov, &count, TRUE))
                return true;
            if (wait == WAIT_TIMEOUT && GetLastError() == ERROR_OPERATION_ABORTED)
                return false;
            check(false, "Pipe wait");
        }
        check(GetOverlappedResult(handle_, &ov, &count, FALSE), "Pipe completion");
        return true;
    }

  public:
    static constexpr DWORD limit = 1024 * 1024;
    Pipe() = default;
    explicit Pipe(HANDLE h) : handle_(h) {}
    Pipe(Pipe &&) = default;
    Pipe &operator=(Pipe &&) = default;
    HANDLE handle() const {
        return handle_;
    }
    static Pipe server(std::wstring const &name) {
        Handle token;
        HANDLE t;
        check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t), "Process token");
        token.reset(t);
        DWORD n = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &n);
        std::vector<BYTE> b(n);
        check(GetTokenInformation(token, TokenUser, b.data(), n, &n), "Token user");
        LPWSTR sid = nullptr;
        check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(b.data())->User.Sid, &sid), "SID");
        std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR sd = nullptr;
        check(
            ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr),
            "Pipe ACL");
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        HANDLE h = CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, limit,
            limit, 0, &sa);
        LocalFree(sd);
        check(h != INVALID_HANDLE_VALUE, "Create pipe");
        return Pipe(h);
    }
    bool connect(DWORD timeout) {
        Handle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED ov{};
        ov.hEvent = ev;
        if (ConnectNamedPipe(handle_, &ov))
            return true;
        auto err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED)
            return true;
        check(err == ERROR_IO_PENDING, "Connect pipe");
        if (WaitForSingleObject(ev, timeout) != WAIT_OBJECT_0) {
            DWORD n;
            CancelIoEx(handle_, &ov);
            GetOverlappedResult(handle_, &ov, &n, TRUE);
            return false;
        }
        DWORD n;
        check(GetOverlappedResult(handle_, &ov, &n, FALSE), "Connect completion");
        return true;
    }
    static Pipe client(std::wstring const &name) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        check(h != INVALID_HANDLE_VALUE, "Open pipe");
        Pipe p(h);
        DWORD mode = PIPE_READMODE_MESSAGE;
        check(SetNamedPipeHandleState(h, &mode, nullptr, nullptr), "Pipe mode");
        return p;
    }
    void send(JsonObject const &j) {
        auto s = str(j);
        check(s.size() <= limit, "Message too large");
        DWORD n = 0;
        check(io(true, s.data(), static_cast<DWORD>(s.size()), n, 2000) && n == s.size(),
              "Pipe send timeout");
    }
    bool receive(JsonObject &j, DWORD timeout = 100) {
        DWORD n = 0;
        if (!io(false, buffer_.data(), limit, n, timeout))
            return false;
        check(n > 0, "Pipe EOF");
        j = json(std::string_view(buffer_.data(), n));
        check(j.GetNamedNumber(L"protocol", 0) == 1, "Protocol mismatch");
        return true;
    }
};
} // namespace ww
