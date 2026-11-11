#include "Index.h"
#include "Text.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace sessions {

namespace fs = std::filesystem;

namespace {

constexpr int SchemaVersion = 1;

std::wstring Lower(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), DWORD(text.size()));
    return text;
}

// Splits on spaces, keeping "quoted phrases" together.
std::vector<std::wstring> Words(std::wstring const& query) {
    std::vector<std::wstring> words;
    std::wstring current;
    bool quoted = false;
    for (wchar_t c : query) {
        if (c == L'"') {
            if (!current.empty()) words.push_back(current);
            current.clear();
            quoted = !quoted;
        } else if (!quoted && iswspace(c)) {
            if (!current.empty()) words.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) words.push_back(current);
    return words;
}

// Common words carry no signal in a keyword search over whole sessions; a
// natural-language query keeps only its meaningful words (meaning search still
// sees the full sentence). Quoted phrases are kept as typed.
std::vector<std::wstring> KeywordWords(std::wstring const& query) {
    static const std::unordered_set<std::wstring> common{
        L"a", L"an", L"and", L"are", L"as", L"at", L"be", L"but", L"by", L"can", L"did", L"do", L"does", L"for", L"from",
        L"had", L"has", L"have", L"how", L"i", L"if", L"in", L"into", L"is", L"it", L"its", L"me", L"my", L"of", L"on", L"or",
        L"our", L"so", L"that", L"the", L"their", L"then", L"there", L"these", L"this", L"to", L"was", L"we", L"were",
        L"what", L"when", L"where", L"which", L"who", L"why", L"will", L"with", L"you", L"your", L"about", L"some", L"any"};
    auto words = Words(query);
    std::vector<std::wstring> kept;
    for (auto const& word : words)
        if (word.find(L' ') != std::wstring::npos || !common.count(Lower(word))) kept.push_back(word);
    return kept.empty() ? words : kept;
}

std::string Term(std::wstring const& word, bool prefix) {
    std::string out = "\"";
    for (char c : Utf8(word)) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    if (prefix) out += "*";
    return out;
}

std::string SessionKey(Family family, std::string const& id) {
    // Web conversation ids already carry their service ("claude:...", "chatgpt:...").
    if (family == Family::Web) return "web:" + id;
    if (family == Family::Antigravity) return "antigravity:" + id;
    return (family == Family::Codex ? "codex:" : "claude:") + id;
}

}  // namespace

std::wstring DefaultAccountName(std::string const& account) {
    auto colon = account.find(':');
    std::string family = account.substr(0, colon);
    std::string id = colon == std::string::npos ? std::string() : account.substr(colon + 1, 8);
    if (family == "gemini") return L"Gemini " + Wide(colon == std::string::npos ? "" : account.substr(colon + 1));
    if (family == "antigravity") return L"Antigravity";
    if (id.empty()) return family == "codex" ? L"ChatGPT (account not recorded)" : L"Claude (account not recorded)";
    return (family == "codex" ? L"ChatGPT account " : L"Claude organization ") + Wide(id);
}

Index::Index(fs::path const& file) : db(file) {
    db.Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY;");
    Migrate();
}

fs::path Index::DefaultFile() { return ExecutableFolder() / L"Sessions.db"; }

