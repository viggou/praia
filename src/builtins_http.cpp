#include "builtins.h"
#include "interpreter.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

namespace {

struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
};

ParsedUrl parseUrl(const std::string& url) {
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

int connectToHost(const std::string& host, int port) {
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

std::string readAll(int sock) {
    std::string data;
    char buf[8192];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0)
        data.append(buf, n);
    return data;
}

Value parseHttpResponse(const std::string& raw) {
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

} // namespace

Value doHttpRequest(const std::string& method, const std::string& url,
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

void httpServerListen(int port, std::shared_ptr<Callable> handler, Interpreter& interp) {
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
