// LezActorsModuleImpl - see the header for the shape and the rules.
#include "lez_actors_module_impl.h"
#include "lez_http.hpp"
#include "core_ffi.h"
#include <QTimer>
#include <openssl/evp.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <pwd.h>
#include <unistd.h>

namespace fs = std::filesystem;
using lez::json;

namespace {

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string ok(json body = json::object()) { body["status"] = "ok"; return body.dump(); }
std::string fail(const std::string& error) { return json{{"status", "error"}, {"error", error}}.dump(); }
std::string unquote(std::string s) {   // logosctl/QML may pass a JSON-quoted string
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') { json j = json::parse(s, nullptr, false); if (j.is_string()) return j.get<std::string>(); }
    return s;
}
std::string homeDir() {
    if (const char* h = getenv("HOME"); h && *h) return h;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return ".";
}
bool writeAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false; f << data; }
    std::error_code ec; fs::rename(tmp, path, ec); return !ec;
}
lez::Id sha256Id(const std::string& prefix, const std::string& name) {
    lez::Id out{}; unsigned int n = 0;
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_sha256(), nullptr);
    EVP_DigestUpdate(c, prefix.data(), prefix.size());
    EVP_DigestUpdate(c, name.data(), name.size());
    EVP_DigestFinal_ex(c, out.data(), &n);
    EVP_MD_CTX_free(c);
    return out;
}
FfiBytes32 ffiId(const lez::Id& id) { FfiBytes32 b{}; std::copy(id.begin(), id.end(), b.data); return b; }
lez::Id fromFfi(const FfiBytes32& b) { lez::Id id{}; std::copy(std::begin(b.data), std::end(b.data), id.begin()); return id; }
std::string ffiErr(FfiOperationError e) { return "wallet error " + std::to_string(int(e)); }

constexpr uint8_t kNativeTransferVariant = 0;      // native_token::Message::Transfer
constexpr int kQuickOpTimeoutMs = 15000;
constexpr long long kCreditWaitMs = 60000;          // async credits land a block or two later

}  // namespace

// =============================================================================================
// lifecycle
// =============================================================================================
LezActorsModuleImpl::~LezActorsModuleImpl() {
    *m_alive = false;
    m_stop = true;
    m_qCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    if (m_wallet) { wallet_ffi_save(m_wallet); wallet_ffi_destroy(m_wallet); m_wallet = nullptr; }
    if (m_loop) { m_loop->stop(); delete m_loop; m_loop = nullptr; }
}

void LezActorsModuleImpl::onContextReady() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    setupDataDir();
    loadSettings();
    if (const char* u = getenv("LEZ_RPC_URL"); u && *u) m_rpcUrl = u;
    writeWalletConfig();
    // the built-ins: the engine's two fixed ids and the guests at SHA-256(prefix + name)
    std::vector<Program> builtins = {{"native token", lez::nativeTokenId(), true}, {"program_loader", lez::programLoaderId(), true}};
    for (const auto& n : lez::builtinGuestNames()) builtins.push_back({n, sha256Id("/LEE-BuiltinProgram/v1/AccountId", n), true});
    std::vector<Program> user;
    for (auto& p : m_programs) if (!p.builtin) user.push_back(p);
    m_programs = builtins;
    m_programs.insert(m_programs.end(), user.begin(), user.end());
    m_loop = new QTimer();   // a QObject on the module's thread, for onLoop()
    m_worker = std::thread([this] { workerLoop(); });
    fprintf(stderr, "[lez_actors] ready: data %s, rpc %s\n", m_dataDir.c_str(), m_rpcUrl.c_str());
}

void LezActorsModuleImpl::onLoop(std::function<void()> fn) {
    if (!m_loop) return;
    QMetaObject::invokeMethod(m_loop, std::move(fn), Qt::QueuedConnection);
}

void LezActorsModuleImpl::publish() {
    std::string s;
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); s = buildSnapshot().dump(); }
    emit stateChanged(s);
}

// =============================================================================================
// persistence
// =============================================================================================
void LezActorsModuleImpl::setupDataDir() {
    const char* ov = getenv("LEZ_ACTORS_DATA");
    m_dataDir = ov && *ov ? ov : homeDir() + "/.lez-actors";
    std::error_code ec;
    fs::create_directories(m_dataDir + "/wallet", ec);
}
std::string LezActorsModuleImpl::walletConfigPath() const { return m_dataDir + "/wallet/wallet_config.json"; }
std::string LezActorsModuleImpl::walletStoragePath() const { return m_dataDir + "/wallet/storage.json"; }

