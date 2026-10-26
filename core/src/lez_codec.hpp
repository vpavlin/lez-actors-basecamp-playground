#pragma once
// Pure helpers for LEZ v0.4 (actor model): ids, balances, actor states, messages, fees.
// No I/O and no Qt, so they are unit-tested on their own (test/codec_test.cpp).
// Ported from jz-progs/prog-tui (rpc.rs, builtin.rs, jobs.rs).
#include <array>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>

namespace lez {

using u128 = unsigned __int128;
using Bytes = std::vector<uint8_t>;
using Id = std::array<uint8_t, 32>;
using json = nlohmann::json;

// ---- hex ------------------------------------------------------------------------------------
inline std::string hex(const uint8_t* p, size_t n) {
    static const char* X = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += X[p[i] >> 4]; s += X[p[i] & 15]; }
    return s;
}
inline std::string hex(const Bytes& b) { return hex(b.data(), b.size()); }
inline std::optional<Bytes> unhex(std::string t) {
    if (t.rfind("0x", 0) == 0 || t.rfind("0X", 0) == 0) t = t.substr(2);
    if (t.size() % 2) return std::nullopt;
    Bytes out; out.reserve(t.size() / 2);
    auto v = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
    for (size_t i = 0; i < t.size(); i += 2) { int a = v(t[i]), b = v(t[i + 1]); if (a < 0 || b < 0) return std::nullopt; out.push_back(uint8_t(a * 16 + b)); }
    return out;
}

// ---- base58 (Bitcoin alphabet; LEZ ids on the wire) ------------------------------------------
inline const char* B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
inline std::string b58(const uint8_t* p, size_t n) {
    size_t zeros = 0; while (zeros < n && p[zeros] == 0) zeros++;
    std::vector<uint8_t> digits;   // base-58 digits, little-endian
    for (size_t i = zeros; i < n; i++) {
        int carry = p[i];
        for (auto& d : digits) { carry += d * 256; d = uint8_t(carry % 58); carry /= 58; }
        while (carry) { digits.push_back(uint8_t(carry % 58)); carry /= 58; }
    }
    std::string s(zeros, '1');
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) s += B58[*it];
    return s;
}
inline std::string b58(const Id& id) { return b58(id.data(), id.size()); }
inline std::optional<Bytes> unb58(const std::string& s) {
    size_t zeros = 0; while (zeros < s.size() && s[zeros] == '1') zeros++;
    std::vector<uint8_t> bytes;    // little-endian
    for (size_t i = zeros; i < s.size(); i++) {
        const char* q = std::strchr(B58, s[i]);
        if (!q || !*q) return std::nullopt;
        int carry = int(q - B58);
        for (auto& b : bytes) { carry += b * 58; b = uint8_t(carry & 0xff); carry >>= 8; }
        while (carry) { bytes.push_back(uint8_t(carry & 0xff)); carry >>= 8; }
    }
    Bytes out(zeros, 0);
    out.insert(out.end(), bytes.rbegin(), bytes.rend());
    return out;
}

// An account or program id as a person types it: base58, "Public/<base58>", or 64 hex chars.
inline std::optional<Id> parseId(std::string t) {
    while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
    while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
    if (t.rfind("Public/", 0) == 0) t = t.substr(7);
    std::optional<Bytes> b;
    std::string h = (t.rfind("0x", 0) == 0) ? t.substr(2) : t;
    if (h.size() == 64) b = unhex(h);
    if (!b) b = unb58(t);
    if (!b || b->size() != 32) return std::nullopt;
    Id id{}; std::copy(b->begin(), b->end(), id.begin());
    return id;
}

// ---- u128 -------------------------------------------------------------------------------------
inline std::string u128str(u128 v) {
    if (v == 0) return "0";
    std::string s; while (v) { s += char('0' + int(v % 10)); v /= 10; }
    return std::string(s.rbegin(), s.rend());
}
// A positive whole number, digits only (no signs, separators or decimals: amounts are raw units).
inline std::optional<u128> parseU128(const std::string& t) {
    if (t.empty() || t.size() > 39) return std::nullopt;
    u128 v = 0;
    for (char c : t) { if (c < '0' || c > '9') return std::nullopt; u128 n = v * 10 + u128(c - '0'); if (n / 10 != v) return std::nullopt; v = n; }
    return v;
}
// The native token keeps a balance as a little-endian u128.
inline u128 decodeU128le(const Bytes& b) {
    u128 v = 0;
    for (size_t i = 0; i < b.size() && i < 16; i++) v |= u128(b[i]) << (8 * i);
    return v;
}

// ---- built-in programs --------------------------------------------------------------------------
inline Id nativeTokenId() { return Id{}; }
inline Id programLoaderId() { Id id; id.fill(0xFE); return id; }
// Names of the guests in lez/programs/, at SHA-256("/LEE-BuiltinProgram/v1/AccountId" + name);
// their ids are computed in the module (it links OpenSSL through core_ffi anyway).
inline const std::vector<std::string>& builtinGuestNames() {
    static const std::vector<std::string> n = {"clock", "fee", "bridge", "bridge_lock", "cross_zone_inbox",
        "cross_zone_outbox", "ping_sender", "ping_receiver", "sequencer_stake", "wrapped_token"};
    return n;
}

