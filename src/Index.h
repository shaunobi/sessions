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
    std::wstring folder;        // short name: Codex project or last part of the path
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
    bool together = false;      // one message holds every word (not just the session as a whole)
};

// One embedded piece of a message, kept in memory for meaning search.
struct ChunkVector {
    int64_t session = 0;
    int64_t message = 0;
    int32_t offset = 0;           // byte offset of the piece in the message text
    std::vector<int8_t> vector;   // L2-normalized, scaled by 127
};

struct PendingMessage {
    int64_t id = 0;
    std::string text;
};

// A conversation synced from a website (Claude, ChatGPT, Gemini).
struct WebConversation {
    Tool tool = Tool::ClaudeWeb;
    std::string id;        // "claude:<uuid>", "chatgpt:<id>" or "gemini:<id>"
    std::string account;   // "claude:<organization>", "codex:<ChatGPT account>", "gemini:<email>"
    std::string title;
    std::string folder;    // project name, if any
    int64_t created = 0;
    int64_t updated = 0;
    std::vector<Message> messages;
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

    // Web conversations, keyed by their service-prefixed id.
    std::unordered_map<std::string, int64_t> KnownWeb(std::string const& account);
    void StoreWeb(WebConversation const& conversation);
    // Removes conversations of this account that the site no longer lists.
    int RemoveWebMissing(std::string const& account, std::vector<std::string> const& present);
    void NameAccount(std::string const& account, Family family, std::string const& suggested);
    void ForgetAccount(std::string const& account);
    void ForgetAll();
    Db& Database() { return db; }

    // Meaning search storage. Pieces are keyed by a hash of their tokens, so
    // re-reading a live session only embeds what is new.
    void UseEmbeddingModel(std::string const& name);
    int PendingEmbeddingCount();
    std::vector<PendingMessage> MessagesToEmbed(int limit);
    bool HasEmbedding(int64_t hash);
    void StoreEmbedding(int64_t hash, std::vector<float> const& vector);
    void StoreChunk(int64_t message, int chunk, int32_t offset, int64_t hash);
    void PruneEmbeddings();
    int64_t VectorsVersion();
    void BumpVectorsVersion();
    std::vector<ChunkVector> LoadVectors();
    // About `bytes` of a message's text starting at a piece's offset.
    std::wstring Passage(int64_t message, int32_t offset, size_t bytes = 320);

    // Reader side.
    std::vector<SessionRow> LoadSessions();
    std::vector<AccountRow> Accounts();
    std::vector<std::wstring> AllTags();
    // Every word must appear in the session's messages or its title.
    std::vector<SearchHit> Search(std::wstring const& query);
    // Whether any of the query's words appears in any message at all.
    bool KnowsAnyWord(std::wstring const& query);
    // Snippet with \x01 ... \x02 around matched terms.
    std::wstring Snippet(std::wstring const& query, int64_t message);

    void RenameAccount(std::string const& account, std::wstring const& name);
    std::string MetaValue(std::string const& key);
    void SetMetaValue(std::string const& key, std::string const& value);
    std::wstring AccountName(std::string const& account);
    void AddTag(std::string const& key, std::wstring const& tag);
    void RemoveTag(std::string const& key, std::wstring const& tag);

    struct Totals {
        int sessions = 0, skipped = 0, messages = 0;
    };
    Totals Count();
    // Files skipped for this reason (for example "encrypted").
    int CountSkipped(std::string const& reason);

private:
    Db db;
    void Migrate();
};

std::wstring DefaultAccountName(std::string const& account);

}  // namespace sessions
