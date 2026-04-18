#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#ifdef HAVE_SQLITE
#include <sqlite3.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ── HTTP helpers ─────────────────────────────────────────────

struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
};

static ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl r;
    std::string rest = url;
    auto scheme = rest.find("://");
    if (scheme != std::string::npos) {
        if (rest.substr(0, scheme) == "https")
            throw RuntimeError("HTTPS not supported (use http:// or sys.exec with curl)", 0);
        rest = rest.substr(scheme + 3);
    }
    auto slash = rest.find('/');
    std::string hostPort = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
    auto colon = hostPort.find(':');
    if (colon != std::string::npos) {
        r.host = hostPort.substr(0, colon);
        r.port = std::stoi(hostPort.substr(colon + 1));
    } else {
        r.host = hostPort;
    }
    if (slash != std::string::npos) r.path = rest.substr(slash);
    return r;
}

static int connectToHost(const std::string& host, int port) {
    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
        throw RuntimeError("Cannot resolve host: " + host, 0);
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0 || connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        if (sock >= 0) close(sock);
        freeaddrinfo(res);
        throw RuntimeError("Cannot connect to " + host + ":" + std::to_string(port), 0);
    }
    freeaddrinfo(res);
    return sock;
}

static std::string readAll(int sock) {
    std::string data;
    char buf[8192];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0)
        data.append(buf, n);
    return data;
}

static Value parseHttpResponse(const std::string& raw) {
    auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
        throw RuntimeError("Invalid HTTP response", 0);

    std::string headerSection = raw.substr(0, headerEnd);
    std::string body = raw.substr(headerEnd + 4);

    // Status code
    int status = 0;
    auto sp1 = headerSection.find(' ');
    if (sp1 != std::string::npos) {
        auto sp2 = headerSection.find(' ', sp1 + 1);
        try { status = std::stoi(headerSection.substr(sp1 + 1, sp2 - sp1 - 1)); } catch (...) {}
    }

    // Headers
    auto hdrs = std::make_shared<PraiaMap>();
    std::istringstream hs(headerSection);
    std::string line;
    std::getline(hs, line); // skip status line
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto c = line.find(": ");
        if (c != std::string::npos) {
            std::string key = line.substr(0, c);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            hdrs->entries[key] = Value(line.substr(c + 2));
        }
    }

    auto result = std::make_shared<PraiaMap>();
    result->entries["status"] = Value(static_cast<double>(status));
    result->entries["body"] = Value(body);
    result->entries["headers"] = Value(hdrs);
    return Value(result);
}

static Value doHttpRequest(const std::string& method, const std::string& url,
                           const std::string& body,
                           const std::unordered_map<std::string, std::string>& extraHeaders) {
    auto p = parseUrl(url);
    int sock = connectToHost(p.host, p.port);

    std::string req = method + " " + p.path + " HTTP/1.1\r\n";
    req += "Host: " + p.host + "\r\n";
    req += "Connection: close\r\n";
    for (auto& [k, v] : extraHeaders) req += k + ": " + v + "\r\n";
    if (!body.empty() && extraHeaders.find("Content-Type") == extraHeaders.end())
        req += "Content-Type: text/plain\r\n";
    if (!body.empty())
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;

    send(sock, req.c_str(), req.size(), 0);
    std::string raw = readAll(sock);
    close(sock);
    return parseHttpResponse(raw);
}

static void httpServerListen(int port, std::shared_ptr<Callable> handler, Interpreter& interp) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw RuntimeError("Cannot create server socket", 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); throw RuntimeError("Cannot bind to port " + std::to_string(port), 0);
    }
    if (listen(fd, 64) < 0) {
        close(fd); throw RuntimeError("Cannot listen on port " + std::to_string(port), 0);
    }

    std::cout << "Server listening on port " << port << std::endl;

    while (true) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int client = accept(fd, (struct sockaddr*)&ca, &cl);
        if (client < 0) continue;

        // Read request (headers + body)
        std::string data;
        char buf[8192];
        while (true) {
            ssize_t n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            data.append(buf, n);
            auto hend = data.find("\r\n\r\n");
            if (hend != std::string::npos) {
                // Check Content-Length for body
                std::string hdr = data.substr(0, hend);
                size_t cl_pos = hdr.find("Content-Length: ");
                if (cl_pos == std::string::npos) cl_pos = hdr.find("content-length: ");
                if (cl_pos != std::string::npos) {
                    int clen = std::stoi(hdr.substr(cl_pos + 16));
                    size_t bodyStart = hend + 4;
                    while ((int)(data.size() - bodyStart) < clen) {
                        n = recv(client, buf, sizeof(buf), 0);
                        if (n <= 0) break;
                        data.append(buf, n);
                    }
                }
                break;
            }
        }

        if (data.empty()) { close(client); continue; }

        // Parse request line
        auto firstLine = data.substr(0, data.find("\r\n"));
        std::string method, path;
        auto sp1 = firstLine.find(' ');
        auto sp2 = firstLine.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            method = firstLine.substr(0, sp1);
            path = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);
        }

        // Parse query string
        std::string query;
        auto qpos = path.find('?');
        if (qpos != std::string::npos) {
            query = path.substr(qpos + 1);
            path = path.substr(0, qpos);
        }

        // Parse headers
        auto hend = data.find("\r\n\r\n");
        auto reqHeaders = std::make_shared<PraiaMap>();
        std::istringstream hs(data.substr(0, hend));
        std::string line;
        std::getline(hs, line); // skip request line
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto c = line.find(": ");
            if (c != std::string::npos) {
                std::string key = line.substr(0, c);
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                reqHeaders->entries[key] = Value(line.substr(c + 2));
            }
        }

        std::string reqBody = (hend != std::string::npos) ? data.substr(hend + 4) : "";

        // Build request map
        auto req = std::make_shared<PraiaMap>();
        req->entries["method"] = Value(method);
        req->entries["path"] = Value(path);
        req->entries["query"] = Value(query);
        req->entries["headers"] = Value(reqHeaders);
        req->entries["body"] = Value(reqBody);

        // Call handler
        std::string respBody = "Internal Server Error";
        int respStatus = 500;
        std::string respContentType = "text/plain";
        std::unordered_map<std::string, std::string> respHeaders;

        try {
            std::vector<Value> args = {Value(req)};
            Value result = handler->call(interp, args);
            if (result.isMap()) {
                auto& e = result.asMap()->entries;
                if (e.count("status") && e["status"].isNumber())
                    respStatus = static_cast<int>(e["status"].asNumber());
                else
                    respStatus = 200;
                if (e.count("body"))
                    respBody = e["body"].toString();
                else
                    respBody = "";
                if (e.count("headers") && e["headers"].isMap()) {
                    for (auto& [k, v] : e["headers"].asMap()->entries)
                        respHeaders[k] = v.toString();
                }
                if (respHeaders.find("Content-Type") == respHeaders.end() &&
                    respHeaders.find("content-type") == respHeaders.end())
                    respHeaders["Content-Type"] = "text/plain";
            } else if (result.isString()) {
                respStatus = 200;
                respBody = result.asString();
                respHeaders["Content-Type"] = "text/plain";
            }
        } catch (const ThrowSignal& t) {
            respStatus = 500;
            respBody = "Error: " + t.value.toString();
        } catch (const RuntimeError& e) {
            respStatus = 500;
            respBody = "Error: " + std::string(e.what());
        }

        // Send response
        std::string resp = "HTTP/1.1 " + std::to_string(respStatus) + " OK\r\n";
        respHeaders["Content-Length"] = std::to_string(respBody.size());
        respHeaders["Connection"] = "close";
        for (auto& [k, v] : respHeaders) resp += k + ": " + v + "\r\n";
        resp += "\r\n" + respBody;
        send(client, resp.c_str(), resp.size(), 0);
        close(client);
    }
    close(fd);
}

// ── JSON parser/stringifier ──────────────────────────────────

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

