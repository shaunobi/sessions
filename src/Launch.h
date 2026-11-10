#pragma once
#include "Index.h"

namespace sessions {

// Opens a session in the tool it came from: the desktop apps through their
// registered links, command-line sessions in Windows Terminal with the tool's
// resume command. Returns false when nothing could be started.
bool OpenSession(SessionRow const& session);

// Web chats: the conversation's address on its site, opening it in the default
// browser, and whether a normal click goes to the browser (ChatGPT, Gemini)
// rather than a desktop app (Claude chats open in the Claude app).
std::wstring WebAddress(SessionRow const& session);
bool OpenInBrowser(SessionRow const& session);
bool OpensInBrowser(Tool tool);

// Selects the transcript file in File Explorer.
void RevealFile(std::wstring const& path);

}  // namespace sessions
