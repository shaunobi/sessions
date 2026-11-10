#pragma once
#include "Db.h"
#include "Sources.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// The local database: one file next to the executable holding the session
// list, the full-text index and the person's own tags and account names.
namespace sessions {

struct SessionRow {
    int64_t id = 0;
    std::string key;            // stable across rebuilds; tags hang off it
    Tool tool = Tool::ClaudeCli;
    std::wstring title;
    std::string account;
    std::wstring cwd;
    std::wstring externalId;
    std::wstring appId;
    std::wstring path;
    int64_t created = 0;
    int64_t lastActivity = 0;
    int messages = 0;
    bool archived = false;
    std::vector<std::wstring> tags;
};

struct AccountRow {
    std::string id;
    Family family = Family::Claude;
    std::wstring name;          // the person's name for it, or a generated one
    bool named = false;
    int sessions = 0;
};

struct SearchHit {
    int64_t session = 0;
    double score = 0;
    int64_t bestMessage = 0;    // message to show a snippet from
};

struct KnownFile {
    int64_t size = 0;
    int64_t modified = 0;
};

class Index {
public:
    explicit Index(std::filesystem::path const& file);
    static std::filesystem::path DefaultFile();

    // Indexer side.
    std::unordered_map<std::wstring, KnownFile> KnownFiles();
    void Remove(std::wstring const& path);
    void Store(SourceFile const& file, ParsedSession const& session);
    void ApplyMetadata(AppMetadata const& meta);
    void ForgetAccount(std::string const& account);
    void ForgetAll();
    Db& Database() { return db; }

    // Reader side.
    std::vector<SessionRow> LoadSessions();
    std::vector<AccountRow> Accounts();
    std::vector<std::wstring> AllTags();
    // Every word must appear in the session's messages or its title.
    std::vector<SearchHit> Search(std::wstring const& query);
    // Snippet with \x01 ... \x02 around matched terms.
    std::wstring Snippet(std::wstring const& query, int64_t message);

    void RenameAccount(std::string const& account, std::wstring const& name);
    void AddTag(std::string const& key, std::wstring const& tag);
    void RemoveTag(std::string const& key, std::wstring const& tag);

    struct Totals {
        int sessions = 0, skipped = 0, messages = 0;
    };
    Totals Count();

private:
    Db db;
    void Migrate();
};

std::wstring DefaultAccountName(std::string const& account);

}  // namespace sessions
