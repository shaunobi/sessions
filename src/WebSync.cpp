#include "WebSync.h"
#include "Json.h"
#include "Text.h"
#include "WebAgents.h"
#include <cstdio>
#include <cwctype>
#include <utility>
#include <unordered_set>

namespace sessions {

namespace {

constexpr size_t MaxMessageBytes = 64 * 1024;
WebSync* activeSync = nullptr;  // for the watchdog timer callback

std::string JsonString(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                out += escaped;
            } else {
                out.push_back(c);
            }
        }
    }
    return out + "\"";
}

}  // namespace

struct WebSync::Service {
    Tool tool;
    wchar_t const* name;
    wchar_t const* syncUrl;    // a light page on the site's origin, or the app page when the script needs it
    wchar_t const* host;       // the sync script runs only once the browser is on this host
    wchar_t const* signInUrl;
    wchar_t const* agent;
    Family accountFamily;
};

// Claude first, then ChatGPT, then Gemini.
static WebSync::Service const Services[] = {
    {Tool::ClaudeWeb, L"Claude", L"https://claude.ai/robots.txt", L"claude.ai", L"https://claude.ai/login", agents::Claude, Family::Claude},
    {Tool::ChatGptWeb, L"ChatGPT", L"https://chatgpt.com/robots.txt", L"chatgpt.com", L"https://chatgpt.com/auth/login", agents::ChatGpt,
     Family::Codex},
    // Google lets several accounts be signed in at once; each has its own /u/N path.
    // Unused slots send the browser back to the first account, which is then skipped.
    {Tool::Gemini, L"Gemini", L"https://gemini.google.com/u/0/app", L"gemini.google.com", L"https://gemini.google.com/app", agents::Gemini,
     Family::Web},
    {Tool::Gemini, L"Gemini", L"https://gemini.google.com/u/1/app", L"gemini.google.com", L"https://gemini.google.com/app", agents::Gemini,
     Family::Web},
    {Tool::Gemini, L"Gemini", L"https://gemini.google.com/u/2/app", L"gemini.google.com", L"https://gemini.google.com/app", agents::Gemini,
     Family::Web},
};

namespace {
std::wstring MetaKey(Tool tool) { return L"web_status_" + std::to_wstring(int(tool)); }
}

WebSync::WebSync(Index& database, Report onReport, std::filesystem::path profile) : index(database), report(std::move(onReport)) {
    WebBrowser::SetProfileFolder(profile.empty() ? ExecutableFolder() / L"Browser" : profile);
}

WebSync::~WebSync() {
    if (watchdog) KillTimer(nullptr, watchdog);
    if (activeSync == this) activeSync = nullptr;
}

void WebSync::SignIn(Tool tool) {
    Service const* service = nullptr;
    for (auto const& s : Services)
        if (s.tool == tool && !service) service = &s;
    if (!service) return;
    std::wstring url = service->signInUrl;
    if (!signIn) {
        signIn = std::make_unique<WebBrowser>();
        signIn->OnClosed([this] {
            if (closeTimer) KillTimer(nullptr, closeTimer);
            closeTimer = 0;
        });
        // Once a site shows a page other than its sign-in pages, sign-in is done:
        // sync straight away rather than waiting for the window to close.
        signIn->OnLoaded([this](bool ok, std::wstring const& address) {
            if (!ok) return;
            std::wstring lower = address;
            for (auto& ch : lower) ch = wchar_t(towlower(ch));
            for (auto const& s : Services) {
                if (lower.find(s.host) == std::wstring::npos) continue;
                for (auto marker : {L"/login", L"/auth", L"/signin", L"/sign-in", L"/oauth", L"/magic-link", L"accounts.google"})
                    if (lower.find(marker) != std::wstring::npos) {
                        signIn->SetTitle(L"Sessions \u00B7 sign in to " + std::wstring(s.name));
                        return;
                    }
                // Past the sign-in pages: sync this site right away and close the
                // window shortly after, so there is nothing more to do.
                signIn->SetTitle(L"Sessions \u00B7 signed in to " + std::wstring(s.name) + L" \u2014 syncing; this window closes itself");
                Sync(s.tool);
                activeSync = this;
                if (closeTimer) KillTimer(nullptr, closeTimer);
                closeTimer = SetTimer(nullptr, 0, 2500, CloseSignIn);
                return;
            }
        });
        signIn->Create(L"Sessions · sign in", true, [this, url](bool ok) {
            if (ok) signIn->Navigate(url);
            else report(L"Could not start the sign-in browser (Microsoft Edge WebView2 is missing).", false, true);
        });
    } else {
        signIn->Show();
        signIn->Navigate(url);
    }
}