// ---- account view (getAccount) -----------------------------------------------------------------
struct AccountView {
    uint64_t nonce = 0;
    std::map<std::string, Bytes> actorStates;   // program id (base58) -> state bytes
    u128 balance() const {
        auto it = actorStates.find(b58(nativeTokenId()));
        return it == actorStates.end() ? 0 : decodeU128le(it->second);
    }
};
// `getAccount` result: {"nonce": n, "data": {"actor_states": {"<program b58>": [bytes...]}}}; null
// when the account isn't on chain.
inline std::optional<AccountView> parseAccount(const json& v) {
    if (!v.is_object() || !v.contains("data") || !v["data"].is_object() || !v["data"].contains("actor_states")) return std::nullopt;
    const json& st = v["data"]["actor_states"];
    if (!st.is_object()) return std::nullopt;
    AccountView a;
    a.nonce = v.contains("nonce") && v["nonce"].is_number_unsigned() ? v["nonce"].get<uint64_t>() : 0;
    for (auto it = st.begin(); it != st.end(); ++it) {
        Bytes b;
        if (it.value().is_array()) for (const auto& x : it.value()) if (x.is_number_unsigned() && x.get<uint64_t>() < 256) b.push_back(uint8_t(x.get<uint64_t>()));
        a.actorStates[it.key()] = b;
    }
    return a;
}

// Printable UTF-8 (no control chars) -> the text; else nullopt (the caller shows hex).
inline std::optional<std::string> printable(const Bytes& b) {
    if (b.empty()) return std::string();
    size_t i = 0;
    while (i < b.size()) {
        uint8_t c = b[i]; int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > b.size()) return std::nullopt;
        for (int k = 1; k < len; k++) if ((b[i + k] & 0xC0) != 0x80) return std::nullopt;
        if (len == 1 && ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f)) return std::nullopt;
        i += len;
    }
    return std::string(b.begin(), b.end());
}

// ---- messages (borsh) ---------------------------------------------------------------------------
// A Vec<u8> in borsh: u32 little-endian length, then the bytes. hello_world's message is a
// Vec<u8> greeting, so "text" messages are this.
inline Bytes borshBytes(const std::string& s) {
    Bytes out; uint32_t n = uint32_t(s.size());
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(n >> (8 * i)));
    out.insert(out.end(), s.begin(), s.end());
    return out;
}
// native_token::Message::Transfer { to: AccountId, amount: u128 } in borsh: variant index, then
// the 32-byte id, then the u128 little-endian. The variant index is checked against the live
// chain in the smoke test (the wallet's own transfer agrees byte for byte).
inline Bytes nativeTransferMessage(uint8_t variant, const Id& to, u128 amount) {
    Bytes out; out.push_back(variant);
    out.insert(out.end(), to.begin(), to.end());
    for (int i = 0; i < 16; i++) out.push_back(uint8_t(amount >> (8 * i)));
    return out;
}

// ---- fees ----------------------------------------------------------------------------------------
// The reserve the sequencer checks the payer for at submission (lez/programs/fee/core/src/assess.rs):
// gas_limit * base_fee_exec + tx_bytes * base_fee_stor + tip; the wallet's tip is 0 and the byte
// count is estimated (a native transfer measures ~511-544 bytes).
inline constexpr uint64_t kWalletGasLimit = 2'000'000;
inline constexpr uint64_t kTxBytesEstimate = 600;
inline u128 feeReserveEstimate(uint64_t gasLimit, uint64_t baseFeeExec, uint64_t baseFeeStor) {
    return u128(gasLimit) * baseFeeExec + u128(kTxBytesEstimate) * baseFeeStor;
}

// What a transfer did, judged from both balances: the wallet says "included" even when it reverts.
enum class TransferOutcome { Moved, Reverted, Unchanged };
inline TransferOutcome transferOutcome(u128 senderBefore, u128 senderAfter, u128 recipientBefore, u128 recipientAfter, u128 amount) {
    u128 spent = senderBefore > senderAfter ? senderBefore - senderAfter : 0;
    if (recipientAfter == recipientBefore + amount) return TransferOutcome::Moved;
    return spent > 0 ? TransferOutcome::Reverted : TransferOutcome::Unchanged;
}

// actor_conversation's acv_hop_ event data (Borsh): actor [32] ++ peer [32] ++ kind u8 ++
// text (u32-LE length ++ UTF-8). Kinds: 0 SyncRequest, 1 SyncReply, 2 AsyncSent, 3 AsyncReceived.
struct Hop { Id actor{}, peer{}; uint8_t kind = 0; std::string text; };
inline const char* hopKindName(uint8_t k) {
    switch (k) { case 0: return "sync request"; case 1: return "sync reply"; case 2: return "async sent"; case 3: return "async received"; }
    return "unknown";
}
inline std::optional<Hop> decodeHop(const Bytes& d) {
    if (d.size() < 32 + 32 + 1 + 4) return std::nullopt;
    Hop h;
    std::copy(d.begin(), d.begin() + 32, h.actor.begin());
    std::copy(d.begin() + 32, d.begin() + 64, h.peer.begin());
    h.kind = d[64];
    if (h.kind > 3) return std::nullopt;
    uint32_t n = uint32_t(d[65]) | uint32_t(d[66]) << 8 | uint32_t(d[67]) << 16 | uint32_t(d[68]) << 24;
    if (d.size() != 69 + size_t(n)) return std::nullopt;   // exactly one Hop, nothing after it
    h.text.assign(d.begin() + 69, d.end());
    return h;
}

}  // namespace lez
