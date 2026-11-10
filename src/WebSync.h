#pragma once
#include "Index.h"
#include "WebBrowser.h"
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>

// Keeps Claude, ChatGPT and Gemini web conversations in the index by reading
// them from each site's backend inside a signed-in browser. Runs on the UI
// thread; the browsers do the network work.
namespace sessions {

class WebSync {
public:
    // status: progress text (empty when idle); changed: conversations were added
    // or updated; finished: the whole run is over.
    using Report = std::function<void(std::wstring const& status, bool changed, bool finished)>;

    WebSync(Index& index, Report report);
    ~WebSync();

    // Opens a visible browser at the service's sign-in page. Closing that
    // window starts a sync.
    void SignIn(Tool tool);
    void SyncAll();
    bool Running() const { return running; }
    // One line per service: last result, for the settings pane.
    std::wstring Summary() const;

    struct Service;  // one per site, defined in WebSync.cpp

private:
    Index& index;
    Report report;
    std::unique_ptr<WebBrowser> worker, signIn;
    std::deque<Service const*> queue;
    Service const* current = nullptr;
    bool running = false, injected = false, changed = false;
    int stored = 0, failures = 0;
    std::wstring lastError;
    std::map<Tool, std::wstring> results;
    UINT_PTR watchdog = 0;
    std::chrono::steady_clock::time_point lastActivity;

    void Next();
    void Finish(std::wstring const& result);
    void OnLoaded(bool succeeded, std::wstring const& url);
    void OnMessage(std::wstring const& json);
    void Touch() { lastActivity = std::chrono::steady_clock::now(); }
    static void CALLBACK Watchdog(HWND, UINT, UINT_PTR, DWORD);
};

}  // namespace sessions