void LezActorsModuleImpl::loadSettings() {
    std::ifstream f(m_dataDir + "/settings.json");
    if (!f) return;
    json s = json::parse(f, nullptr, false);
    if (!s.is_object()) return;
    m_rpcUrl = s.value("rpcUrl", m_rpcUrl);
    m_payer = s.value("payer", "");
    m_selected = s.value("selected", "");
    if (s.contains("programs") && s["programs"].is_array())
        for (const auto& p : s["programs"]) {
            auto id = lez::parseId(p.value("id", ""));
            if (id) m_programs.push_back({p.value("name", "program"), *id, false});
        }
}
void LezActorsModuleImpl::saveSettings() {
    json progs = json::array();
    for (const auto& p : m_programs) if (!p.builtin) progs.push_back({{"name", p.name}, {"id", lez::b58(p.id)}});
    writeAtomic(m_dataDir + "/settings.json", json{{"rpcUrl", m_rpcUrl}, {"payer", m_payer}, {"selected", m_selected}, {"programs", progs}}.dump(1));
}
void LezActorsModuleImpl::writeWalletConfig() {
    // the wallet library's own config: where its sequencer is, and how long it polls
    json c = {{"sequencer", {{"sequencer_addr", m_rpcUrl}}}, {"seq_poll_timeout", "30s"}, {"seq_tx_poll_max_blocks", 15},
              {"seq_poll_max_retries", 10}, {"seq_block_poll_max_amount", 100}};
    writeAtomic(walletConfigPath(), c.dump(4));
}

// =============================================================================================
// the worker thread: every wallet-library call happens here, one at a time
// =============================================================================================
void LezActorsModuleImpl::workerLoop() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lk(m_qMtx);
            m_qCv.wait(lk, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop) return;
            fn = std::move(m_queue.front()); m_queue.pop_front();
        }
        try { fn(); } catch (const std::exception& e) { fprintf(stderr, "[lez_actors] worker: %s\n", e.what()); } catch (...) {}
    }
}
void LezActorsModuleImpl::enqueue(std::function<void()> fn) {
    { std::lock_guard<std::mutex> lk(m_qMtx); m_queue.push_back(std::move(fn)); }
    m_qCv.notify_one();
}
// A quick wallet operation: run it on the worker and wait (bounded). If a long job holds the
// worker, say so instead of blocking the caller past the IPC timeout.
std::string LezActorsModuleImpl::runOnWorker(std::function<json()> fn, int timeoutMs) {
    auto p = std::make_shared<std::promise<json>>();
    auto fut = p->get_future();
    enqueue([fn = std::move(fn), p] { try { p->set_value(fn()); } catch (const std::exception& e) { p->set_value(json{{"status", "error"}, {"error", e.what()}}); } });
    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
        return fail("busy: a transaction is still being sent; try again in a moment");
    json r = fut.get();
    if (!r.contains("status")) r["status"] = "ok";
    return r.dump();
}

// =============================================================================================
// state
// =============================================================================================
std::string LezActorsModuleImpl::snapshot() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    return ok(buildSnapshot());
}
json LezActorsModuleImpl::buildSnapshot() {
    json jobs = json::array();
    for (auto it = m_jobOrder.rbegin(); it != m_jobOrder.rend() && jobs.size() < 20; ++it) {
        const Job& j = m_jobs[*it];
        jobs.push_back({{"jobId", j.id}, {"kind", j.kind}, {"state", j.state}, {"error", j.error}, {"txHash", j.txHash}, {"request", j.request}, {"result", j.result}, {"createdMs", j.createdMs}, {"finishedMs", j.finishedMs}});
    }
    bool exists = fs::exists(walletStoragePath());
    return {{"rpcUrl", m_rpcUrl}, {"payer", m_payer}, {"selected", m_selected}, {"wallet", {{"exists", exists}, {"open", m_wallet != nullptr}, {"accounts", m_accounts}}}, {"jobs", jobs}, {"dataDir", m_dataDir}};
}

std::string LezActorsModuleImpl::setEndpoint(std::string rpcUrl) {
    rpcUrl = unquote(rpcUrl);
    std::string err;
    if (!lez::parseHttpUrl(rpcUrl, &err)) return fail(err);
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        m_rpcUrl = rpcUrl;
        saveSettings();
        writeWalletConfig();
    }
    // the wallet library reads its sequencer from the config at open: reopen on the next unlock
    publish();
    return ok({{"rpcUrl", rpcUrl}, {"note", "unlock the wallet again for it to use the new address"}});
}

