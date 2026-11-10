#include "Sources.h"
#include "Db.h"
#include "Json.h"
#include "LineReader.h"
#include "Text.h"
#include <windows.h>
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
    double lastFocused = 0;
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
                app.folder = value.Str("originCwd");
                if (app.folder.empty()) app.folder = value.Str("cwd");
                app.account = "claude:" + organization;
                if (auto archived = value.Get("isArchived")) app.archived = archived->IsTrue();
                if (auto focused = value.Get("lastFocusedAt"); focused && focused->type == json::Value::Type::Number &&
                                                                focused->number > lastFocused) {
                    lastFocused = focused->number;
                    meta.claudeDesktopAccount = app.account;
                }
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
            // Newer Codex versions record the sidebar name and project on each thread.
            try {
                auto query = db.Prepare(
                    "SELECT t.id, COALESCE(NULLIF(t.name, ''), t.title), t.archived, COALESCE(p.name, '') "
                    "FROM threads t LEFT JOIN projects p ON p.id = t.project_id");
                while (query.Step()) {
                    auto& app = meta.codex[std::string(query.Text(0))];
                    app.title = query.Text(1);
                    app.archived = query.Int(2) != 0;
                    app.project = query.Text(3);
                }
            } catch (DbError const&) {
                auto query = db.Prepare("SELECT id, title, archived FROM threads");
                while (query.Step()) {
                    auto& app = meta.codex[std::string(query.Text(0))];
                    app.title = query.Text(1);
                    app.archived = query.Int(2) != 0;
                }
            }
            // Projects in the Codex sidebar (for example "sembit" for D:\ownCloud\Management).
            try {
                // In sidebar order: when two projects share a folder, Codex files the thread under the first.
                auto projects = db.Prepare(
                    "SELECT p.name, r.path FROM projects p JOIN project_roots r ON r.project_id = p.id ORDER BY p.position, r.position");
                while (projects.Step()) meta.codexProjects.push_back({std::string(projects.Text(0)), projects.WText(1)});
            } catch (DbError const&) {
            }
        } catch (DbError const&) {
        }
    }
    // session_index.jsonl holds the names Codex shows in its sidebar (generated
    // or renamed); the threads table's title is often just the first prompt.
    // Later lines are newer.
    ReadLines(roots.codexHome / L"session_index.jsonl", [&](std::string_view line) {
        json::Value value;
        if (json::Parse(line, value)) {
            auto id = value.Str("id");
            auto name = value.Str("thread_name");
            if (!id.empty() && !name.empty()) meta.codex[std::string(id)].title = name;
        }
        return true;
    });
    return meta;
}

std::string FolderLabel(std::string const& cwd, std::vector<CodexProject> const& projects) {
    std::wstring path = Wide(cwd);
    if (path.rfind(L"\\\\?\\", 0) == 0) path = path.substr(4);
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    if (path.empty()) return {};
    auto lower = [](std::wstring text) {
        if (!text.empty()) CharLowerBuffW(text.data(), DWORD(text.size()));
        for (auto& c : text) if (c == L'/') c = L'\\';
        while (text.size() > 3 && text.back() == L'\\') text.pop_back();
        return text;
    };
    auto target = lower(path);
    // Claude desktop sessions without a folder run in a scratch workspace.
    if (target.find(L"\\scratch-workspaces\\") != std::wstring::npos) return {};
    std::string best;
    size_t bestLength = 0;
    for (auto const& project : projects) {
        auto root = lower(project.root);
        if (root.empty() || root.size() <= bestLength) continue;  // longest root wins; ties keep the earlier project
        if (target == root || (target.rfind(root, 0) == 0 && target.size() > root.size() && target[root.size()] == L'\\')) {
            best = project.name;
            bestLength = root.size();
        }
    }
    if (!best.empty()) return best;
    auto name = fs::path(path).filename().wstring();
    if (name.empty()) name = path;  // a drive root, such as "C:\"
    return Utf8(name);
}

}  // namespace sessions
