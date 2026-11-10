#pragma once
#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// A plain window holding an Edge WebView2 browser, used for signing in to
// Claude, ChatGPT and Gemini and for reading chat history with those
// sign-ins. All browsers share one profile folder, so a sign-in in the
// visible window is available to the hidden one that syncs. Passwords are
// typed into the sites themselves; Sessions never sees them.
// Created and used on the UI thread.
namespace sessions {

class WebBrowser {
public:
    using MessageHandler = std::function<void(std::wstring const& json)>;
    using LoadedHandler = std::function<void(bool succeeded, std::wstring const& url)>;

    WebBrowser();
    ~WebBrowser();
    WebBrowser(WebBrowser const&) = delete;
    WebBrowser& operator=(WebBrowser const&) = delete;

    // The profile folder (cookies and site storage) for every browser.
    static void SetProfileFolder(std::filesystem::path folder);

    // Creates the window (hidden unless `visible`) and its browser; `ready`
    // runs once the browser can navigate, or with false if WebView2 failed.
    void Create(wchar_t const* title, bool visible, std::function<void(bool)> ready);
    bool Ready() const;
    void Navigate(std::wstring const& url);
    void Run(std::wstring const& script);
    void Post(std::wstring const& json);
    void Show();
    void Hide();

    void OnMessage(MessageHandler handler) { message = std::move(handler); }
    void OnLoaded(LoadedHandler handler) { loaded = std::move(handler); }
    void OnClosed(std::function<void()> handler) { closed = std::move(handler); }

private:
    struct Browser;  // the WebView2 objects (kept out of this header: WRL's
                     // Microsoft namespace collides with C++/WinRT's)
    HWND window = nullptr;
    std::unique_ptr<Browser> browser;
    MessageHandler message;
    LoadedHandler loaded;
    std::function<void()> closed;

    void Resize();
    static LRESULT CALLBACK Procedure(HWND handle, UINT msg, WPARAM wParam, LPARAM lParam);
};

}  // namespace sessions
