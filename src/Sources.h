#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// Reading the four session stores. Nothing here writes to another tool's files.
namespace sessions {

enum class Tool : int { ClaudeDesktop = 1, ClaudeCli = 2, CodexDesktop = 3, CodexCli = 4 };
enum class Family : int { Claude = 1, Codex = 2 };

wchar_t const* ToolName(Tool tool);
Family FamilyOf(Tool tool);

enum class Role : int { User = 1, Assistant = 2 };
// What kind of content a message holds. Only Said is indexed today; the
// distinction is kept so tool activity can be added later without a migration.
enum class Kind : int { Said = 0, ToolActivity = 1 };

struct Message {
    Role role = Role::User;
    Kind kind = Kind::Said;
    int64_t timestamp = 0;
    std::string text;
};

struct ParsedSession {
    bool skip = false;          // a subagent, review helper or unreadable file
    std::string skipReason;
    Tool tool = Tool::ClaudeCli;
    std::string externalId;     // Claude session id or Codex thread id
    std::string account;        // "claude:<organization>" or "codex:<account>"
    std::string titleFile;      // a title written into the transcript itself
    std::string firstPrompt;
    std::string cwd;
    int64_t created = 0;
    int64_t lastActivity = 0;
    std::vector<Message> messages;
};

struct SourceFile {
    std::filesystem::path path;
    Family family = Family::Claude;
    int64_t size = 0;
    int64_t modified = 0;
};

struct SourceRoots {
    std::filesystem::path claudeProjects;      // ~/.claude/projects
    std::filesystem::path claudeDesktop;       // %APPDATA%/Claude/claude-code-sessions
    std::filesystem::path claudeConfig;        // ~/.claude.json
    std::filesystem::path codexHome;           // ~/.codex
    static SourceRoots Detect();
    std::vector<std::filesystem::path> Watched() const;
};

std::vector<SourceFile> FindSourceFiles(SourceRoots const& roots);

ParsedSession ParseClaude(std::filesystem::path const& file);
ParsedSession ParseCodex(std::filesystem::path const& file);

// Titles, accounts and archive state that the apps keep outside the transcripts.
struct AppEntry {
    std::string title;
    std::string account;
    std::string appId;    // Claude desktop "local_..." id
    std::string folder;   // the folder the app files the session under (Claude desktop: originCwd)
    std::string project;  // the Codex project the thread belongs to, as named in its sidebar
    bool archived = false;
};

// A Codex project: its display name and the folders it covers.
struct CodexProject {
    std::string name;
    std::wstring root;
};

struct AppMetadata {
    std::unordered_map<std::string, AppEntry> claude;  // by Claude session id
    std::unordered_map<std::string, AppEntry> codex;   // by Codex thread id
    std::string claudeCliAccount;                      // organization signed in to the command line
    // The Claude desktop app opens only sessions of the organization it is
    // showing. Best guess: the organization of the most recently focused session.
    std::string claudeDesktopAccount;
    std::unordered_map<std::string, std::string> accountNames;  // suggested names for new accounts
    std::vector<CodexProject> codexProjects;
};

// The short folder name a card shows: a Codex project name when the folder
// belongs to one, otherwise the last part of the path. Empty for scratch folders.
std::string FolderLabel(std::string const& cwd, std::vector<CodexProject> const& projects);

AppMetadata ReadAppMetadata(SourceRoots const& roots);

}  // namespace sessions