static std::string jsonStringify(const Value& val, int indent, int depth) {
    std::string pad(depth * indent, ' ');
    std::string childPad((depth + 1) * indent, ' ');
    bool pretty = indent > 0;
    std::string nl = pretty ? "\n" : "";
    std::string sep = pretty ? ", " : ",";

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

// ── YAML parser/stringifier ──────────────────────────────────

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

static std::string yamlStringify(const Value& val, int depth) {
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

// ── Helper: make a NativeFunction value ──────────────────────

static std::shared_ptr<NativeFunction> makeNative(
    const std::string& name, int arity,
    std::function<Value(const std::vector<Value>&)> fn) {
    auto f = std::make_shared<NativeFunction>();
    f->funcName = name;
    f->numArgs = arity;
    f->fn = std::move(fn);
    return f;
}

// ── String methods (returned by dot access on strings) ───────

static Value getStringMethod(const std::string& str,
                              const std::string& name, int line) {
    if (name == "upper") {
        return Value(makeNative("upper", 0, [str](const std::vector<Value>&) -> Value {
            std::string r = str;
            std::transform(r.begin(), r.end(), r.begin(), ::toupper);
            return Value(std::move(r));
        }));
    }
    if (name == "lower") {
        return Value(makeNative("lower", 0, [str](const std::vector<Value>&) -> Value {
            std::string r = str;
            std::transform(r.begin(), r.end(), r.begin(), ::tolower);
            return Value(std::move(r));
        }));
    }
    if (name == "strip") {
        return Value(makeNative("strip", 0, [str](const std::vector<Value>&) -> Value {
            size_t start = str.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) return Value(std::string(""));
            size_t end = str.find_last_not_of(" \t\n\r");
            return Value(str.substr(start, end - start + 1));
        }));
    }
    if (name == "split") {
        return Value(makeNative("split", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("split() separator must be a string", 0);
            auto& sep = args[0].asString();
            auto arr = std::make_shared<PraiaArray>();
            if (sep.empty()) {
                for (char c : str)
                    arr->elements.push_back(Value(std::string(1, c)));
                return Value(arr);
            }
            size_t pos = 0, found;
            while ((found = str.find(sep, pos)) != std::string::npos) {
                arr->elements.push_back(Value(str.substr(pos, found - pos)));
                pos = found + sep.size();
            }
            arr->elements.push_back(Value(str.substr(pos)));
            return Value(arr);
        }));
    }
    if (name == "contains") {
        return Value(makeNative("contains", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("contains() argument must be a string", 0);
            return Value(str.find(args[0].asString()) != std::string::npos);
        }));
    }
    if (name == "replace") {
        return Value(makeNative("replace", 2, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("replace() arguments must be strings", 0);
            auto& from = args[0].asString();
            auto& to = args[1].asString();
            std::string result = str;
            size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) {
                result.replace(pos, from.size(), to);
                pos += to.size();
            }
            return Value(std::move(result));
        }));
    }
    if (name == "startsWith") {
        return Value(makeNative("startsWith", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("startsWith() argument must be a string", 0);
            auto& prefix = args[0].asString();
            return Value(str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0);
        }));
    }
    if (name == "endsWith") {
        return Value(makeNative("endsWith", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("endsWith() argument must be a string", 0);
            auto& suffix = args[0].asString();
            return Value(str.size() >= suffix.size() &&
                         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
        }));
    }
    if (name == "title") {
        return Value(makeNative("title", 0, [str](const std::vector<Value>&) -> Value {
            std::string r = str;
            bool capNext = true;
            for (auto& c : r) {
                if (std::isspace(c)) { capNext = true; }
                else if (capNext) { c = std::toupper(c); capNext = false; }
                else { c = std::tolower(c); }
            }
            return Value(std::move(r));
        }));
    }
    if (name == "capitalize") {
        return Value(makeNative("capitalize", 0, [str](const std::vector<Value>&) -> Value {
            std::string r = str;
            if (!r.empty()) {
                r[0] = std::toupper(r[0]);
                for (size_t i = 1; i < r.size(); i++)
                    r[i] = std::tolower(r[i]);
            }
            return Value(std::move(r));
        }));
    }
    if (name == "capitalizeFirst") {
        return Value(makeNative("capitalizeFirst", 0, [str](const std::vector<Value>&) -> Value {
            std::string r = str;
            if (!r.empty()) r[0] = std::toupper(r[0]);
            return Value(std::move(r));
        }));
    }
    if (name == "charCode") {
        return Value(makeNative("charCode", -1, [str](const std::vector<Value>& args) -> Value {
            int idx = 0;
            if (!args.empty() && args[0].isNumber())
                idx = static_cast<int>(args[0].asNumber());
            if (idx < 0) idx += static_cast<int>(str.size());
            if (idx < 0 || idx >= static_cast<int>(str.size()))
                throw RuntimeError("charCode index out of bounds", 0);
            return Value(static_cast<double>(static_cast<unsigned char>(str[idx])));
        }));
    }
    if (name == "test") {
        return Value(makeNative("test", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("test() pattern must be a string", 0);
            try {
                std::regex re(args[0].asString());
                return Value(std::regex_search(str, re));
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
        }));
    }
    if (name == "match") {
        return Value(makeNative("match", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("match() pattern must be a string", 0);
            try {
                std::regex re(args[0].asString());
                std::smatch m;
                if (!std::regex_search(str, m, re)) return Value();
                // Return map with match and groups
                auto result = std::make_shared<PraiaMap>();
                result->entries["match"] = Value(m[0].str());
                result->entries["index"] = Value(static_cast<double>(m.position(0)));
                auto groups = std::make_shared<PraiaArray>();
                for (size_t i = 1; i < m.size(); i++)
                    groups->elements.push_back(Value(m[i].str()));
                result->entries["groups"] = Value(groups);
                return Value(result);
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
        }));
    }
    if (name == "matchAll") {
        return Value(makeNative("matchAll", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("matchAll() pattern must be a string", 0);
            try {
                std::regex re(args[0].asString());
                auto results = std::make_shared<PraiaArray>();
                auto begin = std::sregex_iterator(str.begin(), str.end(), re);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it) {
                    auto entry = std::make_shared<PraiaMap>();
                    entry->entries["match"] = Value((*it)[0].str());
                    entry->entries["index"] = Value(static_cast<double>(it->position(0)));
                    auto groups = std::make_shared<PraiaArray>();
                    for (size_t i = 1; i < it->size(); i++)
                        groups->elements.push_back(Value((*it)[i].str()));
                    entry->entries["groups"] = Value(groups);
                    results->elements.push_back(Value(entry));
                }
                return Value(results);
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
        }));
    }
    if (name == "replacePattern") {
        return Value(makeNative("replacePattern", 2, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("replacePattern() requires string arguments", 0);
            try {
                std::regex re(args[0].asString());
                return Value(std::regex_replace(str, re, args[1].asString()));
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
        }));
    }
    throw RuntimeError("String has no method '" + name + "'", line);
}

// ── Array methods (returned by dot access on arrays) ─────────

static Value getArrayMethod(std::shared_ptr<PraiaArray> arr,
                             const std::string& name, int line) {
    if (name == "push") {
        return Value(makeNative("push", 1, [arr](const std::vector<Value>& args) -> Value {
            arr->elements.push_back(args[0]);
            return Value();
        }));
    }
    if (name == "pop") {
        return Value(makeNative("pop", 0, [arr](const std::vector<Value>&) -> Value {
            if (arr->elements.empty())
                throw RuntimeError("pop() on empty array", 0);
            Value last = arr->elements.back();
            arr->elements.pop_back();
            return last;
        }));
    }
    if (name == "contains") {
        return Value(makeNative("contains", 1, [arr](const std::vector<Value>& args) -> Value {
            for (auto& e : arr->elements)
                if (e == args[0]) return Value(true);
            return Value(false);
        }));
    }
    if (name == "join") {
        return Value(makeNative("join", 1, [arr](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("join() separator must be a string", 0);
            auto& sep = args[0].asString();
            std::string result;
            for (size_t i = 0; i < arr->elements.size(); i++) {
                if (i > 0) result += sep;
                result += arr->elements[i].toString();
            }
            return Value(std::move(result));
        }));
    }
    if (name == "reverse") {
        return Value(makeNative("reverse", 0, [arr](const std::vector<Value>&) -> Value {
            std::reverse(arr->elements.begin(), arr->elements.end());
            return Value();
        }));
    }
    throw RuntimeError("Array has no method '" + name + "'", line);
}

// ── PraiaClass / PraiaMethod ──────────────────────────────────

const ClassMethod* PraiaClass::findMethod(const std::string& name) const {
    auto it = methods.find(name);
    if (it != methods.end()) return it->second;
    if (superclass) return superclass->findMethod(name);
    return nullptr;
}

int PraiaClass::arity() const {
    auto* init = findMethod("init");
    return init ? static_cast<int>(init->params.size()) : 0;
}

Value PraiaClass::call(Interpreter& interp, const std::vector<Value>& args) {
    auto instance = std::make_shared<PraiaInstance>();
    instance->klass = shared_from_this();

    auto* init = findMethod("init");
    if (init) {
        // Bind and call the init method
        auto method = std::make_shared<PraiaMethod>();
        method->methodName = "init";
        method->params = init->params;
        method->decl = init;
        method->closure = closure;
        method->instance = instance;
        // Find which class in the hierarchy defines init
        auto self = shared_from_this();
        while (self && !self->methods.count("init"))
            self = self->superclass;
        method->definingClass = self ? self : shared_from_this();
        method->call(interp, args);
    }

    return Value(instance);
}

Value PraiaMethod::call(Interpreter& interp, const std::vector<Value>& args) {
    auto methodEnv = std::make_shared<Environment>(closure);
    methodEnv->define("this", Value(instance));

    // Store the defining class so super resolves correctly in multi-level inheritance
    if (definingClass && definingClass->superclass) {
        methodEnv->define("__super__", Value(
            std::static_pointer_cast<Callable>(definingClass->superclass)));
    }

    for (size_t i = 0; i < params.size(); i++)
        methodEnv->define(params[i], i < args.size() ? args[i] : Value());

    auto prevEnv = interp.env;
    interp.env = methodEnv;
    try {
        for (const auto& stmt : decl->body)
            interp.execute(stmt.get());
    } catch (const ReturnSignal& ret) {
        interp.env = prevEnv;
        return ret.value;
    } catch (...) {
        interp.env = prevEnv;
        throw;
    }
    interp.env = prevEnv;
    return Value();
}

// ── PraiaFunction / PraiaLambda ───────────────────────────────

Value PraiaLambda::call(Interpreter& interp, const std::vector<Value>& args) {
    auto lambdaEnv = std::make_shared<Environment>(closure);
    for (size_t i = 0; i < params.size(); i++)
        lambdaEnv->define(params[i], i < args.size() ? args[i] : Value());

    auto prevEnv = interp.env;
    interp.env = lambdaEnv;
    try {
        for (const auto& stmt : expr->body)
            interp.execute(stmt.get());
    } catch (const ReturnSignal& ret) {
        interp.env = prevEnv;
        return ret.value;
    } catch (...) {
        interp.env = prevEnv;
        throw;
    }
    interp.env = prevEnv;
    return Value(); // implicit nil
}

Value PraiaFunction::call(Interpreter& interp, const std::vector<Value>& args) {
    auto funcEnv = std::make_shared<Environment>(closure);
    for (size_t i = 0; i < params.size(); i++)
        funcEnv->define(params[i], i < args.size() ? args[i] : Value());

    try {
        interp.executeBlock(body, funcEnv);
    } catch (const ReturnSignal& ret) {
        return ret.value;
    }
    return Value(); // implicit nil
}

