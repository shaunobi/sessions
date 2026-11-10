#include "Embedder.h"
#include "Text.h"
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <algorithm>
#include <cmath>

namespace sessions {

namespace fs = std::filesystem;

struct Embedder::Runtime {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "Sessions"};
    Ort::Session session{nullptr};
    std::vector<std::string> inputNames;
    std::string outputName;
    bool hasTokenTypes = false;
};

Embedder::~Embedder() = default;

fs::path Embedder::ModelFolder() { return ExecutableFolder() / L"models"; }

std::shared_ptr<Embedder> Embedder::Shared(std::wstring* problem) {
    static std::mutex mutex;
    static std::shared_ptr<Embedder> shared;
    static bool tried = false;
    static std::wstring reason;
    std::lock_guard lock(mutex);
    if (!tried) {
        tried = true;
        std::shared_ptr<Embedder> candidate(new Embedder());
        try {
            if (candidate->Load(reason)) shared = candidate;
        } catch (Ort::Exception const& e) {
            reason = L"Embedding model failed to load: " + Wide(e.what());
        } catch (std::exception const& e) {
            reason = L"Embedding model failed to load: " + Wide(e.what());
        }
    }
    if (problem) *problem = reason;
    return shared;
}

bool Embedder::Load(std::wstring& problem) {
    auto folder = ModelFolder();
    auto model = folder / (Wide(EmbeddingModelName) + L".onnx");
    auto vocab = folder / (Wide(EmbeddingModelName) + L".vocab.txt");
    std::error_code error;
    if (!fs::exists(model, error) || !fs::exists(vocab, error)) {
        problem = L"Meaning search is off: the model is not in " + folder.wstring();
        return false;
    }
    if (!tokenizer.Load(vocab)) {
        problem = L"Meaning search is off: the vocabulary could not be read";
        return false;
    }
    runtime = std::make_unique<Runtime>();

    auto open = [&](bool gpu) {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (gpu) {
            // DirectML needs these two settings.
            options.DisableMemPattern();
            options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            OrtDmlApi const* dml = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<void const**>(&dml)));
            Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(options, 0));
        } else {
            // Leave cores for everything else on the machine.
            options.SetIntraOpNumThreads(4);
        }
        runtime->session = Ort::Session(runtime->env, model.c_str(), options);
    };
    try {
        open(true);
        device = L"GPU (DirectML)";
    } catch (Ort::Exception const&) {
        open(false);
        device = L"CPU";
    }

    Ort::AllocatorWithDefaultOptions allocator;
    for (size_t i = 0; i < runtime->session.GetInputCount(); ++i) {
        auto name = runtime->session.GetInputNameAllocated(i, allocator);
        runtime->inputNames.emplace_back(name.get());
        if (runtime->inputNames.back() == "token_type_ids") runtime->hasTokenTypes = true;
    }
    runtime->outputName = runtime->session.GetOutputNameAllocated(0, allocator).get();
    return true;
}

std::vector<std::vector<float>> Embedder::Embed(std::vector<std::vector<int32_t>> const& inputs) {
    std::vector<std::vector<float>> out;
    if (inputs.empty()) return out;
    size_t length = 0;
    for (auto const& input : inputs) length = std::max(length, std::min(input.size(), ChunkTokens) + 2);
    const size_t batch = inputs.size();
    std::vector<int64_t> ids(batch * length, tokenizer.Pad()), mask(batch * length, 0), types(batch * length, 0);
    for (size_t b = 0; b < batch; ++b) {
        auto row = ids.begin() + ptrdiff_t(b * length);
        auto rowMask = mask.begin() + ptrdiff_t(b * length);
        size_t n = std::min(inputs[b].size(), ChunkTokens);
        row[0] = tokenizer.Cls();
        for (size_t k = 0; k < n; ++k) row[ptrdiff_t(k + 1)] = inputs[b][k];
        row[ptrdiff_t(n + 1)] = tokenizer.Sep();
        std::fill(rowMask, rowMask + ptrdiff_t(n + 2), 1);
    }

    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const int64_t shape[2] = {int64_t(batch), int64_t(length)};
    std::vector<Ort::Value> values;
    std::vector<char const*> names;
    for (auto const& name : runtime->inputNames) {
        std::vector<int64_t>* data = name == "attention_mask" ? &mask : name == "token_type_ids" ? &types : &ids;
        values.push_back(Ort::Value::CreateTensor<int64_t>(memory, data->data(), data->size(), shape, 2));
        names.push_back(name.c_str());
    }
    char const* outputName = runtime->outputName.c_str();
    std::vector<Ort::Value> results;
    {
        std::lock_guard lock(mutex);
        results = runtime->session.Run(Ort::RunOptions{nullptr}, names.data(), values.data(), values.size(), &outputName, 1);
    }
    // last_hidden_state [batch, length, 384]: BGE uses the [CLS] position.
    auto info = results[0].GetTensorTypeAndShapeInfo();
    auto dims = info.GetShape();
    const float* data = results[0].GetTensorData<float>();
    const size_t stride = dims.size() == 3 ? size_t(dims[1] * dims[2]) : size_t(dims.back());
    const size_t width = size_t(dims.back());
    for (size_t b = 0; b < batch; ++b) {
        std::vector<float> vector(data + b * stride, data + b * stride + std::min<size_t>(width, EmbeddingSize));
        float norm = 0;
        for (float v : vector) norm += v * v;
        norm = std::sqrt(norm);
        if (norm > 0)
            for (float& v : vector) v /= norm;
        out.push_back(std::move(vector));
    }
    return out;
}

std::vector<float> Embedder::EmbedQuery(std::string const& utf8) {
    auto tokens = tokenizer.Encode("Represent this sentence for searching relevant passages: " + utf8, ChunkTokens);
    std::vector<int32_t> ids;
    for (auto const& token : tokens) ids.push_back(token.id);
    auto vectors = Embed({ids});
    return vectors.empty() ? std::vector<float>() : vectors.front();
}

}  // namespace sessions
