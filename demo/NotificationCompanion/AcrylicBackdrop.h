#pragma once
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Dispatching.h>

namespace companion {
namespace Backdrops = winrt::Microsoft::UI::Composition::SystemBackdrops;
// The Window.SystemBackdrop lifecycle owns connections. Keep input-active
// material policy for this NOACTIVATE surface without actually taking focus.
struct AcrylicBackdrop : winrt::Microsoft::UI::Xaml::Media::SystemBackdropT<AcrylicBackdrop> {
    Backdrops::SystemBackdropConfiguration configuration;
    Backdrops::DesktopAcrylicController controller{nullptr};
    bool supported = Backdrops::DesktopAcrylicController::IsSupported(), connected = false;
    winrt::hresult last_error = S_OK;
    unsigned pending_policy_calls = 0, connections = 0, disconnections = 0;
    AcrylicBackdrop() { configuration.IsInputActive(true); }
    void apply_policy(Backdrops::SystemBackdropConfiguration const& defaults) {
        // WinUI can raise policy changes before a target is connected / its
        // XamlRoot is loaded. The policy result is nullable in that phase.
        if (!defaults) { ++pending_policy_calls; return; }
        configuration.Theme(defaults.Theme());
        configuration.IsHighContrast(defaults.IsHighContrast());
        configuration.HighContrastBackgroundColor(defaults.HighContrastBackgroundColor());
        configuration.IsInputActive(true);
    }
    void update_policy(winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
                       winrt::Microsoft::UI::Xaml::XamlRoot const& root) {
        apply_policy(this->GetDefaultSystemBackdropConfiguration(target,root));
    }
    void OnTargetConnected(winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
                           winrt::Microsoft::UI::Xaml::XamlRoot const& root) noexcept {
        try {
            update_policy(target,root);
            if(!supported) return;
            if(controller) { controller.Close(); controller=nullptr; connected=false; }
            this->DispatcherQueue().EnsureSystemDispatcherQueue();
            controller=Backdrops::DesktopAcrylicController();
            controller.Kind(Backdrops::DesktopAcrylicKind::Base);
            controller.SetSystemBackdropConfiguration(configuration);
            connected=controller.AddSystemBackdropTarget(target);
            if(connected) ++connections;
            if(!connected) last_error=E_FAIL;
        } catch(...) { last_error=winrt::to_hresult();connected=false; }
    }
    void OnTargetDisconnected(winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target) noexcept {
        try { if(controller) {controller.RemoveSystemBackdropTarget(target);controller.Close();controller=nullptr;} }
        catch(...) { last_error=winrt::to_hresult(); }
        connected=false;
        ++disconnections;
    }
    void OnDefaultSystemBackdropConfigurationChanged(winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
                                                     winrt::Microsoft::UI::Xaml::XamlRoot const& root) noexcept {
        try { update_policy(target,root); } catch(...) { last_error=winrt::to_hresult(); }
    }
};
} // namespace companion
