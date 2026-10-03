#pragma once
#include <stdint.h>
#include <windows.h>

#define WIDGET_ABI_VERSION 1u
#define WIDGET_EXPORT extern "C" __declspec(dllexport)

// Set in WidgetApi2::capabilities when the widget's content is a XAML document
// rather than an HWND it draws itself.
//
// A XAML island cannot be created inside a plugin DLL: WinUI builds its resource
// provider per activating module, and doing so from a loaded DLL fail-fasts the
// process (measured: Microsoft.ui.xaml.dll -> ModernResourceProvider::Create
// returns E_INVALIDARG -> RoFailFastWithErrorContext). It also refuses to attach
// to a window whose parent chain leaves the process (E_ACCESSDENIED), which is
// why widgets are top-level windows. So the host creates and owns the island and
// the plugin only *describes* its UI: it hands over a complete XAML document
// through WidgetHostApi2::ui_render and receives clicks through ui_event.
#define WIDGET_CAPABILITY_XAML 0x00000001u

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

// ABI v2. Same leading layout as WidgetHostApi; the host seeds .size with its own
// sizeof, and a plugin must only read a field when .size covers it - the same
// discipline as STARTUPINFOEX.
struct WidgetHostApi2 {
    uint32_t size;
    uint32_t version;
    void *context;
    void(__cdecl *log)(void *, uint32_t level, const char *utf8);
    void(__cdecl *configuration_changed)(void *, const char *json_utf8);
    const char *data_directory_utf8; // valid until destroy()

    // Replaces this widget's whole UI with the given XAML document (UTF-8, a
    // single root element). Returns non-zero when the host rendered it; zero
    // means the XAML could not be parsed or the host has no XAML support, and
    // the widget should report the problem rather than stay silent.
    //
    // Only meaningful for widgets that declared WIDGET_CAPABILITY_XAML. The
    // document is re-parsed each call, so keep it small and push it only when
    // something actually changed.
    uint32_t(__cdecl *ui_render)(void *context, const char *xaml_utf8);
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

// ABI v2. Same leading layout as WidgetApi.
struct WidgetApi2 {
    uint32_t size;
    uint32_t version;
    HRESULT(__cdecl *create)(const WidgetCreateInfo *, void **instance, HWND *content);
    void(__cdecl *destroy)(void *instance);
    HRESULT(__cdecl *configure)(void *instance, const char *configuration_utf8);
    void(__cdecl *layout)(void *instance, uint32_t width_px, uint32_t height_px, uint32_t dpi);
    void(__cdecl *theme)(void *instance, uint32_t dark);

    // WIDGET_CAPABILITY_* bits. Read by the host before create().
    uint32_t capabilities;

    // Called on the host UI thread when a named Button in the widget's XAML is
    // clicked; element_utf8 is its x:Name. Optional even for XAML widgets.
    void(__cdecl *ui_event)(void *instance, const char *element_utf8);
};

typedef HRESULT(__cdecl *WidgetGetApiFn)(uint32_t requested_version, WidgetApi *api);

// Helpers for the size-guarded accesses above.
inline const WidgetHostApi2 *widget_host_v2(const WidgetHostApi *api) {
    return api && api->size >= sizeof(WidgetHostApi2) ? reinterpret_cast<const WidgetHostApi2 *>(api)
                                                      : nullptr;
}
inline WidgetApi2 *widget_api_v2(WidgetApi *api) {
    return api && api->size >= sizeof(WidgetApi2) ? reinterpret_cast<WidgetApi2 *>(api) : nullptr;
}