void WebSync::SignOut(Tool tool, std::function<void(std::wstring)> done) {
    std::vector<std::wstring> domains;
    std::wstring name;
    switch (tool) {
    case Tool::ClaudeWeb:
        domains = {L"claude.ai", L"anthropic.com"};
        name = L"Claude";
        break;
    case Tool::ChatGptWeb:
        domains = {L"chatgpt.com", L"openai.com", L"google.com"};
        name = L"ChatGPT and Google";
        break;
    default:
        domains = {L"gemini.google.com", L"google.com", L"youtube.com"};
        name = L"Google (Gemini)";
        break;
    }
    auto finish = [this, tool, name, done](int deleted) {
        // Forget the remembered status so Settings doesn't show the old account.
        try { index.SetMetaValue(Utf8(L"web_status_" + std::to_wstring(int(tool))), ""); } catch (...) {}
        if (tool == Tool::ChatGptWeb || tool == Tool::Gemini)
            for (auto other : {Tool::ChatGptWeb, Tool::Gemini})
                if (other != tool) try { index.SetMetaValue(Utf8(L"web_status_" + std::to_wstring(int(other))), ""); } catch (...) {}
        done(deleted ? L"Signed out of " + name + L". Press its button to sign in with the account you want."
                     : L"Sessions' browser wasn't signed in to " + name + L".");
    };
    // Any browser of the shared profile can clear its cookies; create one if none exists yet.
    WebBrowser* browser = worker && worker->Ready() ? worker.get() : signIn && signIn->Ready() ? signIn.get() : nullptr;
    if (browser) {
        browser->ClearSites(domains, finish);
        return;
    }
    if (!worker) {
        worker = std::make_unique<WebBrowser>();
        worker->OnMessage([this](std::wstring const& json) { OnMessage(json); });
        worker->OnLoaded([this](bool ok, std::wstring const& url) { OnLoaded(ok, url); });
    }
    worker->Create(L"Sessions \u00B7 sync", false, [this, domains, finish](bool ok) {
        if (ok) worker->ClearSites(domains, finish);
        else finish(0);
    });
}

void CALLBACK WebSync::CloseSignIn(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    auto self = activeSync;
    if (!self) return;
    self->closeTimer = 0;
    if (self->signIn) self->signIn->Hide();
}

bool WebSync::SignedInNow(Tool tool) const {
    auto it = outcomes.find(tool);
    return it != outcomes.end() && it->second.signedIn;
}

std::wstring WebSync::Live(Tool tool) const {
    auto it = live.find(tool);
    return it == live.end() ? std::wstring() : it->second;
}

