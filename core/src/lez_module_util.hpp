#pragma once
// Small helpers shared by the module's source files.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <openssl/evp.h>
#include <pwd.h>
#include <unistd.h>
#include "core_ffi.h"
#include "lez_codec.hpp"

namespace lezmod {
namespace fs = std::filesystem;
using lez::json;

constexpr int kQuickOpTimeoutMs = 15000;   // a quick wallet operation on the worker

inline long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
inline std::string ok(json body = json::object()) { body["status"] = "ok"; return body.dump(); }
inline std::string fail(const std::string& error) { return json{{"status", "error"}, {"error", error}}.dump(); }
inline std::string unquote(std::string s) {   // logosctl/QML may pass a JSON-quoted string
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') { json j = json::parse(s, nullptr, false); if (j.is_string()) return j.get<std::string>(); }
    return s;
}
inline std::string homeDir() {
    if (const char* h = getenv("HOME"); h && *h) return h;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return ".";
}
inline bool writeAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false; f << data; }
    std::error_code ec; fs::rename(tmp, path, ec); return !ec;
}
inline lez::Id sha256Id(const std::string& prefix, const std::string& name) {
    lez::Id out{}; unsigned int n = 0;
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_sha256(), nullptr);
    EVP_DigestUpdate(c, prefix.data(), prefix.size());
    EVP_DigestUpdate(c, name.data(), name.size());
    EVP_DigestFinal_ex(c, out.data(), &n);
    EVP_MD_CTX_free(c);
    return out;
}
inline FfiBytes32 ffiId(const lez::Id& id) { FfiBytes32 b{}; std::copy(id.begin(), id.end(), b.data); return b; }
inline lez::Id fromFfi(const FfiBytes32& b) { lez::Id id{}; std::copy(std::begin(b.data), std::end(b.data), id.begin()); return id; }
inline std::string ffiErr(FfiOperationError e) { return "wallet error " + std::to_string(int(e)); }


}  // namespace lezmod
