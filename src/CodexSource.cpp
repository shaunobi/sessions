#include "Sources.h"
#include "Json.h"
#include "LineReader.h"
#include "Text.h"

// Codex rollouts: ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl[.zst] and
// ~/.codex/archived_sessions. Both the ChatGPT desktop app (Codex) and the
// Codex command line write them; session_meta says which. Subagent and review
// helper threads are skipped after their first line.
namespace sessions {

namespace {

constexpr size_t MaxMessageBytes = 64 * 1024;

std::string Texts(json::Value const* content, std::string_view wanted) {
    std::string out;
    if (!content) return out;
    if (content->IsString()) return IsInjectedContext(content->string) ? std::string() : content->string;
    if (!content->IsArray()) return out;
    for (auto const& item : content->array) {
        if (item.Str("type") != wanted) continue;
        auto text = item.Str("text");
        if (IsInjectedContext(text)) continue;
        if (!out.empty()) out += "\n\n";
        out += text;
    }
    return out;
}

// rollout-2026-11-09T14-56-43-<thread id>.jsonl[.zst]
std::string IdFromName(std::filesystem::path const& file) {
    auto name = Utf8(file.filename().wstring());
    auto dot = name.find('.');
    if (dot != std::string::npos) name.resize(dot);
    constexpr std::string_view prefix = "rollout-";
    if (name.rfind(prefix, 0) == 0 && name.size() > prefix.size() + 20) return name.substr(prefix.size() + 20);
    return name;
}

}  // namespace

ParsedSession ParseCodex(std::filesystem::path const& file) {
    ParsedSession session;
    session.tool = Tool::CodexCli;
    session.externalId = IdFromName(file);
    bool sawMeta = false;

    bool opened = ReadLines(file, [&](std::string_view line) {
        json::Value value;
        if (!json::Parse(line, value) || !value.IsObject()) return true;
        auto type = value.Str("type");
        json::Value const* payload = value.Get("payload");
        int64_t timestamp = ParseIsoMs(value.Str("timestamp"));

        if (type == "session_meta" && payload) {
            if (sawMeta) return true;  // forks repeat their parent's meta
            sawMeta = true;
            auto source = payload->Get("source");
            if (source && source->IsObject()) {
                session.skip = true;
                session.skipReason = "subagent";
                return false;
            }
            auto id = payload->Str("id");
            if (!id.empty()) session.externalId = id;
            session.cwd = payload->Str("cwd");
            auto account = payload->Str("creator_account_id");
            if (!account.empty()) session.account = "codex:" + std::string(account);
            auto originator = payload->Str("originator");
            std::string_view sourceName = source && source->IsString() ? std::string_view(source->string) : "";
            if (sourceName == "vscode" || originator.find("Desktop") != std::string_view::npos)
                session.tool = Tool::CodexDesktop;
            if (int64_t created = ParseIsoMs(payload->Str("timestamp"))) session.created = created;
            return true;
        }

        // Older rollouts wrote items directly, without a payload wrapper.
        json::Value const* item = nullptr;
        if (type == "response_item") item = payload;
        else if (type == "message") item = &value;
        if (!item || item->Str("type") != "message") return true;

        auto role = item->Str("role");
        std::string text;
        if (role == "user") {
            text = Texts(item->Get("content"), "input_text");
            std::string_view view(text);
            if (!view.empty() && view.front() == '<') text = StripTags(view);
            text = Squash(text);
        } else if (role == "assistant") {
            text = Texts(item->Get("content"), "output_text");
        } else {
            return true;
        }
        if (text.empty()) return true;
        if (timestamp) {
            if (!session.created || timestamp < session.created) session.created = timestamp;
            if (timestamp > session.lastActivity) session.lastActivity = timestamp;
        }
        Role who = role == "user" ? Role::User : Role::Assistant;
        if (who == Role::User && session.firstPrompt.empty()) session.firstPrompt = Clip(Squash(text), 300);
        session.messages.push_back({who, Kind::Said, timestamp, Clip(text, MaxMessageBytes)});
        return true;
    });

    if (!opened) {
        session.skip = true;
        session.skipReason = "unreadable";
    } else if (!session.skip && session.messages.empty()) {
        session.skip = true;
        session.skipReason = "no conversation";
    }
    if (session.lastActivity < session.created) session.lastActivity = session.created;
    return session;
}

}  // namespace sessions
