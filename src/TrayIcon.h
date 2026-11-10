#pragma once
#include <windows.h>
#include <functional>

// The Sessions icon in the notification area (lower right of the taskbar).
// Left click and right click call back on the UI thread. Created and used on
// the UI thread, whose message loop delivers its messages.
namespace sessions {

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(TrayIcon const&) = delete;
    TrayIcon& operator=(TrayIcon const&) = delete;

    void Create(std::function<void()> onClick, std::function<void()> onRightClick);
    void Remove();
    void SetTip(wchar_t const* text);

private:
    HWND window = nullptr;
    HICON icon = nullptr;
    UINT taskbarCreated = 0;
    std::function<void()> click, rightClick;
    wchar_t tip[128] = L"Sessions";

    bool Add();
    static LRESULT CALLBACK Procedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam);
};

}  // namespace sessions
