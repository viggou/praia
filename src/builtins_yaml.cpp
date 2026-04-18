#include "builtins.h"
#include "value.h"
#include <cctype>
#include <memory>
#include <sstream>
#include <string>

namespace {

class YamlParser {
    const std::string& src;
    size_t pos = 0;

    char peek() { return pos < src.size() ? src[pos] : '\0'; }
    char advance() { return pos < src.size() ? src[pos++] : '\0'; }
    bool atEnd() { return pos >= src.size(); }

    void skipSpaces() { while (pos < src.size() && src[pos] == ' ') pos++; }

    void skipComment() {
        if (pos < src.size() && src[pos] == '#')
            while (pos < src.size() && src[pos] != '\n') pos++;
    }

    void skipBlankLines() {
        while (pos < src.size()) {
            size_t lineStart = pos;
            skipSpaces();
            skipComment();
            if (pos < src.size() && src[pos] == '\n') { pos++; continue; }
            pos = lineStart;
            break;
        }
    }

    int currentIndent() {
        size_t saved = pos;
        int indent = 0;
        while (pos < src.size() && src[pos] == ' ') { indent++; pos++; }
        pos = saved;
        return indent;
    }

    std::string readLine() {
        std::string line;
        while (pos < src.size() && src[pos] != '\n') line += src[pos++];
        if (pos < src.size()) pos++; // consume \n
        // Strip trailing comment
        size_t comment = std::string::npos;
        bool inQuote = false;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '"' || line[i] == '\'') inQuote = !inQuote;
            if (!inQuote && line[i] == '#') { comment = i; break; }
        }
        if (comment != std::string::npos) line = line.substr(0, comment);
        // Strip trailing whitespace
        while (!line.empty() && std::isspace(line.back())) line.pop_back();
        return line;
    }

    Value parseScalar(const std::string& s) {
        if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL") return Value();
        if (s == "true" || s == "True" || s == "TRUE") return Value(true);
        if (s == "false" || s == "False" || s == "FALSE") return Value(false);
        // Quoted string
        if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))
            return Value(s.substr(1, s.size() - 2));
        // Try number
        try {
            size_t p = 0;
            double d = std::stod(s, &p);
            if (p == s.size()) return Value(d);
        } catch (...) {}
        return Value(s); // plain string
    }

    Value parseValue(int minIndent) {
        skipBlankLines();
        if (atEnd()) return Value();

        int indent = currentIndent();
        if (indent < minIndent) return Value();

        // Check if it's a sequence (- item)
        if (pos + 1 < src.size() && src[pos + indent] == '-' &&
            (pos + indent + 1 >= src.size() || src[pos + indent + 1] == ' ' || src[pos + indent + 1] == '\n')) {
            return parseSequence(indent);
        }

        // Check if it's a mapping (key: value)
        size_t saved = pos;
        skipSpaces();
        std::string line = readLine();
        auto colonPos = line.find(": ");
        if (colonPos == std::string::npos && !line.empty() && line.back() == ':')
            colonPos = line.size() - 1;

        if (colonPos != std::string::npos) {
            pos = saved;
            return parseMapping(indent);
        }

        // It's a scalar
        return parseScalar(line);
    }

    Value parseSequence(int indent) {
        auto arr = std::make_shared<PraiaArray>();
        while (!atEnd()) {
            skipBlankLines();
            if (atEnd()) break;
            int curIndent = currentIndent();
            if (curIndent != indent) break;
            if (src[pos + curIndent] != '-') break;

            pos += curIndent + 1; // skip spaces + '-'
            if (pos < src.size() && src[pos] == ' ') pos++; // skip space after -

            // Check if inline value or nested block
            skipBlankLines();
            if (atEnd() || src[pos] == '\n') {
                pos++;
                arr->elements.push_back(parseValue(indent + 2));
            } else {
                int valIndent = currentIndent();
                // Inline: read rest of line, could be a scalar or start of nested mapping
                size_t saved = pos;
                std::string rest = readLine();
                auto c = rest.find(": ");
                if (c == std::string::npos && !rest.empty() && rest.back() == ':')
                    c = rest.size() - 1;

                if (c != std::string::npos) {
                    // Inline mapping start — reparse
                    pos = saved;
                    arr->elements.push_back(parseMapping(valIndent >= 0 ? valIndent : indent + 2));
                } else {
                    arr->elements.push_back(parseScalar(rest));
                }
            }
        }
        return Value(arr);
    }

    Value parseMapping(int indent) {
        auto map = std::make_shared<PraiaMap>();
        while (!atEnd()) {
            skipBlankLines();
            if (atEnd()) break;
            int curIndent = currentIndent();
            if (curIndent != indent) break;

            skipSpaces();
            std::string line = readLine();
            auto colonPos = line.find(": ");
            bool colonAtEnd = false;
            if (colonPos == std::string::npos && !line.empty() && line.back() == ':') {
                colonPos = line.size() - 1;
                colonAtEnd = true;
            }
            if (colonPos == std::string::npos) break;

            std::string key = line.substr(0, colonPos);
            // Strip quotes from key
            if ((key.front() == '"' && key.back() == '"') ||
                (key.front() == '\'' && key.back() == '\''))
                key = key.substr(1, key.size() - 2);

            if (colonAtEnd || colonPos + 2 >= line.size()) {
                // Value is on next lines
                map->entries[key] = parseValue(indent + 1);
            } else {
                std::string val = line.substr(colonPos + 2);
                // Check for inline flow sequence [a, b]
                if (!val.empty() && val.front() == '[' && val.back() == ']') {
                    auto arr = std::make_shared<PraiaArray>();
                    std::string inner = val.substr(1, val.size() - 2);
                    std::istringstream ss(inner);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        while (!item.empty() && item.front() == ' ') item.erase(0, 1);
                        while (!item.empty() && item.back() == ' ') item.pop_back();
                        arr->elements.push_back(parseScalar(item));
                    }
                    map->entries[key] = Value(arr);
                } else {
                    map->entries[key] = parseScalar(val);
                }
            }
        }
        return Value(map);
    }

