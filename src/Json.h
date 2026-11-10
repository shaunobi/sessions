#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A small JSON reader for one line of a session transcript. Strings are
// decoded to UTF-8. It is strict enough for the well-formed output that Claude
// Code and Codex write, and fails cleanly (returns false) on anything else.
namespace sessions::json {

struct Member;

struct Value {
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<Member> object;

    bool IsNull() const { return type == Type::Null; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }
    bool IsTrue() const { return type == Type::Bool && boolean; }

    // Member lookup; returns nullptr when absent or when this is not an object.
    Value const* Get(std::string_view key) const;
    // The string value of a member, or empty when absent or not a string.
    std::string_view Str(std::string_view key) const;
};

struct Member {
    std::string key;
    Value value;
};

bool Parse(std::string_view text, Value& out);

}  // namespace sessions::json
