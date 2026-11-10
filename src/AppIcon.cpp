#include "AppIcon.h"
#include "ResourceIds.h"
#include <microsoft.ui.xaml.window.h>

namespace sessions {
namespace {
HICON LoadApplicationIcon(HMODULE module,UINT dpi,int widthMetric,int heightMetric) {
    auto width=GetSystemMetricsForDpi(widthMetric,dpi);
    auto height=GetSystemMetricsForDpi(heightMetric,dpi);
    winrt::check_bool(width>0&&height>0);
    // Omitting LR_SHARED gives each requested size its own owned handle.
    auto icon=static_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(IDI_SESSIONS),IMAGE_ICON,width,height,LR_DEFAULTCOLOR));
    if(!icon)winrt::throw_last_error();
    return icon;
}
}

AppIcon::~AppIcon(){Reset();}

void AppIcon::Set(winrt::Microsoft::UI::Xaml::Window const& owner) {
    HWND handle{};
    winrt::check_hresult(owner.as<IWindowNative>()->get_WindowHandle(&handle));
    auto dpi=GetDpiForWindow(handle);
    winrt::check_bool(dpi!=0);
    auto module=GetModuleHandleW(nullptr);
    if(!module)winrt::throw_last_error();
    auto nextLarge=LoadApplicationIcon(module,dpi,SM_CXICON,SM_CYICON);
    HICON nextSmall{};
    try {nextSmall=LoadApplicationIcon(module,dpi,SM_CXSMICON,SM_CYSMICON);}
    catch(...){DestroyIcon(nextLarge);throw;}
    Reset();
    window=handle;largeIcon=nextLarge;smallIcon=nextSmall;
    SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(largeIcon));
    SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(smallIcon));
}

void AppIcon::Reset() noexcept {
    // Only detach icons still owned by us; another caller may have replaced them.
    if(window&&IsWindow(window)){
        if(reinterpret_cast<HICON>(SendMessageW(window,WM_GETICON,ICON_BIG,0))==largeIcon)
            SendMessageW(window,WM_SETICON,ICON_BIG,0);
        if(reinterpret_cast<HICON>(SendMessageW(window,WM_GETICON,ICON_SMALL,0))==smallIcon)
            SendMessageW(window,WM_SETICON,ICON_SMALL,0);
    }
    if(smallIcon&&smallIcon!=largeIcon)DestroyIcon(smallIcon);
    if(largeIcon)DestroyIcon(largeIcon);
    window=nullptr;largeIcon=nullptr;smallIcon=nullptr;
}
}
