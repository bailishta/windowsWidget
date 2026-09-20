#pragma once
#include <stdint.h>
#include <windows.h>

#define WIDGET_ABI_VERSION 1u
#define WIDGET_EXPORT extern "C" __declspec(dllexport)

// Borrowed UTF-8 strings remain valid only for the duration of a call.
// All plugin callbacks run on the host UI thread. Do not throw across this ABI.
struct WidgetHostApi {
    uint32_t size;
    uint32_t version;
    void *context;
    void(__cdecl *log)(void *, uint32_t level, const char *utf8);
    void(__cdecl *configuration_changed)(void *, const char *json_utf8);
    const char *data_directory_utf8; // valid until destroy()
};
struct WidgetCreateInfo {
    uint32_t size;
    HWND parent;
    uint32_t dpi;
    uint32_t dark;
    const char *configuration_utf8;
    const WidgetHostApi *host;
};
struct WidgetApi {
    uint32_t size;
    uint32_t version;
    HRESULT(__cdecl *create)(const WidgetCreateInfo *, void **instance, HWND *content);
    void(__cdecl *destroy)(void *instance);
    HRESULT(__cdecl *configure)(void *instance, const char *configuration_utf8);
    void(__cdecl *layout)(void *instance, uint32_t width_px, uint32_t height_px, uint32_t dpi);
    void(__cdecl *theme)(void *instance, uint32_t dark);
};
typedef HRESULT(__cdecl *WidgetGetApiFn)(uint32_t requested_version, WidgetApi *api);
