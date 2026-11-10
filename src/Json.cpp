#include "Json.h"
#include <charconv>

namespace sessions::json {
namespace {

class Reader {
public:
    explicit Reader(std::string_view text) : p(text.data()), end(text.data() + text.size()) {}

    bool Document(Value& out) {
        Space();
        if (!Read(out, 0)) return false;
        Space();
        return p == end;
    }

private:
    char const* p;
    char const* end;

    void Space() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool Literal(std::string_view word) {
        if (size_t(end - p) < word.size() || std::string_view(p, word.size()) != word) return false;
        p += word.size();
        return true;
    }

    static void Append(std::string& out, uint32_t code) {
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

    bool Hex4(uint32_t& code) {
        if (end - p < 4) return false;
        code = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            char c = *p;
            code <<= 4;
            if (c >= '0' && c <= '9') code |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') code |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool String(std::string& out) {
        // Caller has checked the opening quote.
        ++p;
        out.clear();
        while (p < end) {
            // Copy the plain run in one step.
            char const* run = p;
            while (p < end && *p != '"' && *p != '\\') ++p;
            out.append(run, p);
            if (p >= end) return false;
            if (*p == '"') {
                ++p;
                return true;
            }
            ++p;  // backslash
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t code{};
                if (!Hex4(code)) return false;
                if (code >= 0xD800 && code <= 0xDBFF) {
                    uint32_t low{};
                    if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        if (!Hex4(low)) return false;
                        if (low >= 0xDC00 && low <= 0xDFFF) code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        else { Append(out, 0xFFFD); code = low; }
                    } else {
                        code = 0xFFFD;
                    }
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    code = 0xFFFD;
                }
                Append(out, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool Read(Value& out, int depth) {
        if (depth > 256 || p >= end) return false;
        switch (*p) {
        case '{': {
            ++p;
            out.type = Value::Type::Object;
            Space();
            if (p < end && *p == '}') { ++p; return true; }
            while (true) {
                Space();
                if (p >= end || *p != '"') return false;
                Member& member = out.object.emplace_back();
                if (!String(member.key)) return false;
                Space();
                if (p >= end || *p != ':') return false;
                ++p;
                Space();
                if (!Read(member.value, depth + 1)) return false;
                Space();
                if (p >= end) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; return true; }
                return false;
            }
        }
        case '[': {
            ++p;
            out.type = Value::Type::Array;
            Space();
            if (p < end && *p == ']') { ++p; return true; }
            while (true) {
                Space();
                if (!Read(out.array.emplace_back(), depth + 1)) return false;
                Space();
                if (p >= end) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; return true; }
                return false;
            }
        }
        case '"':
            out.type = Value::Type::String;
            return String(out.string);
        case 't':
            out.type = Value::Type::Bool;
            out.boolean = true;
            return Literal("true");
        case 'f':
            out.type = Value::Type::Bool;
            return Literal("false");
        case 'n':
            return Literal("null");
        default: {
            out.type = Value::Type::Number;
            char const* start = p;
            while (p < end && (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9'))) ++p;
            if (p == start) return false;
            auto result = std::from_chars(start, p, out.number);
            return result.ec == std::errc{} && result.ptr == p;
        }
        }
    }
};

}  // namespace

Value const* Value::Get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (auto const& member : object)
        if (member.key == key) return &member.value;
    return nullptr;
}

std::string_view Value::Str(std::string_view key) const {
    auto value = Get(key);
    return value && value->IsString() ? std::string_view(value->string) : std::string_view();
}

bool Parse(std::string_view text, Value& out) {
    out = Value();
    return Reader(text).Document(out);
}

}  // namespace sessions::json
