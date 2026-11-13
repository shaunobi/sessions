#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Keeps the database in step with the session stores while the app is open:
// a catch-up scan on launch (comparing file sizes and modification times), then
// folder change notifications. Nothing runs once the window closes.
namespace sessions {

struct IndexStatus {
    enum class State { Scanning, Indexing, Ready, Failed } state = State::Scanning;
    int done = 0;
    int total = 0;
    bool dataChanged = false;    // the session list should be reloaded
    std::wstring message;
};

class Indexer {
public:
    using Callback = std::function<void(IndexStatus const&)>;

    Indexer(std::filesystem::path database, Callback callback);
    ~Indexer();
    Indexer(Indexer const&) = delete;
    Indexer& operator=(Indexer const&) = delete;

    void Start();
    void Stop();
    void Rescan();
    void RebuildAccount(std::string const& account);
    void RebuildAll();

    // One synchronous catch-up scan, for the command-line test mode.
    static IndexStatus RunOnce(std::filesystem::path const& database, Callback const& callback, bool embed = true);

private:
    std::filesystem::path database;
    Callback callback;
    std::thread worker;
    void* stopEvent = nullptr;
    void* requestEvent = nullptr;
    std::mutex mutex;
    std::vector<std::string> forgetAccounts;
    bool forgetAll = false;

    void Run();
};

}  // namespace sessions
