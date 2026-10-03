#pragma once
// The host owns every XAML island.
//
// A plugin DLL cannot create WinUI objects: WinUI builds its resource provider per
// activating module, and doing that from a loaded DLL fail-fasts the process
// (measured: Microsoft.ui.xaml.dll -> ModernResourceProvider::Create returns
// E_INVALIDARG -> RoFailFastWithErrorContext). The same code in the host .exe
// works. So the host creates the island and the plugin only describes its UI:
// it hands over a XAML document and gets clicks back by element name.
//
// This file is the host half of that contract.

#include "../common/Common.h"
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.Graphics.h>
#include <functional>
#include <string>

namespace ww {
using namespace winrt::Microsoft::UI::Xaml;

// The Application object the XAML runtime requires. Application::Start is
// deliberately not called: a Win32 host with its own message loop only needs the
// object so control styles and metadata resolve.
struct WidgetXamlApp : ApplicationT<WidgetXamlApp, Markup::IXamlMetadataProvider> {
    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata;
    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const &type) {
        return metadata.GetXamlType(type);
    }
    Markup::IXamlType GetXamlType(winrt::hstring const &name) {
        return metadata.GetXamlType(name);
    }
    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return metadata.GetXmlnsDefinitions();
    }
};

// Per-process XAML start-up, run on the host UI thread before the first island.
inline bool xaml_ready(std::string &error) {
    static int state = 0; // 0 = not tried, 1 = ready, -1 = failed
    static std::string failure;
    if (state == 1)
        return true;
    if (state == -1) {
        error = failure;
        return false;
    }
    try {
        if (!winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread()) {
            static auto controller =
                winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();
            (void)controller;
        }
        if (!Application::Current()) {
            static Application app = winrt::make<WidgetXamlApp>();
            (void)app;
        }
        state = 1;
    } catch (winrt::hresult_error const &e) {
        failure = winrt::to_string(e.message());
        state = -1;
    } catch (...) {
        failure = error_text();
        state = -1;
    }
    if (state != 1)
        error = failure;
    return state == 1;
}

class XamlSurface {
    Hosting::DesktopWindowXamlSource island_{nullptr};
    FrameworkElement root_{nullptr};
    HWND container_ = nullptr;
    bool dark_ = true;
    std::string rendered_; // last markup, so an unchanged redraw costs nothing
    std::function<void(std::string const &)> on_event_;

    // Attaches a Click handler to every named ButtonBase in the parsed object
    // graph. Walking the graph rather than the visual tree works before the tree
    // is loaded, so the handlers are in place before the first frame.
    template <class F> void walk(winrt::Windows::Foundation::IInspectable const &node, F &&visit) {
        if (!node)
            return;
        visit(node);
        if (auto panel = node.try_as<Controls::Panel>())
            for (auto const &child : panel.Children())
                walk(child, visit);
        if (auto border = node.try_as<Controls::Border>())
            walk(border.Child(), visit);
        if (auto items = node.try_as<Controls::ItemsControl>())
            for (auto const &item : items.Items())
                walk(item.try_as<winrt::Windows::Foundation::IInspectable>(), visit);
        if (auto content = node.try_as<Controls::ContentControl>())
            walk(content.Content().try_as<winrt::Windows::Foundation::IInspectable>(), visit);
    }

  public:
    bool active() const {
        return island_ != nullptr;
    }

    // Creates the island on the widget's container. The container must be a
    // top-level window: an island will not attach to a window whose parent chain
    // leaves the process.
    bool attach(HWND container, std::function<void(std::string const &)> on_event, std::string &error) {
        if (island_)
            return true;
        if (!xaml_ready(error))
            return false;
        try {            auto id = winrt::Microsoft::UI::GetWindowIdFromWindow(container);            island_ = Hosting::DesktopWindowXamlSource();            island_.Initialize(id);            container_ = container;
            on_event_ = std::move(on_event);
            return true;
        } catch (winrt::hresult_error const &e) {
            error = winrt::to_string(e.message());
            island_ = nullptr;
            return false;
        }
    }

    void detach() {
        if (!island_)
            return;
        try {
            // Only clear once something was ever rendered: Content(nullptr) on a
            // fresh island is not a no-op, it tears down a tree that never existed.
            if (root_)
                island_.Content(nullptr);
            island_.Close();
        } catch (...) {
        }
        island_ = nullptr;
        root_ = nullptr;
        rendered_.clear();
    }

    void resize(int width, int height) {
        if (!island_)
            return;
        try {
            if (island_.SiteBridge())
                island_.SiteBridge().MoveAndResize(
                    winrt::Windows::Graphics::RectInt32{0, 0, std::max(1, width), std::max(1, height)});
        } catch (...) {
        }
    }

    // Applied to every newly rendered root, so a plugin that re-renders on a
    // theme change does not have to remember to carry the theme itself.
    void theme(bool dark) {
        dark_ = dark;
        if (root_) {
            try {
                root_.RequestedTheme(dark ? ElementTheme::Dark : ElementTheme::Light);
            } catch (...) {
            }
        }
    }

    // Replaces the widget's whole UI. The document is re-parsed each call, which
    // keeps the ABI a plain string and the plugin free of WinRT.
    bool render(const char *xaml_utf8, std::string &error) {
        if (!island_) {
            error = "no XAML surface";
            return false;
        }
        // Widgets run for days. Parsing XAML and swapping the tree on every timer
        // tick would churn objects for no visible change, so an unchanged document
        // is a no-op. Plugins keep their own change detection; this is the backstop.
        std::string requested = xaml_utf8 ? xaml_utf8 : "";
        if (requested == rendered_ && root_)
            return true;
        try {
            auto loaded = Markup::XamlReader::Load(winrt::to_hstring(requested))
                              .try_as<FrameworkElement>();
            if (!loaded) {
                error = "XAML root is not a FrameworkElement";
                return false;
            }
            walk(loaded, [this](winrt::Windows::Foundation::IInspectable const &node) {
                auto button = node.try_as<Controls::Primitives::ButtonBase>();
                if (!button || !on_event_)
                    return;
                auto name = node.try_as<FrameworkElement>() ? node.as<FrameworkElement>().Name() : winrt::hstring{};
                if (name.empty())
                    return;
                auto key = winrt::to_string(name);
                button.Click([this, key](auto &&, auto &&) {
                    if (on_event_)
                        on_event_(key);
                });
            });
            loaded.RequestedTheme(dark_ ? ElementTheme::Dark : ElementTheme::Light);
            // Release the previous tree before publishing the new one: both alive at
            // once would briefly double the widget's XAML memory.
            island_.Content(nullptr);
            root_ = nullptr;
            root_ = loaded;
            island_.Content(root_);
            rendered_ = std::move(requested);
            return true;
        } catch (winrt::hresult_error const &e) {
            error = winrt::to_string(e.message());
            return false;
        } catch (...) {
            error = error_text();
            return false;
        }
    }
};
} // namespace ww
