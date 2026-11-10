#include "Launch.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>

namespace sessions {

namespace {

bool Shell(std::wstring const& target, std::wstring const& parameters = {}, std::wstring const& folder = {}) {
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(),
        parameters.empty() ? nullptr : parameters.c_str(), folder.empty() ? nullptr : folder.c_str(), SW_SHOWNORMAL));
    return result > 32;
}

bool OnPath(wchar_t const* program) {
    wchar_t found[MAX_PATH]{};
    return SearchPathW(nullptr, program, nullptr, MAX_PATH, found, nullptr) > 0;
}

std::wstring Quote(std::wstring const& text) { return L"\"" + text + L"\""; }

// Ids are UUID-like; refuse anything else before it reaches a command line.
bool SafeId(std::wstring const& id) {
    if (id.empty() || id.size() > 80) return false;
    for (wchar_t c : id)
        if (!iswalnum(c) && c != L'-' && c != L'_') return false;
    return true;
}

bool Terminal(std::wstring const& cwd, std::wstring const& command) {
    std::error_code error;
    std::wstring folder = !cwd.empty() && std::filesystem::is_directory(cwd, error) ? cwd : std::wstring();
    std::wstring shell = OnPath(L"pwsh.exe") ? L"pwsh.exe" : L"powershell.exe";
    std::wstring inner = shell + L" -NoExit -Command " + command;
    if (OnPath(L"wt.exe")) {
        std::wstring parameters = folder.empty() ? inner : L"-d " + Quote(folder) + L" " + inner;
        if (Shell(L"wt.exe", parameters)) return true;
    }
    return Shell(shell, L"-NoExit -Command " + command, folder);
}

}  // namespace

bool OpenSession(SessionRow const& session) {
    if (IsWeb(session.tool)) {
        // "claude:<uuid>", "chatgpt:<id>", "gemini:c_<id>": open the conversation in the browser.
        auto colon = session.externalId.find(L':');
        std::wstring id = colon == std::wstring::npos ? session.externalId : session.externalId.substr(colon + 1);
        if (session.tool == Tool::Gemini && id.rfind(L"c_", 0) == 0) id = id.substr(2);
        if (!SafeId(id)) return false;
        switch (session.tool) {
        case Tool::ClaudeWeb: return Shell(L"https://claude.ai/chat/" + id);
        case Tool::ChatGptWeb: return Shell(L"https://chatgpt.com/c/" + id);
        default: return Shell(L"https://gemini.google.com/app/" + id);
        }
    }
    if (!SafeId(session.externalId)) return false;
    switch (session.tool) {
    case Tool::ClaudeDesktop:
        // The desktop app's link for continuing a specific Code session. If the
        // app does not recognize the id it still comes to the front.
        if (SafeId(session.appId) && Shell(L"claude://code/continue?session=" + session.appId)) return true;
        return Shell(L"claude://code/");
    case Tool::ClaudeCli:
        return Terminal(session.cwd, L"claude --resume " + session.externalId);
    case Tool::CodexDesktop:
        return Shell(L"codex://threads/" + session.externalId);
    case Tool::CodexCli:
        return Terminal(session.cwd, L"codex resume " + session.externalId);
    }
    return false;
}

void RevealFile(std::wstring const& path) {
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str());
    if (!item) return;
    SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    ILFree(item);
}

}  // namespace sessions