std::string LezActorsModuleImpl::health() {
    std::string url; { std::lock_guard<std::recursive_mutex> lk(m_mtx); url = m_rpcUrl; }
    std::string err;
    auto u = lez::parseHttpUrl(url, &err);
    if (!u) return fail(err);
    auto h = lez::rpcCall(*u, "checkHealth", json::array(), &err, 3000);
    if (!h) return json{{"status", "error"}, {"error", err}, {"reachable", false}, {"rpcUrl", url},
                        {"hint", "start a local v0.4 sequencer: RISC0_DEV_MODE=1 just run-sequencer-standalone (LEZ dev), or prog-tui"}}.dump();
    json out = {{"reachable", true}, {"rpcUrl", url}};
    if (auto b = lez::rpcCall(*u, "getLastBlockId", json::array(), &err, 3000); b && b->is_number()) out["blockHeight"] = *b;
    if (auto f = lez::rpcCall(*u, "getFeeState", json::array(), &err, 3000); f && f->is_object()) {
        out["baseFeeExec"] = f->value("base_fee_exec", 0); out["baseFeeStor"] = f->value("base_fee_stor", 0);
    }
    return ok(out);
}

std::string LezActorsModuleImpl::feeReserveEstimate() {
    json h = json::parse(health(), nullptr, false);
    if (!h.is_object() || h.value("status", "") != "ok") return fail(h.is_object() ? h.value("error", "sequencer unreachable") : "sequencer unreachable");
    uint64_t e = h.value("baseFeeExec", 0), s = h.value("baseFeeStor", 0);
    return ok({{"reserve", lez::u128str(lez::feeReserveEstimate(lez::kWalletGasLimit, e, s))}, {"gasLimit", lez::kWalletGasLimit}, {"baseFeeExec", e}, {"baseFeeStor", s},
               {"note", "gas_limit x base_fee_exec + ~600 bytes x base_fee_stor; tip 0"}});
}

// =============================================================================================
// wallet
// =============================================================================================
std::string LezActorsModuleImpl::walletStatus() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    bool exists = fs::exists(walletStoragePath());
    bool enc = false;
    if (exists) wallet_ffi_storage_is_encrypted(walletStoragePath().c_str(), &enc);
    return ok({{"exists", exists}, {"open", m_wallet != nullptr}, {"encrypted", enc}, {"accounts", m_accounts}, {"payer", m_payer}});
}

std::string LezActorsModuleImpl::createWallet(std::string password) {
    password = unquote(password);
    if (fs::exists(walletStoragePath())) return fail("a wallet already exists here; unlock it instead");
    std::string r = runOnWorker([this, password]() -> json {
        std::string cfg = walletConfigPath(), sto = walletStoragePath();
        FfiCreateWalletOutput out = wallet_ffi_create_new(cfg.c_str(), sto.c_str(), password.c_str());
        if (!out.wallet) return {{"status", "error"}, {"error", "the wallet library could not create a wallet"}};
        std::string mnemonic = out.mnemonic ? out.mnemonic : "";
        if (out.mnemonic) primitives_ffi_free_cstring(out.mnemonic);
        wallet_ffi_save(out.wallet);
        { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_wallet = out.wallet; m_accounts.clear(); }
        return {{"mnemonic", mnemonic}, {"note", "write the recovery phrase down; it is shown once"}};
    }, kQuickOpTimeoutMs);
    publish();
    return r;
}

std::string LezActorsModuleImpl::unlockWallet(std::string password) {
    password = unquote(password);
    if (!fs::exists(walletStoragePath())) return fail("no wallet yet; create one");
    std::string r = runOnWorker([this, password]() -> json {
        { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (m_wallet) { wallet_ffi_save(m_wallet); wallet_ffi_destroy(m_wallet); m_wallet = nullptr; } }
        WalletHandle* h = nullptr;
        std::string cfg = walletConfigPath(), sto = walletStoragePath();
        FfiOperationError e = wallet_ffi_open(cfg.c_str(), sto.c_str(), password.c_str(), &h);
        if (e != FfiOperationError_Success || !h) return {{"status", "error"}, {"error", e == FfiOperationError_WrongPassword ? "wrong password" : ffiErr(e)}};
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        m_wallet = h;
        return json::object();
    }, kQuickOpTimeoutMs);
    listAccounts();
    publish();
    return r;
}

