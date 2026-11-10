#include "Tokenizer.h"
#include <windows.h>
#include <fstream>

namespace sessions {

namespace {

// Decodes one UTF-8 code point; returns 0xFFFD on malformed input.
char32_t Next(std::string_view text, size_t& i) {
    unsigned char c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    char32_t code = c & (0x3F >> extra);
    for (int k = 0; k < extra; ++k) {
        if (i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) return 0xFFFD;
        code = (code << 6) | (static_cast<unsigned char>(text[i++]) & 0x3F);
    }
    return code;
}

void Append(std::string& out, char32_t code) {
    if (code < 0x80) {
        out.push_back(char(code));
    } else if (code < 0x800) {
        out.push_back(char(0xC0 | (code >> 6)));
        out.push_back(char(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(char(0xE0 | (code >> 12)));
        out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(char(0x80 | (code & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (code >> 18)));
        out.push_back(char(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(char(0x80 | (code & 0x3F)));
    }
}

bool IsSpace(char32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x00A0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F;
}

bool IsControl(char32_t c) {
    return (c < 0x20 && c != '\t' && c != '\n' && c != '\r') || (c >= 0x7F && c < 0xA0) || c == 0xFFFD || c == 0x200B ||
           c == 0x200C || c == 0x200D || c == 0xFEFF;
}

bool IsPunctuation(char32_t c) {
    if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) return true;
    if (c < 0x80) return false;
    if (c > 0xFFFF) return false;
    WORD type{};
    wchar_t w = wchar_t(c);
    return GetStringTypeW(CT_CTYPE1, &w, 1, &type) && (type & C1_PUNCT);
}

bool IsCjk(char32_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x20000 && c <= 0x2A6DF) ||
           (c >= 0x2A700 && c <= 0x2CEAF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x2F800 && c <= 0x2FA1F);
}

bool IsCombiningMark(char32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F);
}

// Lowercase and decompose one character, dropping accents (BERT uncased).
void Fold(char32_t c, std::u32string& out) {
    if (c < 0x80) {
        out.push_back(c >= 'A' && c <= 'Z' ? c + 32 : c);
        return;
    }
    if (c > 0xFFFF) {
        out.push_back(c);
        return;
    }
    wchar_t one[2] = {wchar_t(c), 0};
    CharLowerBuffW(one, 1);
    wchar_t decomposed[8]{};
    int n = NormalizeString(NormalizationD, one, 1, decomposed, 8);
    if (n <= 0) {
        out.push_back(char32_t(one[0]));
        return;
    }
    for (int k = 0; k < n; ++k)
        if (!IsCombiningMark(char32_t(decomposed[k]))) out.push_back(char32_t(decomposed[k]));
}

}  // namespace

bool Tokenizer::Load(std::filesystem::path const& vocabFile) {
    std::ifstream file(vocabFile, std::ios::binary);
    if (!file) return false;
    vocab.clear();
    std::string line;
    int32_t id = 0;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        vocab.emplace(line, id++);
    }
    auto find = [&](char const* token, int32_t fallback) {
        auto it = vocab.find(token);
        return it == vocab.end() ? fallback : it->second;
    };
    cls = find("[CLS]", cls);
    sep = find("[SEP]", sep);
    pad = find("[PAD]", pad);
    unk = find("[UNK]", unk);
    return !vocab.empty();
}

void Tokenizer::WordPieces(std::u32string const& word, uint32_t offset, std::vector<Token>& out) const {
    if (word.size() > 100) {
        out.push_back({unk, offset});
        return;
    }
    std::vector<Token> pieces;
    size_t start = 0;
    std::string piece;
    while (start < word.size()) {
        size_t end = word.size();
        int32_t found = -1;
        while (end > start) {
            piece.clear();
            if (start > 0) piece = "##";
            for (size_t k = start; k < end; ++k) Append(piece, word[k]);
            auto it = vocab.find(piece);
            if (it != vocab.end()) {
                found = it->second;
                break;
            }
            --end;
        }
        if (found < 0) {
            out.push_back({unk, offset});
            return;
        }
        pieces.push_back({found, offset});
        start = end;
    }
    out.insert(out.end(), pieces.begin(), pieces.end());
}

std::vector<Token> Tokenizer::Encode(std::string_view text, size_t limit) const {
    std::vector<Token> out;
    std::u32string word;
    uint32_t wordStart = 0;
    auto flush = [&] {
        if (!word.empty()) WordPieces(word, wordStart, out);
        word.clear();
    };
    size_t i = 0;
    while (i < text.size() && out.size() < limit) {
        uint32_t at = uint32_t(i);
        char32_t c = Next(text, i);
        if (c == 0 || IsControl(c)) continue;
        if (IsSpace(c)) {
            flush();
            continue;
        }
        if (IsPunctuation(c) || IsCjk(c)) {
            flush();
            word.clear();
            wordStart = at;
            Fold(c, word);
            flush();
            continue;
        }
        if (word.empty()) wordStart = at;
        Fold(c, word);
    }
    flush();
    if (out.size() > limit) out.resize(limit);
    return out;
}

}  // namespace sessions
