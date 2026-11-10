#include "Search.h"
#include "Embedder.h"
#include "Text.h"
#include <algorithm>
#include <unordered_map>

namespace sessions {

namespace {

// Reciprocal rank fusion: a session ranked well by either search rises.
constexpr double FusionK = 20.0;
// Meaning-only results: those within this distance of the best match, up to a cap.
constexpr float MeaningWindow = 0.08f;
constexpr float MeaningFloor = 0.55f;
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
    vectors = index.LoadVectors();
    version = current;
}

std::vector<Found> HybridSearch::Run(std::wstring const& query, size_t snippets) {
    auto keyword = index.Search(query);

    std::vector<std::pair<int64_t, Best>> meaning;
    if (auto embedder = Embedder::Shared()) {
        Refresh();
        if (!vectors.empty()) {
            auto q = embedder->EmbedQuery(Utf8(query));
            std::unordered_map<int64_t, Best> best;
            for (auto const& chunk : vectors) {
                if (chunk.vector.size() != q.size()) continue;
                float dot = 0;
                for (size_t i = 0; i < q.size(); ++i) dot += q[i] * float(chunk.vector[i]);
                dot /= 127.0f;
                auto& b = best[chunk.session];
                if (dot > b.similarity) b = {dot, chunk.message, chunk.offset};
            }
            meaning.assign(best.begin(), best.end());
            std::sort(meaning.begin(), meaning.end(), [](auto const& a, auto const& b) { return a.second.similarity > b.second.similarity; });
            if (meaning.size() > 200) meaning.resize(200);
        }
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
        auto& found = merged[keyword[rank].session];
        found.session = keyword[rank].session;
        found.score += keywordWeight / (FusionK + double(rank + 1));
        keywordMessage[found.session] = keyword[rank].bestMessage;
    }
    const float top = meaning.empty() ? 0 : meaning.front().second.similarity;
    for (size_t rank = 0; rank < meaning.size(); ++rank) {
        auto const& [session, best] = meaning[rank];
        bool inKeyword = merged.count(session) > 0;
        bool strong = rank < MeaningCap && best.similarity >= MeaningFloor && best.similarity >= top - MeaningWindow;
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
            found.snippet = index.Passage(it->second.message, it->second.offset, 260);
        }
    }
    return out;
}

}  // namespace sessions