std::string LezActorsModuleImpl::importKey(std::string privateKeyHex) {
    privateKeyHex = unquote(privateKeyHex);
    auto b = lez::unhex(privateKeyHex);
    if (!b || b->size() != 32) return fail("a private key is 64 hex characters");
    std::string r = runOnWorker([this, privateKeyHex]() -> json {
        if (!m_wallet) return {{"status", "error"}, {"error", "unlock the wallet first"}};
        std::string k = privateKeyHex.rfind("0x", 0) == 0 ? privateKeyHex.substr(2) : privateKeyHex;
        FfiOperationError e = wallet_ffi_import_public_account(m_wallet, k.c_str());
        if (e != FfiOperationError_Success) return {{"status", "error"}, {"error", ffiErr(e)}};
        wallet_ffi_save(m_wallet);
        return json::object();
    }, kQuickOpTimeoutMs);
    // a new wallet's first account is empty and is the payer by default: when the payer can't
    // pay and an imported account can, the funded one pays from now on
    json acc = json::parse(listAccounts(), nullptr, false);
    json out = json::parse(r, nullptr, false);
    if (acc.is_object() && out.is_object() && out.value("status", "") == "ok") {
        std::string payerB58; { std::lock_guard<std::recursive_mutex> lk(m_mtx); payerB58 = m_payer; }
        std::string best, payerBal = "0";
        lez::u128 bestBal = 0;
        for (const auto& a : acc.value("accounts", json::array())) {
            std::string id = a.value("accountId", ""), bal = a.value("balance", "0");
            if (id == payerB58) payerBal = bal;
            auto v = lez::parseU128(bal);
            if (a.value("onChain", false) && v && *v > bestBal) { bestBal = *v; best = id; }
        }
        auto pv = lez::parseU128(payerBal);
        if (!best.empty() && best != payerB58 && (!pv || *pv == 0)) {
            { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_payer = best; saveSettings(); }
            out["payerChanged"] = best;
            r = out.dump();
        }
    }
    publish();
    return r;
}

std::string LezActorsModuleImpl::newAccount() {
    std::string r = runOnWorker([this]() -> json {
        if (!m_wallet) return {{"status", "error"}, {"error", "unlock the wallet first"}};
        FfiBytes32 out{};
        FfiOperationError e = wallet_ffi_create_account_public(m_wallet, &out);
        if (e != FfiOperationError_Success) return {{"status", "error"}, {"error", ffiErr(e)}};
        wallet_ffi_save(m_wallet);
        return {{"accountId", lez::b58(fromFfi(out))}, {"note", "a new account is off-chain until something writes to it"}};
    }, kQuickOpTimeoutMs);
    listAccounts();
    publish();
    return r;
}

std::string LezActorsModuleImpl::listAccounts() {
    std::string r = runOnWorker([this]() -> json {
        if (!m_wallet) return {{"status", "error"}, {"error", "unlock the wallet first"}};
        FfiAccountList list{};
        FfiOperationError e = wallet_ffi_list_accounts(m_wallet, &list);
        if (e != FfiOperationError_Success) return {{"status", "error"}, {"error", ffiErr(e)}};
        std::vector<std::string> ids;
        for (size_t i = 0; i < list.count; i++) if (list.entries[i].is_public) ids.push_back(lez::b58(fromFfi(list.entries[i].account_id)));
        wallet_ffi_free_account_list(&list);
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        m_accounts = ids;
        if (m_payer.empty() && !ids.empty()) m_payer = ids.front();
        return {{"accounts", ids}};
    }, kQuickOpTimeoutMs);
    json j = json::parse(r, nullptr, false);
    if (j.is_object() && j.value("status", "") == "ok" && j.contains("accounts")) {
        // with balances from the chain (RPC, not the wallet)
        json rows = json::array();
        for (const auto& a : j["accounts"]) {
            auto id = lez::parseId(a.get<std::string>());
            std::string err; bool onChain = false;
            auto v = id ? fetchAccount(*id, &err, &onChain) : std::nullopt;
            rows.push_back({{"accountId", a}, {"onChain", onChain}, {"balance", v ? lez::u128str(v->balance()) : "0"}, {"error", err}});
        }
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        return ok({{"accounts", rows}, {"payer", m_payer}, {"selected", m_selected}});
    }
    return r;
}

std::string LezActorsModuleImpl::setPayer(std::string accountId) {
    auto id = lez::parseId(unquote(accountId));
    if (!id) return fail("not an account id");
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    std::string b = lez::b58(*id);
    if (std::find(m_accounts.begin(), m_accounts.end(), b) == m_accounts.end()) return fail("the payer must be an account in this wallet (it signs and pays the fee)");
    m_payer = b; saveSettings();
    onLoop([this] { publish(); });
    return ok({{"payer", b}});
}
std::string LezActorsModuleImpl::setSelected(std::string accountId) {
    auto id = lez::parseId(unquote(accountId));
    if (!id) return fail("not an account id");
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    m_selected = lez::b58(*id); saveSettings();
    onLoop([this] { publish(); });
    return ok({{"selected", m_selected}});
}
bool LezActorsModuleImpl::walletHolds(const lez::Id& id) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    std::string b = lez::b58(id);
    return std::find(m_accounts.begin(), m_accounts.end(), b) != m_accounts.end();
}

