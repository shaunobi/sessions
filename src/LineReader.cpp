#include "LineReader.h"
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include "zstd.h"

namespace sessions {

namespace {

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

// Splits a growing buffer into lines; keeps the unfinished tail.
class Splitter {
public:
    explicit Splitter(std::function<bool(std::string_view)> const& callback) : line(callback) {}

    bool Feed(char const* data, size_t size) {
        size_t start = 0;
        for (size_t i = 0; i < size; ++i) {
            if (data[i] != '\n') continue;
            if (pending.empty()) {
                if (!Emit(std::string_view(data + start, i - start))) return false;
            } else {
                pending.append(data + start, i - start);
                if (!Emit(pending)) return false;
                pending.clear();
            }
            start = i + 1;
        }
        pending.append(data + start, size - start);
        return true;
    }

    void Finish() {
        if (!pending.empty()) Emit(pending);
        pending.clear();
    }

private:
    std::function<bool(std::string_view)> const& line;
    std::string pending;

    bool Emit(std::string_view text) {
        if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
        if (text.empty()) return true;
        return line(text);
    }
};

}  // namespace

bool ReadLines(std::filesystem::path const& file, std::function<bool(std::string_view)> const& line) {
    Handle handle;
    // Share read, write and delete: the tools keep appending to live sessions.
    handle.value = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle.value == INVALID_HANDLE_VALUE) return false;

    constexpr size_t chunk = 1 << 20;
    std::vector<char> input(chunk);
    Splitter splitter(line);
    const bool compressed = file.extension() == L".zst";

    if (!compressed) {
        DWORD read{};
        while (ReadFile(handle.value, input.data(), DWORD(input.size()), &read, nullptr) && read > 0) {
            if (!splitter.Feed(input.data(), read)) return true;
        }
        splitter.Finish();
        return true;
    }

    std::unique_ptr<ZSTD_DStream, size_t (*)(ZSTD_DStream*)> stream(ZSTD_createDStream(), ZSTD_freeDStream);
    if (!stream) return false;
    ZSTD_initDStream(stream.get());
    std::vector<char> output(ZSTD_DStreamOutSize() * 4);
    DWORD read{};
    while (ReadFile(handle.value, input.data(), DWORD(input.size()), &read, nullptr) && read > 0) {
        ZSTD_inBuffer in{input.data(), read, 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer out{output.data(), output.size(), 0};
            size_t result = ZSTD_decompressStream(stream.get(), &out, &in);
            if (ZSTD_isError(result)) return false;
            if (out.pos && !splitter.Feed(output.data(), out.pos)) return true;
        }
    }
    // Drain anything still buffered inside the decoder.
    for (;;) {
        ZSTD_inBuffer in{nullptr, 0, 0};
        ZSTD_outBuffer out{output.data(), output.size(), 0};
        size_t result = ZSTD_decompressStream(stream.get(), &out, &in);
        if (ZSTD_isError(result)) break;
        if (out.pos && !splitter.Feed(output.data(), out.pos)) return true;
        if (out.pos < out.size) break;
    }
    splitter.Finish();
    return true;
}

}  // namespace sessions
