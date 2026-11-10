#include "Indexer.h"
#include "Embedder.h"
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
    // When the way transcripts are read changes, re-read everything. Meaning
    // vectors are kept by content, so unchanged text is not embedded again.
    constexpr char const* ParserVersion = "2";
    const bool reparse = index.MetaValue("parser_version") != ParserVersion;
    if (request.forgetAll || reparse || !request.forgetAccounts.empty()) {
        Transaction transaction(index.Database());
        if (request.forgetAll || reparse) index.ForgetAll();
        index.SetMetaValue("parser_version", ParserVersion);
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

uint64_t HashTokens(std::vector<int32_t> const& ids) {
    uint64_t hash = 1469598103934665603ULL;
    for (int32_t id : ids) {
        for (int k = 0; k < 4; ++k) {
            hash ^= uint8_t(uint32_t(id) >> (8 * k));
            hash *= 1099511628211ULL;
        }
    }
    return hash ? hash : 1;  // 0 marks "nothing to embed"
}

// Embeds messages that have no vectors yet, newest sessions first. Stops early
// for shutdown or when a rescan is requested (the request is left signaled).
void EmbedPending(Index& index, Indexer::Callback const& callback, HANDLE stop, HANDLE request) {
    std::wstring problem;
    auto embedder = Embedder::Shared(&problem);
    if (!embedder) return;
    {
        Transaction transaction(index.Database());
        index.UseEmbeddingModel(EmbeddingModelName);
        transaction.Commit();
    }
    const int pending = index.PendingEmbeddingCount();
    if (pending == 0) return;

    IndexStatus status;
    status.state = IndexStatus::State::Indexing;
    status.total = pending;
    auto lastReport = Clock::now() - std::chrono::seconds(1);
    constexpr size_t maxChunksPerMessage = 8;
    constexpr size_t batchSize = 32;
    for (;;) {
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return;
        if (WaitForSingleObject(request, 0) == WAIT_OBJECT_0) {
            SetEvent(request);
            return;
        }
        auto messages = index.MessagesToEmbed(256);
        if (messages.empty()) break;

        struct Piece {
            int64_t message;
            int chunk;
            int32_t offset;
            int64_t hash;
        };
        std::vector<Piece> pieces;
        std::vector<std::vector<int32_t>> toEmbed;
        std::vector<int64_t> toEmbedHash;
        std::unordered_set<int64_t> queued;
        for (auto const& message : messages) {
            auto tokens = embedder->Tokens().Encode(message.text, ChunkTokens * maxChunksPerMessage);
            if (tokens.empty()) {
                pieces.push_back({message.id, 0, 0, 0});
                continue;
            }
            for (size_t start = 0, chunk = 0; start < tokens.size(); start += ChunkTokens, ++chunk) {
                std::vector<int32_t> ids;
                for (size_t k = start; k < std::min(tokens.size(), start + ChunkTokens); ++k) ids.push_back(tokens[k].id);
                int64_t hash = int64_t(HashTokens(ids));
                pieces.push_back({message.id, int(chunk), int32_t(tokens[start].offset), hash});
                if (queued.insert(hash).second && !index.HasEmbedding(hash)) {
                    toEmbed.push_back(std::move(ids));
                    toEmbedHash.push_back(hash);
                }
            }
        }

        // Similar lengths batch together with less padding.
        std::vector<size_t> order(toEmbed.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return toEmbed[a].size() < toEmbed[b].size(); });
        std::vector<std::pair<int64_t, std::vector<float>>> vectors;
        for (size_t start = 0; start < order.size(); start += batchSize) {
            if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return;
            std::vector<std::vector<int32_t>> batch;
            for (size_t k = start; k < std::min(order.size(), start + batchSize); ++k) batch.push_back(toEmbed[order[k]]);
            auto results = embedder->Embed(batch);
            for (size_t k = 0; k < results.size(); ++k) vectors.emplace_back(toEmbedHash[order[start + k]], std::move(results[k]));
        }

        Transaction transaction(index.Database());
        for (auto const& [hash, vector] : vectors) index.StoreEmbedding(hash, vector);
        for (auto const& piece : pieces) index.StoreChunk(piece.message, piece.chunk, piece.offset, piece.hash);
        index.BumpVectorsVersion();
        transaction.Commit();

        status.done = std::min(status.total, status.done + int(messages.size()));
        if (Clock::now() - lastReport > std::chrono::milliseconds(250)) {
            status.message = L"Learning meaning \u00B7 " + std::to_wstring(status.done) + L" of " + std::to_wstring(status.total) +
                             L" messages";
            callback(status);
            lastReport = Clock::now();
        }
    }
    {
        Transaction transaction(index.Database());
        index.PruneEmbeddings();
        transaction.Commit();
    }
    auto totals = index.Count();
    status.state = IndexStatus::State::Ready;
    status.message = L"Up to date \u00B7 " + Plural(totals.sessions, L"session", L"sessions");
    callback(status);
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
                EmbedPending(index, callback, stopEvent, requestEvent);
            } catch (std::exception const& e) {
                IndexStatus failed;
                failed.state = IndexStatus::State::Failed;
                failed.message = L"Index error: " + Wide(e.what());
                callback(failed);
            }
        };
        std::vector<HANDLE> changes;
        for (auto const& root : SourceRoots::Detect().Watched()) {
            HANDLE change = FindFirstChangeNotificationW(root.c_str(), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
            if (change != INVALID_HANDLE_VALUE) changes.push_back(change);
        }
        scan();
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

IndexStatus Indexer::RunOnce(fs::path const& database, Callback const& callback, bool embed) {
    Index index(database);
    HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE noRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto status = Scan(index, {}, callback, never);
    if (embed) {
        IndexStatus last = status;
        EmbedPending(index, [&](IndexStatus const& s) { last = s; callback(s); }, never, noRequest);
        status = last;
    }
    CloseHandle(never);
    CloseHandle(noRequest);
    return status;
}

}  // namespace sessions