// ── Interpreter setup ────────────────────────────────────────

Interpreter::Interpreter() {
    globals = std::make_shared<Environment>();
    env = globals;

    // ── Global functions ──

    globals->define("print", Value(makeNative("print", -1,
        [](const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].toString();
            }
            std::cout << "\n";
            return Value();
        })));

    globals->define("len", Value(makeNative("len", 1,
        [](const std::vector<Value>& args) -> Value {
            if (args[0].isArray())
                return Value(static_cast<double>(args[0].asArray()->elements.size()));
            if (args[0].isString())
                return Value(static_cast<double>(args[0].asString().size()));
            if (args[0].isMap())
                return Value(static_cast<double>(args[0].asMap()->entries.size()));
            throw RuntimeError("len() requires an array, string, or map", 0);
        })));

    globals->define("push", Value(makeNative("push", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("push() requires an array as first argument", 0);
            args[0].asArray()->elements.push_back(args[1]);
            return Value();
        })));

    globals->define("pop", Value(makeNative("pop", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("pop() requires an array", 0);
            auto& elems = args[0].asArray()->elements;
            if (elems.empty())
                throw RuntimeError("pop() on empty array", 0);
            Value last = elems.back();
            elems.pop_back();
            return last;
        })));

    globals->define("type", Value(makeNative("type", 1,
        [](const std::vector<Value>& args) -> Value {
            auto& v = args[0];
            if (v.isNil())      return Value("nil");
            if (v.isBool())     return Value("bool");
            if (v.isNumber())   return Value("number");
            if (v.isString())   return Value("string");
            if (v.isArray())    return Value("array");
            if (v.isMap())      return Value("map");
            if (v.isInstance()) return Value("instance");
            if (v.isCallable()) return Value("function");
            return Value("unknown");
        })));

    globals->define("str", Value(makeNative("str", 1,
        [](const std::vector<Value>& args) -> Value {
            return Value(args[0].toString());
        })));

    globals->define("num", Value(makeNative("num", 1,
        [](const std::vector<Value>& args) -> Value {
            if (args[0].isNumber()) return args[0];
            if (args[0].isString()) {
                try { return Value(std::stod(args[0].asString())); }
                catch (...) { throw RuntimeError("Cannot convert string to number", 0); }
            }
            throw RuntimeError("Cannot convert to number", 0);
        })));

    globals->define("fromCharCode", Value(makeNative("fromCharCode", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("fromCharCode() requires a number", 0);
            return Value(std::string(1, static_cast<char>(static_cast<int>(args[0].asNumber()))));
        })));

    // ── Functional built-ins (work great with |>) ──

    globals->define("sort", Value(makeNative("sort", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isArray())
                throw RuntimeError("sort() requires an array", 0);
            // Copy the array to avoid mutating the original
            auto sorted = std::make_shared<PraiaArray>();
            sorted->elements = args[0].asArray()->elements;
            auto& elems = sorted->elements;

            if (args.size() > 1 && args[1].isCallable()) {
                // Custom comparator — can't call Praia functions from here without interpreter
                // so we only support native comparators. Use array.sort() for custom.
                throw RuntimeError("sort() with comparator not supported in pipe context. Use a plain sort.", 0);
            }
            // Default sort: numbers ascending, strings alphabetical
            std::sort(elems.begin(), elems.end(), [](const Value& a, const Value& b) {
                if (a.isNumber() && b.isNumber()) return a.asNumber() < b.asNumber();
                return a.toString() < b.toString();
            });
            return Value(sorted);
        })));

    globals->define("filter", Value(makeNative("filter", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("filter() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("filter() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto result = std::make_shared<PraiaArray>();
            auto pred = args[1].asCallable();
            for (auto& elem : src) {
                Value test = pred->call(*this, {elem});
                if (test.isTruthy()) result->elements.push_back(elem);
            }
            return Value(result);
        })));

    globals->define("map", Value(makeNative("map", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("map() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("map() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto result = std::make_shared<PraiaArray>();
            auto transform = args[1].asCallable();
            for (auto& elem : src)
                result->elements.push_back(transform->call(*this, {elem}));
            return Value(result);
        })));

    globals->define("each", Value(makeNative("each", 2,
        [this](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("each() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("each() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            for (auto& elem : src)
                fn->call(*this, {elem});
            return args[0]; // return the array for chaining
        })));

    globals->define("keys", Value(makeNative("keys", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("keys() requires a map", 0);
            auto result = std::make_shared<PraiaArray>();
            for (auto& [k, v] : args[0].asMap()->entries)
                result->elements.push_back(Value(k));
            return Value(result);
        })));

    globals->define("values", Value(makeNative("values", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("values() requires a map", 0);
            auto result = std::make_shared<PraiaArray>();
            for (auto& [k, v] : args[0].asMap()->entries)
                result->elements.push_back(v);
            return Value(result);
        })));

    // ── sys namespace ──

    sysMap = std::make_shared<PraiaMap>();

    sysMap->entries["read"] = Value(makeNative("sys.read", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.read() requires a string path", 0);
            std::ifstream f(args[0].asString());
            if (!f.is_open())
                throw RuntimeError("Cannot read file: " + args[0].asString(), 0);
            std::stringstream ss;
            ss << f.rdbuf();
            return Value(ss.str());
        }));

    sysMap->entries["write"] = Value(makeNative("sys.write", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.write() requires a string path", 0);
            std::ofstream f(args[0].asString());
            if (!f.is_open())
                throw RuntimeError("Cannot write file: " + args[0].asString(), 0);
            f << args[1].toString();
            return Value();
        }));

    sysMap->entries["append"] = Value(makeNative("sys.append", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.append() requires a string path", 0);
            std::ofstream f(args[0].asString(), std::ios::app);
            if (!f.is_open())
                throw RuntimeError("Cannot open file: " + args[0].asString(), 0);
            f << args[1].toString();
            return Value();
        }));

    sysMap->entries["exists"] = Value(makeNative("sys.exists", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.exists() requires a string path", 0);
            return Value(fs::exists(args[0].asString()));
        }));

    sysMap->entries["mkdir"] = Value(makeNative("sys.mkdir", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.mkdir() requires a string path", 0);
            fs::create_directories(args[0].asString());
            return Value();
        }));

    sysMap->entries["remove"] = Value(makeNative("sys.remove", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.remove() requires a string path", 0);
            if (!fs::remove(args[0].asString()))
                throw RuntimeError("Cannot remove: " + args[0].asString(), 0);
            return Value();
        }));

    sysMap->entries["exec"] = Value(makeNative("sys.exec", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.exec() requires a string command", 0);
            FILE* pipe = popen(args[0].asString().c_str(), "r");
            if (!pipe)
                throw RuntimeError("Failed to execute command", 0);
            std::string result;
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe))
                result += buf;
            pclose(pipe);
            // Strip trailing newline
            if (!result.empty() && result.back() == '\n')
                result.pop_back();
            return Value(std::move(result));
        }));

    sysMap->entries["exit"] = Value(makeNative("sys.exit", 1,
        [](const std::vector<Value>& args) -> Value {
            int code = 0;
            if (!args.empty() && args[0].isNumber())
                code = static_cast<int>(args[0].asNumber());
            throw ExitSignal{code};
        }));

    // sys.args — defaults to empty, set via setArgs()
    auto emptyArgs = std::make_shared<PraiaArray>();
    sysMap->entries["args"] = Value(emptyArgs);

    globals->define("sys", Value(sysMap));

    // ── http namespace ──

    auto httpMap = std::make_shared<PraiaMap>();
    Interpreter* self = this;

    httpMap->entries["get"] = Value(makeNative("http.get", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("http.get() requires a URL string", 0);
            return doHttpRequest("GET", args[0].asString(), "", {});
        }));

    httpMap->entries["post"] = Value(makeNative("http.post", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("http.post() requires a URL string", 0);
            std::string body;
            std::unordered_map<std::string, std::string> headers;
            if (args[1].isString()) {
                body = args[1].asString();
            } else if (args[1].isMap()) {
                auto& e = args[1].asMap()->entries;
                if (e.count("body")) body = e.at("body").toString();
                if (e.count("headers") && e.at("headers").isMap()) {
                    for (auto& [k, v] : e.at("headers").asMap()->entries)
                        headers[k] = v.toString();
                }
            }
            return doHttpRequest("POST", args[0].asString(), body, headers);
        }));

    httpMap->entries["request"] = Value(makeNative("http.request", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap())
                throw RuntimeError("http.request() requires an options map", 0);
            auto& opts = args[0].asMap()->entries;
            std::string method = "GET", url, body;
            std::unordered_map<std::string, std::string> headers;
            if (opts.count("method")) method = opts.at("method").toString();
            if (opts.count("url")) url = opts.at("url").toString();
            else throw RuntimeError("http.request() requires a 'url' field", 0);
            if (opts.count("body")) body = opts.at("body").toString();
            if (opts.count("headers") && opts.at("headers").isMap()) {
                for (auto& [k, v] : opts.at("headers").asMap()->entries)
                    headers[k] = v.toString();
            }
            return doHttpRequest(method, url, body, headers);
        }));

    httpMap->entries["createServer"] = Value(makeNative("http.createServer", 1,
        [self](const std::vector<Value>& args) -> Value {
            if (!args[0].isCallable())
                throw RuntimeError("http.createServer() requires a handler function", 0);
            auto handler = args[0].asCallable();

            auto server = std::make_shared<PraiaMap>();
            server->entries["listen"] = Value(makeNative("listen", 1,
                [handler, self](const std::vector<Value>& args) -> Value {
                    if (!args[0].isNumber())
                        throw RuntimeError("listen() requires a port number", 0);
                    httpServerListen(static_cast<int>(args[0].asNumber()), handler, *self);
                    return Value();
                }));
            return Value(server);
        }));

    globals->define("http", Value(httpMap));

    // ── json namespace ──

    auto jsonMap = std::make_shared<PraiaMap>();

    jsonMap->entries["parse"] = Value(makeNative("json.parse", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("json.parse() requires a string", 0);
            JsonParser parser(args[0].asString());
            return parser.parse();
        }));

    jsonMap->entries["stringify"] = Value(makeNative("json.stringify", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("json.stringify() requires a value", 0);
            int indent = 0;
            if (args.size() > 1 && args[1].isNumber())
                indent = static_cast<int>(args[1].asNumber());
            return Value(jsonStringify(args[0], indent, 0));
        }));

    globals->define("json", Value(jsonMap));

    // ── yaml namespace ──

    auto yamlMap = std::make_shared<PraiaMap>();

    yamlMap->entries["parse"] = Value(makeNative("yaml.parse", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("yaml.parse() requires a string", 0);
            YamlParser parser(args[0].asString());
            return parser.parse();
        }));

    yamlMap->entries["stringify"] = Value(makeNative("yaml.stringify", 1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty())
                throw RuntimeError("yaml.stringify() requires a value", 0);
            return Value(yamlStringify(args[0], 0));
        }));

    globals->define("yaml", Value(yamlMap));

    // ── net namespace (TCP sockets) ──

    auto netMap = std::make_shared<PraiaMap>();

    netMap->entries["connect"] = Value(makeNative("net.connect", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("net.connect() requires a host string", 0);
            if (!args[1].isNumber())
                throw RuntimeError("net.connect() requires a port number", 0);
            std::string host = args[0].asString();
            int port = static_cast<int>(args[1].asNumber());

            struct addrinfo hints = {}, *res;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
                throw RuntimeError("Cannot resolve host: " + host, 0);
            int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (sock < 0 || connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
                if (sock >= 0) close(sock);
                freeaddrinfo(res);
                throw RuntimeError("Cannot connect to " + host + ":" + std::to_string(port), 0);
            }
            freeaddrinfo(res);
            return Value(static_cast<double>(sock));
        }));

    netMap->entries["listen"] = Value(makeNative("net.listen", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.listen() requires a port number", 0);
            int port = static_cast<int>(args[0].asNumber());

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                throw RuntimeError("Cannot create socket", 0);
            int opt = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                close(fd);
                throw RuntimeError("Cannot bind to port " + std::to_string(port), 0);
            }
            if (::listen(fd, 64) < 0) {
                close(fd);
                throw RuntimeError("Cannot listen on port " + std::to_string(port), 0);
            }
            return Value(static_cast<double>(fd));
        }));

    netMap->entries["accept"] = Value(makeNative("net.accept", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.accept() requires a socket", 0);
            int fd = static_cast<int>(args[0].asNumber());
            struct sockaddr_in ca;
            socklen_t cl = sizeof(ca);
            int client = accept(fd, (struct sockaddr*)&ca, &cl);
            if (client < 0)
                throw RuntimeError("Accept failed", 0);
            return Value(static_cast<double>(client));
        }));

    netMap->entries["send"] = Value(makeNative("net.send", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.send() requires a socket", 0);
            if (!args[1].isString())
                throw RuntimeError("net.send() requires a string", 0);
            int fd = static_cast<int>(args[0].asNumber());
            auto& data = args[1].asString();
            ssize_t sent = ::send(fd, data.c_str(), data.size(), 0);
            if (sent < 0)
                throw RuntimeError("Send failed", 0);
            return Value(static_cast<double>(sent));
        }));

    netMap->entries["recv"] = Value(makeNative("net.recv", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("net.recv() requires a socket", 0);
            int fd = static_cast<int>(args[0].asNumber());
            int maxBytes = 4096;
            if (args.size() > 1 && args[1].isNumber())
                maxBytes = static_cast<int>(args[1].asNumber());

            std::vector<char> buf(maxBytes);
            ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
            if (n < 0)
                throw RuntimeError("Recv failed", 0);
            if (n == 0) return Value(std::string(""));
            return Value(std::string(buf.data(), n));
        }));

    netMap->entries["recvAll"] = Value(makeNative("net.recvAll", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.recvAll() requires a socket", 0);
            int fd = static_cast<int>(args[0].asNumber());
            std::string data;
            char buf[4096];
            ssize_t n;
            while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
                data.append(buf, n);
            return Value(std::move(data));
        }));

    netMap->entries["close"] = Value(makeNative("net.close", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.close() requires a socket", 0);
            close(static_cast<int>(args[0].asNumber()));
            return Value();
        }));

    globals->define("net", Value(netMap));

    // ── bytes namespace ──

    auto bytesMap = std::make_shared<PraiaMap>();

    // bytes.pack(format, values) — format: "u8","i8","u16be","u16le","u32be","u32le","i32be","i32le"
    bytesMap->entries["pack"] = Value(makeNative("bytes.pack", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isArray())
                throw RuntimeError("bytes.pack(format, values) requires a format string and array", 0);
            auto& fmt = args[0].asString();
            auto& vals = args[1].asArray()->elements;
            std::string result;

            // Split format by commas or just use single format for all values
            for (auto& v : vals) {
                if (!v.isNumber())
                    throw RuntimeError("bytes.pack values must be numbers", 0);
                int64_t n = static_cast<int64_t>(v.asNumber());

                if (fmt == "u8" || fmt == "i8") {
                    result += static_cast<char>(n & 0xFF);
                } else if (fmt == "u16be" || fmt == "i16be") {
                    result += static_cast<char>((n >> 8) & 0xFF);
                    result += static_cast<char>(n & 0xFF);
                } else if (fmt == "u16le" || fmt == "i16le") {
                    result += static_cast<char>(n & 0xFF);
                    result += static_cast<char>((n >> 8) & 0xFF);
                } else if (fmt == "u32be" || fmt == "i32be") {
                    result += static_cast<char>((n >> 24) & 0xFF);
                    result += static_cast<char>((n >> 16) & 0xFF);
                    result += static_cast<char>((n >> 8) & 0xFF);
                    result += static_cast<char>(n & 0xFF);
                } else if (fmt == "u32le" || fmt == "i32le") {
                    result += static_cast<char>(n & 0xFF);
                    result += static_cast<char>((n >> 8) & 0xFF);
                    result += static_cast<char>((n >> 16) & 0xFF);
                    result += static_cast<char>((n >> 24) & 0xFF);
                } else {
                    throw RuntimeError("Unknown pack format: " + fmt, 0);
                }
            }
            return Value(std::move(result));
        }));

    // bytes.unpack(format, data) — returns array of numbers
    bytesMap->entries["unpack"] = Value(makeNative("bytes.unpack", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("bytes.unpack(format, data) requires strings", 0);
            auto& fmt = args[0].asString();
            auto& data = args[1].asString();
            auto result = std::make_shared<PraiaArray>();
            size_t pos = 0;
            int size = 0;

            if (fmt == "u8" || fmt == "i8") size = 1;
            else if (fmt == "u16be" || fmt == "u16le" || fmt == "i16be" || fmt == "i16le") size = 2;
            else if (fmt == "u32be" || fmt == "u32le" || fmt == "i32be" || fmt == "i32le") size = 4;
            else throw RuntimeError("Unknown unpack format: " + fmt, 0);

            while (pos + size <= data.size()) {
                auto b = [&](size_t i) -> uint8_t { return static_cast<uint8_t>(data[pos + i]); };
                int64_t val = 0;

                if (fmt == "u8") val = b(0);
                else if (fmt == "i8") val = static_cast<int8_t>(b(0));
                else if (fmt == "u16be") val = (b(0) << 8) | b(1);
                else if (fmt == "u16le") val = b(0) | (b(1) << 8);
                else if (fmt == "i16be") val = static_cast<int16_t>((b(0) << 8) | b(1));
                else if (fmt == "i16le") val = static_cast<int16_t>(b(0) | (b(1) << 8));
                else if (fmt == "u32be") val = (static_cast<int64_t>(b(0)) << 24) | (b(1) << 16) | (b(2) << 8) | b(3);
                else if (fmt == "u32le") val = b(0) | (b(1) << 8) | (b(2) << 16) | (static_cast<int64_t>(b(3)) << 24);
                else if (fmt == "i32be") val = static_cast<int32_t>((b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3));
                else if (fmt == "i32le") val = static_cast<int32_t>(b(0) | (b(1) << 8) | (b(2) << 16) | (b(3) << 24));

                result->elements.push_back(Value(static_cast<double>(val)));
                pos += size;
            }
            return Value(result);
        }));

    globals->define("bytes", Value(bytesMap));

    // ── crypto namespace ──

    auto cryptoMap = std::make_shared<PraiaMap>();

    // MD5 implementation
    cryptoMap->entries["md5"] = Value(makeNative("crypto.md5", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("crypto.md5() requires a string", 0);
            auto& input = args[0].asString();

            // MD5 implementation (RFC 1321)
            uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
            static const uint32_t K[64] = {
                0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
                0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
                0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
                0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
                0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
                0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
                0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
                0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
            };
            static const uint32_t s[64] = {
                7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
                5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
                6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
            };

            // Padding
            std::string msg = input;
            uint64_t origLen = msg.size() * 8;
            msg += static_cast<char>(0x80);
            while (msg.size() % 64 != 56) msg += '\0';
            for (int i = 0; i < 8; i++) msg += static_cast<char>((origLen >> (i * 8)) & 0xFF);

            // Process blocks
            for (size_t offset = 0; offset < msg.size(); offset += 64) {
                uint32_t M[16];
                for (int i = 0; i < 16; i++)
                    M[i] = static_cast<uint8_t>(msg[offset+i*4]) |
                           (static_cast<uint8_t>(msg[offset+i*4+1]) << 8) |
                           (static_cast<uint8_t>(msg[offset+i*4+2]) << 16) |
                           (static_cast<uint8_t>(msg[offset+i*4+3]) << 24);

                uint32_t A = a0, B = b0, C = c0, D = d0;
                for (int i = 0; i < 64; i++) {
                    uint32_t F, g;
                    if (i < 16)      { F = (B & C) | (~B & D); g = i; }
                    else if (i < 32) { F = (D & B) | (~D & C); g = (5*i+1) % 16; }
                    else if (i < 48) { F = B ^ C ^ D;          g = (3*i+5) % 16; }
                    else             { F = C ^ (B | ~D);        g = (7*i) % 16; }
                    F += A + K[i] + M[g];
                    A = D; D = C; C = B;
                    B += (F << s[i]) | (F >> (32 - s[i]));
                }
                a0 += A; b0 += B; c0 += C; d0 += D;
            }

            // Format as hex
            char hex[33];
            auto toHex = [&](uint32_t v, int off) {
                for (int i = 0; i < 4; i++) {
                    snprintf(hex + off + i*2, 3, "%02x", (v >> (i*8)) & 0xFF);
                }
            };
            toHex(a0, 0); toHex(b0, 8); toHex(c0, 16); toHex(d0, 24);
            hex[32] = '\0';
            return Value(std::string(hex));
        }));

    // SHA-256 implementation
    cryptoMap->entries["sha256"] = Value(makeNative("crypto.sha256", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("crypto.sha256() requires a string", 0);
            auto& input = args[0].asString();

            uint32_t h[8] = {
                0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
            };
            static const uint32_t k[64] = {
                0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
                0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
                0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
                0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
                0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
                0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
                0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
                0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
            };
            auto rotr = [](uint32_t x, int n) -> uint32_t { return (x >> n) | (x << (32 - n)); };

            // Padding
            std::string msg = input;
            uint64_t origLen = msg.size() * 8;
            msg += static_cast<char>(0x80);
            while (msg.size() % 64 != 56) msg += '\0';
            for (int i = 7; i >= 0; i--) msg += static_cast<char>((origLen >> (i * 8)) & 0xFF);

            // Process blocks
            for (size_t offset = 0; offset < msg.size(); offset += 64) {
                uint32_t w[64];
                for (int i = 0; i < 16; i++)
                    w[i] = (static_cast<uint8_t>(msg[offset+i*4]) << 24) |
                           (static_cast<uint8_t>(msg[offset+i*4+1]) << 16) |
                           (static_cast<uint8_t>(msg[offset+i*4+2]) << 8) |
                            static_cast<uint8_t>(msg[offset+i*4+3]);
                for (int i = 16; i < 64; i++) {
                    uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15]>>3);
                    uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2]>>10);
                    w[i] = w[i-16] + s0 + w[i-7] + s1;
                }

                uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
                for (int i = 0; i < 64; i++) {
                    uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
                    uint32_t ch = (e & f) ^ (~e & g);
                    uint32_t t1 = hh + S1 + ch + k[i] + w[i];
                    uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
                    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    uint32_t t2 = S0 + maj;
                    hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
                }
                h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
            }

            char hex[65];
            for (int i = 0; i < 8; i++) snprintf(hex + i*8, 9, "%08x", h[i]);
            hex[64] = '\0';
            return Value(std::string(hex));
        }));

    globals->define("crypto", Value(cryptoMap));

    // ── sqlite namespace ──

