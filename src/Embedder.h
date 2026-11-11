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

constexpr size_t ChunkTokens = 254;  // plus [CLS] and [SEP]
// The model built into the executable, used when no model folder is installed.
constexpr char const* BuiltInModelName = "bge-small-en-v1.5";

class Embedder {
public:
    ~Embedder();

    // Loads the model and vocabulary embedded in the executable. Returns
    // nullptr (with a reason) when unavailable. Shared by the indexer and the
    // search thread; loaded once.
    static std::shared_ptr<Embedder> Shared(std::wstring* problem = nullptr);

    Tokenizer const& Tokens() const { return tokenizer; }
    // Which model is loaded; stored with the vectors so a change re-embeds everything.
    std::string const& Name() const { return name; }
    // Where a model is looked for, in order: the folder named by the
    // SESSIONS_MODEL environment variable, then a Model folder beside the
    // executable. Each holds model.onnx (+ model.onnx_data) and vocab.txt; the
    // folder's name is the model's name. Otherwise the built-in model.
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
    std::string name;
    std::mutex mutex;  // DirectML sessions take one run at a time
    bool Load(std::wstring& problem);
};

}  // namespace sessions