void WebSync::Sync(std::optional<Tool> only) {
    if (running) {
        if (!only) {
            rerun = true;  // run everything again when this run ends
            return;
        }
        if (current && current->tool == *only) return;
        // Jump the queue: this site next.
        std::deque<Service const*> rest;
        for (auto service : queue)
            if (service->tool != *only) rest.push_back(service);
        for (auto it = std::rbegin(Services); it != std::rend(Services); ++it)
            if (it->tool == *only) rest.push_front(&*it);
        queue = std::move(rest);
        outcomes.erase(*only);
        live[*only] = L"Next to sync";
        report(L"", false, false);
        return;
    }
    running = true;
    changed = false;
    outcomes.clear();
    seenAccounts.clear();
    rounds = 0;
    listedUpdated.clear();
    queue.clear();
    for (auto const& s : Services)
        if (!only || s.tool == *only) {
            queue.push_back(&s);
            live[s.tool] = L"Waiting its turn to sync";
        }
    activeSync = this;
    if (!watchdog) watchdog = SetTimer(nullptr, 0, 15000, Watchdog);
    if (!worker) {
        worker = std::make_unique<WebBrowser>();
        worker->OnMessage([this](std::wstring const& json) { OnMessage(json); });
        worker->OnLoaded([this](bool ok, std::wstring const& url) { OnLoaded(ok, url); });
        worker->Create(L"Sessions · sync", false, [this](bool ok) {
            if (!ok) {
                running = false;
                report(L"Web sync needs Microsoft Edge WebView2, which could not start.", false, true);
                return;
            }
            Next();
        });
        return;
    }
    Next();
}

void WebSync::Next() {
    if (queue.empty()) {
        current = nullptr;
        running = false;
        SaveOutcomes();
        report(L"", changed, true);
        if (std::exchange(rerun, false)) SyncAll();
        return;
    }
    current = queue.front();
    queue.pop_front();
    if (!SignedInNow(current->tool)) live[current->tool] = L"Checking sign-in";
    injected = false;
    moreToRead = false;
    visitAccount.clear();
    stored = 0;
    failures = 0;
    lastError.clear();
    Touch();
    report(std::wstring(current->name) + L" · connecting", false, false);
    worker->Navigate(current->syncUrl);
}

void WebSync::Finish(std::wstring const& result) {
    if (current) {
        auto& outcome = outcomes[current->tool];
        // Gemini also tries extra Google account slots (/u/1, /u/2); an empty slot
        // is not a problem with the sign-in.
        bool extraSlot = std::wstring(current->syncUrl).find(L"/u/0/") == std::wstring::npos &&
                         std::wstring(current->syncUrl).find(L"/u/") != std::wstring::npos;
        if (!result.empty()) {
            if (!extraSlot) outcome.problem = result;
            else if (!outcome.signedIn && outcome.problem.empty()) outcome.problem = result;
        }
        outcome.read += stored;
        // Read a batch and there is more: come back after the other sites.
        if (moreToRead && result.empty() && ++rounds < 400) {
            seenAccounts.erase(visitAccount);
            queue.push_back(current);
            live[current->tool] = L"Signed in · " + std::to_wstring(outcome.read) + L" chats read so far; the rest after the other sites";
        }
        moreToRead = false;
        // Done with this site when no more of its slots are queued.
        bool more = false;
        for (auto service : queue) more = more || service->tool == current->tool;
        if (!more) {
            live.erase(current->tool);
            SaveOutcomes();
        }
    }
    Next();
}

// Remembers each site's result (signed in as whom, or what went wrong) so
// Settings shows it after a restart too.
void WebSync::SaveOutcomes() {
    for (auto const& [tool, outcome] : outcomes) {
        std::wstring who;
        for (auto const& name : outcome.who) who += (who.empty() ? L"" : L", ") + name;
        std::wstring value = std::wstring(outcome.signedIn ? L"1" : L"0") + L"\x1F" + who + L"\x1F" +
                             (outcome.signedIn && outcome.problem.rfind(L"not signed in", 0) == 0 ? std::wstring() : outcome.problem) +
                             L"\x1F" + std::to_wstring(NowMs());
        try {
            index.SetMetaValue(Utf8(MetaKey(tool)), Utf8(value));
        } catch (...) {
        }
    }
}

