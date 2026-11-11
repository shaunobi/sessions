#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace sessions {

std::wstring Wide(std::string_view utf8);
std::string Utf8(std::wstring_view wide);

// "2026-11-09T22:56:43.663Z" (or with an offset) to Unix milliseconds; 0 when unreadable.
int64_t ParseIsoMs(std::string_view text);
int64_t NowMs();

// Collapses runs of whitespace to one space and trims both ends.
std::string Squash(std::string_view text);
// Cuts UTF-8 text to at most `bytes` bytes without splitting a character.
std::string Clip(std::string_view text, size_t bytes);
// Removes <tag ...> and </tag> markup, keeping the text between tags.
std::string StripTags(std::string_view text);
// Decodes &amp; &lt; &gt; &quot; &apos; and &#39; (relayed messages arrive XML-escaped).
std::string DecodeEntities(std::string_view text);
// True for injected context (system reminders, environment blocks, command echoes)
// that the person did not type, in either tool's transcripts.
bool IsInjectedContext(std::string_view text);
// What the person typed in one block of a user message: an app's wrapper tags
// removed, the request pulled out of a "Files mentioned by the user" wrapper,
// and empty when what is left is context the app added.
std::string OwnWords(std::string_view text);
// ChatGPT text without its citation markers (private-use \uE200 ... \uE201 spans and
// 【...†...】 references) or the tool records it sometimes shows as replies.
// Empty when the message is only such a record.
std::string CleanChatGptText(std::string_view text);

std::filesystem::path HomeFolder();
std::filesystem::path RoamingFolder();
std::filesystem::path ExecutableFolder();

int64_t FileTimeMs(std::filesystem::file_time_type time);

}  // namespace sessions