// =============================================================================================
// chain reads
// =============================================================================================
std::optional<lez::AccountView> LezActorsModuleImpl::fetchAccount(const lez::Id& id, std::string* err, bool* onChain) {
    std::string url; { std::lock_guard<std::recursive_mutex> lk(m_mtx); url = m_rpcUrl; }
    auto u = lez::parseHttpUrl(url, err);
    if (onChain) *onChain = false;
    if (!u) return std::nullopt;
    auto r = lez::rpcCall(*u, "getAccount", json::array({lez::b58(id)}), err, 4000);
    if (!r) return std::nullopt;
    if (r->is_null()) return lez::AccountView{};   // not on chain: an empty view
    auto v = lez::parseAccount(*r);
    if (!v) { if (err) *err = "unexpected getAccount result"; return std::nullopt; }
    if (onChain) *onChain = true;
    return v;
}

std::string LezActorsModuleImpl::programName(const std::string& programB58) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    for (const auto& p : m_programs) if (lez::b58(p.id) == programB58) return p.name;
    return "";
}

json LezActorsModuleImpl::describeAccount(const lez::Id& id, const std::optional<lez::AccountView>& v, bool onChain) {
    json states = json::array();
    if (v) for (const auto& [prog, bytes] : v->actorStates) {
        json s = {{"programId", prog}, {"hex", lez::hex(bytes)}, {"size", bytes.size()}};
        if (auto n = programName(prog); !n.empty()) s["programName"] = n;
        if (prog == lez::b58(lez::nativeTokenId())) s["balance"] = lez::u128str(lez::decodeU128le(bytes));
        else if (auto t = lez::printable(bytes)) s["text"] = *t;
        states.push_back(s);
    }
    return {{"accountId", lez::b58(id)}, {"accountIdHex", lez::hex(id.data(), id.size())}, {"onChain", onChain},
            {"nonce", v ? v->nonce : 0}, {"balance", v ? lez::u128str(v->balance()) : "0"}, {"actorStates", states}, {"inWallet", walletHolds(id)}};
}

std::string LezActorsModuleImpl::getAccount(std::string accountId) {
    auto id = lez::parseId(unquote(accountId));
    if (!id) return fail("not an account id (base58, Public/<id>, or 64 hex characters)");
    std::string err; bool onChain = false;
    auto v = fetchAccount(*id, &err, &onChain);
    if (!v) return fail(err);
    return ok(describeAccount(*id, v, onChain));
}

std::string LezActorsModuleImpl::resolveProgram(std::string headerId) {
    auto id = lez::parseId(unquote(headerId));
    if (!id) return fail("not a program id");
    // a deployed program is a header account written by the program loader
    std::string err; bool onChain = false;
    auto v = fetchAccount(*id, &err, &onChain);
    if (!v) return fail(err);
    bool deployed = onChain && v->actorStates.count(lez::b58(lez::programLoaderId())) > 0;
    bool builtin = false;
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); for (const auto& p : m_programs) if (p.builtin && p.id == *id) builtin = true; }
    return ok({{"programId", lez::b58(*id)}, {"exists", deployed || builtin}, {"deployed", deployed}, {"builtin", builtin}});
}

// =============================================================================================
// programs
// =============================================================================================
std::string LezActorsModuleImpl::listKnownPrograms() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    json arr = json::array();
    for (const auto& p : m_programs) arr.push_back({{"name", p.name}, {"programId", lez::b58(p.id)}, {"programIdHex", lez::hex(p.id.data(), p.id.size())}, {"builtin", p.builtin}});
    return ok({{"programs", arr}});
}
std::string LezActorsModuleImpl::addProgram(std::string requestJson) {
    json r = json::parse(unquote(requestJson), nullptr, false);
    if (!r.is_object()) return fail("addProgram takes {name, programId}");
    std::string name = r.value("name", ""), idText = r.value("programId", r.value("headerIdHex", ""));
    auto id = lez::parseId(idText);
    if (name.empty() || name.size() > 64) return fail("give the program a name (up to 64 characters)");
    if (!id) return fail("not a program id");
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    for (auto& p : m_programs) if (p.id == *id) { if (p.builtin) return fail("that's a built-in program"); p.name = name; saveSettings(); onLoop([this] { publish(); }); return ok({{"programId", lez::b58(*id)}, {"renamed", true}}); }
    m_programs.push_back({name, *id, false});
    saveSettings();
    onLoop([this] { publish(); });
    return ok({{"programId", lez::b58(*id)}});
}
std::string LezActorsModuleImpl::removeProgram(std::string headerId) {
    auto id = lez::parseId(unquote(headerId));
    if (!id) return fail("not a program id");
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    auto it = std::find_if(m_programs.begin(), m_programs.end(), [&](const Program& p) { return p.id == *id && !p.builtin; });
    if (it == m_programs.end()) return fail("not a program you added");
    m_programs.erase(it); saveSettings();
    onLoop([this] { publish(); });
    return ok();
}

