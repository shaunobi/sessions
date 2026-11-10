#include "Db.h"
#include "Text.h"
#include "sqlite3.h"

namespace sessions {

namespace {
[[noreturn]] void Fail(sqlite3* db, char const* what) {
    std::string message = what;
    if (db) {
        message += ": ";
        message += sqlite3_errmsg(db);
    }
    throw DbError(message);
}
}  // namespace

Statement::Statement(sqlite3* handle, std::string_view sql) : db(handle) {
    if (sqlite3_prepare_v2(db, sql.data(), int(sql.size()), &statement, nullptr) != SQLITE_OK) Fail(db, "prepare");
}

Statement::~Statement() {
    if (statement) sqlite3_finalize(statement);
}

Statement::Statement(Statement&& other) noexcept : db(other.db), statement(other.statement) {
    other.statement = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (statement) sqlite3_finalize(statement);
        db = other.db;
        statement = other.statement;
        other.statement = nullptr;
    }
    return *this;
}

Statement& Statement::Bind(int index, int64_t value) {
    if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) Fail(db, "bind");
    return *this;
}

Statement& Statement::Bind(int index, std::string_view utf8) {
    // An empty view may have no data pointer; SQLite would store that as NULL.
    char const* data = utf8.data() ? utf8.data() : "";
    if (sqlite3_bind_text(statement, index, data, int(utf8.size()), SQLITE_TRANSIENT) != SQLITE_OK) Fail(db, "bind");
    return *this;
}

Statement& Statement::BindNull(int index) {
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) Fail(db, "bind");
    return *this;
}

bool Statement::Step() {
    int result = sqlite3_step(statement);
    if (result == SQLITE_ROW) return true;
    if (result == SQLITE_DONE) return false;
    Fail(db, "step");
}

void Statement::Reset() {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
}

int64_t Statement::Int(int column) const { return sqlite3_column_int64(statement, column); }
double Statement::Real(int column) const { return sqlite3_column_double(statement, column); }

std::string_view Statement::Text(int column) const {
    auto text = reinterpret_cast<char const*>(sqlite3_column_text(statement, column));
    return text ? std::string_view(text, size_t(sqlite3_column_bytes(statement, column))) : std::string_view();
}

std::wstring Statement::WText(int column) const { return Wide(Text(column)); }
bool Statement::IsNull(int column) const { return sqlite3_column_type(statement, column) == SQLITE_NULL; }

Db::Db(std::filesystem::path const& file, bool readOnly) {
    int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    flags |= SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI;
    std::string name = Utf8(file.wstring());
    if (sqlite3_open_v2(name.c_str(), &db, flags, nullptr) != SQLITE_OK) {
        std::string message = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        db = nullptr;
        throw DbError("open " + name + ": " + message);
    }
    sqlite3_busy_timeout(db, 5000);
}

Db::~Db() {
    if (db) sqlite3_close_v2(db);
}

Db::Db(Db&& other) noexcept : db(other.db) { other.db = nullptr; }

Db& Db::operator=(Db&& other) noexcept {
    if (this != &other) {
        if (db) sqlite3_close_v2(db);
        db = other.db;
        other.db = nullptr;
    }
    return *this;
}

void Db::Exec(std::string_view sql) {
    std::string text(sql);
    char* error = nullptr;
    if (sqlite3_exec(db, text.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "exec";
        sqlite3_free(error);
        throw DbError(message);
    }
}

int64_t Db::LastId() const { return sqlite3_last_insert_rowid(db); }
int Db::Changes() const { return sqlite3_changes(db); }

Transaction::Transaction(Db& database) : db(database) { db.Exec("BEGIN IMMEDIATE"); }

Transaction::~Transaction() {
    if (!done) {
        try { db.Exec("ROLLBACK"); } catch (...) {}
    }
}

void Transaction::Commit() {
    db.Exec("COMMIT");
    done = true;
}

}  // namespace sessions
