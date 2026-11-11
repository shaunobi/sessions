#include "Search.h"
#include "Embedder.h"
#include "Text.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace sessions {

namespace {

// Reciprocal rank fusion: a session ranked well by either search rises.
constexpr double FusionK = 20.0;
// Meaning-only results: those within this distance of the best match, up to a cap.
// Models differ in how similar unrelated text looks (one puts noise at 0.65, another at
// 0.25), so meaning results are judged against this query's own spread of scores:
// how many standard deviations above the average session a result stands.
float Tuned(wchar_t const* name, float fallback) {  // SESSIONS_<name> overrides, for tuning tests
    wchar_t value[32];
    return GetEnvironmentVariableW(name, value, 32) > 0 ? float(_wtof(value)) : fallback;
}
const float MeaningZ = Tuned(L"SESSIONS_MEANING_Z", 3.0f);        // a meaning-only result stands at least this far out
const float MeaningWindowZ = Tuned(L"SESSIONS_WINDOW_Z", 3.0f);   // and within this many deviations of the best
const float UnknownWordsZ = Tuned(L"SESSIONS_UNKNOWN_Z", 6.0f);  // nonsense words: only a standout match counts
// Gibberish still lands around 0.65-0.69, as close as some real questions, so
// a query none of whose words appear anywhere only gets results this similar.
constexpr size_t MeaningCap = 40;

struct Best {
    float similarity = -2;
    int64_t message = 0;
    int32_t offset = 0;
};

}  // namespace

HybridSearch::HybridSearch(std::filesystem::path const& database) : index(database) {}

void HybridSearch::Refresh() {
    auto current = index.VectorsVersion();
    if (current == version) return;
    auto loaded = index.LoadVectors();
    // Grouped by session, so the search keeps each session's best piece without a lookup per piece.
    std::stable_sort(loaded.begin(), loaded.end(), [](auto const& a, auto const& b) { return a.session < b.session; });
    pieces.clear();
    matrix.clear();
    dims = loaded.empty() ? 0 : loaded.front().vector.size();
    pieces.reserve(loaded.size());
    matrix.reserve(loaded.size() * dims);
    for (auto const& chunk : loaded) {
        if (chunk.vector.size() != dims) continue;
        pieces.push_back({chunk.session, chunk.message, chunk.offset});
        matrix.insert(matrix.end(), chunk.vector.begin(), chunk.vector.end());
    }
    titleVectors = index.LoadTitleVectors();
    version = current;
}

