#pragma once
#include <string_view>

namespace sessions {

// A file embedded in the executable as RCDATA (see Sessions.rc); empty when
// absent. The memory stays valid for the life of the process.
std::string_view EmbeddedFile(int id);

}  // namespace sessions