// =============================================================================================
// jobs: calls and transfers
// =============================================================================================
std::string LezActorsModuleImpl::startJob(const std::string& kind, const json& request) {
    std::string id;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        id = kind + "-" + std::to_string(++m_jobSeq) + "-" + std::to_string(nowMs() % 100000);
        Job j; j.id = id; j.kind = kind; j.request = request; j.createdMs = nowMs();
        m_jobs[id] = j; m_jobOrder.push_back(id);
        while (m_jobOrder.size() > 50) { m_jobs.erase(m_jobOrder.front()); m_jobOrder.erase(m_jobOrder.begin()); }
    }
    enqueue([this, id, kind, alive = m_alive] {
        if (!*alive) return;
        Job copy;
        { std::lock_guard<std::recursive_mutex> lk(m_mtx); copy = m_jobs[id]; m_jobs[id].state = "running"; }
        onLoop([this] { publish(); });
        if (kind == "call") runCall(copy); else runTransfer(copy);
        finishJob(id, [copy](Job& j) { j = copy; j.finishedMs = nowMs(); });
    });
    onLoop([this] { publish(); });
    return ok({{"jobId", id}, {"state", "queued"}});
}
void LezActorsModuleImpl::finishJob(const std::string& id, std::function<void(Job&)> update) {
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); auto it = m_jobs.find(id); if (it != m_jobs.end()) update(it->second); }
    onLoop([this] { publish(); });
}
std::string LezActorsModuleImpl::job(std::string jobId) {
    jobId = unquote(jobId);
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    auto it = m_jobs.find(jobId);
    if (it == m_jobs.end()) return fail("no such job");
    const Job& j = it->second;
    return ok({{"jobId", j.id}, {"kind", j.kind}, {"state", j.state}, {"error", j.error}, {"txHash", j.txHash}, {"request", j.request}, {"result", j.result}});
}

// Sends one public v0.4 transaction through the wallet library and waits for inclusion.
// mentions[0] is the root actor the message is delivered to. Returns the tx hash or "".
static std::string sendPublic(WalletHandle* w, std::vector<std::pair<lez::Id, std::pair<lez::Id, bool>>> actors,
                              const lez::Bytes& message, const lez::Id& payer, bool* included, std::string* err) {
    std::vector<FfiAccountMention> mentions;
    for (const auto& [account, progSign] : actors) {
        FfiAccountMention m{};
        // LEZ refuses a payer that is a non-signing account of the transaction: the payer signs
        // anyway, so a mention of the payer always authorizes
        bool sign = progSign.second || account == payer;
        FfiOperationError e = wallet_ffi_resolve_public_account(ffiId(account), sign, &m.identity);
        if (e != FfiOperationError_Success) { *err = "can't resolve account " + lez::b58(account) + ": " + ffiErr(e); for (auto& x : mentions) wallet_ffi_free_account_identity(&x.identity); return ""; }
        m.program_account_id = ffiId(progSign.first);
        m.authorizes = sign;
        m.address_openings = nullptr; m.address_openings_size = 0;
        mentions.push_back(m);
    }
    FfiBytes32 p = ffiId(payer);
    FfiTransactionResult res{};
    FfiOperationError e = wallet_ffi_send_generic_public_transaction(w, mentions.data(), mentions.size(), 0, message.data(), message.size(), &p, &res);
    for (auto& x : mentions) wallet_ffi_free_account_identity(&x.identity);
    if (e != FfiOperationError_Success || !res.tx_hash) { *err = "the wallet couldn't build or send the transaction (" + ffiErr(e) + "); the reason is in the host log, after [ffi]"; wallet_ffi_free_transaction_result(&res); return ""; }
    std::string hash = res.tx_hash;
    wallet_ffi_free_transaction_result(&res);
    auto hb = lez::unhex(hash);
    bool inc = false;
    if (hb && hb->size() == 32) { FfiBytes32 h{}; std::copy(hb->begin(), hb->end(), h.data); wallet_ffi_poll_transaction_status(w, h, &inc); }
    *included = inc;
    return hash;
}