#ifdef HAVE_SQLITE
    auto sqliteMap = std::make_shared<PraiaMap>();

    sqliteMap->entries["open"] = Value(makeNative("sqlite.open", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sqlite.open() requires a path string", 0);

            sqlite3* raw = nullptr;
            int rc = sqlite3_open(args[0].asString().c_str(), &raw);
            if (rc != SQLITE_OK) {
                std::string err = sqlite3_errmsg(raw);
                sqlite3_close(raw);
                throw RuntimeError("Cannot open database: " + err, 0);
            }

            // Wrap in shared_ptr for automatic cleanup
            auto db = std::make_shared<sqlite3*>(raw);

            auto dbMap = std::make_shared<PraiaMap>();

            // db.query(sql, params?) → array of maps
            dbMap->entries["query"] = Value(makeNative("query", -1,
                [db](const std::vector<Value>& args) -> Value {
                    if (args.empty() || !args[0].isString())
                        throw RuntimeError("query() requires a SQL string", 0);
                    if (!*db)
                        throw RuntimeError("Database is closed", 0);

                    sqlite3_stmt* stmt = nullptr;
                    int rc = sqlite3_prepare_v2(*db, args[0].asString().c_str(), -1, &stmt, nullptr);
                    if (rc != SQLITE_OK) {
                        std::string err = sqlite3_errmsg(*db);
                        throw RuntimeError("SQL error: " + err, 0);
                    }

                    // Bind parameters
                    if (args.size() > 1 && args[1].isArray()) {
                        auto& params = args[1].asArray()->elements;
                        for (size_t i = 0; i < params.size(); i++) {
                            int idx = static_cast<int>(i + 1);
                            auto& p = params[i];
                            if (p.isNil()) sqlite3_bind_null(stmt, idx);
                            else if (p.isBool()) sqlite3_bind_int(stmt, idx, p.asBool() ? 1 : 0);
                            else if (p.isNumber()) sqlite3_bind_double(stmt, idx, p.asNumber());
                            else if (p.isString()) sqlite3_bind_text(stmt, idx, p.asString().c_str(), -1, SQLITE_TRANSIENT);
                            else sqlite3_bind_text(stmt, idx, p.toString().c_str(), -1, SQLITE_TRANSIENT);
                        }
                    }

                    // Execute and collect rows
                    auto rows = std::make_shared<PraiaArray>();
                    int cols = sqlite3_column_count(stmt);

                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                        auto row = std::make_shared<PraiaMap>();
                        for (int c = 0; c < cols; c++) {
                            std::string name = sqlite3_column_name(stmt, c);
                            int type = sqlite3_column_type(stmt, c);
                            switch (type) {
                                case SQLITE_NULL:
                                    row->entries[name] = Value();
                                    break;
                                case SQLITE_INTEGER:
                                    row->entries[name] = Value(static_cast<double>(sqlite3_column_int64(stmt, c)));
                                    break;
                                case SQLITE_FLOAT:
                                    row->entries[name] = Value(sqlite3_column_double(stmt, c));
                                    break;
                                default:
                                    row->entries[name] = Value(std::string(
                                        reinterpret_cast<const char*>(sqlite3_column_text(stmt, c))));
                                    break;
                            }
                        }
                        rows->elements.push_back(Value(row));
                    }

                    sqlite3_finalize(stmt);
                    return Value(rows);
                }));

            // db.run(sql, params?) → {changes, lastId}
            dbMap->entries["run"] = Value(makeNative("run", -1,
                [db](const std::vector<Value>& args) -> Value {
                    if (args.empty() || !args[0].isString())
                        throw RuntimeError("run() requires a SQL string", 0);
                    if (!*db)
                        throw RuntimeError("Database is closed", 0);

                    sqlite3_stmt* stmt = nullptr;
                    int rc = sqlite3_prepare_v2(*db, args[0].asString().c_str(), -1, &stmt, nullptr);
                    if (rc != SQLITE_OK) {
                        std::string err = sqlite3_errmsg(*db);
                        throw RuntimeError("SQL error: " + err, 0);
                    }

                    // Bind parameters
                    if (args.size() > 1 && args[1].isArray()) {
                        auto& params = args[1].asArray()->elements;
                        for (size_t i = 0; i < params.size(); i++) {
                            int idx = static_cast<int>(i + 1);
                            auto& p = params[i];
                            if (p.isNil()) sqlite3_bind_null(stmt, idx);
                            else if (p.isBool()) sqlite3_bind_int(stmt, idx, p.asBool() ? 1 : 0);
                            else if (p.isNumber()) sqlite3_bind_double(stmt, idx, p.asNumber());
                            else if (p.isString()) sqlite3_bind_text(stmt, idx, p.asString().c_str(), -1, SQLITE_TRANSIENT);
                            else sqlite3_bind_text(stmt, idx, p.toString().c_str(), -1, SQLITE_TRANSIENT);
                        }
                    }

                    rc = sqlite3_step(stmt);
                    sqlite3_finalize(stmt);

                    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                        std::string err = sqlite3_errmsg(*db);
                        throw RuntimeError("SQL error: " + err, 0);
                    }

                    auto result = std::make_shared<PraiaMap>();
                    result->entries["changes"] = Value(static_cast<double>(sqlite3_changes(*db)));
                    result->entries["lastId"] = Value(static_cast<double>(sqlite3_last_insert_rowid(*db)));
                    return Value(result);
                }));

            // db.close()
            dbMap->entries["close"] = Value(makeNative("close", 0,
                [db](const std::vector<Value>&) -> Value {
                    if (*db) {
                        sqlite3_close(*db);
                        *db = nullptr;
                    }
                    return Value();
                }));

            return Value(dbMap);
        }));

    globals->define("sqlite", Value(sqliteMap));