std::vector<Found> HybridSearch::Run(std::wstring const& query, size_t snippets) {
    auto keyword = index.Search(query);

    std::vector<std::pair<int64_t, Best>> meaning;
    std::vector<float> all;  // every session's meaning score, for this query's spread
    if (auto embedder = Embedder::Shared()) {
        Refresh();
        auto q = pieces.empty() ? std::vector<float>() : embedder->EmbedQuery(Utf8(query));
        if (!pieces.empty() && q.size() == dims) {
            // The query in the same int8 form as the stored vectors: whole-number dot products
            // the compiler turns into wide multiply-adds.
            std::vector<int16_t> qi(dims);
            for (size_t i = 0; i < dims; ++i) qi[i] = int16_t(std::lround(std::clamp(q[i], -1.0f, 1.0f) * 127.0f));
            constexpr float Scale = 1.0f / (127.0f * 127.0f);
            int64_t session = -1;
            Best b;
            int count = 0;
            std::unordered_map<int64_t, int> pieceCount;
            for (size_t row = 0; row < pieces.size(); ++row) {
                int8_t const* v = matrix.data() + row * dims;
                int32_t dot = 0;
                for (size_t i = 0; i < dims; ++i) dot += int32_t(qi[i]) * int32_t(v[i]);
                auto const& piece = pieces[row];
                if (piece.session != session) {
                    if (session >= 0) {
                        meaning.push_back({session, b});
                        pieceCount[session] = count;
                    }
                    session = piece.session;
                    b = {};
                    count = 0;
                }
                ++count;
                float similarity = float(dot) * Scale;
                if (similarity > b.similarity) b = {similarity, piece.message, piece.offset};
            }
            if (session >= 0) {
                meaning.push_back({session, b});
                pieceCount[session] = count;
            }
            // What the session is about as a whole: its name and first prompt, at half
            // the weight of its best piece. A session listed by name only has just that.
            auto titleScore = [&](int64_t id) -> float {
                auto it = titleVectors.find(id);
                if (it == titleVectors.end() || it->second.size() != dims) return -2;
                int32_t dot = 0;
                for (size_t i = 0; i < dims; ++i) dot += int32_t(qi[i]) * int32_t(it->second[i]);
                return float(dot) * Scale;
            };
            std::unordered_set<int64_t> scored;
            for (auto& [id, best] : meaning) {
                scored.insert(id);
                if (float t = titleScore(id); t > -2) best.similarity = (best.similarity + 0.5f * t) / 1.5f;
                // Long sessions get more chances at a high score; take a little back.
                best.similarity -= 0.01f * std::log(float(std::max(pieceCount[id], 1)));
            }
            for (auto const& [id, vector] : titleVectors)
                if (!scored.count(id)) {
                    Best only;
                    only.similarity = titleScore(id);
                    // A name alone is thinner evidence than a session's own words.
                    if (only.similarity > -2) meaning.push_back({id, {only.similarity - 0.04f, 0, 0}});
                }
            for (auto const& m : meaning) all.push_back(m.second.similarity);
            std::sort(meaning.begin(), meaning.end(), [](auto const& a, auto const& b) { return a.second.similarity > b.second.similarity; });
            if (meaning.size() > 200) meaning.resize(200);
        }
    }

    // For comparing models: the meaning ranking alone, uncut (SESSIONS_MEANING_ONLY=1).
    if (GetEnvironmentVariableW(L"SESSIONS_MEANING_ONLY", nullptr, 0) > 0) {
        std::vector<Found> only;
        for (auto const& [session, best] : meaning) only.push_back({session, double(best.similarity), {}, true, best.similarity});
        return only;
    }

    // Short queries are usually names or terms: favor exact words. Sentences
    // describe an idea: favor meaning.
    size_t wordCount = 0;
    bool inWord = false;
    for (wchar_t c : query) {
        bool letter = !iswspace(c);
        if (letter && !inWord) ++wordCount;
        inWord = letter;
    }
    const double keywordWeight = wordCount <= 2 ? 1.5 : 1.0;
    const double meaningWeight = wordCount >= 4 ? 1.6 : 1.0;

    std::unordered_map<int64_t, Found> merged;
    std::unordered_map<int64_t, int64_t> keywordMessage;
    for (size_t rank = 0; rank < keyword.size(); ++rank) {
        // Words scattered across a session, nowhere together, say little about a
        // question of three or more words: leave those to meaning search.
        if (!keyword[rank].together && wordCount >= 3) continue;
        auto& found = merged[keyword[rank].session];
        found.session = keyword[rank].session;
        double weight = keywordWeight;
        found.score += weight / (FusionK + double(rank + 1));
        keywordMessage[found.session] = keyword[rank].bestMessage;
    }
    const float top = meaning.empty() ? 0 : meaning.front().second.similarity;
    float mean = 0, spread = 0;
    if (!meaning.empty()) {
        // Over every session the model scored (meaning holds the best 200; the full list is in `all`).
        for (float v : all) mean += v;
        mean /= float(all.size());
        for (float v : all) spread += (v - mean) * (v - mean);
        spread = std::sqrt(spread / float(all.size()));
        if (spread <= 0) spread = 1;
    }
    auto z = [&](float v) { return (v - mean) / spread; };
    if (!meaning.empty() && z(top) < UnknownWordsZ && keyword.empty() && !index.KnowsAnyWord(query)) meaning.clear();
    for (size_t rank = 0; rank < meaning.size(); ++rank) {
        auto const& [session, best] = meaning[rank];
        bool inKeyword = merged.count(session) > 0;
        bool strong = rank < MeaningCap && z(best.similarity) >= MeaningZ && z(best.similarity) >= z(top) - MeaningWindowZ;
        if (!inKeyword && !strong) continue;
        auto& found = merged[session];
        found.session = session;
        found.similarity = best.similarity;
        found.score += meaningWeight / (FusionK + double(rank + 1));
        if (!inKeyword) found.byMeaning = true;
    }

    std::vector<Found> out;
    out.reserve(merged.size());
    for (auto& [_, found] : merged) out.push_back(std::move(found));
    std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.score > b.score; });

    std::unordered_map<int64_t, Best> meaningBySession(meaning.begin(), meaning.end());
    for (size_t i = 0; i < out.size() && i < snippets; ++i) {
        auto& found = out[i];
        if (!found.byMeaning) {
            found.snippet = index.Snippet(query, keywordMessage[found.session]);
        } else if (auto it = meaningBySession.find(found.session); it != meaningBySession.end()) {
            found.snippet = it->second.message ? index.Passage(it->second.message, it->second.offset, 260)
                                               : std::wstring(L"Matched by its name; the messages are not read yet.");
        }
    }
    return out;
}

}  // namespace sessions
