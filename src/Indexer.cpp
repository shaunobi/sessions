#include "Indexer.h"
#include "Index.h"
#include "Sources.h"
#include "Text.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <utility>
#include <unordered_set>

namespace sessions {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

struct ScanRequest {
    std::vector<std::string> forgetAccounts;
    bool forgetAll = false;
};

std::wstring Plural(int n, wchar_t const* one, wchar_t const* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

IndexStatus Scan(Index& index, ScanRequest const& request, Indexer::Callback const& callback, HANDLE stop) {
    IndexStatus status;
    status.state = IndexStatus::State::Scanning;
    status.message = L"Scanning";
    callback(status);

    auto roots = SourceRoots::Detect();
    bool changed = false;
    if (request.forgetAll || !request.forgetAccounts.empty()) {
        Transaction transaction(index.Database());
        if (request.forgetAll) index.ForgetAll();
        for (auto const& account : request.forgetAccounts) index.ForgetAccount(account);
        transaction.Commit();
        changed = true;
    }

    // Read the apps' titles and accounts first so cards look right while indexing.
    auto meta = ReadAppMetadata(roots);
    auto files = FindSourceFiles(roots);
    auto known = index.KnownFiles();
    std::vector<SourceFile> todo;
    {
        std::unordered_set<std::wstring> present;
        for (auto const& file : files) {
            auto path = file.path.wstring();
            present.insert(path);
            auto it = known.find(path);
            if (it == known.end() || it->second.size != file.size || it->second.modified != file.modified) todo.push_back(file);
        }
        Transaction transaction(index.Database());
        for (auto const& [path, _] : known) {
            if (!present.count(path)) {
                index.Remove(path);
                changed = true;
            }
        }
        transaction.Commit();
    }
    // Newest first, so today's sessions appear before the long tail.
    std::sort(todo.begin(), todo.end(), [](auto const& a, auto const& b) { return a.modified > b.modified; });

    if (!todo.empty()) {
        status.state = IndexStatus::State::Indexing;
        status.total = int(todo.size());
        status.message = L"Indexing " + Plural(status.total, L"file", L"files");
        callback(status);

        std::mutex mutex;
        std::condition_variable ready, room;
        std::deque<std::pair<size_t, ParsedSession>> results;
        std::atomic<size_t> next{0};
        std::atomic<bool> cancelled{false};
        const size_t queueLimit = 32;
        const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
        const size_t count = std::min<size_t>(todo.size(), std::clamp<unsigned>(cores / 2, 2u, 12u));
        std::vector<std::thread> workers;
        size_t finishedWorkers = 0;
        for (size_t w = 0; w < count; ++w) {
            workers.emplace_back([&] {
                // Background priority: indexing should never make the machine feel busy.
                SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                for (;;) {
                    size_t i = next.fetch_add(1);
                    if (i >= todo.size() || cancelled) break;
                    ParsedSession parsed;
                    try {
                        parsed = todo[i].family == Family::Claude ? ParseClaude(todo[i].path) : ParseCodex(todo[i].path);
                    } catch (...) {
                        parsed.skip = true;
                        parsed.skipReason = "error";
                    }
                    std::unique_lock lock(mutex);
                    room.wait(lock, [&] { return results.size() < queueLimit || cancelled; });
                    results.emplace_back(i, std::move(parsed));
                    ready.notify_one();
                }
                std::lock_guard lock(mutex);
                ++finishedWorkers;
                ready.notify_one();
                SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            });
        }

        auto lastCommit = Clock::now(), lastReport = Clock::now();
        std::unique_ptr<Transaction> transaction = std::make_unique<Transaction>(index.Database());
        int pending = 0;
        for (;;) {
            std::pair<size_t, ParsedSession> item;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return !results.empty() || finishedWorkers == count; });
                if (results.empty()) break;
                item = std::move(results.front());
                results.pop_front();
                room.notify_one();
            }
            if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
                cancelled = true;
                room.notify_all();
                continue;
            }
            index.Store(todo[item.first], item.second);
            ++pending;
            ++status.done;
            changed = true;
            auto now = Clock::now();
            if (pending >= 200 || now - lastCommit > std::chrono::seconds(2)) {
                index.ApplyMetadata(meta);
                transaction->Commit();
                transaction = std::make_unique<Transaction>(index.Database());
                pending = 0;
                lastCommit = now;
                status.dataChanged = true;
            }
            if (status.dataChanged || now - lastReport > std::chrono::milliseconds(250)) {
                status.message = L"Indexing " + std::to_wstring(status.done) + L" of " + std::to_wstring(status.total);
                callback(status);
                status.dataChanged = false;
                lastReport = now;
            }
        }
        for (auto& worker : workers) worker.join();
        transaction->Commit();
        if (cancelled) {
            status.state = IndexStatus::State::Failed;
            status.message = L"Stopped";
            return status;
        }
    }

    {
        meta = ReadAppMetadata(roots);
        Transaction transaction(index.Database());
        index.ApplyMetadata(meta);
        transaction.Commit();
    }
    auto totals = index.Count();
    status.state = IndexStatus::State::Ready;
    status.dataChanged = true;  // titles or accounts may have changed even with no new files
    (void)changed;
    status.done = status.total;
    status.message = L"Up to date · " + Plural(totals.sessions, L"session", L"sessions");
    callback(status);
    return status;
}

}  // namespace

