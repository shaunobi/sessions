#pragma once
#include "Index.h"
#include <filesystem>
#include <functional>
#include <string>

// Reads a ChatGPT data export (Settings > Data controls > Export data): the
// zip ChatGPT emails, its extracted folder, or its conversations*.json files.
// Every conversation is stored as if web sync had read it, so the slow sync
// only has to keep up with new chats afterwards.
namespace sessions {

struct ImportResult {
    int conversations = 0;  // stored
    int skipped = 0;        // empty or unreadable
    std::wstring problem;   // why nothing was imported, if so
};

// `progress` gets "N of M" text now and then. Uses its own connection to the
// database, so it can run on a worker thread.
ImportResult ImportChatGptExport(std::filesystem::path const& database, std::filesystem::path const& source,
                                 std::function<void(std::wstring const&)> progress = {});

}  // namespace sessions
