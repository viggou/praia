#include "builtins.h"
#include "value.h"
#include <cctype>
#include <memory>
#include <sstream>
#include <string>

namespace {

class JsonParser {
    const std::string& src;
    size_t pos = 0;

    void skipWhitespace() { while (pos < src.size() && std::isspace(src[pos])) pos++; }
    char peek() { return pos < src.size() ? src[pos] : '\0'; }
    char advance() { return src[pos++]; }

    // "at position 42, near 'foo|bar'" where | marks the current position
    std::string context() const {
        std::string s = " at position " + std::to_string(pos) + ", near '";
        size_t start = pos > 10 ? pos - 10 : 0;
        size_t end = std::min(src.size(), pos + 10);
        for (size_t i = start; i < end; i++) {
            char c = src[i];
            if (i == pos) s += '|';
            if (c == '\n') s += "\\n";
            else if (c == '\t') s += "\\t";
            else s += c;
        }
        if (pos >= src.size()) s += "|";
        return s + "'";
    }

    [[noreturn]] void fail(const std::string& msg) const {
        throw RuntimeError(msg + context(), 0);
    }

    Value parseValue() {
        skipWhitespace();
        char c = peek();
        if (c == '"') return parseString();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || std::isdigit(c)) return parseNumber();
        if (c == '\0') fail("Unexpected end of JSON input");
        fail(std::string("Unexpected character '") + c + "' in JSON");
    }

    Value parseString() {
        advance(); // opening "
        std::string result;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\') {
                pos++;
                switch (src[pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case 'r': result += '\r'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'u': {
                        pos++;
                        if (pos + 4 > src.size())
                            throw RuntimeError("Truncated unicode escape in JSON", 0);
                        std::string hex = src.substr(pos, 4);
                        pos += 3; // +1 from the loop increment below
                        int cp;
                        try { cp = std::stoi(hex, nullptr, 16); }
                        catch (...) { throw RuntimeError("Invalid unicode escape: \\u" + hex, 0); }
                        if (cp < 0x80) result += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: result += src[pos]; break;
                }
            } else {
                result += src[pos];
            }
            pos++;
        }
        if (pos < src.size()) pos++; // closing "
        return Value(std::move(result));
    }

    Value parseNumber() {
        size_t start = pos;
        if (src[pos] == '-') pos++;
        while (pos < src.size() && std::isdigit(src[pos])) pos++;
        if (pos < src.size() && src[pos] == '.') {
            pos++;
            while (pos < src.size() && std::isdigit(src[pos])) pos++;
        }
        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            pos++;
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) pos++;
            while (pos < src.size() && std::isdigit(src[pos])) pos++;
        }
        return Value(std::stod(src.substr(start, pos - start)));
    }

    Value parseObject() {
        advance(); // {
        auto map = std::make_shared<PraiaMap>();
        skipWhitespace();
        if (peek() == '}') { pos++; return Value(map); }
        while (true) {
            skipWhitespace();
            if (peek() != '"') fail("Expected string key in JSON object");
            Value key = parseString();
            skipWhitespace();
            if (peek() != ':') fail("Expected ':' after key in JSON object");
            advance();
            map->entries[key.asString()] = parseValue();
            skipWhitespace();
            if (peek() == '}') { pos++; break; }
            if (peek() != ',') fail("Expected ',' or '}' in JSON object");
            advance();
        }
        return Value(map);
    }

    Value parseArray() {
        advance(); // [
        auto arr = std::make_shared<PraiaArray>();
        skipWhitespace();
        if (peek() == ']') { pos++; return Value(arr); }
        while (true) {
            arr->elements.push_back(parseValue());
            skipWhitespace();
            if (peek() == ']') { pos++; break; }
            if (peek() != ',') fail("Expected ',' or ']' in JSON array");
            advance();
        }
        return Value(arr);
    }

    Value parseBool() {
        if (src.compare(pos, 4, "true") == 0) { pos += 4; return Value(true); }
        if (src.compare(pos, 5, "false") == 0) { pos += 5; return Value(false); }
        fail("Invalid JSON boolean (expected 'true' or 'false')");
    }

    Value parseNull() {
        if (src.compare(pos, 4, "null") == 0) { pos += 4; return Value(); }
        fail("Invalid JSON null (expected 'null')");
    }

public:
    JsonParser(const std::string& s) : src(s) {}
    Value parse() {
        Value v = parseValue();
        skipWhitespace();
        if (pos != src.size())
            fail("Unexpected content after JSON value");
        return v;
    }
};

} // namespace

Value jsonParse(const std::string& src) {
    JsonParser parser(src);
    return parser.parse();
}

std::string jsonStringify(const Value& val, int indent, int depth) {
    std::string pad(depth * indent, ' ');
    std::string childPad((depth + 1) * indent, ' ');
    bool pretty = indent > 0;
    std::string nl = pretty ? "\n" : "";

    if (val.isNil()) return "null";
    if (val.isBool()) return val.asBool() ? "true" : "false";
    if (val.isNumber()) {
        std::ostringstream o; o << val.asNumber(); return o.str();
    }
    if (val.isString()) {
        std::string r = "\"";
        for (char c : val.asString()) {
            switch (c) {
                case '"': r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n"; break;
                case '\t': r += "\\t"; break;
                case '\r': r += "\\r"; break;
                default: r += c;
            }
        }
        return r + "\"";
    }
    if (val.isArray()) {
        auto& elems = val.asArray()->elements;
        if (elems.empty()) return "[]";
        std::string r = "[" + nl;
        for (size_t i = 0; i < elems.size(); i++) {
            if (i > 0) r += "," + nl;
            r += childPad + jsonStringify(elems[i], indent, depth + 1);
        }
        return r + nl + pad + "]";
    }
    if (val.isMap()) {
        auto& entries = val.asMap()->entries;
        if (entries.empty()) return "{}";
        std::string r = "{" + nl;
        bool first = true;
        for (auto& [k, v] : entries) {
            if (!first) r += "," + nl;
            first = false;
            r += childPad + "\"" + k + "\":" + (pretty ? " " : "");
            r += jsonStringify(v, indent, depth + 1);
        }
        return r + nl + pad + "}";
    }
    return "null"; // functions, futures, instances → null
}
