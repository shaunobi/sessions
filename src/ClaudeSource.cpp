#include "Sources.h"
#include "Json.h"
#include "LineReader.h"
#include "Text.h"

// Claude Code transcripts: ~/.claude/projects/<folder>/<session id>.jsonl.
// The desktop app and the command line write the same format; each line
// records its "entrypoint". Subagent transcripts live in subfolders and are
// not read.
namespace sessions {

namespace {

constexpr size_t MaxMessageBytes = 64 * 1024;

bool Contains(std::string_view line, std::string_view needle) {
    return line.find(needle) != std::string_view::npos;
}

// Only these line types carry anything we keep; the rest (API request logs,
// file history, attachments) can be large and are skipped without parsing.
bool Wanted(std::string_view line) {
    return Contains(line, "\"type\":\"user\"") || Contains(line, "\"type\":\"assistant\"") ||
           Contains(line, "\"type\":\"custom-title\"") || Contains(line, "\"type\":\"ai-title\"") ||
           Contains(line, "\"type\":\"bridge-session\"") || Contains(line, "\"type\":\"summary\"");
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// Text blocks, plus replies sent through a messaging tool: in project threads
// the assistant talks to the person only by calling post_message.
std::string Said(json::Value const* content) {
    if (!content) return {};
    if (content->IsString()) return content->string;
    std::string out;
    if (!content->IsArray()) return out;
    for (auto const& block : content->array) {
        std::string_view text;
        auto type = block.Str("type");
        if (type == "text") {
            text = block.Str("text");
        } else if (type == "tool_use" && EndsWith(block.Str("name"), "__post_message")) {
            if (auto input = block.Get("input")) text = input->Str("text");
        } else {
            continue;
        }
        if (IsInjectedContext(text)) continue;
        if (!out.empty()) out += "\n\n";
        out += text;
    }
    return out;
}

// The person's own words: drop injected context and unwrap relayed messages.
std::string Prompt(std::string text) {
    if (IsInjectedContext(text)) return {};
    std::string_view view(text);
    while (!view.empty() && (view.front() == ' ' || view.front() == '\n')) view.remove_prefix(1);
    if (!view.empty() && view.front() == '<') text = DecodeEntities(StripTags(view));
    if (IsInjectedContext(text)) return {};
    return text;
}

}  // namespace

ParsedSession ParseClaude(std::filesystem::path const& file) {
    ParsedSession session;
    session.externalId = Utf8(file.stem().wstring());
    std::string customTitle, aiTitle, summary, entrypoint, lastAssistantId;

    bool opened = ReadLines(file, [&](std::string_view line) {
        if (!Wanted(line)) return true;
        json::Value value;
        if (!json::Parse(line, value) || !value.IsObject()) return true;
        auto type = value.Str("type");
        if (type == "custom-title") { customTitle = value.Str("customTitle"); return true; }
        if (type == "ai-title") { aiTitle = value.Str("aiTitle"); return true; }
        if (type == "summary") { summary = value.Str("summary"); return true; }
        if (type == "bridge-session") {
            auto organization = value.Str("ownerOrganizationUuid");
            if (!organization.empty()) session.account = "claude:" + std::string(organization);
            return true;
        }
        if (type != "user" && type != "assistant") return true;
        if (auto sidechain = value.Get("isSidechain"); sidechain && sidechain->IsTrue()) return true;
        if (entrypoint.empty()) entrypoint = value.Str("entrypoint");
        if (session.cwd.empty()) session.cwd = value.Str("cwd");
        int64_t timestamp = ParseIsoMs(value.Str("timestamp"));
        if (timestamp) {
            if (!session.created || timestamp < session.created) session.created = timestamp;
            if (timestamp > session.lastActivity) session.lastActivity = timestamp;
        }
        auto message = value.Get("message");
        if (!message) return true;

        if (type == "user") {
            for (auto flag : {"isMeta", "isCompactSummary", "isVisibleInTranscriptOnly"})
                if (auto v = value.Get(flag); v && v->IsTrue()) return true;
            auto text = Squash(Prompt(Said(message->Get("content"))));
            if (text.empty()) return true;
            lastAssistantId.clear();
            if (session.firstPrompt.empty()) session.firstPrompt = Clip(text, 300);
            session.messages.push_back({Role::User, Kind::Said, timestamp, Clip(text, MaxMessageBytes)});
            return true;
        }

        auto text = Said(message->Get("content"));
        if (text.empty()) return true;
        std::string id(message->Str("id"));
        // One API message is split across lines, one per content block.
        if (!id.empty() && id == lastAssistantId && !session.messages.empty() &&
            session.messages.back().role == Role::Assistant) {
            auto& previous = session.messages.back().text;
            if (previous.size() < MaxMessageBytes) previous = Clip(previous + "\n\n" + text, MaxMessageBytes);
            return true;
        }
        lastAssistantId = id;
        session.messages.push_back({Role::Assistant, Kind::Said, timestamp, Clip(text, MaxMessageBytes)});
        return true;
    });

    if (!opened) {
        session.skip = true;
        session.skipReason = "unreadable";
        return session;
    }
    session.tool = entrypoint == "claude-desktop" ? Tool::ClaudeDesktop : Tool::ClaudeCli;
    session.titleFile = !customTitle.empty() ? customTitle : !aiTitle.empty() ? aiTitle : summary;
    if (session.messages.empty()) {
        session.skip = true;
        session.skipReason = "no conversation";
    }
    return session;
}

}  // namespace sessions
