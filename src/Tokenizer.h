#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// BERT "uncased" WordPiece tokenizer, as used by the BGE embedding model:
// clean, lowercase, strip accents, split on whitespace and punctuation (CJK
// characters stand alone), then greedy longest-match word pieces.
namespace sessions {

struct Token {
    int32_t id = 0;
    uint32_t offset = 0;  // byte offset in the UTF-8 input where its word starts
};

class Tokenizer {
public:
    bool Load(std::filesystem::path const& vocabFile);
    // One token per line, as in vocab.txt.
    bool LoadText(std::string_view text);
    bool Loaded() const { return !vocab.empty(); }
    // Word pieces without [CLS]/[SEP]; stops after `limit` tokens.
    std::vector<Token> Encode(std::string_view utf8, size_t limit = SIZE_MAX) const;

    int32_t Cls() const { return cls; }
    int32_t Sep() const { return sep; }
    int32_t Pad() const { return pad; }

private:
    std::unordered_map<std::string, int32_t> vocab;
    int32_t cls = 101, sep = 102, pad = 0, unk = 100;

    void WordPieces(std::u32string const& word, uint32_t offset, std::vector<Token>& out) const;
};

}  // namespace sessions
