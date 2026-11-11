#include "WebBrowser.h"
#include "ResourceIds.h"
#include <objbase.h>  // WIN32_LEAN_AND_MEAN leaves out the COM `interface` keyword WebView2.h needs
#include <WebView2.h>
#include <wrl/event.h>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace sessions {

namespace {

constexpr wchar_t ClassName[] = L"SessionsWebBrowser";
std::filesystem::path profileFolder;

// One browser environment (Edge process group) shared by every window.
ComPtr<ICoreWebView2Environment> environment;
bool environmentStarting = false;
std::vector<std::function<void(ICoreWebView2Environment*)>> environmentWaiters;

void WithEnvironment(std::function<void(ICoreWebView2Environment*)> use) {
    if (environment) {
        use(environment.Get());
        return;
    }
    environmentWaiters.push_back(std::move(use));
    if (environmentStarting) return;
    environmentStarting = true;
    std::filesystem::create_directories(profileFolder);
    HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, profileFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* created) -> HRESULT {
                environmentStarting = false;
                if (SUCCEEDED(result)) environment = created;
                auto waiters = std::move(environmentWaiters);
                environmentWaiters.clear();
                for (auto& waiter : waiters) waiter(environment.Get());
                return S_OK;
            })
            .Get());
    if (FAILED(started)) {
        environmentStarting = false;
        auto waiters = std::move(environmentWaiters);
        environmentWaiters.clear();
        for (auto& waiter : waiters) waiter(nullptr);
    }
}

}  // namespace

struct WebBrowser::Browser {
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
};

WebBrowser::WebBrowser() = default;

bool WebBrowser::Ready() const { return browser && browser->view; }

void WebBrowser::SetProfileFolder(std::filesystem::path folder) { profileFolder = std::move(folder); }

WebBrowser::~WebBrowser() {
    if (browser && browser->controller) browser->controller->Close();
    browser.reset();
    if (window) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        DestroyWindow(window);
    }
}

void WebBrowser::Create(wchar_t const* title, bool visible, std::function<void(bool)> ready) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = Procedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SESSIONS));
    windowClass.lpszClassName = ClassName;
    RegisterClassExW(&windowClass);
    const double scale = GetDpiForSystem() / 96.0;
    window = CreateWindowExW(0, ClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, int(1100 * scale),
                             int(820 * scale), nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    browser = std::make_unique<Browser>();
    if (visible) Show();

    WithEnvironment([this, ready](ICoreWebView2Environment* env) {
        if (!env || !window) {
            ready(false);
            return;
        }
        env->CreateCoreWebView2Controller(
            window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, ready](HRESULT result, ICoreWebView2Controller* created) -> HRESULT {
                            if (FAILED(result) || !created) {
                                ready(false);
                                return S_OK;
                            }
                            auto& controller = browser->controller;
                            auto& view = browser->view;
                            controller = created;
                            controller->get_CoreWebView2(&view);
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(view->get_Settings(&settings))) {
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                            }
                            view->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR json = nullptr;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                                            std::wstring text(json);
                                            CoTaskMemFree(json);
                                            if (message) message(text);
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                nullptr);
                            // Pages that open a new window (an account switch, a sign-in
                            // popup) load in this same window instead, so there is only
                            // ever one sign-in window to follow.
                            view->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                                            args->put_Handled(TRUE);
                                            sender->Navigate(uri);
                                            CoTaskMemFree(uri);
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                nullptr);
                            view->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        BOOL success = FALSE;
                                        args->get_IsSuccess(&success);
                                        LPWSTR url = nullptr;
                                        std::wstring address;
                                        if (SUCCEEDED(sender->get_Source(&url)) && url) {
                                            address = url;
                                            CoTaskMemFree(url);
                                        }
                                        if (loaded) loaded(success != FALSE, address);
                                        return S_OK;
                                    })
                                    .Get(),
                                nullptr);
                            Resize();
                            ready(true);
                            return S_OK;
                        })
                        .Get());
    });
}

void WebBrowser::Navigate(std::wstring const& url) {
    if (Ready()) browser->view->Navigate(url.c_str());
}

void WebBrowser::Run(std::wstring const& script) {
    if (Ready()) browser->view->ExecuteScript(script.c_str(), nullptr);
}

void WebBrowser::Post(std::wstring const& json) {
    if (Ready()) browser->view->PostWebMessageAsJson(json.c_str());
}

void WebBrowser::Show() {
    if (!window) return;
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    if (browser && browser->controller) browser->controller->put_IsVisible(TRUE);
}

void WebBrowser::SetTitle(std::wstring const& title) {
    if (window) SetWindowTextW(window, title.c_str());
}

void WebBrowser::Hide() {
    if (window) ShowWindow(window, SW_HIDE);
}

void WebBrowser::ClearSites(std::vector<std::wstring> const& domains, std::function<void(int)> done) {
    ComPtr<ICoreWebView2_2> view2;
    ComPtr<ICoreWebView2CookieManager> cookies;
    if (!Ready() || FAILED(browser->view.As(&view2)) || FAILED(view2->get_CookieManager(&cookies))) {
        done(0);
        return;
    }
    // All cookies in the profile, then keep those whose domain is one of the sites.
    auto matches = [domains](std::wstring domain) {
        while (!domain.empty() && domain.front() == L'.') domain.erase(domain.begin());
        for (auto const& site : domains)
            if (domain == site || (domain.size() > site.size() && domain.compare(domain.size() - site.size(), site.size(), site) == 0 &&
                                   domain[domain.size() - site.size() - 1] == L'.'))
                return true;
        return false;
    };
    cookies->GetCookies(nullptr, Callback<ICoreWebView2GetCookiesCompletedHandler>(
                                     [cookies, matches, done](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
                                         int deleted = 0;
                                         UINT count = 0;
                                         if (SUCCEEDED(result) && list && SUCCEEDED(list->get_Count(&count))) {
                                             for (UINT i = 0; i < count; ++i) {
                                                 ComPtr<ICoreWebView2Cookie> cookie;
                                                 LPWSTR domain = nullptr;
                                                 if (FAILED(list->GetValueAtIndex(i, &cookie)) || FAILED(cookie->get_Domain(&domain))) continue;
                                                 bool remove = domain && matches(domain);
                                                 CoTaskMemFree(domain);
                                                 if (remove && SUCCEEDED(cookies->DeleteCookie(cookie.Get()))) ++deleted;
                                             }
                                         }
                                         done(deleted);
                                         return S_OK;
                                     })
                                     .Get());
}

void WebBrowser::Resize() {
    if (!browser || !browser->controller || !window) return;
    RECT bounds{};
    GetClientRect(window, &bounds);
    browser->controller->put_Bounds(bounds);
}

LRESULT CALLBACK WebBrowser::Procedure(HWND handle, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto self = reinterpret_cast<WebBrowser*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (self) self->Resize();
        return 0;
    case WM_CLOSE:
        // Closing only hides the window; the browser and its sign-ins stay.
        ShowWindow(handle, SW_HIDE);
        if (self && self->closed) self->closed();
        return 0;
    }
    return DefWindowProcW(handle, msg, wParam, lParam);
}

}  // namespace sessions
