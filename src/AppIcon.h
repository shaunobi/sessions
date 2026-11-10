#pragma once
#include <windows.h>
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Xaml.h>

namespace sessions {
// Owns both window-icon sizes until the window closes. Set/Reset run on its UI thread.
class AppIcon final {
public:
    AppIcon() = default;
    ~AppIcon();
    AppIcon(AppIcon const&) = delete;
    AppIcon& operator=(AppIcon const&) = delete;
    void Set(winrt::Microsoft::UI::Xaml::Window const& window);
    void Reset() noexcept;
private:
    HWND window{};
    HICON largeIcon{},smallIcon{};
};
}
