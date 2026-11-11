#pragma once
#include "Index.h"
#include "WebBrowser.h"
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>
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

    // `profile`: the browser profile folder; by default Browser beside the executable.
    WebSync(Index& index, Report report, std::filesystem::path profile = {});
    ~WebSync();

    // Opens a visible browser at the service's sign-in page. Closing that
    // window starts a sync.
    void SignIn(Tool tool);
    // Signs Sessions' browser out of a site so the next sign-in can choose another
    // account. For ChatGPT and Gemini this also signs out of Google, which they
    // sign in through. `done` receives a sentence for the person.
    void SignOut(Tool tool, std::function<void(std::wstring)> done);
    void SyncAll() { Sync(std::nullopt); }
    // Syncs one site now (ahead of anything queued), or every site.
    void Sync(std::optional<Tool> only);
    // What is happening with this site right now ("Reading chats · 42 so far"), or empty.
    std::wstring Live(Tool tool) const;
    bool Running() const { return running; }
    // What Settings shows for each site, from the last check (kept across restarts).
    struct SiteStatus {
        Tool tool = Tool::ClaudeWeb;
        bool checked = false;     // ever checked
        bool signedIn = false;
        std::wstring who;         // accounts found, by their names in Sessions
        std::wstring problem;     // why not signed in, or what failed
        int64_t checkedAt = 0;
        int chats = 0;
    };
    std::vector<SiteStatus> Statuses();

    struct Service;  // one per site, defined in WebSync.cpp

private:
    Index& index;
    Report report;
    std::unique_ptr<WebBrowser> worker, signIn;
    std::deque<Service const*> queue;
    Service const* current = nullptr;
    bool running = false, injected = false, changed = false, rerun = false;
    int stored = 0, failures = 0;
    std::wstring lastError;
    struct Outcome {
        bool signedIn = false;
        std::set<std::wstring> who;
        std::wstring problem;
        int read = 0;
    };
    std::map<Tool, Outcome> outcomes;     // this run, per site
    std::set<std::string> seenAccounts;   // accounts already read this run
    void SaveOutcomes();
    std::map<Tool, std::wstring> live;
    UINT_PTR closeTimer = 0;
    static void CALLBACK CloseSignIn(HWND, UINT, UINT_PTR, DWORD);
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