Indexer::Indexer(fs::path db, Callback cb) : database(std::move(db)), callback(std::move(cb)) {
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Indexer::~Indexer() {
    Stop();
    CloseHandle(stopEvent);
    CloseHandle(requestEvent);
}

void Indexer::Start() {
    if (!worker.joinable()) worker = std::thread([this] { Run(); });
}

void Indexer::Stop() {
    SetEvent(stopEvent);
    if (worker.joinable()) worker.join();
}

void Indexer::Rescan() { SetEvent(requestEvent); }

void Indexer::RebuildAccount(std::string const& account) {
    {
        std::lock_guard lock(mutex);
        forgetAccounts.push_back(account);
    }
    SetEvent(requestEvent);
}

void Indexer::RebuildAll() {
    {
        std::lock_guard lock(mutex);
        forgetAll = true;
    }
    SetEvent(requestEvent);
}

void Indexer::Run() {
    try {
        Index index(database);
        auto scan = [&] {
            ScanRequest request;
            {
                std::lock_guard lock(mutex);
                request.forgetAccounts.swap(forgetAccounts);
                request.forgetAll = std::exchange(forgetAll, false);
            }
            try {
                Scan(index, request, callback, stopEvent);
            } catch (std::exception const& e) {
                IndexStatus failed;
                failed.state = IndexStatus::State::Failed;
                failed.message = L"Index error: " + Wide(e.what());
                callback(failed);
            }
        };
        scan();

        std::vector<HANDLE> changes;
        for (auto const& root : SourceRoots::Detect().Watched()) {
            HANDLE change = FindFirstChangeNotificationW(root.c_str(), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
            if (change != INVALID_HANDLE_VALUE) changes.push_back(change);
        }
        std::vector<HANDLE> handles{stopEvent, requestEvent};
        handles.insert(handles.end(), changes.begin(), changes.end());

        auto lastScan = Clock::now();
        for (;;) {
            DWORD result = WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) break;
            if (result == WAIT_FAILED) break;
            bool requested = result == WAIT_OBJECT_0 + 1;
            if (!requested && result < WAIT_OBJECT_0 + handles.size()) {
                FindNextChangeNotification(handles[result - WAIT_OBJECT_0]);
                // Live sessions write constantly: let changes settle, and scan at most
                // every 15 seconds for them.
                auto quietUntil = std::max(Clock::now() + std::chrono::seconds(4), lastScan + std::chrono::seconds(15));
                for (;;) {
                    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(quietUntil - Clock::now()).count();
                    if (wait <= 0) break;
                    DWORD again = WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, DWORD(wait));
                    if (again == WAIT_OBJECT_0) goto done;
                    if (again == WAIT_OBJECT_0 + 1) break;
                    if (again > WAIT_OBJECT_0 + 1 && again < WAIT_OBJECT_0 + handles.size())
                        FindNextChangeNotification(handles[again - WAIT_OBJECT_0]);
                }
            }
            scan();
            lastScan = Clock::now();
        }
    done:
        for (auto change : changes) FindCloseChangeNotification(change);
    } catch (std::exception const& e) {
        IndexStatus failed;
        failed.state = IndexStatus::State::Failed;
        failed.message = L"Could not open the index: " + Wide(e.what());
        callback(failed);
    }
}

IndexStatus Indexer::RunOnce(fs::path const& database, Callback const& callback) {
    Index index(database);
    HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto status = Scan(index, {}, callback, never);
    CloseHandle(never);
    return status;
}

}  // namespace sessions
