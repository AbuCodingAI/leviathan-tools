// json.hpp - a tiny, dependency-free JSON parser + string escaper.
// Used by the Architect LSP client to build JSON-RPC requests and to
// parse language-server responses (hover contents, etc.). Header-only,
// no GTK, so it can be unit-tested standalone.
#ifndef ARCHITECT_JSON_HPP
#define ARCHITECT_JSON_HPP

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolv = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    bool isObject() const { return type == Type::Object; }
    bool isArray()  const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }

    // Safe object lookup; returns nullptr if absent or not an object.
    const Value* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    std::string asString(const std::string& def = "") const {
        return type == Type::String ? str : def;
    }
    long asInt(long def = 0) const {
        return type == Type::Number ? (long)num : def;
    }
};

// ---- Parser -------------------------------------------------------------
class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    // Returns true on success; fills out. On failure returns false.
    bool parse(Value& out) {
        skipWs();
        if (!parseValue(out)) return false;
        return true;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skipWs() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') i_++;
            else break;
        }
    }
    bool parseValue(Value& v) {
        skipWs();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        switch (c) {
            case '{': return parseObject(v);
            case '[': return parseArray(v);
            case '"': { v.type = Type::String; return parseString(v.str); }
            case 't': case 'f': return parseBool(v);
            case 'n': return parseNull(v);
            default:  return parseNumber(v);
        }
    }
    bool parseObject(Value& v) {
        v.type = Type::Object;
        i_++; // {
        skipWs();
        if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
        while (true) {
            skipWs();
            if (i_ >= s_.size() || s_[i_] != '"') return false;
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            i_++;
            Value child;
            if (!parseValue(child)) return false;
            v.obj[key] = std::move(child);
            skipWs();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { i_++; continue; }
            if (s_[i_] == '}') { i_++; return true; }
            return false;
        }
    }
    bool parseArray(Value& v) {
        v.type = Type::Array;
        i_++; // [
        skipWs();
        if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
        while (true) {
            Value child;
            if (!parseValue(child)) return false;
            v.arr.push_back(std::move(child));
            skipWs();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { i_++; continue; }
            if (s_[i_] == ']') { i_++; return true; }
            return false;
        }
    }
    void appendUtf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7F) out.push_back((char)cp);
        else if (cp <= 0x7FF) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool parseString(std::string& out) {
        if (s_[i_] != '"') return false;
        i_++;
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= s_.size()) return false;
                char e = s_[i_++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (i_ + 4 > s_.size()) return false;
                        uint32_t cp = 0;
                        for (int k = 0; k < 4; k++) {
                            int h = hexVal(s_[i_++]);
                            if (h < 0) return false;
                            cp = (cp << 4) | (uint32_t)h;
                        }
                        // Handle UTF-16 surrogate pair.
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                            i_ += 2;
                            uint32_t lo = 0;
                            for (int k = 0; k < 4; k++) {
                                int h = hexVal(s_[i_++]);
                                if (h < 0) return false;
                                lo = (lo << 4) | (uint32_t)h;
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }
    bool parseBool(Value& v) {
        if (s_.compare(i_, 4, "true") == 0) { i_ += 4; v.type = Type::Bool; v.boolv = true; return true; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; v.type = Type::Bool; v.boolv = false; return true; }
        return false;
    }
    bool parseNull(Value& v) {
        if (s_.compare(i_, 4, "null") == 0) { i_ += 4; v.type = Type::Null; return true; }
        return false;
    }
    bool parseNumber(Value& v) {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) i_++;
        bool any = false;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '+' || c == '-') { i_++; any = true; }
            else break;
        }
        if (!any) return false;
        try { v.num = std::stod(s_.substr(start, i_ - start)); }
        catch (...) { return false; }
        v.type = Type::Number;
        return true;
    }
};

inline bool parse(const std::string& text, Value& out) {
    Parser p(text);
    return p.parse(out);
}

// ---- Escaper ------------------------------------------------------------
// Escape an arbitrary UTF-8 byte string so it is a valid JSON string body
// (does not add surrounding quotes).
inline std::string escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

} // namespace json

#endif // ARCHITECT_JSON_HPP
