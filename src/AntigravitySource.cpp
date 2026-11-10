#include "Sources.h"
#include "Db.h"
#include "Text.h"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>

// Antigravity (Google) keeps conversations as protobuf "trajectories":
//  - desktop app: ~/.gemini/antigravity/conversations/<id>.pb, AES-256-GCM
//    encrypted (12-byte nonce, ciphertext, 16-byte tag) with a fixed key built
//    into its language server;
//  - agy command line: ~/.gemini/antigravity-cli/conversations/<id>.db, SQLite
//    with one protobuf step per row.
// Field numbers are observed, not documented (see docs/decisions.md).
namespace sessions {

namespace fs = std::filesystem;

namespace {

constexpr size_t MaxMessageBytes = 64 * 1024;

// ---- Minimal protobuf reader -------------------------------------------------

struct Field {
    uint32_t number = 0;
    uint32_t wire = 0;
    uint64_t value = 0;          // varint
    std::string_view bytes;      // length-delimited
};

class Proto {
public:
    explicit Proto(std::string_view data) : p(data.data()), end(data.data() + data.size()) {}

    bool Next(Field& field) {
        if (p >= end) return false;
        uint64_t key{};
        if (!Varint(key)) return Fail();
        field = Field{};
        field.number = uint32_t(key >> 3);
        field.wire = uint32_t(key & 7);
        switch (field.wire) {
        case 0: return Varint(field.value) || Fail();
        case 1: return Skip(8);
        case 5: return Skip(4);
        case 2: {
            uint64_t length{};
            if (!Varint(length) || length > uint64_t(end - p)) return Fail();
            field.bytes = std::string_view(p, size_t(length));
            p += length;
            return true;
        }
        default: return Fail();
        }
    }

private:
    char const* p;
    char const* end;

