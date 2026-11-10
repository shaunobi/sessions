#pragma once
#include <filesystem>
#include <functional>
#include <string_view>

namespace sessions {

// Calls `line` for each line of a text file, decompressing Zstandard (.zst)
// files on the fly. Return false from the callback to stop early.
// Returns false when the file could not be opened or decompressed.
bool ReadLines(std::filesystem::path const& file, std::function<bool(std::string_view)> const& line);

}  // namespace sessions