#endif
}

void Interpreter::setArgs(const std::vector<std::string>& args) {
    auto arr = std::make_shared<PraiaArray>();
    for (auto& a : args)
        arr->elements.push_back(Value(a));
    sysMap->entries["args"] = Value(arr);
}

void Interpreter::setCurrentFile(const std::string& path) {
    currentFile = fs::absolute(path).string();
}

// ── Grain (module) loading ───────────────────────────────────

std::string Interpreter::resolveGrainPath(const std::string& path, int line) {
    // 1. Relative path (starts with ./ or ../)
    if (path.rfind("./", 0) == 0 || path.rfind("../", 0) == 0) {
        std::string base = currentFile.empty() ? fs::current_path().string()
                                                : fs::path(currentFile).parent_path().string();
        std::string resolved = (fs::path(base) / (path + ".praia")).string();
        if (fs::exists(resolved)) return fs::canonical(resolved).string();
        throw RuntimeError("Grain not found: " + path + " (looked in " + resolved + ")", line);
    }

    // 2. grains/ directory (project-level)
    if (!currentFile.empty()) {
        // Walk up from the current file to find a grains/ directory
        fs::path dir = fs::path(currentFile).parent_path();
        for (int i = 0; i < 10; i++) { // limit depth
            auto candidate = dir / "grains" / (path + ".praia");
            if (fs::exists(candidate)) return fs::canonical(candidate).string();
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    }

    // 3. grains/ relative to cwd
    {
        auto candidate = fs::current_path() / "grains" / (path + ".praia");
        if (fs::exists(candidate)) return fs::canonical(candidate).string();
    }

    // 4. ~/.praia/grains/ (global, for future package manager)
    {
        const char* home = std::getenv("HOME");
        if (home) {
            auto candidate = fs::path(home) / ".praia" / "grains" / (path + ".praia");
            if (fs::exists(candidate)) return fs::canonical(candidate).string();
        }
    }

    throw RuntimeError("Grain not found: " + path, line);
}

Value Interpreter::loadGrain(const std::string& importPath, int line) {
    std::string resolved = resolveGrainPath(importPath, line);

    // Check for duplicate import in the same file
    if (importedInCurrentFile.count(resolved))
        throw RuntimeError("Grain '" + importPath + "' is already imported in this file", line);
    importedInCurrentFile.insert(resolved);

    // Return cached grain if already loaded by another file
    auto cached = grainCache.find(resolved);
    if (cached != grainCache.end()) return cached->second;

    // Read source
    std::ifstream f(resolved);
    if (!f.is_open())
        throw RuntimeError("Cannot read grain: " + resolved, line);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    // Lex + parse
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    if (lexer.hasError())
        throw RuntimeError("Syntax error in grain: " + importPath, line);

    Parser parser(tokens);
    auto program = parser.parse();
    if (parser.hasError())
        throw RuntimeError("Parse error in grain: " + importPath, line);

    // Execute in isolated scope
    auto grainEnv = std::make_shared<Environment>(globals);
    auto prevEnv = env;
    auto prevFile = currentFile;
    auto prevImports = importedInCurrentFile;
    env = grainEnv;
    currentFile = resolved;
    importedInCurrentFile.clear();

    Value exports;
    try {
        for (const auto& stmt : program)
            execute(stmt.get());
        // If no export statement was hit, export nothing
        exports = Value(std::make_shared<PraiaMap>());
    } catch (const ExportSignal& es) {
        exports = Value(es.exports);
    } catch (...) {
        env = prevEnv;
        currentFile = prevFile;
        importedInCurrentFile = prevImports;
        throw;
    }
    env = prevEnv;
    currentFile = prevFile;
    importedInCurrentFile = prevImports;

    // Keep the AST alive and cache the result
    grainAsts.push_back(std::move(program));
    grainCache[resolved] = exports;
    return exports;
}

void Interpreter::interpret(const std::vector<StmtPtr>& program) {
    std::lock_guard<std::recursive_mutex> lock(interpMutex);
    try {
        for (const auto& stmt : program)
            execute(stmt.get());
    } catch (const ThrowSignal& t) {
        std::cerr << "[line " << t.line << "] Uncaught error: "
                  << t.value.toString() << std::endl;
    } catch (const RuntimeError& e) {
        std::cerr << "[line " << e.line << "] Runtime error: " << e.what() << std::endl;
    }
}

void Interpreter::interpretRepl(const std::vector<StmtPtr>& program) {
    std::lock_guard<std::recursive_mutex> lock(interpMutex);
    try {
        for (const auto& stmt : program) {
            if (auto* es = dynamic_cast<const ExprStmt*>(stmt.get())) {
                Value val = evaluate(es->expr.get());
                if (!val.isNil())
                    std::cout << val.toString() << "\n";
            } else {
                execute(stmt.get());
            }
        }
    } catch (const ThrowSignal& t) {
        std::cerr << "[line " << t.line << "] Uncaught error: "
                  << t.value.toString() << std::endl;
    } catch (const RuntimeError& e) {
        std::cerr << "[line " << e.line << "] Runtime error: " << e.what() << std::endl;
    }
}

void Interpreter::executeBlock(const BlockStmt* block,
                                std::shared_ptr<Environment> newEnv) {
    auto previous = env;
    env = newEnv;
    try {
        for (const auto& stmt : block->statements)
            execute(stmt.get());
    } catch (...) {
        env = previous;
        throw;
    }
    env = previous;
}

// ── Statement execution ──────────────────────────────────────

void Interpreter::execute(const Stmt* stmt) {
    if (auto* s = dynamic_cast<const ExprStmt*>(stmt)) {
        evaluate(s->expr.get());

    } else if (auto* s = dynamic_cast<const LetStmt*>(stmt)) {
        Value val;
        if (s->initializer) val = evaluate(s->initializer.get());
        env->define(s->name, std::move(val));

    } else if (auto* s = dynamic_cast<const BlockStmt*>(stmt)) {
        executeBlock(s, std::make_shared<Environment>(env));

    } else if (auto* s = dynamic_cast<const IfStmt*>(stmt)) {
        if (evaluate(s->condition.get()).isTruthy()) {
            execute(s->thenBranch.get());
        } else {
            bool handled = false;
            for (const auto& elif : s->elifBranches) {
                if (evaluate(elif.condition.get()).isTruthy()) {
                    execute(elif.body.get());
                    handled = true;
                    break;
                }
            }
            if (!handled && s->elseBranch)
                execute(s->elseBranch.get());
        }

    } else if (auto* s = dynamic_cast<const WhileStmt*>(stmt)) {
        try {
            while (evaluate(s->condition.get()).isTruthy()) {
                try { execute(s->body.get()); }
                catch (const ContinueSignal&) { /* skip to next iteration */ }
            }
        } catch (const BreakSignal&) { /* exit loop */ }

    } else if (auto* s = dynamic_cast<const ForStmt*>(stmt)) {
        Value startVal = evaluate(s->start.get());
        Value endVal   = evaluate(s->end.get());
        if (!startVal.isNumber() || !endVal.isNumber())
            throw RuntimeError("Range bounds must be numbers", s->line);

        double from = startVal.asNumber();
        double to   = endVal.asNumber();
        auto* bodyBlock = static_cast<const BlockStmt*>(s->body.get());

        try {
            for (double i = from; i < to; i++) {
                auto iterEnv = std::make_shared<Environment>(env);
                iterEnv->define(s->varName, Value(i));
                try { executeBlock(bodyBlock, iterEnv); }
                catch (const ContinueSignal&) { /* skip to next iteration */ }
            }
        } catch (const BreakSignal&) { /* exit loop */ }

    } else if (auto* s = dynamic_cast<const ForInStmt*>(stmt)) {
        Value iterable = evaluate(s->iterable.get());
        if (!iterable.isArray())
            throw RuntimeError("for-in requires an array", s->line);
        auto* bodyBlock = static_cast<const BlockStmt*>(s->body.get());

        try {
            for (const auto& elem : iterable.asArray()->elements) {
                auto iterEnv = std::make_shared<Environment>(env);
                iterEnv->define(s->varName, elem);
                try { executeBlock(bodyBlock, iterEnv); }
                catch (const ContinueSignal&) { /* skip to next iteration */ }
            }
        } catch (const BreakSignal&) { /* exit loop */ }

    } else if (auto* s = dynamic_cast<const FuncStmt*>(stmt)) {
        auto func = std::make_shared<PraiaFunction>();
        func->funcName = s->name;
        func->params = s->params;
        func->body = static_cast<const BlockStmt*>(s->body.get());
        func->closure = env;
        env->define(s->name, Value(func));

    } else if (auto* s = dynamic_cast<const ClassStmt*>(stmt)) {
        std::shared_ptr<PraiaClass> superclass;
        if (!s->superclass.empty()) {
            Value superVal = env->get(s->superclass, s->line);
            if (!superVal.isCallable())
                throw RuntimeError("Superclass must be a class", s->line);
            superclass = std::dynamic_pointer_cast<PraiaClass>(superVal.asCallable());
            if (!superclass)
                throw RuntimeError("'" + s->superclass + "' is not a class", s->line);
        }

        auto klass = std::make_shared<PraiaClass>();
        klass->className = s->name;
        klass->superclass = superclass;
        klass->closure = env;
        for (auto& m : s->methods)
            klass->methods[m.name] = &m;
        env->define(s->name, Value(std::static_pointer_cast<Callable>(klass)));

    } else if (auto* s = dynamic_cast<const ReturnStmt*>(stmt)) {
        Value val;
        if (s->value) val = evaluate(s->value.get());
        throw ReturnSignal{std::move(val)};

    } else if (dynamic_cast<const BreakStmt*>(stmt)) {
        throw BreakSignal{};

    } else if (dynamic_cast<const ContinueStmt*>(stmt)) {
        throw ContinueSignal{};

    } else if (auto* s = dynamic_cast<const ThrowStmt*>(stmt)) {
        Value val = evaluate(s->value.get());
        throw ThrowSignal{std::move(val), s->line};

    } else if (auto* s = dynamic_cast<const TryCatchStmt*>(stmt)) {
        try {
            execute(s->tryBody.get());
        } catch (const ThrowSignal& ts) {
            auto catchEnv = std::make_shared<Environment>(env);
            catchEnv->define(s->errorVar, ts.value);
            executeBlock(static_cast<const BlockStmt*>(s->catchBody.get()), catchEnv);
        } catch (const RuntimeError& re) {
            auto catchEnv = std::make_shared<Environment>(env);
            catchEnv->define(s->errorVar, Value(std::string(re.what())));
            executeBlock(static_cast<const BlockStmt*>(s->catchBody.get()), catchEnv);
        }

    } else if (auto* s = dynamic_cast<const EnsureStmt*>(stmt)) {
        Value cond = evaluate(s->condition.get());
        if (!cond.isTruthy())
            execute(s->elseBody.get());

    } else if (auto* s = dynamic_cast<const UseStmt*>(stmt)) {
        Value grain = loadGrain(s->path, s->line);
        env->define(s->alias, grain);

    } else if (auto* s = dynamic_cast<const ExportStmt*>(stmt)) {
        auto exports = std::make_shared<PraiaMap>();
        for (auto& name : s->names) {
            exports->entries[name] = env->get(name, s->line);
        }
        throw ExportSignal{exports};
    }
}

// ── Expression evaluation ────────────────────────────────────

Value Interpreter::evaluate(const Expr* expr) {
    // ── Literals ──

    if (auto* e = dynamic_cast<const NumberExpr*>(expr))
        return Value(e->value);

    if (auto* e = dynamic_cast<const StringExpr*>(expr))
        return Value(e->value);

    if (auto* e = dynamic_cast<const BoolExpr*>(expr))
        return Value(e->value);

    if (dynamic_cast<const NilExpr*>(expr))
        return Value();

    // ── Variables ──

    if (auto* e = dynamic_cast<const IdentifierExpr*>(expr))
        return env->get(e->name, e->line);

    if (auto* e = dynamic_cast<const ThisExpr*>(expr))
        return env->get("this", e->line);

    if (auto* e = dynamic_cast<const SuperExpr*>(expr)) {
        // Get the instance
        Value thisVal = env->get("this", e->line);
        if (!thisVal.isInstance())
            throw RuntimeError("'super' used outside of a method", e->line);
        auto instance = thisVal.asInstance();

        // Get the superclass from the defining class (not the instance's class)
        Value superVal = env->get("__super__", e->line);
        auto super = std::dynamic_pointer_cast<PraiaClass>(superVal.asCallable());
        if (!super)
            throw RuntimeError("Class has no superclass", e->line);

        // Look up the method on the superclass
        auto* methodDecl = super->findMethod(e->method);
        if (!methodDecl)
            throw RuntimeError("Superclass has no method '" + e->method + "'", e->line);

        // Bind it to the current instance, with the super's class as defining class
        auto bound = std::make_shared<PraiaMethod>();
        bound->methodName = e->method;
        bound->params = methodDecl->params;
        bound->decl = methodDecl;
        bound->closure = super->closure;
        bound->instance = instance;
        // Find which class actually defines this method (for correct super chaining)
        bound->definingClass = super;
        auto* check = super->methods.count(e->method) ? super.get() : nullptr;
        if (!check && super->superclass) {
            // The method is inherited further up — find the actual defining class
            auto walk = super;
            while (walk && !walk->methods.count(e->method))
                walk = walk->superclass;
            if (walk) bound->definingClass = walk;
        }
        return Value(std::static_pointer_cast<Callable>(bound));
    }

    if (auto* e = dynamic_cast<const AssignExpr*>(expr)) {
        Value val = evaluate(e->value.get());
        env->set(e->name, val, e->line);
        return val;
    }

    // ── Unary ──

    if (auto* e = dynamic_cast<const UnaryExpr*>(expr)) {
        Value operand = evaluate(e->operand.get());
        if (e->op == TokenType::MINUS) {
            if (!operand.isNumber())
                throw RuntimeError("Operand of '-' must be a number", e->line);
            return Value(-operand.asNumber());
        }
        if (e->op == TokenType::NOT)
            return Value(!operand.isTruthy());
        if (e->op == TokenType::BIT_NOT) {
            if (!operand.isNumber())
                throw RuntimeError("Operand of '~' must be a number", e->line);
            return Value(static_cast<double>(~static_cast<int64_t>(operand.asNumber())));
        }
        throw RuntimeError("Unknown unary operator", e->line);
    }

    // ── Postfix (i++, i--) ──

    if (auto* e = dynamic_cast<const PostfixExpr*>(expr)) {
        auto* ident = dynamic_cast<const IdentifierExpr*>(e->operand.get());
        if (!ident)
            throw RuntimeError("Postfix operator requires a variable", e->line);

        Value cur = env->get(ident->name, e->line);
        if (!cur.isNumber())
            throw RuntimeError("Postfix operator requires a number", e->line);

        double old = cur.asNumber();
        double next = (e->op == TokenType::INCREMENT) ? old + 1 : old - 1;
        env->set(ident->name, Value(next), e->line);
        return Value(old);
    }

    // ── Binary ──

    if (auto* e = dynamic_cast<const BinaryExpr*>(expr)) {
        if (e->op == TokenType::OR) {
            Value left = evaluate(e->left.get());
            return left.isTruthy() ? left : evaluate(e->right.get());
        }
        if (e->op == TokenType::AND) {
            Value left = evaluate(e->left.get());
            return !left.isTruthy() ? left : evaluate(e->right.get());
        }

        Value left  = evaluate(e->left.get());
        Value right = evaluate(e->right.get());

        switch (e->op) {
        case TokenType::PLUS:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() + right.asNumber());
            if (left.isString() || right.isString())
                return Value(left.toString() + right.toString());
            throw RuntimeError("Operands of '+' must be numbers or strings", e->line);
        case TokenType::MINUS:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() - right.asNumber());
            throw RuntimeError("Operands of '-' must be numbers", e->line);
        case TokenType::STAR:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() * right.asNumber());
            throw RuntimeError("Operands of '*' must be numbers", e->line);
        case TokenType::SLASH:
            if (left.isNumber() && right.isNumber()) {
                if (right.asNumber() == 0)
                    throw RuntimeError("Division by zero", e->line);
                return Value(left.asNumber() / right.asNumber());
            }
            throw RuntimeError("Operands of '/' must be numbers", e->line);
        case TokenType::PERCENT:
            if (left.isNumber() && right.isNumber()) {
                if (right.asNumber() == 0)
                    throw RuntimeError("Modulo by zero", e->line);
                return Value(std::fmod(left.asNumber(), right.asNumber()));
            }
            throw RuntimeError("Operands of '%' must be numbers", e->line);
        case TokenType::LT:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() < right.asNumber());
            throw RuntimeError("Operands of '<' must be numbers", e->line);
        case TokenType::GT:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() > right.asNumber());
            throw RuntimeError("Operands of '>' must be numbers", e->line);
        case TokenType::LTE:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() <= right.asNumber());
            throw RuntimeError("Operands of '<=' must be numbers", e->line);
        case TokenType::GTE:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() >= right.asNumber());
            throw RuntimeError("Operands of '>=' must be numbers", e->line);
        case TokenType::EQ:  return Value(left == right);
        case TokenType::NEQ: return Value(left != right);

        case TokenType::BIT_AND:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<double>(static_cast<int64_t>(left.asNumber()) & static_cast<int64_t>(right.asNumber())));
            throw RuntimeError("Operands of '&' must be numbers", e->line);
        case TokenType::BIT_OR:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<double>(static_cast<int64_t>(left.asNumber()) | static_cast<int64_t>(right.asNumber())));
            throw RuntimeError("Operands of '|' must be numbers", e->line);
        case TokenType::BIT_XOR:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<double>(static_cast<int64_t>(left.asNumber()) ^ static_cast<int64_t>(right.asNumber())));
            throw RuntimeError("Operands of '^' must be numbers", e->line);
        case TokenType::SHL:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<double>(static_cast<int64_t>(left.asNumber()) << static_cast<int64_t>(right.asNumber())));
            throw RuntimeError("Operands of '<<' must be numbers", e->line);
        case TokenType::SHR:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<double>(static_cast<int64_t>(left.asNumber()) >> static_cast<int64_t>(right.asNumber())));
            throw RuntimeError("Operands of '>>' must be numbers", e->line);

        default:
            throw RuntimeError("Unknown binary operator", e->line);
        }
    }

    // ── Function call ──

    if (auto* e = dynamic_cast<const CallExpr*>(expr)) {
        Value callee = evaluate(e->callee.get());
        if (!callee.isCallable())
            throw RuntimeError("Can only call functions", e->line);

        std::vector<Value> args;
        for (const auto& arg : e->args)
            args.push_back(evaluate(arg.get()));

        auto func = callee.asCallable();
        if (func->arity() != -1 &&
            static_cast<int>(args.size()) != func->arity()) {
            throw RuntimeError(
                "Expected " + std::to_string(func->arity()) +
                " argument(s) but got " + std::to_string(args.size()), e->line);
        }

        return func->call(*this, args);
    }

    // ── Array literal ──

    if (auto* e = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
        auto arr = std::make_shared<PraiaArray>();
        for (const auto& elem : e->elements)
            arr->elements.push_back(evaluate(elem.get()));
        return Value(arr);
    }

    // ── Map literal ──

    // ── Pipe ──

    if (auto* e = dynamic_cast<const PipeExpr*>(expr)) {
        Value leftVal = evaluate(e->left.get());

        // If right side is a call: f(x, y) → f(leftVal, x, y)
        if (auto* call = dynamic_cast<const CallExpr*>(e->right.get())) {
            Value callee = evaluate(call->callee.get());
            if (!callee.isCallable())
                throw RuntimeError("Pipe target must be a function", e->line);

            std::vector<Value> args;
            args.push_back(leftVal);
            for (const auto& arg : call->args)
                args.push_back(evaluate(arg.get()));

            auto func = callee.asCallable();
            if (func->arity() != -1 &&
                static_cast<int>(args.size()) != func->arity())
                throw RuntimeError("Expected " + std::to_string(func->arity()) +
                    " argument(s) but got " + std::to_string(args.size()), e->line);

            return func->call(*this, args);
        }

        // Right side is just a function name: f → f(leftVal)
        Value callee = evaluate(e->right.get());
        if (!callee.isCallable())
            throw RuntimeError("Pipe target must be a function", e->line);

        std::vector<Value> args = {leftVal};
        auto func = callee.asCallable();
        if (func->arity() != -1 &&
            static_cast<int>(args.size()) != func->arity())
            throw RuntimeError("Expected " + std::to_string(func->arity()) +
                " argument(s) but got " + std::to_string(args.size()), e->line);

        return func->call(*this, args);
    }

    // ── Async / Await ──

    if (auto* e = dynamic_cast<const AsyncExpr*>(expr)) {
        // The inner expression should be a function call
        auto* call = dynamic_cast<const CallExpr*>(e->expr.get());
        if (!call)
            throw RuntimeError("async requires a function call", e->line);

        // Evaluate callee and args on the current thread
        Value callee = evaluate(call->callee.get());
        if (!callee.isCallable())
            throw RuntimeError("Can only call functions", e->line);

        std::vector<Value> args;
        for (const auto& arg : call->args)
            args.push_back(evaluate(arg.get()));

        auto callable = callee.asCallable();
        int arity = callable->arity();
        if (arity != -1 && static_cast<int>(args.size()) != arity)
            throw RuntimeError("Expected " + std::to_string(arity) +
                " argument(s) but got " + std::to_string(args.size()), e->line);

        // Spawn the call in a background thread
        // Native functions (http.get, sys.exec, etc.) run without the lock = true parallelism
        // Praia functions need the lock (only one runs at a time, like Python's GIL)
        Interpreter* self = this;
        auto sharedFuture = std::async(std::launch::async,
            [callable, args, self]() -> Value {
                if (dynamic_cast<NativeFunction*>(callable.get())) {
                    return callable->call(*self, args);
                }
                std::lock_guard<std::recursive_mutex> lock(self->interpMutex);
                return callable->call(*self, args);
            }).share();

        auto fut = std::make_shared<PraiaFuture>();
        fut->future = sharedFuture;
        return Value(fut);
    }

    if (auto* e = dynamic_cast<const AwaitExpr*>(expr)) {
        Value val = evaluate(e->expr.get());
        if (!val.isFuture())
            throw RuntimeError("Can only await a future", e->line);

        auto& future = val.asFuture()->future;

        // Release the interpreter lock while waiting so other async tasks can run
        interpMutex.unlock();
        Value result;
        try {
            result = future.get();
            interpMutex.lock();
        } catch (...) {
            interpMutex.lock();
            throw;
        }
        return result;
    }

    // ── Lambda ──

    if (auto* e = dynamic_cast<const LambdaExpr*>(expr)) {
        auto lam = std::make_shared<PraiaLambda>();
        lam->params = e->params;
        lam->expr = e;
        lam->closure = env;
        return Value(std::static_pointer_cast<Callable>(lam));
    }

    // ── Map literal ──

    if (auto* e = dynamic_cast<const MapLiteralExpr*>(expr)) {
        auto map = std::make_shared<PraiaMap>();
        for (size_t i = 0; i < e->keys.size(); i++)
            map->entries[e->keys[i]] = evaluate(e->values[i].get());
        return Value(map);
    }

    // ── Index access ──

    if (auto* e = dynamic_cast<const IndexExpr*>(expr)) {
        Value obj = evaluate(e->object.get());
        Value idx = evaluate(e->index.get());
        if (obj.isArray()) {
            if (!idx.isNumber())
                throw RuntimeError("Array index must be a number", e->line);
            auto& elems = obj.asArray()->elements;
            int i = static_cast<int>(idx.asNumber());
            if (i < 0) i += static_cast<int>(elems.size());
            if (i < 0 || i >= static_cast<int>(elems.size()))
                throw RuntimeError("Array index out of bounds", e->line);
            return elems[i];
        }
        if (obj.isString()) {
            if (!idx.isNumber())
                throw RuntimeError("String index must be a number", e->line);
            auto& str = obj.asString();
            int i = static_cast<int>(idx.asNumber());
            if (i < 0) i += static_cast<int>(str.size());
            if (i < 0 || i >= static_cast<int>(str.size()))
                throw RuntimeError("String index out of bounds", e->line);
            return Value(std::string(1, str[i]));
        }
        if (obj.isMap()) {
            if (!idx.isString())
                throw RuntimeError("Map key must be a string", e->line);
            auto& entries = obj.asMap()->entries;
            auto it = entries.find(idx.asString());
            if (it == entries.end())
                throw RuntimeError("Map has no key '" + idx.asString() + "'", e->line);
            return it->second;
        }
        throw RuntimeError("Can only index into arrays, strings, and maps", e->line);
    }

    // ── Index assignment ──

    if (auto* e = dynamic_cast<const IndexAssignExpr*>(expr)) {
        Value obj = evaluate(e->object.get());
        Value idx = evaluate(e->index.get());
        Value val = evaluate(e->value.get());
        if (obj.isArray()) {
            if (!idx.isNumber())
                throw RuntimeError("Array index must be a number", e->line);
            auto& elems = obj.asArray()->elements;
            int i = static_cast<int>(idx.asNumber());
            if (i < 0) i += static_cast<int>(elems.size());
            if (i < 0 || i >= static_cast<int>(elems.size()))
                throw RuntimeError("Array index out of bounds", e->line);
            elems[i] = val;
            return val;
        }
        if (obj.isMap()) {
            if (!idx.isString())
                throw RuntimeError("Map key must be a string", e->line);
            obj.asMap()->entries[idx.asString()] = val;
            return val;
        }
        throw RuntimeError("Can only assign to array or map indices", e->line);
    }

    // ── Dot access ──

    if (auto* e = dynamic_cast<const DotExpr*>(expr)) {
        Value obj = evaluate(e->object.get());

        // Universal methods — work on any type
        if (e->field == "toString") {
            Value captured = obj;
            return Value(makeNative("toString", 0,
                [captured](const std::vector<Value>&) -> Value {
                    return Value(captured.toString());
                }));
        }
        if (e->field == "toNum") {
            Value captured = obj;
            return Value(makeNative("toNum", 0,
                [captured](const std::vector<Value>&) -> Value {
                    if (captured.isNumber()) return captured;
                    if (captured.isBool()) return Value(captured.asBool() ? 1.0 : 0.0);
                    if (captured.isString()) {
                        auto& s = captured.asString();
                        // Case-insensitive bool strings
                        std::string lower = s;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        if (lower == "true") return Value(1.0);
                        if (lower == "false") return Value(0.0);
                        // Try numeric parse
                        try {
                            size_t pos = 0;
                            double d = std::stod(s, &pos);
                            if (pos == s.size()) return Value(d);
                        } catch (...) {}
                        throw RuntimeError("Cannot convert '" + s + "' to number", 0);
                    }
                    throw RuntimeError("Cannot convert this type to number", 0);
                }));
        }

        if (obj.isInstance()) {
            auto inst = obj.asInstance();
            // Fields first
            auto fit = inst->fields.find(e->field);
            if (fit != inst->fields.end()) return fit->second;
            // Then methods
            auto* methodDecl = inst->klass->findMethod(e->field);
            if (methodDecl) {
                auto bound = std::make_shared<PraiaMethod>();
                bound->methodName = e->field;
                bound->params = methodDecl->params;
                bound->decl = methodDecl;
                bound->instance = inst;
                // Find which class in the hierarchy defines this method
                auto walk = inst->klass;
                while (walk && !walk->methods.count(e->field))
                    walk = walk->superclass;
                bound->definingClass = walk ? walk : inst->klass;
                bound->closure = bound->definingClass->closure;
                return Value(std::static_pointer_cast<Callable>(bound));
            }
            throw RuntimeError("Instance has no property '" + e->field + "'", e->line);
        }
        if (obj.isMap()) {
            auto& entries = obj.asMap()->entries;
            auto it = entries.find(e->field);
            if (it != entries.end()) return it->second;
            throw RuntimeError("Map has no field '" + e->field + "'", e->line);
        }
        if (obj.isString())
            return getStringMethod(obj.asString(), e->field, e->line);
        if (obj.isArray())
            return getArrayMethod(obj.asArray(), e->field, e->line);

        throw RuntimeError("Cannot access field '" + e->field + "' on this type", e->line);
    }

    // ── Dot assignment ──

    if (auto* e = dynamic_cast<const DotAssignExpr*>(expr)) {
        Value obj = evaluate(e->object.get());
        Value val = evaluate(e->value.get());
        if (obj.isInstance()) {
            obj.asInstance()->fields[e->field] = val;
            return val;
        }
        if (obj.isMap()) {
            obj.asMap()->entries[e->field] = val;
            return val;
        }
        throw RuntimeError("Can only set fields on instances and maps", e->line);
    }

    // ── String interpolation ──

    if (auto* e = dynamic_cast<const InterpolatedStringExpr*>(expr)) {
        std::string result;
        for (const auto& part : e->parts)
            result += evaluate(part.get()).toString();
        return Value(std::move(result));
    }

    throw RuntimeError("Unknown expression type", expr->line);
}
