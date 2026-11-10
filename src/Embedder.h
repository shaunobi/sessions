#pragma once
#include "Tokenizer.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Turns text into meaning vectors with a small local embedding model (BGE
// small, English) through ONNX Runtime from Windows Machine Learning. Runs on
// the GPU through DirectML when available, otherwise on the CPU.
namespace sessions {

constexpr int EmbeddingSize = 384;
constexpr size_t ChunkTokens = 254;  // plus [CLS] and [SEP]
constexpr char const* EmbeddingModelName = "bge-small-en-v1.5";

class Embedder {
public:
    ~Embedder();

    // Loads the model and vocabulary embedded in the executable. Returns
    // nullptr (with a reason) when unavailable. Shared by the indexer and the
    // search thread; loaded once.
    static std::shared_ptr<Embedder> Shared(std::wstring* problem = nullptr);

    Tokenizer const& Tokens() const { return tokenizer; }
    std::wstring const& Device() const { return device; }

    // Each input is a list of word-piece ids (at most ChunkTokens). Returns one
    // L2-normalized vector of EmbeddingSize floats per input.
    std::vector<std::vector<float>> Embed(std::vector<std::vector<int32_t>> const& inputs);

    // Embeds a search query with the model's retrieval instruction.
    std::vector<float> EmbedQuery(std::string const& utf8);

private:
    Embedder() = default;
    struct Runtime;
    std::unique_ptr<Runtime> runtime;
    Tokenizer tokenizer;
    std::wstring device;
    std::mutex mutex;  // DirectML sessions take one run at a time
    bool Load(std::wstring& problem);
};

}  // namespace sessions
