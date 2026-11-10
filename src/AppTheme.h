#pragma once
#include <cstdint>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.UI.h>

namespace sessions {
// Canonical brand reference. design/tokens.json documents these native tokens;
// it is not a second runtime configuration or a dependency of application startup.
inline constexpr uint32_t AccentArgb = 0xFF0F766E;
inline constexpr wchar_t AccentName[] = L"Deep Teal";
inline constexpr wchar_t AccentHex[] = L"#0F766E";
inline constexpr winrt::Windows::UI::Color AccentColor{
    uint8_t(AccentArgb >> 24), uint8_t(AccentArgb >> 16),
    uint8_t(AccentArgb >> 8), uint8_t(AccentArgb)};

// Call once, after merging XamlControlsResources and before creating controls.
// Overrides resources within this application only; Windows settings are untouched.
// Custom XAML should use SessionsAccentBrush / SessionsOnAccentBrush theme resources.
void ApplyAppTheme(winrt::Microsoft::UI::Xaml::Application const& application);
}
