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
    {Tool::Gemini, L"Gemini", L"https://gemini.google.com/app", L"gemini.google.com", L"https://gemini.google.com/app", agents::Gemini,
     Family::Web},
};

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
        if (s.tool == tool) service = &s;
    if (!service) return;
    std::wstring url = service->signInUrl;
    if (!signIn) {
        signIn = std::make_unique<WebBrowser>();
        signIn->OnClosed([this] { SyncAll(); });
        // Once a site shows a page other than its sign-in pages, sign-in is done:
        // sync straight away rather than waiting for the window to close.
        signIn->OnLoaded([this](bool ok, std::wstring const& address) {
            if (!ok) return;
            std::wstring lower = address;
            for (auto& ch : lower) ch = wchar_t(towlower(ch));
            for (auto const& s : Services) {
                if (lower.find(s.host) == std::wstring::npos) continue;
                for (auto marker : {L"/login", L"/auth", L"/signin", L"/sign-in", L"/oauth", L"/magic-link", L"accounts.google"})
                    if (lower.find(marker) != std::wstring::npos) return;
                SyncAll();
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

void WebSync::SyncAll() {
    if (running) {
        rerun = true;  // run again when this one ends, to pick up a new sign-in
        return;
    }
    running = true;
    changed = false;
    queue.clear();
    for (auto const& s : Services) queue.push_back(&s);
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
        report(L"", changed, true);
        if (std::exchange(rerun, false)) SyncAll();
        return;
    }
    current = queue.front();
    queue.pop_front();
    injected = false;
    stored = 0;
    failures = 0;
    lastError.clear();
    Touch();
    report(std::wstring(current->name) + L" · connecting", false, false);
    worker->Navigate(current->syncUrl);
}

void WebSync::Finish(std::wstring const& result) {
    if (current) results[current->tool] = result;
    Next();
}

void WebSync::OnLoaded(bool succeeded, std::wstring const& url) {
    if (!current || injected) return;
    Touch();
    if (url.find(current->host) == std::wstring::npos) {
        // Sent somewhere else, typically a sign-in page.
        Finish(L"not signed in (sent to " + url + L")");
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

    if (type == "status") {
        report(Wide(message.Str("text")), false, false);
    } else if (type == "signedOut") {
        auto detail = message.Str("detail");
        Finish(L"not signed in. Click its button above to sign in" + (detail.empty() ? std::wstring() : L" (" + Wide(detail) + L")"));
    } else if (type == "error") {
        ++failures;
        lastError = Wide(message.Str("text"));
        if (auto fatal = message.Get("fatal"); fatal && fatal->IsTrue()) Finish(L"error: " + lastError);
    } else if (type == "list") {
        std::string account(message.Str("account"));
        index.NameAccount(account, current->accountFamily, std::string(message.Str("accountName")));
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
                auto it = known.find(id);
                // Read conversations that are new or changed since the last sync.
                if (it == known.end() || (updated && it->second != updated)) {
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
        try {
            Transaction transaction(index.Database());
            index.StoreWeb(c);
            transaction.Commit();
            ++stored;
            changed = true;
            if (stored % 25 == 0) report(std::wstring(current->name) + L" · " + std::to_wstring(stored) + L" conversations read", true, false);
        } catch (std::exception const& e) {
            ++failures;
            lastError = Wide(e.what());
        }
    } else if (type == "done") {
        std::wstring result = L"synced";
        if (stored) result += L" · " + std::to_wstring(stored) + L" new or changed";
        if (failures) result += L" · " + std::to_wstring(failures) + L" problems (last: " + lastError + L")";
        Finish(result);
    }
}

void CALLBACK WebSync::Watchdog(HWND, UINT, UINT_PTR, DWORD) {
    auto self = activeSync;
    if (!self || !self->running || !self->current) return;
    // A site that stops answering for three minutes is skipped until the next sync.
    if (std::chrono::steady_clock::now() - self->lastActivity > std::chrono::minutes(3)) self->Finish(L"stopped answering");
}

std::wstring WebSync::Summary() const {
    std::wstring out;
    for (auto const& s : Services) {
        auto it = results.find(s.tool);
        out += std::wstring(s.name) + L": " + (it == results.end() ? L"not synced yet" : it->second) + L"\n";
    }
    if (!out.empty()) out.pop_back();
    return out;
}

}  // namespace sessions
