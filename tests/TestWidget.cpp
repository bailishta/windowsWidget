#include "../src/common/Common.h"
#include "../sdk/WidgetSdk.h"
using namespace ww;
namespace {
struct Test {
    HWND h;
    WidgetHostApi const *host;
    void report_language() {
        auto message = "test UI language=" + std::to_string(GetThreadUILanguage());
        host->log(host->context, 0, message.c_str());
    }
};
} // namespace
WIDGET_EXPORT HRESULT __cdecl WidgetGetApi(uint32_t version, WidgetApi *api) {
    if (version != 1 || !api || api->size < sizeof(WidgetApi))
        return E_NOINTERFACE;
    *api = {sizeof(WidgetApi),
            1,
            [](const WidgetCreateInfo *info, void **out, HWND *hwnd) -> HRESULT {
                try {
                    auto j = json(info->configuration_utf8);
                    auto mode = get(j, L"mode");
                    if (mode == "crash")
                        TerminateProcess(GetCurrentProcess(), 42);
                    if (mode == "hang")
                        Sleep(INFINITE);
                    Sleep(DWORD(j.GetNamedNumber(L"delayMs", 0)));
                    auto p = new Test{};
                    p->host = info->host;
                    p->report_language();
                    p->h = CreateWindowExW(0, L"STATIC", L"Test widget", WS_CHILD | WS_VISIBLE, 0, 0, 200,
                                           100, info->parent, nullptr, GetModuleHandleW(nullptr), nullptr);
                    *out = p;
                    *hwnd = p->h;
                    return S_OK;
                } catch (...) {
                    return E_FAIL;
                }
            },
            [](void *p) {
                auto t = static_cast<Test *>(p);
                if (IsWindow(t->h))
                    DestroyWindow(t->h);
                delete t;
            },
            [](void *, const char *) -> HRESULT { return S_OK; },
            [](void *, uint32_t, uint32_t, uint32_t) {},
            [](void *p, uint32_t) { static_cast<Test *>(p)->report_language(); }};
    return S_OK;
}