std::string LezActorsModuleImpl::callProgram(std::string requestJson) {
    json r = json::parse(unquote(requestJson), nullptr, false);
    if (!r.is_object()) return fail("callProgram takes {programId, accountId, message, encoding, sign?}");
    auto prog = lez::parseId(r.value("programId", r.value("programIdHex", "")));
    auto acct = lez::parseId(r.value("accountId", r.value("accountIdHex", "")));
    if (!prog) return fail("not a program id");
    if (!acct) return fail("not an account id");
    std::string enc = r.value("encoding", "hex");
    if (enc != "hex" && enc != "text" && enc != "raw-text") return fail("encoding is hex, text (a borsh Vec<u8>, what hello_world takes) or raw-text");
    if (enc == "hex" && !lez::unhex(r.value("message", ""))) return fail("the message isn't hex");
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (!m_wallet) return fail("unlock the wallet first"); if (m_payer.empty()) return fail("choose a payer: an account in this wallet that pays the fee"); }
    return startJob("call", r);
}

void LezActorsModuleImpl::runCall(Job& j) {
    const json& r = j.request;
    lez::Id prog = *lez::parseId(r.value("programId", r.value("programIdHex", "")));
    lez::Id acct = *lez::parseId(r.value("accountId", r.value("accountIdHex", "")));
    std::string enc = r.value("encoding", "hex"), msg = r.value("message", "");
    lez::Bytes message = enc == "hex" ? *lez::unhex(msg) : enc == "text" ? lez::borshBytes(msg) : lez::Bytes(msg.begin(), msg.end());
    bool sign = r.value("sign", false);
    lez::Id payer; { std::lock_guard<std::recursive_mutex> lk(m_mtx); payer = *lez::parseId(m_payer); }
    if (sign && !walletHolds(acct)) { j.state = "failed"; j.error = "the account must be in this wallet to sign"; return; }

    std::string err; bool onChain = false;
    auto before = fetchAccount(acct, &err, &onChain);
    std::string pb58 = lez::b58(prog);
    lez::Bytes stateBefore = before && before->actorStates.count(pb58) ? before->actorStates[pb58] : lez::Bytes{};
    j.state = "sending";
    bool included = false;
    std::string hash = sendPublic(m_wallet, {{acct, {prog, sign}}}, message, payer, &included, &err);
    if (hash.empty()) { j.state = "failed"; j.error = err; return; }
    j.txHash = hash;
    if (!included) { j.state = "failed"; j.error = "not included within the wallet's poll window"; return; }
    // a reverted call is still included and charged: the state is the only way to tell
    lez::Bytes stateAfter = stateBefore;
    for (int i = 0; i < 6; i++) {
        bool oc = false;
        auto after = fetchAccount(acct, &err, &oc);
        if (after && after->actorStates.count(pb58)) stateAfter = after->actorStates[pb58];
        if (stateAfter != stateBefore) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    bool changed = stateAfter != stateBefore;
    json res = {{"included", true}, {"stateBefore", lez::hex(stateBefore)}, {"stateAfter", lez::hex(stateAfter)}, {"reverted", !changed}};
    if (!sign && acct == payer) res["signedAsPayer"] = true;
    if (auto t = lez::printable(stateBefore)) res["stateBeforeText"] = *t;
    if (auto t = lez::printable(stateAfter)) res["stateAfterText"] = *t;
    if (!changed) res["revertReason"] = "reverted, reason unavailable (it is in the sequencer's log); the fee was still charged";
    j.result = res;
    j.state = changed ? "applied" : "reverted";
}

std::string LezActorsModuleImpl::transfer(std::string requestJson) {
    json r = json::parse(unquote(requestJson), nullptr, false);
    if (!r.is_object()) return fail("transfer takes {from, to, amount}");
    auto from = lez::parseId(r.value("from", r.value("fromHex", "")));
    auto to = lez::parseId(r.value("to", r.value("toHex", "")));
    std::string amountText = r.contains("amount") && r["amount"].is_number_unsigned() ? std::to_string(r["amount"].get<uint64_t>()) : r.value("amount", "");
    auto amount = lez::parseU128(amountText);
    if (!from) return fail("not a sender account id");
    if (!to) return fail("not a recipient account id");
    if (*from == *to) return fail("sender and recipient are the same account");
    if (!amount || *amount == 0) return fail("the amount is a positive whole number of raw units");
    if (!walletHolds(*from)) return fail("the sender must be an account in this wallet: it signs the transfer");
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (!m_wallet) return fail("unlock the wallet first"); }
    // pre-checks against the chain, before anything is signed
    json fr = json::parse(feeReserveEstimate(), nullptr, false);
    if (!fr.is_object() || fr.value("status", "") != "ok") return fail(fr.is_object() ? fr.value("error", "sequencer unreachable") : "sequencer unreachable");
    lez::u128 reserve = *lez::parseU128(fr.value("reserve", "0"));
    std::string err; bool fromOn = false, toOn = false;
    auto fv = fetchAccount(*from, &err, &fromOn);
    if (!fv) return fail(err);
    lez::u128 payerIsSender = 1;
    std::string payerB58; { std::lock_guard<std::recursive_mutex> lk(m_mtx); payerB58 = m_payer; }
    if (payerB58.empty() || payerB58 != lez::b58(*from)) payerIsSender = 0;
    if (fv->balance() < *amount + (payerIsSender ? reserve : 0))
        return fail("the sender holds " + lez::u128str(fv->balance()) + "; sending " + lez::u128str(*amount) +
                    (payerIsSender ? " needs about " + lez::u128str(*amount + reserve) + " (the amount plus ~" + lez::u128str(reserve) + " held for the fee)" : " needs more"));
    auto tv = fetchAccount(*to, &err, &toOn);
    if (!tv) return fail(err);
    if (!toOn && !walletHolds(*to))
        return fail("the recipient isn't on chain and isn't in this wallet: an unused account needs its public key (not supported in this version)");
    json req = r; req["amount"] = lez::u128str(*amount); req["from"] = lez::b58(*from); req["to"] = lez::b58(*to);
    return startJob("transfer", req);
}