void WebSync::OnLoaded(bool succeeded, std::wstring const& url) {
    if (!current || injected) return;
    Touch();
    if (url.find(current->host) == std::wstring::npos) {
        // Sent somewhere else, typically a sign-in page.
        Finish(L"not signed in (the site sent the browser to " + url + L")");
        return;
    }
    if (!succeeded) {
        Finish(L"could not reach the site");
        return;
    }
    injected = true;
    worker->Run(std::wstring(L"(function(){\n") + agents::Common + current->agent + L"\n})();");
}

void WebSync::OnMessage(std::wstring const& text) {
    if (!current) return;
    Touch();
    json::Value message;
    if (!json::Parse(Utf8(text), message) || !message.IsObject()) return;
    auto type = message.Str("type");

    if (type == "identity") {
        // Signed in: say so straight away, before the (possibly long) listing.
        auto& outcome = outcomes[current->tool];
        outcome.signedIn = true;
        std::wstring who = Wide(message.Str("who"));
        live[current->tool] = who.empty() ? L"Signed in \u00B7 finding your chats" : L"Signed in as " + who + L" \u00B7 finding your chats";
        report(L"", false, false);
    } else if (type == "status") {
        std::wstring text = Wide(message.Str("text"));
        // "ChatGPT · 40 of 300" → shown in the site's row as progress.
        auto dot = text.find(L" \u00B7 ");
        std::wstring detail = dot == std::wstring::npos ? text : text.substr(dot + 3);
        if (outcomes[current->tool].signedIn && !detail.empty()) {
            if (detail == L"listing") detail = L"finding your chats";
            else if (detail.find(L" of ") != std::wstring::npos) detail = L"reading chats, " + detail;
            live[current->tool] = L"Signed in \u00B7 " + detail;
        }
        report(text, false, false);
    } else if (type == "signedOut") {
        auto detail = message.Str("detail");
        Finish(L"not signed in" + (detail.empty() ? std::wstring() : L" (" + Wide(detail) + L")"));
    } else if (type == "error") {
        ++failures;
        lastError = Wide(message.Str("text"));
        if (auto fatal = message.Get("fatal"); fatal && fatal->IsTrue()) Finish(L"error: " + lastError);
    } else if (type == "list") {
        std::string account(message.Str("account"));
        index.NameAccount(account, current->accountFamily, std::string(message.Str("accountName")));
        auto& outcome = outcomes[current->tool];
        outcome.signedIn = true;
        outcome.who.insert(index.AccountName(account));
        {
            std::wstring who;
            for (auto const& name : outcome.who) who += (who.empty() ? L"" : L", ") + name;
            live[current->tool] = L"Signed in as " + who + L" \u00B7 reading chats";
            report(L"", false, false);
        }
        SaveOutcomes();  // signed in is known now; don't wait for the end of a long read
        if (!seenAccounts.insert(account).second) {
            // The same account again (an unused Google account slot): nothing more to read.
            worker->Post(L"{\"type\":\"fetch\",\"ids\":[]}");
            return;
        }
        visitAccount = account;
        // ChatGPT reads slowly (it rate-limits), so its batches are smaller.
        const int batch = current->tool == Tool::ChatGptWeb ? 120 : 200;
        int wanted = 0;
        auto known = index.KnownWeb(account);
        std::vector<std::string> present;
        std::string ids = "{\"type\":\"fetch\",\"ids\":[";
        bool first = true;
        if (auto items = message.Get("items"); items && items->IsArray()) {
            for (auto const& item : items->array) {
                std::string id(item.Str("id"));
                if (id.empty()) continue;
                present.push_back(id);
                int64_t updated = ParseIsoMs(item.Str("updated"));
                if (updated) listedUpdated[id] = updated;
                auto it = known.find(id);
                // Read conversations that are new or changed since the last sync.
                if (it == known.end() || (updated && it->second != updated)) {
                    if (++wanted > batch) {
                        moreToRead = true;
                        continue;
                    }
                    ids += (first ? "" : ",") + JsonString(id);
                    first = false;
                }
            }
        }
        if (auto complete = message.Get("complete"); complete && complete->IsTrue()) {
            Transaction transaction(index.Database());
            if (index.RemoveWebMissing(account, present) > 0) changed = true;
            transaction.Commit();
        }
        worker->Post(Wide(ids + "]}"));
    } else if (type == "conversation") {
        WebConversation c;
        c.tool = current->tool;
        c.id = message.Str("id");
        c.account = message.Str("account");
        c.title = Squash(message.Str("title"));
        c.folder = message.Str("project");
        c.created = ParseIsoMs(message.Str("created"));
        c.updated = ParseIsoMs(message.Str("updated"));
        if (auto messages = message.Get("messages"); messages && messages->IsArray()) {
            for (auto const& m : messages->array) {
                Message entry;
                entry.role = m.Str("role") == "user" ? Role::User : Role::Assistant;
                entry.timestamp = ParseIsoMs(m.Str("ts"));
                entry.text = Clip(m.Str("text"), MaxMessageBytes);
                if (entry.text.empty()) continue;
                if (entry.timestamp && (!c.created || entry.timestamp < c.created)) c.created = entry.timestamp;
                if (entry.timestamp > c.updated) c.updated = entry.timestamp;
                c.messages.push_back(std::move(entry));
            }
        }
        if (c.id.empty()) return;
        // Stored with the time the list gave, so the next sync sees it as unchanged.
        if (auto it = listedUpdated.find(c.id); it != listedUpdated.end() && it->second >= c.created) c.updated = it->second;
        try {
            Transaction transaction(index.Database());
            index.StoreWeb(c);
            transaction.Commit();
            ++stored;
            changed = true;
            {
                std::wstring who;
                for (auto const& name : outcomes[current->tool].who) who += (who.empty() ? L"" : L", ") + name;
                live[current->tool] = L"Signed in as " + who + L" \u00B7 " + std::to_wstring(stored) + L" new or changed chats read so far";
                if (stored % 5 == 0) report(L"", false, false);
            }
            if (stored % 25 == 0) report(std::wstring(current->name) + L" · " + std::to_wstring(stored) + L" conversations read", true, false);
        } catch (std::exception const& e) {
            ++failures;
            lastError = Wide(e.what());
        }
    } else if (type == "done") {
        Finish(failures ? std::to_wstring(failures) + L" conversations could not be read (last: " + lastError + L")" : std::wstring());
    }
}

