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
    // `slot` 2 and up: an extra sign-in to ChatGPT only, in its own profile folder.
    WebSync(Index& index, Report report, std::filesystem::path profile = {}, int slot = 1);
    int Slot() const { return slot; }
    ~WebSync();

    // Opens a visible browser at the service's sign-in page. Closing that
    // window starts a sync.
    void SignIn(Tool tool);
    // Signs Sessions' browser out of a site so the next sign-in can choose another
    // account. For ChatGPT and Gemini this also signs out of Google, which they
    // sign in through. `done` receives a sentence for the person.
    void SignOut(Tool tool, std::function<void(std::wstring)> done);
    // Checks every site that is due: not checked within its interval (kept across
    // restarts) and not paused after a "slow down". `force` checks sites that are not due too.
    void SyncAll(bool force = false) {
        forced = force;
        Sync(std::nullopt);
        forced = false;
    }
    // Syncs one site now (ahead of anything queued), or every site.
    void Sync(std::optional<Tool> only);
    // What is happening with this site right now ("Reading chats · 42 so far"), or empty.
    std::wstring Live(Tool tool) const;
    // Signed in according to the check running right now (before it is saved).
    bool SignedInNow(Tool tool) const;
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
    std::filesystem::path profile;
    int slot = 1;
    std::wstring Key(std::wstring const& base, Tool tool) const;  // meta key for this slot
    std::unique_ptr<WebBrowser> NewBrowser();
    bool forced = false;
    bool Due(Tool tool);     // long enough since this site was last checked
    bool Paused(Tool tool);  // the site asked Sessions to slow down; leave it alone until then
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
    // A visit reads at most a batch of chats, then the other sites get a turn and
    // the site comes back for the rest; so one big history never holds up the others.
    bool moreToRead = false;
    std::string visitAccount;
    std::map<std::string, int64_t> listedUpdated;  // the list's time for each chat, kept as its version
    int rounds = 0;
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
