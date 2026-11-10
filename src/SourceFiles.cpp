#include "Sources.h"
#include "Db.h"
#include "Json.h"
#include "LineReader.h"
#include "Text.h"
#include <fstream>
#include <sstream>

namespace sessions {

namespace fs = std::filesystem;

wchar_t const* ToolName(Tool tool) {
    switch (tool) {
    case Tool::ClaudeDesktop: return L"Claude desktop";
    case Tool::ClaudeCli: return L"Claude CLI";
    case Tool::CodexDesktop: return L"Codex desktop";
    case Tool::CodexCli: return L"Codex CLI";
    }
    return L"Unknown";
}

Family FamilyOf(Tool tool) {
    return tool == Tool::CodexDesktop || tool == Tool::CodexCli ? Family::Codex : Family::Claude;
}

SourceRoots SourceRoots::Detect() {
    SourceRoots roots;
    auto home = HomeFolder();
    roots.claudeProjects = home / L".claude" / L"projects";
    roots.claudeConfig = home / L".claude.json";
    roots.claudeDesktop = RoamingFolder() / L"Claude" / L"claude-code-sessions";
    roots.codexHome = home / L".codex";
    return roots;
}

std::vector<fs::path> SourceRoots::Watched() const {
    std::vector<fs::path> out;
    for (auto const& path : {claudeProjects, claudeDesktop, codexHome / L"sessions", codexHome / L"archived_sessions"}) {
        std::error_code error;
        if (fs::is_directory(path, error)) out.push_back(path);
    }
    return out;
}

namespace {

void Add(std::vector<SourceFile>& out, fs::directory_entry const& entry, Family family) {
    std::error_code error;
    SourceFile file;
    file.path = entry.path();
    file.family = family;
    file.size = int64_t(entry.file_size(error));
    if (error) return;
    file.modified = FileTimeMs(entry.last_write_time(error));
    if (error) return;
    out.push_back(std::move(file));
}

bool EndsWith(std::wstring const& text, std::wstring_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ReadWhole(fs::path const& file) {
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

}  // namespace

std::vector<SourceFile> FindSourceFiles(SourceRoots const& roots) {
    std::vector<SourceFile> out;
    std::error_code error;
    // Claude: top-level transcripts only; <session>/subagents/*.jsonl are helpers.
    for (auto const& folder : fs::directory_iterator(roots.claudeProjects, error)) {
        if (!folder.is_directory(error)) continue;
        std::error_code inner;
        for (auto const& entry : fs::directory_iterator(folder.path(), inner)) {
            if (entry.is_regular_file(inner) && entry.path().extension() == L".jsonl") Add(out, entry, Family::Claude);
        }
    }
    for (auto const& name : {L"sessions", L"archived_sessions"}) {
        auto root = roots.codexHome / name;
        if (!fs::is_directory(root, error)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error);
             it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (error) break;
            if (!it->is_regular_file(error)) continue;
            auto filename = it->path().filename().wstring();
            if (filename.rfind(L"rollout-", 0) == 0 && (EndsWith(filename, L".jsonl") || EndsWith(filename, L".jsonl.zst")))
                Add(out, *it, Family::Codex);
        }
    }
    return out;
}

AppMetadata ReadAppMetadata(SourceRoots const& roots) {
    AppMetadata meta;
    std::error_code error;

    // Claude desktop sidebar entries: <account>/<organization>/local_<id>.json.
    for (auto const& accountFolder : fs::directory_iterator(roots.claudeDesktop, error)) {
        if (!accountFolder.is_directory(error)) continue;
        std::error_code e1;
        for (auto const& orgFolder : fs::directory_iterator(accountFolder.path(), e1)) {
            if (!orgFolder.is_directory(e1)) continue;
            auto organization = Utf8(orgFolder.path().filename().wstring());
            std::error_code e2;
            for (auto const& entry : fs::directory_iterator(orgFolder.path(), e2)) {
                auto name = entry.path().filename().wstring();
                if (!entry.is_regular_file(e2) || name.rfind(L"local_", 0) != 0 || entry.path().extension() != L".json") continue;
                json::Value value;
                if (!json::Parse(ReadWhole(entry.path()), value)) continue;
                auto cliSession = value.Str("cliSessionId");
                if (cliSession.empty()) continue;
                AppEntry app;
                app.title = DecodeEntities(value.Str("title"));
                app.appId = value.Str("sessionId");
                app.account = "claude:" + organization;
                if (auto archived = value.Get("isArchived")) app.archived = archived->IsTrue();
                meta.claude[std::string(cliSession)] = std::move(app);
            }
        }
    }

    // The organization the command line is signed in to. Only the account
    // block is read; ~/.claude.json holds no credentials there.
    {
        json::Value config;
        if (fs::exists(roots.claudeConfig, error) && json::Parse(ReadWhole(roots.claudeConfig), config)) {
            if (auto account = config.Get("oauthAccount")) {
                auto organization = account->Str("organizationUuid");
                if (!organization.empty()) {
                    meta.claudeCliAccount = "claude:" + std::string(organization);
                    meta.accountNames[meta.claudeCliAccount] = std::string(account->Str("organizationName"));
                }
            }
        }
    }

    // Codex: thread titles from its own index (read-only), then session_index.jsonl.
    auto state = roots.codexHome / L"state_5.sqlite";
    if (fs::exists(state, error)) {
        try {
            Db db;
            try {
                db = Db(state, true);
                db.Exec("SELECT 1 FROM threads LIMIT 1");
            } catch (DbError const&) {
                db = Db(fs::path(L"file:" + state.generic_wstring() + L"?immutable=1"), true);
            }
            auto query = db.Prepare("SELECT id, title, archived FROM threads");
            while (query.Step()) {
                auto& app = meta.codex[std::string(query.Text(0))];
                app.title = query.Text(1);
                app.archived = query.Int(2) != 0;
            }
        } catch (DbError const&) {
        }
    }
    ReadLines(roots.codexHome / L"session_index.jsonl", [&](std::string_view line) {
        json::Value value;
        if (json::Parse(line, value)) {
            auto id = value.Str("id");
            auto name = value.Str("thread_name");
            if (!id.empty() && !name.empty()) {
                auto& app = meta.codex[std::string(id)];
                if (app.title.empty()) app.title = name;
            }
        }
        return true;
    });
    return meta;
}

}  // namespace sessions
