#include "Embedded.h"
#include <windows.h>

namespace sessions {

std::string_view EmbeddedFile(int id) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!found) return {};
    HGLOBAL loaded = LoadResource(module, found);
    if (!loaded) return {};
    auto data = static_cast<char const*>(LockResource(loaded));
    return data ? std::string_view(data, SizeofResource(module, found)) : std::string_view();
}

}  // namespace sessions
