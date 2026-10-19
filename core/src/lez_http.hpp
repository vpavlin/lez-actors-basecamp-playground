#pragma once
// A minimal blocking JSON-RPC 2.0 client over plain HTTP (POSIX sockets, no Qt, no curl). The
// app targets a local v0.4 sequencer (http://127.0.0.1:3040); https (a future testnet) is a
// "later". Every call has a hard timeout so an unreachable sequencer never stalls the module.
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace lez {

struct Url { std::string host, port = "80", path = "/"; };

inline std::optional<Url> parseHttpUrl(const std::string& u, std::string* err) {
    if (u.rfind("http://", 0) != 0) { if (err) *err = u.rfind("https://", 0) == 0 ? "https is not supported yet: use the sequencer's http:// address" : "the RPC URL must start with http://"; return std::nullopt; }
    std::string rest = u.substr(7);
    Url r;
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (slash != std::string::npos) r.path = rest.substr(slash);
    if (!hostport.empty() && hostport[0] == '[') {   // [ipv6]:port
        size_t rb = hostport.find(']');
        if (rb == std::string::npos) { if (err) *err = "bad IPv6 address in the URL"; return std::nullopt; }
        r.host = hostport.substr(1, rb - 1);
        if (rb + 1 < hostport.size() && hostport[rb + 1] == ':') r.port = hostport.substr(rb + 2);
    } else {
        size_t c = hostport.rfind(':');
        r.host = c == std::string::npos ? hostport : hostport.substr(0, c);
        if (c != std::string::npos) r.port = hostport.substr(c + 1);
    }
    if (r.host.empty()) { if (err) *err = "no host in the URL"; return std::nullopt; }
    return r;
}

// POST a body, return the response body. timeoutMs covers connect + send + receive.
inline std::optional<std::string> httpPost(const Url& url, const std::string& body, int timeoutMs, std::string* err) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto left = [&]() { return int(std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count())); };
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &res) != 0 || !res) { if (err) *err = "can't resolve " + url.host; return std::nullopt; }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, left()) == 1) { int e = 0; socklen_t l = sizeof e; getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l); rc = e ? -1 : 0; }
        }
        if (rc == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { if (err) *err = "the sequencer at " + url.host + ":" + url.port + " is not answering"; return std::nullopt; }
    std::string req = "POST " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" + url.port +
                      "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < req.size()) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, left()) != 1) { close(fd); if (err) *err = "timed out sending to the sequencer"; return std::nullopt; }
        ssize_t n = send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) { close(fd); if (err) *err = "send failed"; return std::nullopt; }
        sent += size_t(n);
    }
    std::string resp; char buf[16384];
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        int pr = poll(&p, 1, left());
        if (pr != 1) { close(fd); if (err) *err = "timed out waiting for the sequencer"; return std::nullopt; }
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) { if (errno == EAGAIN) continue; close(fd); if (err) *err = "receive failed"; return std::nullopt; }
        if (n == 0) break;
        resp.append(buf, size_t(n));
        // Content-Length known and complete: stop without waiting for close.
        size_t he = resp.find("\r\n\r\n");
        if (he != std::string::npos) {
            std::string head = resp.substr(0, he);
            for (auto& c : head) c = char(tolower((unsigned char)c));
            size_t cl = head.find("content-length:");
            if (cl != std::string::npos && resp.size() >= he + 4 + std::stoul(head.substr(cl + 15))) break;
        }
    }
    close(fd);
    size_t he = resp.find("\r\n\r\n");
    if (resp.rfind("HTTP/1.", 0) != 0 || he == std::string::npos) { if (err) *err = "not an HTTP response"; return std::nullopt; }
    std::string head = resp.substr(0, he), bodyOut = resp.substr(he + 4);
    std::string lower = head; for (auto& c : lower) c = char(tolower((unsigned char)c));
    if (lower.find("transfer-encoding: chunked") != std::string::npos) {   // de-chunk
        std::string out; size_t i = 0;
        while (i < bodyOut.size()) {
            size_t e = bodyOut.find("\r\n", i); if (e == std::string::npos) break;
            size_t n = std::stoul(bodyOut.substr(i, e - i), nullptr, 16); if (n == 0) break;
            out += bodyOut.substr(e + 2, n); i = e + 2 + n + 2;
        }
        bodyOut = out;
    }
    int status = std::atoi(head.c_str() + 9);
    if (status != 200) { if (err) *err = "HTTP " + std::to_string(status) + " from the sequencer"; return std::nullopt; }
    return bodyOut;
}

// One JSON-RPC call; the `result` on success. `isNull` tells a null result (e.g. an account that
// isn't on chain, or checkHealth's "healthy") apart from an error.
inline std::optional<nlohmann::json> rpcCall(const Url& url, const std::string& method, const nlohmann::json& params, std::string* err, int timeoutMs = 4000) {
    nlohmann::json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
    auto body = httpPost(url, req.dump(), timeoutMs, err);
    if (!body) return std::nullopt;
    nlohmann::json r = nlohmann::json::parse(*body, nullptr, false);
    if (r.is_discarded() || !r.is_object()) { if (err) *err = method + ": not JSON"; return std::nullopt; }
    if (r.contains("error")) { if (err) *err = method + ": " + r["error"].dump(); return std::nullopt; }
    return r.contains("result") ? r["result"] : nlohmann::json(nullptr);
}

}  // namespace lez
