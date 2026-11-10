#include "TrayIcon.h"
#include "ResourceIds.h"
#include <shellapi.h>
#include <cwchar>

namespace sessions {

namespace {
constexpr UINT TrayMessage = WM_APP + 41;
constexpr UINT TrayId = 1;
constexpr wchar_t ClassName[] = L"SessionsTrayWindow";
}  // namespace

TrayIcon::~TrayIcon() { Remove(); }

void TrayIcon::Create(std::function<void()> onClick, std::function<void()> onRightClick) {
    click = std::move(onClick);
    rightClick = std::move(onRightClick);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = Procedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = ClassName;
    RegisterClassExW(&windowClass);
    // A hidden top-level window (not message-only) so it hears TaskbarCreated
    // when Explorer restarts and the icon has to be added again.
    window = CreateWindowExW(0, ClassName, L"Sessions tray", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_SESSIONS), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    Add();
}

bool TrayIcon::Add() {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window;
    data.uID = TrayId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = TrayMessage;
    data.hIcon = icon;
    wcsncpy_s(data.szTip, tip, _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    return true;
}

void TrayIcon::SetTip(wchar_t const* text) {
    wcsncpy_s(tip, text, _TRUNCATE);
    if (!window) return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window;
    data.uID = TrayId;
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(data.szTip, tip, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::Remove() {
    if (window) {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window;
        data.uID = TrayId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        DestroyWindow(window);
        window = nullptr;
    }
    if (icon) {
        DestroyIcon(icon);
        icon = nullptr;
    }
}

LRESULT CALLBACK TrayIcon::Procedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam) {
    auto self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (self && message == TrayMessage) {
        // NOTIFYICON_VERSION_4: the event is in the low word of lParam.
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            // Clicking the icon gives this process the right to bring a window forward.
            if (self->click) self->click();
            return 0;
        case WM_CONTEXTMENU:
            if (self->rightClick) self->rightClick();
            return 0;
        }
        return 0;
    }
    if (self && self->taskbarCreated && message == self->taskbarCreated) {
        self->Add();
        return 0;
    }
    return DefWindowProcW(handle, message, wParam, lParam);
}

}  // namespace sessions
