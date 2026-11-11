#pragma once
#include "Index.h"
#include <filesystem>
#include <string>
#include <vector>

// Keyword search plus meaning search, merged into one ranked list of sessions.
namespace sessions {

struct Found {
    int64_t session = 0;
    double score = 0;
    std::wstring snippet;       // \x01 ... \x02 marks matched words
    bool byMeaning = false;     // found by meaning only, no matching words
    float similarity = 0;
};

class HybridSearch {
public:
    explicit HybridSearch(std::filesystem::path const& database);
    // Runs both searches; builds snippets for the first `snippets` results.
    std::vector<Found> Run(std::wstring const& query, size_t snippets = 300);
    Index& Database() { return index; }
    bool MeaningReady() const { return !pieces.empty(); }

private:
    Index index;
    // Every embedded piece of text: where it is, and its vector in one flat block
    // (row i at matrix[i * dims]) so a search is one pass over contiguous memory.
    struct Piece {
        int64_t session = 0;
        int64_t message = 0;
        int32_t offset = 0;
    };
    std::vector<Piece> pieces;
    std::vector<int8_t> matrix;
    size_t dims = 0;
    int64_t version = -1;
    void Refresh();
};

}  // namespace sessions
