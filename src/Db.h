#pragma once
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

// Thin RAII wrapper over the SQLite C API (compiled into the executable).
namespace sessions {

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Statement {
public:
    Statement() = default;
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(Statement const&) = delete;
    Statement& operator=(Statement const&) = delete;

    Statement& Bind(int index, int64_t value);
    Statement& Bind(int index, std::string_view utf8);
    Statement& BindNull(int index);
    // Returns true while a row is available.
    bool Step();
    void Run() { Step(); Reset(); }
    void Reset();

    int64_t Int(int column) const;
    double Real(int column) const;
    std::string_view Text(int column) const;
    std::wstring WText(int column) const;
    bool IsNull(int column) const;

private:
    sqlite3* db = nullptr;
    sqlite3_stmt* statement = nullptr;
};

class Db {
public:
    Db() = default;
    explicit Db(std::filesystem::path const& file, bool readOnly = false);
    ~Db();
    Db(Db&& other) noexcept;
    Db& operator=(Db&& other) noexcept;
    Db(Db const&) = delete;
    Db& operator=(Db const&) = delete;

    bool IsOpen() const { return db != nullptr; }
    void Exec(std::string_view sql);
    Statement Prepare(std::string_view sql) { return Statement(db, sql); }
    int64_t LastId() const;
    int Changes() const;
    sqlite3* Handle() const { return db; }

private:
    sqlite3* db = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT, rolled back if not committed.
class Transaction {
public:
    explicit Transaction(Db& db);
    ~Transaction();
    void Commit();

private:
    Db& db;
    bool done = false;
};

}  // namespace sessions