void Index::Migrate() {
    db.Exec(R"sql(
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS accounts(
  id TEXT PRIMARY KEY,
  family INTEGER NOT NULL,
  name TEXT NOT NULL DEFAULT '',
  suggested TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS sessions(
  id INTEGER PRIMARY KEY,
  path TEXT NOT NULL UNIQUE,
  family INTEGER NOT NULL,
  key TEXT NOT NULL,
  external_id TEXT NOT NULL,
  file_size INTEGER NOT NULL,
  file_modified INTEGER NOT NULL,
  skipped TEXT NOT NULL DEFAULT '',
  tool_file INTEGER NOT NULL,
  tool INTEGER NOT NULL,
  account_file TEXT NOT NULL DEFAULT '',
  account TEXT NOT NULL DEFAULT '',
  title_file TEXT NOT NULL DEFAULT '',
  title_app TEXT NOT NULL DEFAULT '',
  first_prompt TEXT NOT NULL DEFAULT '',
  cwd TEXT NOT NULL DEFAULT '',
  app_id TEXT NOT NULL DEFAULT '',
  created INTEGER NOT NULL DEFAULT 0,
  last_activity INTEGER NOT NULL DEFAULT 0,
  message_count INTEGER NOT NULL DEFAULT 0,
  archived INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS sessions_key ON sessions(key);
CREATE TABLE IF NOT EXISTS messages(
  id INTEGER PRIMARY KEY,
  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
  ord INTEGER NOT NULL,
  role INTEGER NOT NULL,
  kind INTEGER NOT NULL,
  timestamp INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS messages_session ON messages(session_id);
CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(text, tokenize='unicode61 remove_diacritics 2');
CREATE TABLE IF NOT EXISTS chunks(
  message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
  chunk INTEGER NOT NULL,
  offset INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  PRIMARY KEY(message_id, chunk));
CREATE INDEX IF NOT EXISTS chunks_hash ON chunks(hash);
CREATE TABLE IF NOT EXISTS embeddings(hash INTEGER PRIMARY KEY, vector BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS tags(
  session_key TEXT NOT NULL,
  tag TEXT NOT NULL,
  PRIMARY KEY(session_key, tag));
)sql");
    // Columns added after the first version.
    bool hasFolder = false;
    {
        auto columns = db.Prepare("PRAGMA table_info(sessions)");
        while (columns.Step()) hasFolder = hasFolder || columns.Text(1) == "folder";
    }
    if (!hasFolder) db.Exec("ALTER TABLE sessions ADD COLUMN folder TEXT NOT NULL DEFAULT ''");
    auto set = db.Prepare("INSERT OR IGNORE INTO meta(key, value) VALUES('schema', ?1)");
    set.Bind(1, int64_t(SchemaVersion)).Run();
}

std::unordered_map<std::wstring, KnownFile> Index::KnownFiles() {
    std::unordered_map<std::wstring, KnownFile> out;
    auto query = db.Prepare("SELECT path, file_size, file_modified FROM sessions");
    while (query.Step()) out[query.WText(0)] = {query.Int(1), query.Int(2)};
    return out;
}

void Index::Remove(std::wstring const& path) {
    auto id = db.Prepare("SELECT id FROM sessions WHERE path = ?1");
    id.Bind(1, Utf8(path));
    if (!id.Step()) return;
    int64_t session = id.Int(0);
    auto fts = db.Prepare("DELETE FROM messages_fts WHERE rowid IN (SELECT id FROM messages WHERE session_id = ?1)");
    fts.Bind(1, session).Run();
    auto rows = db.Prepare("DELETE FROM sessions WHERE id = ?1");
    rows.Bind(1, session).Run();
}

void Index::Store(SourceFile const& file, ParsedSession const& parsed) {
    Remove(file.path.wstring());
    auto insert = db.Prepare(R"sql(
INSERT INTO sessions(path, family, key, external_id, file_size, file_modified, skipped, tool_file, tool,
  account_file, account, title_file, first_prompt, cwd, created, last_activity, message_count)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?8, ?9, ?9, ?10, ?11, ?12, ?13, ?14, ?15))sql");
    insert.Bind(1, Utf8(file.path.wstring()))
        .Bind(2, int64_t(file.family))
        .Bind(3, SessionKey(file.family, parsed.externalId))
        .Bind(4, parsed.externalId)
        .Bind(5, file.size)
        .Bind(6, file.modified)
        .Bind(7, parsed.skip ? (parsed.skipReason.empty() ? std::string("skipped") : parsed.skipReason) : std::string())
        .Bind(8, int64_t(parsed.tool))
        .Bind(9, parsed.account)
        .Bind(10, parsed.titleFile)
        .Bind(11, parsed.firstPrompt)
        .Bind(12, parsed.cwd)
        .Bind(13, parsed.created)
        .Bind(14, parsed.lastActivity ? parsed.lastActivity : file.modified)
        .Bind(15, int64_t(parsed.skip ? 0 : parsed.messages.size()));
    insert.Run();
    if (parsed.skip) return;
    int64_t session = db.LastId();
    auto message = db.Prepare("INSERT INTO messages(session_id, ord, role, kind, timestamp) VALUES(?1, ?2, ?3, ?4, ?5)");
    auto text = db.Prepare("INSERT INTO messages_fts(rowid, text) VALUES(?1, ?2)");
    int64_t ord = 0;
    for (auto const& m : parsed.messages) {
        message.Bind(1, session).Bind(2, ord++).Bind(3, int64_t(m.role)).Bind(4, int64_t(m.kind)).Bind(5, m.timestamp);
        message.Run();
        text.Bind(1, db.LastId()).Bind(2, m.text);
        text.Run();
    }
}

void Index::ApplyMetadata(AppMetadata const& meta) {
    struct Row {
        int64_t id;
        Family family;
        std::string external, accountFile;
        Tool toolFile;
        std::string cwd;
    };
    std::vector<Row> rows;
    {
        auto query = db.Prepare("SELECT id, family, external_id, account_file, tool_file, cwd FROM sessions WHERE skipped = '' AND family IN (1, 2, 4)");
        while (query.Step())
            rows.push_back({query.Int(0), Family(query.Int(1)), std::string(query.Text(2)), std::string(query.Text(3)), Tool(query.Int(4)),
                            std::string(query.Text(5))});
    }
    auto update = db.Prepare("UPDATE sessions SET tool = ?2, account = ?3, title_app = ?4, app_id = ?5, archived = ?6, folder = ?7 WHERE id = ?1");
    auto account = db.Prepare("INSERT OR IGNORE INTO accounts(id, family) VALUES(?1, ?2)");
    std::unordered_set<std::string> accounts;
    for (auto const& row : rows) {
        Tool tool = row.toolFile;
        std::string owner = row.accountFile;
        AppEntry const* app = nullptr;
        if (row.family == Family::Claude) {
            if (auto it = meta.claude.find(row.external); it != meta.claude.end()) app = &it->second;
            if (app) {
                tool = Tool::ClaudeDesktop;
                owner = app->account;
            } else if (owner.empty()) {
                // Recorded nowhere in the transcript: assume the command line's current sign-in.
                owner = meta.claudeCliAccount.empty() ? "claude:" : meta.claudeCliAccount;
            }
        } else if (row.family == Family::Antigravity) {
            owner = "antigravity:";
        } else {
            if (auto it = meta.codex.find(row.external); it != meta.codex.end()) app = &it->second;
            if (owner.empty()) owner = "codex:";
        }
        std::string agyTitle;
        if (row.family == Family::Antigravity)
            if (auto it = meta.antigravityTitles.find(row.external); it != meta.antigravityTitles.end()) agyTitle = it->second;
        update.Bind(1, row.id)
            .Bind(2, int64_t(tool))
            .Bind(3, owner)
            .Bind(4, app ? std::string_view(app->title) : std::string_view(agyTitle))
            .Bind(5, app ? std::string_view(app->appId) : std::string_view())
            .Bind(6, int64_t(app && app->archived))
            .Bind(7, app && !app->project.empty() ? app->project
                     : FolderLabel(app && !app->folder.empty() ? app->folder : row.cwd,
                                   row.family == Family::Codex ? meta.codexProjects : std::vector<CodexProject>{}));
        update.Run();
        if (accounts.insert(owner).second) account.Bind(1, owner).Bind(2, int64_t(row.family)).Run();
    }
    auto active = db.Prepare("INSERT OR REPLACE INTO meta(key, value) VALUES('claude_desktop_account', ?1)");
    active.Bind(1, meta.claudeDesktopAccount).Run();
    auto suggest = db.Prepare("UPDATE accounts SET suggested = ?2 WHERE id = ?1");
    for (auto const& [id, name] : meta.accountNames) suggest.Bind(1, id).Bind(2, name).Run();
}

std::unordered_map<std::string, int64_t> Index::KnownWeb(std::string const& account) {
    std::unordered_map<std::string, int64_t> out;
    auto q = db.Prepare("SELECT external_id, file_modified FROM sessions WHERE family = 3 AND account = ?1");
    q.Bind(1, account);
    while (q.Step()) out[std::string(q.Text(0))] = q.Int(1);
    return out;
}

void Index::StoreWeb(WebConversation const& c) {
    SourceFile file;
    file.path = Wide("web:" + c.id);
    file.family = Family::Web;
    file.size = int64_t(c.messages.size());
    file.modified = c.updated;
    ParsedSession parsed;
    parsed.tool = c.tool;
    parsed.externalId = c.id;
    parsed.account = c.account;
    parsed.titleFile = c.title;
    parsed.created = c.created;
    parsed.lastActivity = c.updated;
    parsed.messages = c.messages;
    for (auto const& m : c.messages)
        if (m.role == Role::User) {
            parsed.firstPrompt = Clip(Squash(m.text), 300);
            break;
        }
    if (parsed.messages.empty()) {
        parsed.skip = true;
        parsed.skipReason = "no conversation";
    }
    Store(file, parsed);
    auto folder = db.Prepare("UPDATE sessions SET folder = ?2 WHERE path = ?1");
    folder.Bind(1, Utf8(file.path.wstring())).Bind(2, c.folder).Run();
}

int Index::RemoveWebMissing(std::string const& account, std::vector<std::string> const& present) {
    std::unordered_set<std::string> keep(present.begin(), present.end());
    std::vector<std::wstring> gone;
    {
        auto q = db.Prepare("SELECT external_id, path FROM sessions WHERE family = 3 AND account = ?1");
        q.Bind(1, account);
        while (q.Step())
            if (!keep.count(std::string(q.Text(0)))) gone.push_back(q.WText(1));
    }
    // A site that answers with nothing, or with far fewer chats than before, is
    // more likely signed out or failing than emptied: keep everything then.
    size_t before = gone.size() + present.size();
    if (present.empty() || (gone.size() > 5 && gone.size() * 2 > before)) return 0;
    for (auto const& path : gone) Remove(path);
    return int(gone.size());
}

void Index::NameAccount(std::string const& account, Family family, std::string const& suggested) {
    auto add = db.Prepare("INSERT OR IGNORE INTO accounts(id, family) VALUES(?1, ?2)");
    add.Bind(1, account).Bind(2, int64_t(family)).Run();
    if (suggested.empty()) return;
    auto name = db.Prepare("UPDATE accounts SET suggested = ?2 WHERE id = ?1 AND suggested = ''");
    name.Bind(1, account).Bind(2, suggested).Run();
}

void Index::ForgetAccount(std::string const& account) {
    db.Exec("DROP TABLE IF EXISTS temp.forget; CREATE TEMP TABLE forget(id INTEGER PRIMARY KEY);");
    auto pick = db.Prepare("INSERT INTO temp.forget SELECT id FROM sessions WHERE account = ?1");
    pick.Bind(1, account).Run();
    db.Exec(
        "DELETE FROM messages_fts WHERE rowid IN (SELECT m.id FROM messages m JOIN temp.forget f ON f.id = m.session_id);"
        "DELETE FROM sessions WHERE id IN (SELECT id FROM temp.forget); DROP TABLE temp.forget;");
}

void Index::ForgetAll() {
    // Files are re-read from disk; synced web conversations are kept.
    db.Exec("DELETE FROM messages_fts WHERE rowid IN (SELECT m.id FROM messages m JOIN sessions s ON s.id = m.session_id "
            "WHERE s.family IN (1, 2, 4)); DELETE FROM sessions WHERE family IN (1, 2, 4);");
}

std::vector<SessionRow> Index::LoadSessions() {
    std::unordered_map<std::string, std::vector<std::wstring>> tags;
    {
        auto query = db.Prepare("SELECT session_key, tag FROM tags ORDER BY tag");
        while (query.Step()) tags[std::string(query.Text(0))].push_back(query.WText(1));
    }
    std::vector<SessionRow> out;
    auto query = db.Prepare(R"sql(
SELECT id, key, tool, COALESCE(NULLIF(title_app, ''), NULLIF(title_file, ''), first_prompt), account, cwd, external_id,
  app_id, path, created, last_activity, message_count, archived, folder, first_prompt
FROM sessions WHERE skipped = '' ORDER BY last_activity DESC)sql");
    while (query.Step()) {
        SessionRow row;
        row.id = query.Int(0);
        row.key = query.Text(1);
        row.tool = Tool(query.Int(2));
        row.title = query.WText(3);
        row.account = query.Text(4);
        row.cwd = query.WText(5);
        row.externalId = query.WText(6);
        row.appId = query.WText(7);
        row.path = query.WText(8);
        row.created = query.Int(9);
        row.lastActivity = query.Int(10);
        row.messages = int(query.Int(11));
        row.archived = query.Int(12) != 0;
        row.folder = query.WText(13);
        if (auto it = tags.find(row.key); it != tags.end()) row.tags = it->second;
        bool meaningful = false;
        for (wchar_t c : row.title) meaningful = meaningful || iswalnum(c);
        if (!meaningful) row.title = query.WText(14);
        if (row.title.empty()) row.title = L"Untitled session";
        out.push_back(std::move(row));
    }
    return out;
}

std::vector<AccountRow> Index::Accounts() {
    std::vector<AccountRow> out;
    auto query = db.Prepare(R"sql(
SELECT a.id, a.family, a.name, a.suggested, (SELECT count(*) FROM sessions s WHERE s.account = a.id AND s.skipped = '')
FROM accounts a ORDER BY a.family, a.id)sql");
    while (query.Step()) {
        AccountRow row;
        row.id = query.Text(0);
        row.family = Family(query.Int(1));
        row.name = query.WText(2);
        row.named = !row.name.empty();
        if (row.name.empty()) row.name = query.WText(3);
        if (row.name.empty()) row.name = DefaultAccountName(row.id);
        row.sessions = int(query.Int(4));
        if (row.sessions) out.push_back(std::move(row));
    }
    return out;
}

std::vector<std::wstring> Index::AllTags() {
    std::vector<std::wstring> out;
    auto query = db.Prepare("SELECT DISTINCT tag FROM tags ORDER BY tag COLLATE NOCASE");
    while (query.Step()) out.push_back(query.WText(0));
    return out;
}

std::vector<SearchHit> Index::Search(std::wstring const& query) {
    auto words = KeywordWords(query);
    if (words.empty()) return {};

    std::unordered_map<int64_t, std::wstring> titles;
    {
        auto q = db.Prepare("SELECT id, COALESCE(NULLIF(title_app, ''), NULLIF(title_file, ''), first_prompt) FROM sessions WHERE skipped = ''");
        while (q.Step()) titles[q.Int(0)] = Lower(q.WText(1));
    }
    const double total = double(std::max<size_t>(titles.size(), 1));

    struct Acc {
        double score = 0;
        size_t words = 0;
    };
    std::unordered_map<int64_t, Acc> sessions;
    auto counts = db.Prepare(R"sql(
SELECT m.session_id, count(*) FROM messages_fts JOIN messages m ON m.id = messages_fts.rowid
WHERE messages_fts MATCH ?1 GROUP BY m.session_id)sql");
    for (size_t i = 0; i < words.size(); ++i) {
        const bool last = i + 1 == words.size();
        std::unordered_map<int64_t, double> found;
        try {
            counts.Bind(1, Term(words[i], last && words[i].size() >= 2));
            while (counts.Step()) found[counts.Int(0)] = 1.0 + std::log(double(counts.Int(1)));
            counts.Reset();
        } catch (DbError const&) {
            counts.Reset();
        }
        auto needle = Lower(words[i]);
        for (auto const& [id, title] : titles)
            if (title.find(needle) != std::wstring::npos) found[id] += 3.0;
        const double idf = std::log(1.0 + total / double(std::max<size_t>(found.size(), 1)));
        for (auto const& [id, weight] : found) {
            auto& acc = sessions[id];
            if (acc.words == i) {
                acc.score += weight * idf;
                acc.words = i + 1;
            }
        }
    }

    std::vector<SearchHit> hits;
    for (auto const& [id, acc] : sessions)
        if (acc.words == words.size()) hits.push_back({id, acc.score, 0});
    std::sort(hits.begin(), hits.end(), [](auto const& a, auto const& b) { return a.score > b.score; });

    // Pick the best message per session for its snippet: all words first, then any word.
    std::string all, any;
    for (size_t i = 0; i < words.size(); ++i) {
        auto term = Term(words[i], i + 1 == words.size() && words[i].size() >= 2);
        all += (all.empty() ? "" : " ") + term;
        any += (any.empty() ? "" : " OR ") + term;
    }
    std::unordered_map<int64_t, int64_t> best;
    std::unordered_map<int64_t, size_t> together;  // sessions with one message holding every word, best first
    auto pick = db.Prepare(R"sql(
SELECT m.session_id, messages_fts.rowid FROM messages_fts JOIN messages m ON m.id = messages_fts.rowid
WHERE messages_fts MATCH ?1 ORDER BY bm25(messages_fts) LIMIT 20000)sql");
    bool first = true;
    for (auto const& expression : {all, any}) {
        try {
            pick.Bind(1, expression);
            while (pick.Step()) {
                if (best.try_emplace(pick.Int(0), pick.Int(1)).second && first) together.emplace(pick.Int(0), together.size());
            }
            pick.Reset();
        } catch (DbError const&) {
            pick.Reset();
        }
        first = false;
    }
    for (auto& hit : hits) {
        if (auto it = best.find(hit.session); it != best.end()) hit.bestMessage = it->second;
        hit.together = together.count(hit.session) > 0;
    }
    // Long sessions mention almost everything somewhere: words found together in
    // one message rank ahead of words scattered across a session.
    std::stable_sort(hits.begin(), hits.end(), [&](auto const& a, auto const& b) {
        auto ta = together.find(a.session), tb = together.find(b.session);
        bool ia = ta != together.end(), ib = tb != together.end();
        if (ia != ib) return ia;
        if (ia) return ta->second < tb->second;
        return false;
    });
    return hits;
}

bool Index::KnowsAnyWord(std::wstring const& query) {
    auto words = KeywordWords(query);
    auto q = db.Prepare("SELECT 1 FROM messages_fts WHERE messages_fts MATCH ?1 LIMIT 1");
    for (size_t i = 0; i < words.size(); ++i) {
        bool letters = false;
        for (wchar_t c : words[i]) letters = letters || iswalnum(c);
        if (!letters) continue;
        try {
            q.Bind(1, Term(words[i], false));
            bool found = q.Step();
            q.Reset();
            if (found) return true;
        } catch (DbError const&) {
            q.Reset();
        }
    }
    return false;
}

std::wstring Index::Snippet(std::wstring const& query, int64_t message) {
    auto words = KeywordWords(query);
    if (words.empty() || !message) return {};
    std::string any;
    for (size_t i = 0; i < words.size(); ++i)
        any += (any.empty() ? "" : " OR ") + Term(words[i], i + 1 == words.size() && words[i].size() >= 2);
    try {
        auto q = db.Prepare(
            "SELECT snippet(messages_fts, 0, char(1), char(2), '\xE2\x80\xA6', 24) FROM messages_fts "
            "WHERE messages_fts MATCH ?1 AND rowid = ?2");
        q.Bind(1, any).Bind(2, message);
        if (q.Step()) return q.WText(0);
    } catch (DbError const&) {
    }
    return {};
}

void Index::UseEmbeddingModel(std::string const& name) {
    auto current = db.Prepare("SELECT value FROM meta WHERE key = 'embedding_model'");
    std::string stored = current.Step() ? std::string(current.Text(0)) : std::string();
    current.Reset();
    if (stored == name) return;
    db.Exec("DELETE FROM chunks; DELETE FROM embeddings;");
    auto set = db.Prepare("INSERT OR REPLACE INTO meta(key, value) VALUES('embedding_model', ?1)");
    set.Bind(1, name).Run();
}

int Index::PendingEmbeddingCount() {
    auto q = db.Prepare("SELECT count(*) FROM messages m WHERE NOT EXISTS (SELECT 1 FROM chunks c WHERE c.message_id = m.id)");
    return q.Step() ? int(q.Int(0)) : 0;
}

std::vector<PendingMessage> Index::MessagesToEmbed(int limit) {
    std::vector<PendingMessage> out;
    // Newest sessions first, so recent work becomes searchable by meaning soonest.
    auto q = db.Prepare(R"sql(
SELECT m.id, f.text FROM messages m JOIN sessions s ON s.id = m.session_id JOIN messages_fts f ON f.rowid = m.id
WHERE NOT EXISTS (SELECT 1 FROM chunks c WHERE c.message_id = m.id)
ORDER BY s.last_activity DESC, m.id LIMIT ?1)sql");
    q.Bind(1, int64_t(limit));
    while (q.Step()) out.push_back({q.Int(0), std::string(q.Text(1))});
    return out;
}

bool Index::HasEmbedding(int64_t hash) {
    auto q = db.Prepare("SELECT 1 FROM embeddings WHERE hash = ?1");
    q.Bind(1, hash);
    return q.Step();
}

void Index::StoreEmbedding(int64_t hash, std::vector<float> const& vector) {
    std::string blob(vector.size(), '\0');
    for (size_t i = 0; i < vector.size(); ++i)
        blob[i] = char(int8_t(std::lround(std::clamp(vector[i], -1.0f, 1.0f) * 127.0f)));
    auto q = db.Prepare("INSERT OR REPLACE INTO embeddings(hash, vector) VALUES(?1, ?2)");
    q.Bind(1, hash).BindBlob(2, blob.data(), blob.size()).Run();
}

void Index::StoreChunk(int64_t message, int chunk, int32_t offset, int64_t hash) {
    auto q = db.Prepare("INSERT OR REPLACE INTO chunks(message_id, chunk, offset, hash) VALUES(?1, ?2, ?3, ?4)");
    q.Bind(1, message).Bind(2, int64_t(chunk)).Bind(3, int64_t(offset)).Bind(4, hash).Run();
}

void Index::PruneEmbeddings() {
    db.Exec("DELETE FROM embeddings WHERE hash NOT IN (SELECT hash FROM chunks)");
}

int64_t Index::VectorsVersion() {
    auto q = db.Prepare("SELECT value FROM meta WHERE key = 'vectors_version'");
    return q.Step() ? std::atoll(std::string(q.Text(0)).c_str()) : 0;
}

void Index::BumpVectorsVersion() {
    db.Exec("INSERT INTO meta(key, value) VALUES('vectors_version', '1') "
            "ON CONFLICT(key) DO UPDATE SET value = CAST(value AS INTEGER) + 1");
}

std::vector<ChunkVector> Index::LoadVectors() {
    std::vector<ChunkVector> out;
    auto q = db.Prepare(R"sql(
SELECT m.session_id, c.message_id, c.offset, e.vector FROM chunks c
JOIN embeddings e ON e.hash = c.hash JOIN messages m ON m.id = c.message_id
JOIN sessions s ON s.id = m.session_id WHERE s.skipped = '')sql");
    while (q.Step()) {
        ChunkVector chunk;
        chunk.session = q.Int(0);
        chunk.message = q.Int(1);
        chunk.offset = int32_t(q.Int(2));
        auto blob = q.Text(3);
        chunk.vector.assign(reinterpret_cast<int8_t const*>(blob.data()), reinterpret_cast<int8_t const*>(blob.data()) + blob.size());
        out.push_back(std::move(chunk));
    }
    return out;
}

std::wstring Index::Passage(int64_t message, int32_t offset, size_t bytes) {
    auto q = db.Prepare("SELECT text FROM messages_fts WHERE rowid = ?1");
    q.Bind(1, message);
    if (!q.Step()) return {};
    auto text = q.Text(0);
    size_t start = std::min<size_t>(size_t(std::max(offset, 0)), text.size());
    std::string piece = Clip(text.substr(start), bytes);
    return (start > 0 ? L"…" : L"") + Wide(Squash(piece));
}

std::string Index::MetaValue(std::string const& key) {
    auto q = db.Prepare("SELECT value FROM meta WHERE key = ?1");
    q.Bind(1, key);
    return q.Step() ? std::string(q.Text(0)) : std::string();
}

void Index::SetMetaValue(std::string const& key, std::string const& value) {
    auto q = db.Prepare("INSERT OR REPLACE INTO meta(key, value) VALUES(?1, ?2)");
    q.Bind(1, key).Bind(2, value).Run();
}

std::wstring Index::AccountName(std::string const& account) {
    for (auto const& row : Accounts())
        if (row.id == account) return row.name;
    return DefaultAccountName(account);
}

void Index::RenameAccount(std::string const& account, std::wstring const& name) {
    auto q = db.Prepare("UPDATE accounts SET name = ?2 WHERE id = ?1");
    q.Bind(1, account).Bind(2, Utf8(name)).Run();
}

void Index::AddTag(std::string const& key, std::wstring const& tag) {
    auto q = db.Prepare("INSERT OR IGNORE INTO tags(session_key, tag) VALUES(?1, ?2)");
    q.Bind(1, key).Bind(2, Utf8(tag)).Run();
}

void Index::RemoveTag(std::string const& key, std::wstring const& tag) {
    auto q = db.Prepare("DELETE FROM tags WHERE session_key = ?1 AND tag = ?2");
    q.Bind(1, key).Bind(2, Utf8(tag)).Run();
}

int Index::CountSkipped(std::string const& reason) {
    auto q = db.Prepare("SELECT count(*) FROM sessions WHERE skipped = ?1");
    q.Bind(1, reason);
    return q.Step() ? int(q.Int(0)) : 0;
}

Index::Totals Index::Count() {
    Totals totals;
    auto q = db.Prepare("SELECT sum(skipped = ''), sum(skipped <> ''), (SELECT count(*) FROM messages) FROM sessions");
    if (q.Step()) {
        totals.sessions = int(q.Int(0));
        totals.skipped = int(q.Int(1));
        totals.messages = int(q.Int(2));
    }
    return totals;
}

}  // namespace sessions
