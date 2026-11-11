#include "ChatGptImport.h"
#include "Json.h"
#include "Text.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>

namespace sessions {

namespace fs = std::filesystem;

namespace {

constexpr size_t MaxMessageBytes = 64 * 1024;

std::string ReadAll(fs::path const& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// The top-level array split into one string per conversation, so each is
// parsed on its own (a whole export can be hundreds of megabytes).
std::vector<std::string_view> Elements(std::string_view text) {
    std::vector<std::string_view> out;
    size_t i = text.find('[');
    if (i == std::string_view::npos) return out;
    int depth = 0;
    bool inString = false;
    size_t start = 0;
    for (++i; i < text.size(); ++i) {
        char c = text[i];
        if (inString) {
            if (c == '\\') ++i;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{' || c == '[') {
            if (depth++ == 0) start = i;
        } else if (c == '}' || c == ']') {
            if (depth == 0) break;  // end of the top-level array
            if (--depth == 0) out.push_back(text.substr(start, i - start + 1));
        }
    }
    return out;
}

int64_t Ms(json::Value const* v) { return v && v->type == json::Value::Type::Number ? int64_t(v->number * 1000) : 0; }

std::string TextOf(json::Value const& message) {
    auto content = message.Get("content");
    if (!content) return {};
    if (auto parts = content->Get("parts"); parts && parts->IsArray()) {
        std::string out;
        for (auto const& p : parts->array)
            if (p.IsString() && !p.string.empty()) out += (out.empty() ? "" : "\n\n") + p.string;
        return out;
    }
    return std::string(content->Str("text"));
}

// The same choice of messages as web sync: what was shown in the chat, on the branch shown.
bool Shown(json::Value const& m) {
    auto author = m.Get("author");
    auto role = author ? author->Str("role") : std::string_view();
    if (role != "user" && role != "assistant") return false;
    if (auto meta = m.Get("metadata"))
        if (auto hidden = meta->Get("is_visually_hidden_from_conversation"); hidden && hidden->IsTrue()) return false;
    auto recipient = m.Str("recipient");
    if (!recipient.empty() && recipient != "all") return false;
    auto channel = m.Str("channel");
    if (!channel.empty() && channel != "final") return false;
    auto content = m.Get("content");
    auto type = content ? content->Str("content_type") : std::string_view();
    return type.empty() || type == "text" || type == "multimodal_text";
}

std::vector<fs::path> ConversationFiles(fs::path const& folder) {
    std::vector<fs::path> out;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, error); !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        auto name = it->path().filename().wstring();
        for (auto& c : name) c = wchar_t(towlower(c));
        if (it->is_regular_file() && name.rfind(L"conversations", 0) == 0 && it->path().extension() == L".json") out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Extracts the conversation files of a zip with Windows' own tar.
bool Extract(fs::path const& zip, fs::path const& into) {
    fs::create_directories(into);
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring tar = (fs::path(system) / L"tar.exe").wstring();
    std::wstring command = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + into.wstring() + L"\" \"conversations*.json\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    WaitForSingleObject(process.hProcess, 10 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
}

}  // namespace

ImportResult ImportChatGptExport(fs::path const& database, fs::path const& source, std::function<void(std::wstring const&)> progress) {
    ImportResult result;
    auto say = [&](std::wstring const& text) { if (progress) progress(text); };
    std::vector<fs::path> files;
    fs::path temporary;
    std::error_code error;
    if (fs::is_directory(source, error)) {
        files = ConversationFiles(source);
    } else if (source.extension() == L".zip" || source.extension() == L".ZIP") {
        say(L"Unpacking the export");
        temporary = fs::temp_directory_path(error) / (L"sessions-chatgpt-" + std::to_wstring(GetTickCount64()));
        if (!Extract(source, temporary)) {
            result.problem = L"Could not unpack the zip (is it the file ChatGPT sent?).";
            fs::remove_all(temporary, error);
            return result;
        }
        files = ConversationFiles(temporary);
    } else {
        files = {source};
    }
    if (files.empty()) {
        result.problem = L"No conversations.json in it. Choose the zip ChatGPT emailed you.";
        if (!temporary.empty()) fs::remove_all(temporary, error);
        return result;
    }

    Index index(database);
    // Stored under the account web sync uses, so the two agree on which chats are known.
    std::string account;
    {
        auto q = index.Database().Prepare(
            "SELECT account FROM sessions WHERE family = 3 AND tool = ?1 AND account != '' GROUP BY account ORDER BY count(*) DESC LIMIT 1");
        q.Bind(1, int64_t(Tool::ChatGptWeb));
        if (q.Step()) account = std::string(q.Text(0));
    }
    if (account.empty()) account = "codex:export";
    auto known = index.KnownWeb(account);

    for (auto const& file : files) {
        std::string text = ReadAll(file);
        auto elements = Elements(text);
        size_t done = 0;
        // Committed in small steps so web sync and the indexer can write in between.
        auto transaction = std::make_unique<Transaction>(index.Database());
        for (auto element : elements) {
            if (done % 100 == 99) {
                transaction->Commit();
                transaction = std::make_unique<Transaction>(index.Database());
            }
            if (++done % 100 == 0) say(L"Loading ChatGPT export · " + std::to_wstring(done) + L" of " + std::to_wstring(elements.size()));
            json::Value c;
            if (!json::Parse(element, c) || !c.IsObject()) {
                ++result.skipped;
                continue;
            }
            std::string id(c.Str("conversation_id"));
            if (id.empty()) id = c.Str("id");
            if (id.empty()) {
                ++result.skipped;
                continue;
            }
            WebConversation w;
            w.tool = Tool::ChatGptWeb;
            w.id = "chatgpt:" + id;
            w.account = account;
            w.title = Squash(c.Str("title"));
            w.created = Ms(c.Get("create_time"));
            w.updated = Ms(c.Get("update_time"));
            // Already read by sync at this version or later: keep that copy.
            if (auto it = known.find(w.id); it != known.end() && it->second > 0 && it->second >= w.updated) continue;
            auto mapping = c.Get("mapping");
            std::vector<json::Value const*> chain;
            std::string node(c.Str("current_node"));
            for (int guard = 0; mapping && !node.empty() && guard < 100000; ++guard) {
                auto entry = mapping->Get(node);
                if (!entry) break;
                if (auto m = entry->Get("message"); m && m->IsObject()) chain.push_back(m);
                auto parent = entry->Get("parent");
                node = parent && parent->IsString() ? parent->string : std::string();
            }
            std::reverse(chain.begin(), chain.end());
            for (auto m : chain) {
                if (!Shown(*m)) continue;
                std::string said = CleanChatGptText(TextOf(*m));
                if (said.find_first_not_of(" \n\r\t") == std::string::npos) continue;
                Message entry;
                entry.role = m->Get("author")->Str("role") == "user" ? Role::User : Role::Assistant;
                entry.timestamp = Ms(m->Get("create_time"));
                entry.text = Clip(said, MaxMessageBytes);
                w.messages.push_back(std::move(entry));
            }
            if (w.messages.empty()) {
                ++result.skipped;
                continue;
            }
            index.StoreWeb(w);
            ++result.conversations;
        }
        transaction->Commit();
    }
    if (!temporary.empty()) fs::remove_all(temporary, error);
    if (!result.conversations && !result.skipped) result.problem = L"Every chat in the export was already in Sessions.";
    return result;
}

}  // namespace sessions
