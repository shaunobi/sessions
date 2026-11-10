#pragma once
#include "Index.h"

namespace sessions {

// Opens a session in the tool it came from: the desktop apps through their
// registered links, command-line sessions in Windows Terminal with the tool's
// resume command. Returns false when nothing could be started.
bool OpenSession(SessionRow const& session);

// Selects the transcript file in File Explorer.
void RevealFile(std::wstring const& path);

}  // namespace sessions