    bool Fail() {
        p = end;
        return false;
    }
    bool Skip(size_t n) {
        if (size_t(end - p) < n) return Fail();
        p += n;
        return true;
    }
    bool Varint(uint64_t& out) {
        out = 0;
        for (int shift = 0; shift < 64 && p < end; shift += 7) {
            uint8_t c = uint8_t(*p++);
            out |= uint64_t(c & 0x7F) << shift;
            if (c < 0x80) return true;
        }
        return false;
    }
};

// The first length-delimited field with this number, or nothing.
std::optional<std::string_view> Sub(std::string_view message, uint32_t number) {
    Proto proto(message);
    Field field;
    while (proto.Next(field))
        if (field.number == number && field.wire == 2) return field.bytes;
    return std::nullopt;
}

uint64_t VarintField(std::string_view message, uint32_t number) {
    Proto proto(message);
    Field field;
    while (proto.Next(field))
        if (field.number == number && field.wire == 0) return field.value;
    return 0;
}

int64_t TimestampMs(std::string_view timestamp) {
    return int64_t(VarintField(timestamp, 1)) * 1000 + int64_t(VarintField(timestamp, 2) / 1000000);
}

std::string FromFileUri(std::string_view uri) {
    // file:///C:/Users/... to C:\Users\...
    constexpr std::string_view prefix = "file:///";
    if (uri.substr(0, prefix.size()) != prefix) return std::string(uri);
    std::string path(uri.substr(prefix.size()));
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            out.push_back(char(std::stoi(path.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(path[i] == '/' ? '\\' : path[i]);
        }
    }
    return out;
}

// One step of a trajectory: the person's message (type 14, field 19.2) or the
// AI's prose reply (type 15, field 20.1). Tool steps are skipped.
void ReadStep(std::string_view step, ParsedSession& session) {
    uint64_t type = VarintField(step, 1);
    int64_t timestamp = 0;
    if (auto meta = Sub(step, 5))
        if (auto time = Sub(*meta, 1)) timestamp = TimestampMs(*time);
    if (timestamp) {
        if (!session.created || timestamp < session.created) session.created = timestamp;
        if (timestamp > session.lastActivity) session.lastActivity = timestamp;
    }
    std::string text;
    Role role = Role::User;
    if (type == 14) {
        auto user = Sub(step, 19);
        if (!user) return;
        if (auto said = Sub(*user, 2)) text = std::string(*said);
        if (session.cwd.empty())
            if (auto context = Sub(*user, 12))
                if (auto workspace = Sub(*context, 12)) session.cwd = FromFileUri(*workspace);
    } else if (type == 15) {
        auto reply = Sub(step, 20);
        if (!reply) return;
        if (auto said = Sub(*reply, 1)) text = std::string(*said);
        role = Role::Assistant;
    } else {
        return;
    }
    if (text.find_first_not_of(" \r\n\t") == std::string::npos) return;
    if (role == Role::User && session.firstPrompt.empty()) session.firstPrompt = Clip(Squash(text), 300);
    session.messages.push_back({role, Kind::Said, timestamp, Clip(text, MaxMessageBytes)});
}

std::string ReadFileBytes(fs::path const& file) {
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// ---- Decryption --------------------------------------------------------------

bool Decrypt(std::string const& key, std::string_view sealed, std::string& plain) {
    constexpr size_t nonceSize = 12, tagSize = 16;
    if (key.size() != 32 || sealed.size() < nonceSize + tagSize) return false;
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_KEY_HANDLE handle{};
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0 &&
        BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0 &&
        BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0, (PUCHAR)key.data(), ULONG(key.size()), 0) == 0) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = (PUCHAR)sealed.data();
        info.cbNonce = ULONG(nonceSize);
        info.pbTag = (PUCHAR)(sealed.data() + sealed.size() - tagSize);
        info.cbTag = ULONG(tagSize);
        auto cipher = sealed.substr(nonceSize, sealed.size() - nonceSize - tagSize);
        plain.assign(cipher.size(), '\0');
        ULONG written = 0;
        // GCM checks the tag: a wrong key fails here rather than giving garbage.
        ok = BCryptDecrypt(handle, (PUCHAR)cipher.data(), ULONG(cipher.size()), &info, nullptr, 0, (PUCHAR)plain.data(),
                           ULONG(plain.size()), &written, 0) == 0;
        plain.resize(written);
    }
    if (handle) BCryptDestroyKey(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

fs::path LanguageServer() {
    wchar_t local[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return fs::path(local) / L"Programs" / L"Antigravity" / L"resources" / L"app" / L"extensions" / L"antigravity" / L"bin" /
           L"language_server_windows_x64.exe";
}

// The key is a fixed 32-character ASCII constant inside Antigravity's language
// server, stored as two 16-character halves near the word "Codeium". Rather
// than copy it into this program, it is found in the installed program: every
// 32-character printable stretch around "Codeium" is tried against a real
// conversation, and only one that authenticates is accepted.
std::string FindKey(std::string_view sample) {
    std::string program = ReadFileBytes(LanguageServer());
    if (program.empty()) return {};
    auto printable = [&](size_t i) { return program[i] >= 0x21 && program[i] <= 0x7E; };
    std::string plain;
    for (size_t at = program.find("Codeium"); at != std::string::npos; at = program.find("Codeium", at + 1)) {
        size_t from = at >= 40 ? at - 40 : 0;
        for (size_t start = from; start <= at && start + 32 <= program.size(); ++start) {
            bool usable = true;
            for (size_t i = start; i < start + 32 && usable; ++i) usable = printable(i);
            if (!usable) continue;
            std::string candidate = program.substr(start, 32);
            if (Decrypt(candidate, sample, plain)) return candidate;
        }
    }
    return {};
}

std::mutex keyMutex;
std::string key;
bool keySearched = false;

bool DecryptConversation(fs::path const& file, std::string_view sealed, std::string& plain) {
    std::lock_guard lock(keyMutex);
    if (!key.empty() && Decrypt(key, sealed, plain)) return true;
    if (keySearched) return false;
    // First use, or Antigravity was updated with a new key: look for it once,
    // testing candidates against the smallest conversation (quickest to check).
    keySearched = true;
    fs::path smallest = file;
    std::error_code error;
    uintmax_t best = fs::file_size(file, error);
    for (auto const& entry : fs::directory_iterator(file.parent_path(), error)) {
        if (entry.path().extension() != L".pb") continue;
        auto size = entry.file_size(error);
        if (!error && size > 28 && size < best) {
            best = size;
            smallest = entry.path();
        }
    }
    key = FindKey(ReadFileBytes(smallest));
    return !key.empty() && Decrypt(key, sealed, plain);
}

}  // namespace

ParsedSession ParseAntigravityDesktop(fs::path const& file) {
    ParsedSession session;
    session.tool = Tool::AntigravityDesktop;
    session.externalId = Utf8(file.stem().wstring());
    session.account = "antigravity:";
    std::string sealed = ReadFileBytes(file), plain;
    if (sealed.empty()) {
        session.skip = true;
        session.skipReason = "unreadable";
        return session;
    }
    if (!DecryptConversation(file, sealed, plain)) {
        session.skip = true;
        session.skipReason = "encrypted";  // reported in the status line, never skipped silently
        return session;
    }
    Proto proto(plain);
    Field field;
    while (proto.Next(field)) {
        // Field 1 is the trajectory id; the file name is the conversation id that titles and links use.
        if (field.number == 2 && field.wire == 2) ReadStep(field.bytes, session);
    }
    if (session.messages.empty()) {
        session.skip = true;
        session.skipReason = "no conversation";
    }
    return session;
}

ParsedSession ParseAntigravityCli(fs::path const& file) {
    ParsedSession session;
    session.tool = Tool::AntigravityCli;
    session.externalId = Utf8(file.stem().wstring());
    session.account = "antigravity:";
    try {
        Db db(file, true);
        auto steps = db.Prepare("SELECT step_payload FROM steps ORDER BY idx");
        while (steps.Step()) ReadStep(steps.Text(0), session);
        auto meta = db.Prepare("SELECT data FROM trajectory_metadata_blob LIMIT 1");
        if (meta.Step() && session.cwd.empty())
            if (auto workspace = Sub(meta.Text(0), 7)) session.cwd = FromFileUri(*workspace);
    } catch (DbError const&) {
        session.skip = true;
        session.skipReason = "unreadable";
        return session;
    }
    if (session.messages.empty()) {
        session.skip = true;
        session.skipReason = "no conversation";
    }
    return session;
}

std::unordered_map<std::string, std::string> ReadAntigravityTitles() {
    // %APPDATA%\Antigravity\User\globalStorage\state.vscdb, key
    // antigravityUnifiedStateSync.trajectorySummaries: base64 protobuf, one entry
    // per conversation (1: id, 2.1: base64 of a summary whose field 1 is the title).
    // Only this key is read; the same file holds Antigravity's sign-in token.
    std::unordered_map<std::string, std::string> titles;
    auto file = RoamingFolder() / L"Antigravity" / L"User" / L"globalStorage" / L"state.vscdb";
    std::error_code error;
    if (!fs::exists(file, error)) return titles;
    auto decode64 = [](std::string_view text) {
        std::string out;
        DWORD size = 0;
        if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) return out;
        out.resize(size);
        if (!CryptStringToBinaryA(text.data(), DWORD(text.size()), CRYPT_STRING_BASE64, (BYTE*)out.data(), &size, nullptr, nullptr))
            out.clear();
        out.resize(size);
        return out;
    };
    try {
        Db db;
        try {
            db = Db(file, true);
            db.Exec("SELECT 1 FROM ItemTable LIMIT 1");
        } catch (DbError const&) {
            db = Db(fs::path(L"file:" + file.generic_wstring() + L"?immutable=1"), true);
        }
        auto query = db.Prepare("SELECT value FROM ItemTable WHERE key = 'antigravityUnifiedStateSync.trajectorySummaries'");
        if (!query.Step()) return titles;
        std::string all = decode64(query.Text(0));
        Proto entries(all);
        Field entry;
        while (entries.Next(entry)) {
            if (entry.number != 1 || entry.wire != 2) continue;
            auto id = Sub(entry.bytes, 1);
            auto wrapper = Sub(entry.bytes, 2);
            if (!id || !wrapper) continue;
            auto encoded = Sub(*wrapper, 1);
            if (!encoded) continue;
            std::string summary = decode64(*encoded);
            if (auto title = Sub(summary, 1)) titles[std::string(*id)] = std::string(*title);
        }
    } catch (DbError const&) {
    }
    return titles;
}

}  // namespace sessions
