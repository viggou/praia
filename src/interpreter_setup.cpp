#include "builtins.h"
#include "interpreter.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <thread>
#ifdef HAVE_SQLITE
#include <sqlite3.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

namespace fs = std::filesystem;

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
                catch (...) {
                    throw RuntimeError("num(): cannot parse \"" + args[0].asString() +
                                       "\" as a number", 0);
                }
            }
            throw RuntimeError("num(): cannot convert " + args[0].toString() +
                               " (" + std::string(
                                   args[0].isBool()     ? "bool"
                                 : args[0].isNil()      ? "nil"
                                 : args[0].isArray()    ? "array"
                                 : args[0].isMap()      ? "map"
                                 : args[0].isInstance() ? "instance"
                                 : args[0].isCallable() ? "function"
                                                        : "unknown") + ") to a number", 0);
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
                throw RuntimeError("sys.exec(): failed to launch `" +
                                   args[0].asString() + "`", 0);
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

    // sys.input(prompt?) — read a line from stdin. Returns nil on EOF.
    sysMap->entries["input"] = Value(makeNative("sys.input", -1,
        [](const std::vector<Value>& args) -> Value {
            if (!args.empty() && args[0].isString()) {
                std::cout << args[0].asString() << std::flush;
            }
            std::string line;
            if (!std::getline(std::cin, line)) return Value(); // EOF
            return Value(std::move(line));
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

    httpMap->entries["encodeURI"] = Value(makeNative("http.encodeURI", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("http.encodeURI() requires a string", 0);
            auto& input = args[0].asString();
            std::string result;
            result.reserve(input.size() * 3);
            for (unsigned char c : input) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                    result += static_cast<char>(c);
                } else {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", c);
                    result += hex;
                }
            }
            return Value(std::move(result));
        }));

    httpMap->entries["decodeURI"] = Value(makeNative("http.decodeURI", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("http.decodeURI() requires a string", 0);
            auto& input = args[0].asString();
            std::string result;
            result.reserve(input.size());
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            for (size_t i = 0; i < input.size(); ++i) {
                if (input[i] == '%' && i + 2 < input.size()) {
                    int hi = hexVal(input[i + 1]), lo = hexVal(input[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        result += static_cast<char>((hi << 4) | lo);
                        i += 2;
                        continue;
                    }
                }
                result += input[i];
            }
            return Value(std::move(result));
        }));

    globals->define("http", Value(httpMap));

    // ── json namespace ──

    auto jsonMap = std::make_shared<PraiaMap>();

    jsonMap->entries["parse"] = Value(makeNative("json.parse", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("json.parse() requires a string", 0);
            return jsonParse(args[0].asString());
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
            return yamlParse(args[0].asString());
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

    // ── random namespace ──

    auto randomMap = std::make_shared<PraiaMap>();
    auto rng = std::make_shared<std::mt19937>(std::random_device{}());

    randomMap->entries["int"] = Value(makeNative("random.int", 2,
        [rng](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("random.int() requires two numbers", 0);
            int lo = static_cast<int>(args[0].asNumber());
            int hi = static_cast<int>(args[1].asNumber());
            std::uniform_int_distribution<int> dist(lo, hi);
            return Value(static_cast<double>(dist(*rng)));
        }));

    randomMap->entries["float"] = Value(makeNative("random.float", 0,
        [rng](const std::vector<Value>&) -> Value {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return Value(dist(*rng));
        }));

    randomMap->entries["choice"] = Value(makeNative("random.choice", 1,
        [rng](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("random.choice() requires an array", 0);
            auto& elems = args[0].asArray()->elements;
            if (elems.empty())
                throw RuntimeError("random.choice() on empty array", 0);
            std::uniform_int_distribution<size_t> dist(0, elems.size() - 1);
            return elems[dist(*rng)];
        }));

    randomMap->entries["shuffle"] = Value(makeNative("random.shuffle", 1,
        [rng](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("random.shuffle() requires an array", 0);
            auto& elems = args[0].asArray()->elements;
            std::shuffle(elems.begin(), elems.end(), *rng);
            return args[0];
        }));

    randomMap->entries["seed"] = Value(makeNative("random.seed", 1,
        [rng](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("random.seed() requires a number", 0);
            rng->seed(static_cast<unsigned>(args[0].asNumber()));
            return Value();
        }));

    globals->define("random", Value(randomMap));

    // ── time namespace ──

    auto timeMap = std::make_shared<PraiaMap>();

    timeMap->entries["now"] = Value(makeNative("time.now", 0,
        [](const std::vector<Value>&) -> Value {
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            return Value(static_cast<double>(ms));
        }));

    timeMap->entries["sleep"] = Value(makeNative("time.sleep", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("time.sleep() requires milliseconds", 0);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(args[0].asNumber())));
            return Value();
        }));

    timeMap->entries["format"] = Value(makeNative("time.format", -1,
        [](const std::vector<Value>& args) -> Value {
            std::string fmt = "%Y-%m-%d %H:%M:%S";
            double timestamp = 0;

            if (!args.empty() && args[0].isString())
                fmt = args[0].asString();
            if (args.size() > 1 && args[1].isNumber())
                timestamp = args[1].asNumber();

            std::time_t t;
            if (timestamp > 0) {
                t = static_cast<std::time_t>(timestamp / 1000.0);
            } else {
                t = std::time(nullptr);
            }
            std::tm tm = *std::localtime(&t);
            std::ostringstream oss;
            oss << std::put_time(&tm, fmt.c_str());
            return Value(oss.str());
        }));

    timeMap->entries["epoch"] = Value(makeNative("time.epoch", 0,
        [](const std::vector<Value>&) -> Value {
            return Value(static_cast<double>(std::time(nullptr)));
        }));

    globals->define("time", Value(timeMap));

    // ── math namespace (built-in, replaces grains/math.praia for C++ math) ──

    auto mathMap = std::make_shared<PraiaMap>();

    mathMap->entries["PI"] = Value(3.14159265358979323846);
    mathMap->entries["E"] = Value(2.71828182845904523536);
    mathMap->entries["INF"] = Value(std::numeric_limits<double>::infinity());

    auto mathFn1 = [&](const std::string& name, double(*fn)(double)) {
        mathMap->entries[name] = Value(makeNative("math." + name, 1,
            [fn](const std::vector<Value>& args) -> Value {
                if (!args[0].isNumber())
                    throw RuntimeError("math function requires a number", 0);
                return Value(fn(args[0].asNumber()));
            }));
    };

    mathFn1("sqrt", std::sqrt);
    mathFn1("sin", std::sin);
    mathFn1("cos", std::cos);
    mathFn1("tan", std::tan);
    mathFn1("asin", std::asin);
    mathFn1("acos", std::acos);
    mathFn1("atan", std::atan);
    mathFn1("floor", std::floor);
    mathFn1("ceil", std::ceil);
    mathFn1("round", std::round);
    mathFn1("abs", std::fabs);
    mathFn1("log", std::log);
    mathFn1("log2", std::log2);
    mathFn1("log10", std::log10);
    mathFn1("exp", std::exp);

    mathMap->entries["pow"] = Value(makeNative("math.pow", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.pow() requires two numbers", 0);
            return Value(std::pow(args[0].asNumber(), args[1].asNumber()));
        }));

    mathMap->entries["min"] = Value(makeNative("math.min", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.min() requires two numbers", 0);
            return Value(std::fmin(args[0].asNumber(), args[1].asNumber()));
        }));

    mathMap->entries["max"] = Value(makeNative("math.max", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("math.max() requires two numbers", 0);
            return Value(std::fmax(args[0].asNumber(), args[1].asNumber()));
        }));

    mathMap->entries["clamp"] = Value(makeNative("math.clamp", 3,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber() || !args[2].isNumber())
                throw RuntimeError("math.clamp() requires three numbers", 0);
            double x = args[0].asNumber(), lo = args[1].asNumber(), hi = args[2].asNumber();
            return Value(std::fmax(lo, std::fmin(x, hi)));
        }));

    globals->define("math", Value(mathMap));

    // ── OS extras on sys ──

    sysMap->entries["env"] = Value(makeNative("sys.env", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("sys.env() requires a string", 0);
            const char* val = std::getenv(args[0].asString().c_str());
            if (!val) return Value();
            return Value(std::string(val));
        }));

    sysMap->entries["cwd"] = Value(makeNative("sys.cwd", 0,
        [](const std::vector<Value>&) -> Value {
            return Value(fs::current_path().string());
        }));

#if defined(__APPLE__)
    sysMap->entries["platform"] = Value("darwin");
#elif defined(__linux__)
    sysMap->entries["platform"] = Value("linux");
#elif defined(_WIN32)
    sysMap->entries["platform"] = Value("windows");
#else
    sysMap->entries["platform"] = Value("unknown");
#endif

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
