#include "AppTheme.h"
#include <array>
#include <string>
#include <utility>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Media;
using Windows::Foundation::IInspectable;
using Windows::UI::Color;

namespace sessions {
namespace {
constexpr Color White{255,255,255,255}, Black{255,0,0,0};
constexpr Color Mix(Color from, Color to, unsigned percent) {
    auto channel=[percent](uint8_t a,uint8_t b) {
        return uint8_t((unsigned(a)*(100-percent)+unsigned(b)*percent+50)/100);
    };
    return {255,channel(from.R,to.R),channel(from.G,to.G),channel(from.B,to.B)};
}
constexpr std::array<Color,3> Dark{
    Mix(AccentColor,Black,15),Mix(AccentColor,Black,30),Mix(AccentColor,Black,45)};
constexpr std::array<Color,3> Light{
    Mix(AccentColor,White,20),Mix(AccentColor,White,40),Mix(AccentColor,White,65)};

void Put(ResourceDictionary const& resources,wchar_t const* key,IInspectable const& value) {
    resources.Insert(box_value(key),value);
}
void Brush(ResourceDictionary const& resources,wchar_t const* key,Color color) {
    Put(resources,key,SolidColorBrush(color));
}
void Alias(ResourceDictionary const& resources,wchar_t const* key,wchar_t const* source) {
    Put(resources,key,resources.Lookup(box_value(source)));
}

void ControlAliases(ResourceDictionary const& resources) {
    // These are the actual aliases used by this project's bundled WinUI 3
    // generic.xaml. Override aliases as well as semantic brushes: replacing an
    // AccentFill brush alone need not replace an already resolved StaticResource.
    for(auto prefix:{L"SliderThumbBackground",L"SliderTrackValueFill",L"ToggleSwitchFillOn",
        L"ToggleSwitchStrokeOn",L"AccentButtonBackground",L"ToggleButtonBackgroundChecked",
        L"RadioButtonOuterEllipseCheckedStroke",L"RadioButtonOuterEllipseCheckedFill",
        L"CheckBoxCheckBackgroundStrokeChecked",L"CheckBoxCheckBackgroundStrokeIndeterminate",
        L"CheckBoxCheckBackgroundFillChecked",L"CheckBoxCheckBackgroundFillIndeterminate"}) {
        Alias(resources,prefix,L"AccentFillColorDefaultBrush");
        auto hover=std::wstring(prefix)+L"PointerOver",pressed=std::wstring(prefix)+L"Pressed";
        Alias(resources,hover.c_str(),L"AccentFillColorSecondaryBrush");
        Alias(resources,pressed.c_str(),L"AccentFillColorTertiaryBrush");
    }
    for(auto prefix:{L"HyperlinkButtonForeground",L"HyperlinkForeground"}) {
        Alias(resources,prefix,L"AccentTextFillColorPrimaryBrush");
        auto hover=std::wstring(prefix)+L"PointerOver",pressed=std::wstring(prefix)+L"Pressed";
        Alias(resources,hover.c_str(),L"AccentTextFillColorSecondaryBrush");
        Alias(resources,pressed.c_str(),L"AccentTextFillColorTertiaryBrush");
    }
    for(auto key:{L"NavigationViewSelectionIndicatorForeground",L"TreeViewItemSelectionIndicatorForeground",
        L"TreeViewItemSelectionIndicatorForegroundPointerOver",L"TreeViewItemSelectionIndicatorForegroundPressed",
        L"ListViewItemSelectionIndicatorBrush",L"ListViewItemSelectionIndicatorPointerOverBrush",
        L"ListViewItemSelectionIndicatorPressedBrush",L"ComboBoxItemPillFillBrush",
        L"ProgressBarForeground",L"ProgressRingForegroundThemeBrush",L"InfoBadgeBackground",
        L"TextControlElevationBorderFocusedBrush",L"TextControlBorderBrushFocused",
        L"GridViewItemSelectedBorderBrush",L"GridViewItemCheckBoxSelectedBrush",
        L"ListViewItemCheckBoxSelectedBrush",L"ItemContainerSelectionVisualBackground",
        L"SelectorBarItemPillFill",L"SystemControlBackgroundAccentBrush",
        L"SystemControlHighlightAccentBrush",L"SystemControlHighlightAltAccentBrush"})
        Alias(resources,key,L"AccentFillColorDefaultBrush");
    for(auto key:{L"SystemControlForegroundAccentBrush",L"SystemControlHyperlinkTextBrush"})
        Alias(resources,key,L"AccentTextFillColorPrimaryBrush");
    Alias(resources,L"TextControlSelectionHighlightColor",L"AccentFillColorSelectedTextBackgroundBrush");
    Alias(resources,L"SystemControlFocusVisualPrimaryBrush",L"FocusStrokeColorOuterBrush");
    Alias(resources,L"SystemControlFocusVisualSecondaryBrush",L"FocusStrokeColorInnerBrush");
    for(auto prefix:{L"AccentButtonForeground",L"ToggleButtonForegroundChecked",L"ToggleSwitchKnobFillOn",
        L"RadioButtonCheckGlyphFill",L"CheckBoxCheckGlyphForegroundChecked",L"CheckBoxCheckGlyphForegroundIndeterminate"}) {
        Alias(resources,prefix,L"TextOnAccentFillColorPrimaryBrush");
        auto hover=std::wstring(prefix)+L"PointerOver",pressed=std::wstring(prefix)+L"Pressed";
        Alias(resources,hover.c_str(),L"TextOnAccentFillColorPrimaryBrush");
        Alias(resources,pressed.c_str(),L"TextOnAccentFillColorSecondaryBrush");
    }
}

ResourceDictionary Theme(bool dark) {
    ResourceDictionary resources;
    Put(resources,L"SystemAccentColor",box_value(AccentColor));
    for(size_t index=0;index<3;++index) {
        auto darkKey=L"SystemAccentColorDark"+std::to_wstring(index+1);
        auto lightKey=L"SystemAccentColorLight"+std::to_wstring(index+1);
        Put(resources,darkKey.c_str(),box_value(Dark[index]));
        Put(resources,lightKey.c_str(),box_value(Light[index]));
    }
    const auto fill=dark?Light[1]:AccentColor;
    const auto hover=dark?Light[2]:Dark[0];
    const auto pressed=dark?Mix(AccentColor,White,30):Dark[1];
    const auto text=dark?Light[2]:AccentColor;
    const auto onAccent=dark?Black:White;
    Brush(resources,L"AccentFillColorDefaultBrush",fill);
    Brush(resources,L"AccentFillColorSecondaryBrush",hover);
    Brush(resources,L"AccentFillColorTertiaryBrush",pressed);
    Brush(resources,L"AccentFillColorSelectedTextBackgroundBrush",fill);
    Brush(resources,L"AccentTextFillColorPrimaryBrush",text);
    Brush(resources,L"AccentTextFillColorSecondaryBrush",dark?Light[1]:Dark[0]);
    Brush(resources,L"AccentTextFillColorTertiaryBrush",dark?Light[1]:Dark[1]);
    Brush(resources,L"TextOnAccentFillColorPrimaryBrush",onAccent);
    Brush(resources,L"TextOnAccentFillColorSecondaryBrush",onAccent);
    Brush(resources,L"TextOnAccentFillColorDefaultBrush",onAccent);
    Brush(resources,L"TextOnAccentFillColorSelectedTextBrush",onAccent);
    Brush(resources,L"AccentAAFillColorDefaultBrush",fill);
    Brush(resources,L"AccentAAFillColorSecondaryBrush",hover);
    Brush(resources,L"AccentAAFillColorTertiaryBrush",pressed);
    Brush(resources,L"TextOnAccentAAFillColorPrimaryBrush",onAccent);
    Brush(resources,L"TextOnAccentAAFillColorSecondaryBrush",onAccent);
    Brush(resources,L"FocusStrokeColorOuterBrush",dark?Light[2]:AccentColor);
    Brush(resources,L"FocusStrokeColorInnerBrush",dark?Black:White);
    Brush(resources,L"SessionsAccentBrush",fill);
    Brush(resources,L"SessionsOnAccentBrush",onAccent);
    Brush(resources,L"SessionsAccentSubtleBrush",dark?Mix(AccentColor,Black,55):Mix(AccentColor,White,92));
    ControlAliases(resources);
    return resources;
}

ResourceDictionary HighContrast() {
    // Use dynamic system colors in contrast themes. The brand teal is not allowed
    // to replace the user's chosen high-contrast text, controls, or focus colors.
    std::wstring markup=LR"(<ResourceDictionary xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'>
<SolidColorBrush x:Key='AccentFillColorDefaultBrush' Color='{ThemeResource SystemColorWindowColor}'/>
<SolidColorBrush x:Key='AccentFillColorSecondaryBrush' Color='{ThemeResource SystemColorWindowColor}'/>
<SolidColorBrush x:Key='AccentFillColorTertiaryBrush' Color='{ThemeResource SystemColorWindowColor}'/>
<SolidColorBrush x:Key='AccentFillColorSelectedTextBackgroundBrush' Color='{ThemeResource SystemColorWindowColor}'/>
<SolidColorBrush x:Key='AccentTextFillColorPrimaryBrush' Color='{ThemeResource SystemColorWindowTextColor}'/>
<SolidColorBrush x:Key='AccentTextFillColorSecondaryBrush' Color='{ThemeResource SystemColorWindowTextColor}'/>
<SolidColorBrush x:Key='AccentTextFillColorTertiaryBrush' Color='{ThemeResource SystemColorWindowTextColor}'/>
<SolidColorBrush x:Key='TextOnAccentFillColorPrimaryBrush' Color='{ThemeResource SystemColorWindowTextColor}'/>
<SolidColorBrush x:Key='TextOnAccentFillColorSecondaryBrush' Color='{ThemeResource SystemColorWindowTextColor}'/>
<SolidColorBrush x:Key='TextOnAccentFillColorDefaultBrush' Color='{ThemeResource SystemColorWindowTextColor}'/>
<SolidColorBrush x:Key='TextOnAccentFillColorSelectedTextBrush' Color='{ThemeResource SystemColorWindowTextColor}'/>
<SolidColorBrush x:Key='AccentAAFillColorDefaultBrush' Color='{ThemeResource SystemColorWindowColor}'/>
<SolidColorBrush x:Key='AccentAAFillColorSecondaryBrush' Color='{ThemeResource SystemColorWindowColor}'/>
<SolidColorBrush x:Key='AccentAAFillColorTertiaryBrush' Color='{ThemeResource SystemColorWindowColor}'/>
<SolidColorBrush x:Key='TextOnAccentAAFillColorPrimaryBrush' Color='{ThemeResource SystemColorWindowTextColor}'/>
<SolidColorBrush x:Key='TextOnAccentAAFillColorSecondaryBrush' Color='{ThemeResource SystemColorWindowTextColor}'/>
<SolidColorBrush x:Key='FocusStrokeColorOuterBrush' Color='{ThemeResource SystemColorWindowTextColor}'/>
<SolidColorBrush x:Key='FocusStrokeColorInnerBrush' Color='{ThemeResource SystemColorWindowColor}'/>
<SolidColorBrush x:Key='SessionsAccentBrush' Color='{ThemeResource SystemColorHighlightColor}'/>
<SolidColorBrush x:Key='SessionsOnAccentBrush' Color='{ThemeResource SystemColorHighlightTextColor}'/>
<SolidColorBrush x:Key='SessionsAccentSubtleBrush' Color='{ThemeResource SystemColorWindowColor}'/>
 )";
    // Match the bundled generic.xaml HighContrast aliases rather than reusing
    // the teal theme's aliases. Checked, hover and pressed foreground/fill
    // pairs must follow the user's system contrast colors independently.
    auto nativeBrush=[&](wchar_t const* key,wchar_t const* color) {
        markup+=L"<SolidColorBrush x:Key='"+std::wstring(key)+L"' Color='{ThemeResource "+color+L"}'/>";
    };
    for(auto key:{L"AccentButtonBackgroundPressed",L"AccentButtonForegroundPointerOver",L"CheckBoxCheckBackgroundFillCheckedPressed",L"CheckBoxCheckBackgroundStrokeCheckedPressed",L"CheckBoxCheckGlyphForegroundCheckedPointerOver",L"RadioButtonOuterEllipseCheckedFillPointerOver",L"RadioButtonOuterEllipseCheckedFillPressed",L"RadioButtonOuterEllipseCheckedStrokePressed",L"SystemControlBackgroundAccentBrush",L"ToggleButtonForegroundCheckedPointerOver",L"ToggleSwitchFillOnPointerOver",L"ToggleSwitchFillOnPressed"})
        nativeBrush(key,L"SystemColorButtonFaceColor");
    for(auto key:{L"AccentButtonBackgroundPointerOver",L"CheckBoxCheckBackgroundFillCheckedPointerOver",L"CheckBoxCheckBackgroundStrokeCheckedPointerOver",L"CheckBoxCheckGlyphForegroundCheckedPressed",L"RadioButtonCheckGlyphFillPointerOver",L"RadioButtonCheckGlyphFillPressed",L"RadioButtonOuterEllipseCheckedStrokePointerOver",L"SliderThumbBackground",L"SystemControlForegroundAccentBrush",L"ToggleButtonBackgroundCheckedPointerOver",L"ToggleSwitchKnobFillOnPointerOver",L"ToggleSwitchKnobFillOnPressed",L"ToggleSwitchStrokeOnPointerOver",L"ToggleSwitchStrokeOnPressed",L"TreeViewItemSelectionIndicatorForegroundPointerOver",L"TreeViewItemSelectionIndicatorForegroundPressed"})
        nativeBrush(key,L"SystemColorButtonTextColor");
    for(auto key:{L"AccentButtonBackground",L"AccentButtonForegroundPressed",L"CheckBoxCheckBackgroundFillChecked",L"CheckBoxCheckBackgroundFillIndeterminate",L"CheckBoxCheckBackgroundFillIndeterminatePressed",L"CheckBoxCheckBackgroundStrokeChecked",L"CheckBoxCheckBackgroundStrokeIndeterminate",L"CheckBoxCheckBackgroundStrokeIndeterminatePointerOver",L"CheckBoxCheckBackgroundStrokeIndeterminatePressed",L"CheckBoxCheckGlyphForegroundIndeterminatePointerOver",L"ComboBoxItemPillFillBrush",L"GridViewItemSelectedBorderBrush",L"HyperlinkButtonForegroundPressed",L"HyperlinkForegroundPressed",L"InfoBadgeBackground",L"ItemContainerSelectionVisualBackground",L"ProgressBarForeground",L"ProgressRingForegroundThemeBrush",L"RadioButtonCheckGlyphFill",L"RadioButtonOuterEllipseCheckedStroke",L"SelectorBarItemPillFill",L"SliderThumbBackgroundPointerOver",L"SliderTrackValueFill",L"SliderTrackValueFillPointerOver",L"SliderTrackValueFillPressed",L"SystemControlHighlightAccentBrush",L"TextControlBorderBrushFocused",L"TextControlElevationBorderFocusedBrush",L"TextControlSelectionHighlightColor",L"ToggleButtonBackgroundChecked",L"ToggleButtonForegroundCheckedPressed",L"ToggleSwitchFillOn",L"ToggleSwitchStrokeOn",L"TreeViewItemSelectionIndicatorForeground"})
        nativeBrush(key,L"SystemColorHighlightColor");
    for(auto key:{L"AccentButtonForeground",L"CheckBoxCheckBackgroundFillIndeterminatePointerOver",L"CheckBoxCheckGlyphForegroundChecked",L"CheckBoxCheckGlyphForegroundIndeterminate",L"CheckBoxCheckGlyphForegroundIndeterminatePressed",L"ListViewItemSelectionIndicatorBrush",L"ListViewItemSelectionIndicatorPointerOverBrush",L"ListViewItemSelectionIndicatorPressedBrush",L"NavigationViewSelectionIndicatorForeground",L"RadioButtonOuterEllipseCheckedFill",L"SystemControlHighlightAltAccentBrush",L"ToggleButtonBackgroundCheckedPressed",L"ToggleButtonForegroundChecked",L"ToggleSwitchKnobFillOn"})
        nativeBrush(key,L"SystemColorHighlightTextColor");
    for(auto key:{L"HyperlinkButtonForeground",L"HyperlinkForeground",L"SliderThumbBackgroundPressed",L"SystemControlHyperlinkTextBrush"})
        nativeBrush(key,L"SystemColorHotlightColor");
    for(auto key:{L"GridViewItemCheckBoxSelectedBrush",L"ListViewItemCheckBoxSelectedBrush",L"SystemControlFocusVisualSecondaryBrush"})
        nativeBrush(key,L"SystemColorWindowColor");
    for(auto key:{L"HyperlinkButtonForegroundPointerOver",L"HyperlinkForegroundPointerOver",L"SystemControlFocusVisualPrimaryBrush"})
        nativeBrush(key,L"SystemColorWindowTextColor");
    markup+=L"</ResourceDictionary>";
    return Markup::XamlReader::Load(markup).as<ResourceDictionary>();
}
}

void ApplyAppTheme(Application const& application) {
    auto resources=application.Resources();
    if(resources.HasKey(box_value(L"SessionsThemeApplied")))return;
    ResourceDictionary theme;
    theme.ThemeDictionaries().Insert(box_value(L"Light"),Theme(false));
    theme.ThemeDictionaries().Insert(box_value(L"Default"),Theme(true));
    theme.ThemeDictionaries().Insert(box_value(L"HighContrast"),HighContrast());
    resources.MergedDictionaries().Append(theme);
    Put(resources,L"SessionsThemeApplied",box_value(true));
}
}
