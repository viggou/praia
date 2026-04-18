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

    Value parseValue() {
        skipWhitespace();
        char c = peek();
        if (c == '"') return parseString();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || std::isdigit(c)) return parseNumber();
        throw RuntimeError("Invalid JSON at position " + std::to_string(pos), 0);
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
                        std::string hex = src.substr(pos, 4);
                        pos += 3; // +1 from the loop increment below
                        int cp = std::stoi(hex, nullptr, 16);
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
            if (peek() != '"')
                throw RuntimeError("Expected string key in JSON object", 0);
            Value key = parseString();
            skipWhitespace();
            if (advance() != ':')
                throw RuntimeError("Expected ':' in JSON object", 0);
            map->entries[key.asString()] = parseValue();
            skipWhitespace();
            if (peek() == '}') { pos++; break; }
            if (advance() != ',')
                throw RuntimeError("Expected ',' or '}' in JSON object", 0);
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
            if (advance() != ',')
                throw RuntimeError("Expected ',' or ']' in JSON array", 0);
        }
        return Value(arr);
    }

    Value parseBool() {
        if (src.compare(pos, 4, "true") == 0) { pos += 4; return Value(true); }
        if (src.compare(pos, 5, "false") == 0) { pos += 5; return Value(false); }
        throw RuntimeError("Invalid JSON boolean", 0);
    }

    Value parseNull() {
        if (src.compare(pos, 4, "null") == 0) { pos += 4; return Value(); }
        throw RuntimeError("Invalid JSON null", 0);
    }

public:
    JsonParser(const std::string& s) : src(s) {}
    Value parse() {
        Value v = parseValue();
        skipWhitespace();
        if (pos != src.size())
            throw RuntimeError("Unexpected content after JSON value", 0);
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
