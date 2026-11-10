#include "Text.h"
#include <windows.h>
#include <shlobj.h>
#include <array>
#include <chrono>

namespace sessions {

std::wstring Wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), size);
    return out;
}

std::string Utf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), size, nullptr, nullptr);
    return out;
}

namespace {
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
bool Digits(std::string_view text, size_t at, size_t count, int& out) {
    if (text.size() < at + count) return false;
    out = 0;
    for (size_t i = at; i < at + count; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        out = out * 10 + (text[i] - '0');
    }
    return true;
}
}  // namespace

int64_t ParseIsoMs(std::string_view t) {
    int year{}, month{}, day{}, hour{}, minute{}, second{};
    if (!Digits(t, 0, 4, year) || !Digits(t, 5, 2, month) || !Digits(t, 8, 2, day)) return 0;
    if (!Digits(t, 11, 2, hour) || !Digits(t, 14, 2, minute) || !Digits(t, 17, 2, second)) return 0;
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
    size_t at = 19;
    int64_t millis = 0;
    if (at < t.size() && t[at] == '.') {
        ++at;
        int scale = 100;
        while (at < t.size() && t[at] >= '0' && t[at] <= '9') {
            millis += (t[at] - '0') * scale;
            scale /= 10;
            ++at;
        }
    }
    int64_t offset = 0;
    if (at < t.size() && (t[at] == '+' || t[at] == '-')) {
        int oh{}, om{};
        if (Digits(t, at + 1, 2, oh) && Digits(t, at + 4, 2, om)) offset = (oh * 60 + om) * 60000LL * (t[at] == '+' ? 1 : -1);
    }
    int64_t days = DaysFromCivil(year, unsigned(month), unsigned(day));
    return ((days * 24 + hour) * 60 + minute) * 60000LL + second * 1000LL + millis - offset;
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string Squash(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            space = !out.empty();
        } else {
            if (space) out.push_back(' ');
            space = false;
            out.push_back(c);
        }
    }
    return out;
}

std::string Clip(std::string_view text, size_t bytes) {
    if (text.size() <= bytes) return std::string(text);
    size_t cut = bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return std::string(text.substr(0, cut)) + "\xE2\x80\xA6";
}

std::string StripTags(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '<') {
            size_t j = i + 1;
            if (j < text.size() && text[j] == '/') ++j;
            size_t nameStart = j;
            while (j < text.size() && ((text[j] >= 'a' && text[j] <= 'z') || text[j] == '-' || text[j] == '_')) ++j;
            if (j > nameStart) {
                size_t close = text.find('>', j);
                if (close != std::string_view::npos && close - i < 400) {
                    out.push_back(' ');
                    i = close;
                    continue;
                }
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string DecodeEntities(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '&') {
            size_t end = text.find(';', i);
            if (end != std::string_view::npos && end - i <= 8) {
                auto name = text.substr(i + 1, end - i - 1);
                std::string replacement;
                if (name == "amp") replacement = "&";
                else if (name == "lt") replacement = "<";
                else if (name == "gt") replacement = ">";
                else if (name == "quot") replacement = "\"";
                else if (name == "apos" || name == "#39") replacement = "'";
                if (!replacement.empty()) {
                    out += replacement;
                    i = end;
                    continue;
                }
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

bool IsInjectedContext(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\r' || text.front() == '\t'))
        text.remove_prefix(1);
    static constexpr std::array<std::string_view, 24> prefixes{
        "<system-reminder>", "<task-notification>", "<local-command-", "<command-name>", "<command-message>",
        "<command-args>", "<environment_context>", "<user_instructions>", "<permissions", "<turn_aborted>",
        "<user_shell_command>", "<subagent_notification>", "<skill>", "<side-sessions-event>", "<ci-monitor-event>",
        "<project_claude_message>", "Base directory for this skill", "[Request interrupted", "Caveat: The messages below",
        "# AGENTS.md instructions", "<INSTRUCTIONS>", "<artifact-content", "<user-prompt-submit-hook>", "<bash-"};
    for (auto prefix : prefixes)
        if (text.substr(0, prefix.size()) == prefix) return true;
    return text.empty();
}

std::filesystem::path HomeFolder() {
    PWSTR path{};
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::filesystem::path RoamingFolder() {
    PWSTR path{};
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::filesystem::path ExecutableFolder() {
    std::wstring buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

int64_t FileTimeMs(std::filesystem::file_time_type time) {
    using namespace std::chrono;
    return duration_cast<milliseconds>(clock_cast<system_clock>(time).time_since_epoch()).count();
}

}  // namespace sessions