void CALLBACK WebSync::Watchdog(HWND, UINT, UINT_PTR, DWORD) {
    auto self = activeSync;
    if (!self || !self->running || !self->current) return;
    // A site that stops answering for three minutes is skipped until the next sync.
    if (std::chrono::steady_clock::now() - self->lastActivity > std::chrono::minutes(3)) self->Finish(L"stopped answering");
}

std::vector<WebSync::SiteStatus> WebSync::Statuses() {
    std::vector<SiteStatus> out;
    for (auto tool : {Tool::ClaudeWeb, Tool::ChatGptWeb, Tool::Gemini}) {
        SiteStatus status;
        status.tool = tool;
        std::string saved;
        try { saved = index.MetaValue(Utf8(MetaKey(tool))); } catch (...) {}
        auto parts = std::vector<std::wstring>();
        std::wstring field;
        for (wchar_t c : Wide(saved)) {
            if (c == L'\x1F') { parts.push_back(field); field.clear(); }
            else field.push_back(c);
        }
        parts.push_back(field);
        if (parts.size() >= 4) {
            status.checked = true;
            status.signedIn = parts[0] == L"1";
            status.who = parts[1];
            status.problem = parts[2];
            status.checkedAt = _wtoi64(parts[3].c_str());
        }
        try {
            auto q = index.Database().Prepare("SELECT count(*) FROM sessions WHERE family = 3 AND tool = ?1 AND skipped = ''");
            q.Bind(1, int64_t(tool));
            if (q.Step()) status.chats = int(q.Int(0));
        } catch (...) {
        }
        out.push_back(std::move(status));
    }
    return out;
}

}  // namespace sessions