void LezActorsModuleImpl::runTransfer(Job& j) {
    lez::Id from = *lez::parseId(j.request.value("from", "")), to = *lez::parseId(j.request.value("to", ""));
    lez::u128 amount = *lez::parseU128(j.request.value("amount", "0"));
    lez::Id payer; { std::lock_guard<std::recursive_mutex> lk(m_mtx); payer = m_payer.empty() ? from : *lez::parseId(m_payer); }
    std::string err; bool on = false;
    auto sb = fetchAccount(from, &err, &on); auto rb = fetchAccount(to, &err, &on);
    auto pb = fetchAccount(payer, &err, &on);
    lez::u128 senderBefore = sb ? sb->balance() : 0, recipientBefore = rb ? rb->balance() : 0, payerBefore = pb ? pb->balance() : 0;
    j.state = "sending";
    bool included = false;
    // the sender's native actor is the root; the recipient's native actor is touched by the credit
    std::string hash = sendPublic(m_wallet, {{from, {lez::nativeTokenId(), true}}, {to, {lez::nativeTokenId(), false}}},
                                  lez::nativeTransferMessage(kNativeTransferVariant, to, amount), payer, &included, &err);
    if (hash.empty()) { j.state = "failed"; j.error = err; return; }
    j.txHash = hash;
    if (!included) { j.state = "failed"; j.error = "not included within the wallet's poll window"; return; }
    // the credit is an async message: give it up to a minute to land
    lez::u128 senderAfter = senderBefore, recipientAfter = recipientBefore, payerAfter = payerBefore;
    long long deadline = nowMs() + kCreditWaitMs;
    for (;;) {
        if (auto a = fetchAccount(from, &err, &on)) senderAfter = a->balance();
        if (auto a = fetchAccount(to, &err, &on)) recipientAfter = a->balance();
        if (auto a = fetchAccount(payer, &err, &on)) payerAfter = a->balance();
        if (recipientAfter != recipientBefore || nowMs() > deadline) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    bool samePayer = payer == from;
    lez::u128 senderSpent = senderBefore > senderAfter ? senderBefore - senderAfter : 0;
    lez::u128 fee = samePayer ? (senderSpent > amount ? senderSpent - amount : 0) : (payerBefore > payerAfter ? payerBefore - payerAfter : 0);
    auto outcome = lez::transferOutcome(samePayer ? senderBefore : senderBefore, senderAfter, recipientBefore, recipientAfter, amount);
    if (!samePayer && outcome == lez::TransferOutcome::Unchanged && fee > 0) outcome = lez::TransferOutcome::Reverted;
    j.result = {{"included", true}, {"moved", outcome == lez::TransferOutcome::Moved ? lez::u128str(amount) : "0"}, {"fee", lez::u128str(fee)},
                {"senderBefore", lez::u128str(senderBefore)}, {"senderAfter", lez::u128str(senderAfter)},
                {"recipientBefore", lez::u128str(recipientBefore)}, {"recipientAfter", lez::u128str(recipientAfter)}, {"payer", lez::b58(payer)}};
    if (outcome == lez::TransferOutcome::Moved) j.state = "applied";
    else if (outcome == lez::TransferOutcome::Reverted) { j.state = "reverted"; j.result["reverted"] = true; j.result["revertReason"] = "reverted, reason unavailable (it is in the sequencer's log); nothing credited, the fee was charged"; }
    else { j.state = "unknown"; j.error = "neither balance changed within a minute"; }
}