public:
    YamlParser(const std::string& s) : src(s) {}
    Value parse() { return parseValue(0); }
};

} // namespace

Value yamlParse(const std::string& src) {
    YamlParser parser(src);
    return parser.parse();
}

std::string yamlStringify(const Value& val, int depth) {
    std::string pad(depth * 2, ' ');

    if (val.isNil()) return "null";
    if (val.isBool()) return val.asBool() ? "true" : "false";
    if (val.isNumber()) { std::ostringstream o; o << val.asNumber(); return o.str(); }
    if (val.isString()) {
        auto& s = val.asString();
        bool needsQuote = s.empty() || s.find(": ") != std::string::npos ||
                          s.find('#') != std::string::npos || s.find('\n') != std::string::npos ||
                          s == "true" || s == "false" || s == "null";
        if (needsQuote) {
            std::string r = "\"";
            for (char c : s) {
                if (c == '"') r += "\\\"";
                else if (c == '\\') r += "\\\\";
                else if (c == '\n') r += "\\n";
                else r += c;
            }
            return r + "\"";
        }
        return s;
    }
    if (val.isArray()) {
        auto& elems = val.asArray()->elements;
        if (elems.empty()) return "[]";
        std::string r;
        for (size_t i = 0; i < elems.size(); i++) {
            if (i > 0 || depth > 0) r += pad;
            r += "- ";
            if (elems[i].isMap() || elems[i].isArray()) {
                r += "\n" + yamlStringify(elems[i], depth + 1);
            } else {
                r += yamlStringify(elems[i], depth + 1) + "\n";
            }
        }
        return r;
    }
    if (val.isMap()) {
        auto& entries = val.asMap()->entries;
        if (entries.empty()) return "{}";
        std::string r;
        for (auto& [k, v] : entries) {
            if (depth > 0) r += pad;
            r += k + ":";
            if (v.isMap() || v.isArray()) {
                r += "\n" + yamlStringify(v, depth + 1);
            } else {
                r += " " + yamlStringify(v, depth + 1) + "\n";
            }
        }
        return r;
    }
    return "null";
}
