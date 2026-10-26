// LezActorsModuleImpl, part 2: the wizard. A local v0.4 dev chain running inside this module
// (core_ffi's embedded sequencer, patches/0001), the dev chain's funded accounts, and the guided
// examples: deploy LEZ's tutorial programs and run their steps.
#include "lez_actors_module_impl.h"
#include "lez_bedrock.hpp"
#include "lez_examples.hpp"
#include "lez_http.hpp"
#include "lez_module_util.hpp"
#include <QTimer>
#include <cstring>
#include <thread>

using namespace lezmod;

namespace {

// LEZ's usual sequencer port. LEZ_ACTORS_CHAIN_PORT moves the app's chain, e.g. to run it next
// to another local sequencer (prog-tui's, LEZ's `just run-sequencer-standalone`).
uint16_t localPort() {
    static const uint16_t port = [] {
        const char* e = getenv("LEZ_ACTORS_CHAIN_PORT");
        int p = e && *e ? atoi(e) : 0;
        return uint16_t(p > 0 && p < 65536 ? p : 3040);
    }();
    return port;
}
std::string localUrl() { return "http://127.0.0.1:" + std::to_string(localPort()); }
// The Bedrock node and the indexer sit on the next ports (ADR 0003).
int nodeApiPort() { return localPort() + 1; }
int nodeSwarmPort() { return localPort() + 2; }
int nodeBlendPort() { return localPort() + 3; }
int indexerPort() { return localPort() + 4; }
std::string indexerUrl() { return "http://127.0.0.1:" + std::to_string(indexerPort()); }
// marks a chain home made for a Bedrock node (a 0.3.x home is standalone: its checkpoint names a
// channel no node has)
const char* kBedrockMarker = ".bedrock";
// LEZ's debug sequencer config (lez/sequencer/service/configs/debug), for one node in this
// process: no gossip, no metrics listener, a block every 3 s (the debug config's 15 s makes a
// 4-transaction deploy take a minute), following the Bedrock node at @NODE@. The genesis funds the
// two dev accounts below.
const char* kChainTemplate = R"({
  "home": ".",
  "max_num_tx_in_block": 20,
  "mempool_max_size": 1000,
  "block_create_timeout": "3s",
  "retry_pending_blocks_timeout": "5s",
  "metrics_address": null,
  "bedrock_config": {
    "channel_id": "0101010101010101010101010101010101010101010101010101010101010101",
    "node_url": "@NODE@"
  },
  "genesis": {
    "channel_params": { "minimum_sequencer_stake": 149, "posting_timeframe": 300, "posting_timeout": 25, "exit_delay": 100, "max_block_size": 1048576 },
    "actions": [
      { "supply_account": { "recipient": { "identified": { "key": "6aa4c365699b89d963ef85a59868b9612b6001f90750b4970662a62776332e81" } }, "balance": 10000000000000 } },
      { "supply_account": { "recipient": { "identified": { "key": "891ae258f92ca8447af6cf09c41d96b3a06187d098b91ab2cc17b42e3c4a2fa1" } }, "balance": 20000000000000 } },
      { "supply_private_account": { "address": "44KVvzvh2ephQU9aR5snboGUXKgS1FnPYh1W4KZKwmhm", "balance": 10000000000000 } },
      { "supply_private_account": { "address": "478hD6LXFTYu5gRRkWkCyXPRsWgCwAfRredoCUfxr5b9", "balance": 20000000000000 } }
    ]
  }
})";
// the Bedrock funding key `sequencer_service setup` is given in LEZ's dev instructions: a key the
// node's wallet holds (bedrock/node-config.yaml), which pays the sequencer's inscriptions
const char* kFundingPublicKey = "2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26";
// the dev genesis' funded public accounts: LEZ Justfile wallet-import-test-accounts (public dev
// keys, never real ones)
struct DevAccount { const char* key; const char* id; };
const DevAccount kDevAccounts[] = {
    {"f434f8741720014586ae43356d2aec6257da086222f604ddb75d69733b86fc4c", "2RHZhw9h534Zr3eq2RGhQete2Hh667foECzXPmSkGni2"},   // 20 T
    {"7f273098f25b71e6c005a9519f2678da8d1c7f01f6a27778e2d9948abdf901fb", "CbgR6tj5kWx5oziiFptM7jMvrQeYY3Mzaao6ciuhSr2r"},   // 10 T
};
constexpr int kMaxSegments = 8;

bool noise(const std::string& line) {
    return line.find("jsonrpsee_server::server] connection") != std::string::npos ||
           line.find("risc0_zkvm::host::server::exec::executor") != std::string::npos ||
           line.find("ModuleProxy:") != std::string::npos ||          // the host's own call tracing
           line.find("LogosAPIConsumer") != std::string::npos;
}

}  // namespace

// =============================================================================================
// the local chain
// =============================================================================================
std::string LezActorsModuleImpl::localChainStatus() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    return ok({{"state", m_chainState}, {"error", m_chainError}, {"rpcUrl", localUrl()}, {"home", m_dataDir + "/chain"},
               {"inProcess", m_chain != nullptr}, {"logSeq", m_tap.lastSeq()},
               {"nodeUrl", "http://127.0.0.1:" + std::to_string(nodeApiPort())}, {"nodeRunning", lez::childAlive(m_nodePid)},
               {"indexerUrl", indexerUrl()}, {"indexerRunning", lez::childAlive(m_indexerPid)}});
}

std::string LezActorsModuleImpl::startLocalChain() {
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        if (m_chainState == "running" || m_chainState == "starting" || m_chainState == "stopping") return localChainStatus();
    }
    // something already answers on the port: use it rather than fail to bind
    std::string err;
    if (auto u = lez::parseHttpUrl(localUrl(), &err); u && lez::rpcCall(*u, "checkHealth", json::array(), &err, 1500)) {
        bool changed;
        { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_chainState = "external"; m_chainError = ""; changed = m_rpcUrl != localUrl(); m_rpcUrl = localUrl(); saveSettings(); writeWalletConfig(); }
        if (changed) enqueue([this] { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (m_wallet && !m_walletPw.empty()) { wallet_ffi_save(m_wallet); wallet_ffi_destroy(m_wallet); m_wallet = nullptr; WalletHandle* h = nullptr; if (wallet_ffi_open(walletConfigPath().c_str(), walletStoragePath().c_str(), m_walletPw.c_str(), &h) == FfiOperationError_Success) m_wallet = h; } });
        publish();
        return ok({{"state", "external"}, {"note", "a sequencer already answers on 127.0.0.1:" + std::to_string(localPort()) + "; using it"}});
    }
    startChainThread(false);
    return localChainStatus();
}

std::string LezActorsModuleImpl::stopLocalChain() {
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        if (m_chainState == "external") { m_chainState = "stopped"; publish(); return ok({{"state", "stopped"}, {"note", "that sequencer isn't ours; it keeps running"}}); }
        if (m_chainState != "running" && m_chainState != "failed") return localChainStatus();
    }
    startChainThread(true);
    return localChainStatus();
}

std::string LezActorsModuleImpl::resetLocalChain() {
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        if (m_chainState == "starting" || m_chainState == "stopping") return fail("wait until the chain has started or stopped");
        if (m_chainState == "external") return fail("the chain on 127.0.0.1:" + std::to_string(localPort()) + " isn't this app's; it can't reset it");
        m_chainState = "stopping"; m_chainError = "";
    }
    if (m_chainThread.joinable()) m_chainThread.join();
    m_chainThread = std::thread([this, alive = m_alive] {
        SequencerServiceFFI* c;
        { std::lock_guard<std::recursive_mutex> lk(m_mtx); c = m_chain; m_chain = nullptr; }
        if (c) sequencer_ffi_stop_sequencer(c);
        stopBedrock();
        std::string home;
        {
            std::lock_guard<std::recursive_mutex> lk(m_mtx);
            home = m_dataDir + "/chain";
            // the deployed examples lived on that chain
            for (const auto& [name, id] : m_examplePrograms) {
                auto pid = lez::parseId(id);
                m_programs.erase(std::remove_if(m_programs.begin(), m_programs.end(), [&](const Program& p) { return pid && p.id == *pid && !p.builtin; }), m_programs.end());
            }
            m_examplePrograms.clear();
            saveSettings();
        }
        // only ever the chain dir under this module's data dir
        std::error_code ec;
        if (home.size() > 6 && home.compare(home.size() - 6, 6, "/chain") == 0) fs::remove_all(home, ec);
        { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_chainState = "stopped"; m_chainError = ec ? "couldn't delete " + home + ": " + ec.message() : ""; }
        if (*alive) onLoop([this] { publish(); });
    });
    publish();
    return ok({{"state", "stopping"}, {"note", "stopping and deleting the local chain; start it again for a fresh genesis"}});
}

void LezActorsModuleImpl::startChainThread(bool stop) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        m_chainState = stop ? "stopping" : "starting";
        m_chainError = "";
    }
    if (m_chainThread.joinable()) m_chainThread.join();
    if (stop) {
        m_chainThread = std::thread([this, alive = m_alive] {
            SequencerServiceFFI* c;
            { std::lock_guard<std::recursive_mutex> lk(m_mtx); c = m_chain; m_chain = nullptr; }
            if (c) sequencer_ffi_stop_sequencer(c);
            stopBedrock();
            { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_chainState = "stopped"; }
            if (*alive) onLoop([this] { publish(); });
        });
        publish();
        return;
    }
    m_chainThread = std::thread([this, alive = m_alive] {
        std::string home, tmpl;
        { std::lock_guard<std::recursive_mutex> lk(m_mtx); home = m_dataDir + "/chain"; tmpl = m_dataDir + "/chain-template.json"; }
        long long seq = m_tap.lastSeq();
        auto failWith = [&](const std::string& what) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::string detail;
            for (const auto& l : m_tap.since(seq, 400)) if (l.text.find("[local-chain]") != std::string::npos) detail = l.text.substr(l.text.find("[local-chain]") + 14);
            { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_chainState = "failed"; m_chainError = what + (detail.empty() ? "" : ": " + detail); }
            fprintf(stderr, "[lez_actors] local chain: %s\n", m_chainError.c_str());
            if (*alive) onLoop([this] { publish(); });
        };
        // guest execution in-process, without real proofs (a dev chain)
        setenv("RISC0_DEV_MODE", "1", 0);
        sequencer_ffi_init_logger("info");
        std::error_code ec;
        // a 0.3.x chain is standalone (no Bedrock): its checkpoint names a channel the new node
        // doesn't have, so it can't be resumed. Keep it aside instead of deleting it.
        if (fs::exists(home + "/sequencer_config.json") && !fs::exists(home + "/" + kBedrockMarker)) {
            std::string aside = home + ".standalone-0.3";
            fs::remove_all(aside, ec);
            fs::rename(home, aside, ec);
            fprintf(stderr, "[lez_actors] the old standalone chain moved to %s\n", aside.c_str());
        }
        fs::create_directories(home, ec);
        writeAtomic(home + "/" + kBedrockMarker, "logos-blockchain 6b33abb\n");
        if (std::string e = startBedrock(home); !e.empty()) return failWith(e);
        std::string t = kChainTemplate;
        t.replace(t.find("@NODE@"), 6, "http://127.0.0.1:" + std::to_string(nodeApiPort()));
        if (!writeAtomic(tmpl, t)) return failWith("can't write " + tmpl);
        if (sequencer_ffi_local_setup(home.c_str(), tmpl.c_str(), kFundingPublicKey) != FfiOperationError_Success) return failWith("setting up the chain failed");
        InitializedSequencerServiceFFIResult r = sequencer_ffi_start_local(home.c_str(), localPort());
        if (r.error != FfiOperationError_Success || !r.value) { stopBedrock(); return failWith("the sequencer didn't start (is port " + std::to_string(localPort()) + " taken?)"); }
        // the indexer reads finalized blocks off the node; it starts once the channel exists
        {
            const std::string lib = lez::coreFfiLibDir(reinterpret_cast<const void*>(&sequencer_ffi_start_local));
            const std::string exe = lib + "/indexer_service", dir = home + "/indexer";
            fs::create_directories(dir, ec);
            writeAtomic(dir + "/indexer.json", lez::indexerConfig(nodeApiPort()));
            lez::killStale(home + "/indexer.pid", exe);
            pid_t ip = lez::spawnChild(exe, {dir + "/indexer.json", "-p", std::to_string(indexerPort()), "-d", dir}, dir,
                                       {{"RUST_LOG", "info"}}, home + "/indexer.log");
            if (ip > 0) lez::writePidFile(home + "/indexer.pid", ip);
            else fprintf(stderr, "[lez_actors] no indexer (%s): events won't be queryable\n", exe.c_str());
            std::lock_guard<std::recursive_mutex> lk(m_mtx);
            m_indexerPid = ip > 0 ? ip : 0;
        }
        bool changed;
        {
            std::lock_guard<std::recursive_mutex> lk(m_mtx);
            m_chain = r.value;
            m_chainState = "running";
            changed = m_rpcUrl != localUrl();
            m_rpcUrl = localUrl();
            saveSettings();
            writeWalletConfig();
        }
        fprintf(stderr, "[lez_actors] local chain running on %s (home %s)\n", localUrl().c_str(), home.c_str());
        // the wallet library reads its sequencer address at open: reopen an unlocked wallet
        if (changed) enqueue([this] {
            std::lock_guard<std::recursive_mutex> lk(m_mtx);
            if (!m_wallet || m_walletPw.empty()) return;
            wallet_ffi_save(m_wallet); wallet_ffi_destroy(m_wallet); m_wallet = nullptr;
            WalletHandle* h = nullptr;
            if (wallet_ffi_open(walletConfigPath().c_str(), walletStoragePath().c_str(), m_walletPw.c_str(), &h) == FfiOperationError_Success) m_wallet = h;
        });
        if (*alive) onLoop([this] { publish(); });
    });
    publish();
}

// Starts the Bedrock node for `home` and waits until its API answers. "" or what failed.
std::string LezActorsModuleImpl::startBedrock(const std::string& home) {
    const std::string lib = lez::coreFfiLibDir(reinterpret_cast<const void*>(&sequencer_ffi_start_local));
    const std::string exe = lib + "/logos-blockchain-node", dir = home + "/node";
    if (access(exe.c_str(), X_OK) != 0) return "this build has no Bedrock node (" + exe + ")";
    lez::killStale(home + "/node.pid", exe);
    if (lez::portOpen(nodeApiPort())) return "something else already listens on 127.0.0.1:" + std::to_string(nodeApiPort()) + " (the Bedrock node's port; LEZ_ACTORS_CHAIN_PORT moves all of them)";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!writeAtomic(dir + "/node-config.yaml", lez::nodeConfig(nodeApiPort(), nodeSwarmPort(), nodeBlendPort())) ||
        !writeAtomic(dir + "/deployment-settings.yaml", lez::kDeploymentSettings))
        return "can't write the node config in " + dir;
    pid_t pid = lez::spawnChild(exe, {"node-config.yaml", "--deployment", "deployment-settings.yaml"}, dir,
                                {{"POL_PROOF_DEV_MODE", "true"}}, home + "/node.log");
    if (pid <= 0) return "the Bedrock node didn't start";
    lez::writePidFile(home + "/node.pid", pid);
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_nodePid = pid; }
    for (int i = 0; i < 600; i++) {   // up to 60 s
        if (lez::portOpen(nodeApiPort())) return "";
        if (!lez::childAlive(pid)) { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_nodePid = 0; return "the Bedrock node exited at start (see " + home + "/node.log)"; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    stopBedrock();
    return "the Bedrock node didn't open its API on port " + std::to_string(nodeApiPort()) + " within 60 s";
}

void LezActorsModuleImpl::stopBedrock() {
    pid_t n, i;
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); n = m_nodePid; i = m_indexerPid; m_nodePid = m_indexerPid = 0; }
    lez::stopChild(i);
    lez::stopChild(n);
}

std::string LezActorsModuleImpl::localChainLog(std::string sinceSeq) {
    sinceSeq = unquote(sinceSeq);
    long long since = 0;
    try { since = sinceSeq.empty() ? 0 : std::stoll(sinceSeq); } catch (...) {}
    json lines = json::array();
    for (const auto& l : m_tap.since(since, 1500)) {
        if (noise(l.text) || l.text.empty()) continue;
        lines.push_back({{"seq", l.seq}, {"text", l.text}});
    }
    while (lines.size() > 400) lines.erase(lines.begin());
    return ok({{"lines", lines}, {"lastSeq", m_tap.lastSeq()}});
}

std::string LezActorsModuleImpl::useDevAccounts() {
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (!m_wallet) return fail("create or unlock a wallet first"); }
    json added = json::array();
    for (const auto& d : kDevAccounts) {
        if (walletHolds(*lez::parseId(d.id))) continue;
        json r = json::parse(importKey(d.key), nullptr, false);
        if (!r.is_object() || r.value("status", "") != "ok") return fail("importing a dev account failed: " + (r.is_object() ? r.value("error", "") : std::string("?")));
        added.push_back(d.id);
    }
    // the richer one pays
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_payer = kDevAccounts[0].id; if (m_selected.empty()) m_selected = kDevAccounts[0].id; saveSettings(); }
    listAccounts();
    publish();
    return ok({{"imported", added}, {"payer", kDevAccounts[0].id},
               {"note", "the dev chain's two funded accounts are in the wallet; the 20 T one pays the fees"}});
}

// =============================================================================================
// the guided examples
// =============================================================================================
std::optional<lez::Id> LezActorsModuleImpl::exampleProgram(const std::string& name) {
    std::string idText;
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); auto it = m_examplePrograms.find(name); if (it == m_examplePrograms.end()) return std::nullopt; idText = it->second; }
    auto id = lez::parseId(idText);
    if (!id) return std::nullopt;
    // still there? (a reset chain forgets it)
    std::string err; bool onChain = false;
    auto v = fetchAccount(*id, &err, &onChain);
    if (!v || !onChain || !v->actorStates.count(lez::b58(lez::programLoaderId()))) return std::nullopt;
    return id;
}

std::string LezActorsModuleImpl::listExamples() {
    json arr = json::array();
    for (const auto& e : lez::examples()) {
        json steps = json::array();
        for (const auto& s : e.steps) steps.push_back({{"id", s.id}, {"label", s.label}, {"expect", s.expect}, {"explain", s.explain}, {"takesText", s.takesText}, {"defaultText", s.defaultText}});
        auto pid = exampleProgram(e.name);
        auto elf = lez::exampleElf(e.name.c_str());
        arr.push_back({{"name", e.name}, {"title", e.title}, {"about", e.about}, {"needs", e.needs}, {"steps", steps},
                       {"deployed", pid.has_value()}, {"programId", pid ? lez::b58(*pid) : ""}, {"elfSize", elf.second}});
    }
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    return ok({{"examples", arr}, {"exampleAccounts", m_exampleAccounts}});
}

std::string LezActorsModuleImpl::deployExample(std::string name) {
    name = unquote(name);
    const lez::Example* e = lez::findExample(name);
    if (!e) return fail("no such example");
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (!m_wallet) return fail("unlock the wallet first"); if (m_payer.empty()) return fail("choose a payer first (or use the dev accounts)"); }
    return startJob("deploy", {{"example", name}});
}

void LezActorsModuleImpl::runDeploy(Job& j) {
    std::string name = j.request.value("example", "");
    const lez::Example* e = lez::findExample(name);
    lez::Id payer; { std::lock_guard<std::recursive_mutex> lk(m_mtx); payer = *lez::parseId(m_payer); }
    std::vector<std::string> order = e->needs;
    order.push_back(name);
    json deployed = json::object();
    long long seq = m_tap.lastSeq();
    for (const auto& n : order) {
        if (auto have = exampleProgram(n)) { deployed[n] = {{"programId", lez::b58(*have)}, {"already", true}}; continue; }
        auto elf = lez::exampleElf(n.c_str());
        if (!elf.first) { j.state = "failed"; j.error = "the " + n + " binary isn't in this build"; return; }
        j.state = "deploying " + n;
        finishJob(j.id, [st = j.state](Job& x) { x.state = st; });
        // one header and up to kMaxSegments segment accounts, all fresh wallet accounts. The
        // segment count must match ceil(user_elf / 96 KiB); a wrong count is refused before
        // anything is sent, so try them in turn with the same accounts. Any other failure means
        // the count was right and an upload went wrong (seen: "segment 0 transaction did not
        // finalize" on a healthy chain); a partial upload can't be resumed (wallet FIXME), so
        // start again at that count with fresh accounts, twice at most.
        auto freshAccounts = [&](std::vector<FfiBytes32>& accts) -> bool {
            for (auto& a : accts) {
                if (wallet_ffi_create_account_public(m_wallet, &a) != FfiOperationError_Success) return false;
                std::lock_guard<std::recursive_mutex> lk(m_mtx);
                m_helperAccounts.push_back(lez::b58(fromFfi(a)));
            }
            wallet_ffi_save(m_wallet);
            { std::lock_guard<std::recursive_mutex> lk(m_mtx); saveSettings(); }
            return true;
        };
        std::vector<FfiBytes32> accts(1 + kMaxSegments);
        if (!freshAccounts(accts)) { j.state = "failed"; j.error = "the wallet couldn't create deploy accounts"; return; }
        FfiBytes32 p = ffiId(payer);
        bool done = false;
        std::string tx;
        int used = 0, retries = 0;
        for (int segs = 1; segs <= kMaxSegments && !done; segs++) {
            long long before = m_tap.lastSeq();
            FfiTransactionResult res{};
            FfiOperationError rc = wallet_ffi_program_loader_deploy(m_wallet, &accts[0], &accts[1], size_t(segs), elf.first, elf.second, false, &p, &res);
            if (rc == FfiOperationError_Success) { done = true; used = segs; tx = res.tx_hash ? res.tx_hash : ""; }
            wallet_ffi_free_transaction_result(&res);
            if (done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::string why = m_tap.ffiErrorSince(before);
            if (why.find("segment(s) but") != std::string::npos || why.empty()) continue;   // not this count
            if (retries++ >= 2) break;
            fprintf(stderr, "[lez_actors] deploying %s with %d segments failed (%s); again with fresh accounts\n", n.c_str(), segs, why.c_str());
            if (!freshAccounts(accts)) break;
            segs--;   // the same count again
        }
        if (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::string why = m_tap.ffiErrorSince(seq);
            j.state = "failed"; j.error = "deploying " + n + " failed" + (why.empty() ? "" : ": " + why);
            return;
        }
        lez::Id pid = fromFfi(accts[0]);
        {
            std::lock_guard<std::recursive_mutex> lk(m_mtx);
            m_examplePrograms[n] = lez::b58(pid);
            bool known = false;
            for (auto& prog : m_programs) if (prog.id == pid) known = true;
            if (!known) m_programs.push_back({n, pid, false});
            saveSettings();
        }
        deployed[n] = {{"programId", lez::b58(pid)}, {"segments", used}, {"elfBytes", elf.second}, {"txHash", tx}};
        if (!tx.empty()) j.txHash = tx;
    }
    j.result = {{"deployed", deployed}, {"note", "the program id is its header account: the program loader keeps the bytecode in segment accounts it points at"}};
    j.state = "applied";
}

std::string LezActorsModuleImpl::runExampleStep(std::string requestJson) {
    json r = json::parse(unquote(requestJson), nullptr, false);
    if (!r.is_object()) return fail("runExampleStep takes {example, step, text?}");
    const lez::Example* e = lez::findExample(r.value("example", ""));
    if (!e) return fail("no such example");
    const lez::ExampleStep* s = lez::findStep(*e, r.value("step", ""));
    if (!s) return fail("no such step");
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (!m_wallet) return fail("unlock the wallet first"); if (m_payer.empty()) return fail("choose a payer first (or use the dev accounts)"); }
    if (!exampleProgram(e->name)) return fail("deploy " + e->name + " first");
    for (const auto& n : e->needs) if (!exampleProgram(n)) return fail("deploy " + n + " first (" + e->name + " calls it)");
    json req = {{"example", e->name}, {"step", s->id}, {"label", s->label}, {"expect", s->expect}, {"explain", s->explain},
                {"text", r.value("text", s->defaultText)}};
    return startJob("example", req);
}

// On the worker: two wallet accounts for the examples, never the payer.
std::vector<std::string> LezActorsModuleImpl::ensureExampleAccounts(std::string* err) {
    std::vector<std::string> keep;
    std::string payer;
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); payer = m_payer; for (const auto& a : m_exampleAccounts) if (a != payer && std::find(m_accounts.begin(), m_accounts.end(), a) != m_accounts.end()) keep.push_back(a); }
    while (keep.size() < 2) {
        FfiBytes32 out{};
        if (wallet_ffi_create_account_public(m_wallet, &out) != FfiOperationError_Success) { *err = "the wallet couldn't create an example account"; return {}; }
        keep.push_back(lez::b58(fromFfi(out)));
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        m_accounts.push_back(keep.back());
    }
    wallet_ffi_save(m_wallet);
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_exampleAccounts = keep; saveSettings(); }
    return keep;
}

void LezActorsModuleImpl::runExample(Job& j) {
    std::string err;
    auto accts = ensureExampleAccounts(&err);
    if (accts.size() < 2) { j.state = "failed"; j.error = err; return; }
    lez::Id a = *lez::parseId(accts[0]), b = *lez::parseId(accts[1]);
    std::string ex = j.request.value("example", ""), step = j.request.value("step", ""), text = j.request.value("text", "");
    auto prog = exampleProgram(ex);
    if (!prog) { j.state = "failed"; j.error = ex + " isn't deployed on this chain"; return; }
    lez::Bytes textBytes(text.begin(), text.end());
    auto actor = [](const lez::Id& acct, const lez::Id& p, bool sign) { return json{{"accountId", lez::b58(acct)}, {"programId", lez::b58(p)}, {"sign", sign}}; };
    json actors = json::array();
    lez::Bytes msg;
    if (ex == "hello_world") {
        actors.push_back(actor(a, *prog, false)); msg = lez::borshBytes(text);
    } else if (ex == "hello_world_with_authorization") {
        actors.push_back(actor(a, *prog, step == "signed")); msg = lez::borshBytes(text);
    } else if (ex == "hello_world_with_move_function") {
        if (step == "write") { actors.push_back(actor(a, *prog, false)); msg = lez::moveFnWrite(textBytes); }
        else if (step == "append-direct") { actors.push_back(actor(b, *prog, false)); msg = lez::moveFnAppend(textBytes); }
        else {
            // MoveData states the source's bytes; the wrong variant claims one byte more
            bool oc = false;
            auto v = fetchAccount(a, &err, &oc);
            std::string pb = lez::b58(*prog);
            lez::Bytes held = v && v->actorStates.count(pb) ? v->actorStates[pb] : lez::Bytes{};
            if (step == "move-wrong") held.push_back('?');
            actors.push_back(actor(a, *prog, false)); actors.push_back(actor(b, *prog, false));
            msg = lez::moveFnMoveData(held, b);
        }
    } else if (ex == "simple_tail_call") {
        auto hello = exampleProgram("hello_world");
        if (!hello) { j.state = "failed"; j.error = "hello_world isn't deployed"; return; }
        actors.push_back(actor(a, *prog, false)); actors.push_back(actor(a, *hello, false));
        msg = lez::actorBytes(a, *hello);
    } else if (ex == "actor_conversation") {
        lez::Id payer; { std::lock_guard<std::recursive_mutex> lk(m_mtx); payer = *lez::parseId(m_payer); }
        if (step == "ping" || step == "ping-undeclared") {
            // "ping" declares account 2, so the engine runs Pong in this transaction; without it
            // ("ping-undeclared") the engine refuses: UnboundAsyncDestination
            actors.push_back(actor(a, *prog, false));
            if (step == "ping") actors.push_back(actor(b, *prog, false));
            msg = lez::acvPing(b, textBytes);
        } else {
            // the read targets the payer's native-token actor: a funded account
            actors.push_back(actor(a, *prog, false)); actors.push_back(actor(payer, lez::nativeTokenId(), false));
            lez::u128 min = step == "require-ok" ? lez::u128(1000) : step == "require-fail" ? lez::u128(1) << 100 : 0;
            msg = step == "ask" ? lez::acvAskBalance(payer) : lez::acvRequire(payer, min);
        }
    } else { j.state = "failed"; j.error = "unknown example"; return; }
    j.request["actors"] = actors;
    j.request["messageHex"] = lez::hex(msg);
    j.request["accounts"] = accts;
    runCall(j);
    attachFlow(j, ex, step, a, b, *prog);
    if (ex != "actor_conversation" || j.txHash.empty()) return;

    // The conversation, read back from the chain: each hop writes one line into its own actor's
    // state ("->" sent, "<-" received), so the lines a transaction added to each mentioned
    // actor_conversation actor are its hops, drawn at once. Each hop also emits an acv_hop_ event;
    // those arrive when the block finalizes, a block or two later, and stamp the hops in order
    // (stampHops). The state lines say which actor a hop ran on; the events don't carry the account.
    uint64_t txBlock = blockOfTx(j.txHash);
    std::string pb = lez::b58(*prog);
    json hops = json::array();
    for (const auto& s2 : j.result.value("actors", json::array())) {
        if (s2.value("programId", "") != pb) continue;
        for (const auto& line : newLines(*lez::unhex(s2.value("before", "")), *lez::unhex(s2.value("after", ""))))
            hops.push_back({{"accountId", s2.value("accountId", "")}, {"text", line}, {"sameTx", true}, {"block", txBlock}});
    }
    j.result["hops"] = hops;
    j.result["txBlock"] = txBlock;
    // its events may already be in (finality can beat this read): stamp the hops now
    {
        std::vector<json> mine;
        { std::lock_guard<std::recursive_mutex> lk(m_mtx); for (const auto& e : m_events) if (e.value("txHash", "") == j.txHash) mine.push_back(e); }
        json evs = json::array();
        for (const auto& e : mine) if (e.value("selector", "") == "6163765f686f705f") evs.push_back({{"block", e["block"]}, {"text", e.value("text", "")}});
        if (!evs.empty()) {
            j.result["hopEvents"] = evs; j.result["finalizedBlock"] = evs[0]["block"];
            for (size_t i = 0; i < hops.size() && i < evs.size(); i++) { j.result["hops"][i]["finalizedBlock"] = evs[i]["block"]; j.result["hops"][i]["eventText"] = evs[i]["text"]; }
        }
    }
    if (step == "ping-undeclared" && j.state == "reverted")
        j.result["revertReason"] = "the engine refused it (UnboundAsyncDestination): an async message to a public actor must be declared in the transaction; the fee was charged";
}

std::string LezActorsModuleImpl::programSource(std::string name) {
    name = unquote(name);
    auto src = lez::exampleSource(name.c_str());
    if (!src.first) return fail("no source for " + name);
    return ok({{"name", name}, {"source", std::string(src.first, src.second)},
               {"origin", name == "actor_conversation" ? "this playground (core/examples/src)" : name == "native_token" ? "LEZ, lee/state_machine/core/src/native_token.rs" : "LEZ, examples/program_deployment/methods/guest/src/bin"}});
}

// The step's messages, resolved to real actors, each with what became of it, for the actor map.
void LezActorsModuleImpl::attachFlow(Job& j, const std::string& ex, const std::string& step, const lez::Id& a, const lez::Id& b, const lez::Id& prog) {
    lez::StepFlow f = lez::flowFor(ex, step);
    if (f.messages.empty() || !j.result.is_object()) return;
    lez::Id payer; { std::lock_guard<std::recursive_mutex> lk(m_mtx); payer = *lez::parseId(m_payer); }
    auto hello = exampleProgram("hello_world");
    auto actorOf = [&](const std::string& sym) -> json {
        if (sym == "tx") return nullptr;
        auto slash = sym.find('/');
        std::string acc = sym.substr(0, slash), pr = sym.substr(slash + 1);
        lez::Id account = acc == "A" ? a : acc == "B" ? b : payer;
        lez::Id program = pr == "native" ? lez::nativeTokenId() : pr == "hello" && hello ? *hello : prog;
        return {{"accountId", lez::b58(account)}, {"programId", lez::b58(program)},
                {"accountLabel", acc == "A" ? "example account 1" : acc == "B" ? "example account 2" : "payer"},
                {"programName", pr == "native" ? "native token" : pr == "hello" ? "hello_world" : ex}};
    };
    bool applied = j.state == "applied";
    json msgs = json::array();
    for (size_t i = 0; i < f.messages.size(); i++) {
        int n = int(i) + 1;
        const auto& m = f.messages[i];
        std::string status = applied ? "ok"
            : f.failsAt == 0 ? "undone"
            : n < f.failsAt ? "undone"
            : n == f.failsAt ? (f.refused ? "refused" : "panicked")
            : "never ran";
        json msg = {{"n", n}, {"from", actorOf(m.from)}, {"to", actorOf(m.to)}, {"kind", m.kind}, {"name", m.name}, {"status", status}};
        // the code that handles it, for the code view next to the map
        lez::CodeRef c = lez::codeFor(ex, step, n);
        if (!c.file.empty()) {
            auto src = lez::exampleSource(c.file.c_str());
            json ranges = json::array();
            if (src.first) for (const auto& [first, last] : lez::resolveRanges(std::string(src.first, src.second), c)) ranges.push_back({first, last});
            msg["code"] = {{"file", c.file}, {"ranges", ranges}, {"says", c.says}};
            if (!c.panicAnchor.empty() && src.first) {
                lez::CodeRef p{c.file, {{c.panicAnchor, 1}}, ""};
                auto pr = lez::resolveRanges(std::string(src.first, src.second), p);
                if (!pr.empty()) msg["code"]["panicLine"] = pr[0].first;
            }
        }
        msgs.push_back(msg);
    }
    j.result["flow"] = msgs;
}

// The lines `after` added to `before` (an actor_conversation state is a rolling window of lines).
std::vector<std::string> LezActorsModuleImpl::newLines(const lez::Bytes& before, const lez::Bytes& after) {
    auto split = [](const lez::Bytes& b) {
        std::vector<std::string> out; std::string cur;
        for (uint8_t c : b) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur += char(c); }
        if (!cur.empty()) out.push_back(cur);
        out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& l) { return l.rfind("#pending", 0) == 0; }), out.end());
        return out;
    };
    auto b = split(before), a = split(after);
    // the longest tail of `before` that `after` starts with is what was already there
    for (size_t k = std::min(a.size(), b.size()); k > 0; k--)
        if (std::equal(b.end() - long(k), b.end(), a.begin())) return {a.begin() + long(k), a.end()};
    return a;
}

uint64_t LezActorsModuleImpl::blockOfTx(const std::string& txHashHex) {
    // The sequencer's getTransaction answers [tx, block id]. core_ffi's
    // sequencer_ffi_query_block_by_tx_hash gave a later block than the one holding the tx (53 for a
    // tx in 51, 2026-10-26, logos-blockchain 6b33abb3), so it's only the fallback.
    {
        std::string err, rpc;
        { std::lock_guard<std::recursive_mutex> lk(m_mtx); rpc = m_rpcUrl; }
        if (auto u = lez::parseHttpUrl(rpc, &err)) {
            auto r = lez::rpcCall(*u, "getTransaction", json::array({txHashHex}), &err, 3000);
            if (r && r->is_array() && r->size() >= 2 && (*r)[1].is_number_unsigned()) return (*r)[1].get<uint64_t>();
        }
    }
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    auto hb = lez::unhex(txHashHex);
    if (!m_chain || !hb || hb->size() != 32) return 0;
    FfiHashType h{}; std::copy(hb->begin(), hb->end(), h.data);
    auto r = sequencer_ffi_query_block_by_tx_hash(m_chain, h);
    if (r.error != FfiOperationError_Success || !r.value) return 0;
    uint64_t block = *r.value;
    sequencer_ffi_free_query_block_id_by_transaction(r.value);
    return block;
}

// =============================================================================================
// program events: the finalized record (ADR 0003)
// =============================================================================================
// The sequencer stores events only for finalized blocks, and the indexer reads finalized blocks
// off the Bedrock node, so this list trails the chain's head by a block or two: it's the
// canonical record, where a step's state diff is the instant preview. A thread polls the local
// chain's indexer every 2 s from a cursor, in <=1000-block windows (the indexer's span cap), and
// keeps the newest kEventsKept.
namespace {

constexpr size_t kEventsKept = 2000;

std::string b64decode(const std::string& in) {
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        auto p = tbl.find(char(c));
        if (p == std::string::npos) continue;
        val = (val << 6) + int(p); bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

bool printable(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (c < 0x20 && c != '\n' && c != '\t') return false;
    return s.find('\x7f') == std::string::npos;
}

// Known selectors by name; an 8-byte ASCII literal (b"acv_hop_") as its text; else raw hex (the
// sha256("<program>::<Event>") convention can't be reversed without the event's name).
std::string selectorName(const std::string& hex) {
    static const std::map<std::string, std::string> known = {
        {"6163765f686f705f", "actor_conversation hop"},
        {"98abe0ff895d39c5", "hello_event::Greeted"},
        {"cd499ae548cdf23d", "bridge::Deposit"},
    };
    if (auto it = known.find(hex); it != known.end()) return it->second;
    auto b = lez::unhex(hex);
    if (b && b->size() == 8) {
        std::string t(b->begin(), b->end());
        bool ascii = true;
        for (unsigned char c : t) if (c < 0x21 || c > 0x7e) ascii = false;
        if (ascii) return "\"" + t + "\"";
    }
    return "";
}

}  // namespace

void LezActorsModuleImpl::eventsLoop() {
    while (!m_stop) {
        for (int i = 0; i < 20 && !m_stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (m_stop) break;
        refreshEvents();
    }
}

void LezActorsModuleImpl::refreshEvents() {
    bool running;
    { std::lock_guard<std::recursive_mutex> lk(m_mtx); running = m_chainState == "running" && lez::childAlive(m_indexerPid); }
    if (!running) return;
    std::string err;
    auto url = lez::parseHttpUrl(indexerUrl(), &err);
    if (!url) return;
    auto st = lez::rpcCall(*url, "getStatus", json::array(), &err, 2000);
    if (!st || !st->is_object()) { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_eventsError = err; return; }
    long long tip = st->contains("indexed_block_id") && (*st)["indexed_block_id"].is_number() ? (*st)["indexed_block_id"].get<long long>() : -1;
    std::vector<json> fresh;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        // a reset chain starts again from genesis: so does the list
        if (tip >= 0 && m_indexerTip >= 0 && tip < m_indexerTip) { m_events.clear(); m_eventsFrom = 0; }
        m_indexerTip = tip;
        m_eventsError = "";
    }
    for (int page = 0; page < 5 && tip >= 0; page++) {
        uint64_t from; { std::lock_guard<std::recursive_mutex> lk(m_mtx); from = m_eventsFrom; }
        if ((long long)from > tip) break;
        uint64_t to = std::min<uint64_t>(from + 999, uint64_t(tip));
        auto r = lez::rpcCall(*url, "getEvents", json::array({{{"from_block", from}, {"to_block", to}}}), &err, 4000);
        if (!r || !r->is_array()) { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_eventsError = err; break; }
        for (const auto& e : *r) {
            if (!e.is_object()) continue;
            std::string data = b64decode(e.value("data", ""));
            std::string sel = e.value("selector", ""), prog = e.value("program_account_id", "");
            json row = {{"block", e.value("block_id", 0)}, {"txIndex", e.value("tx_index", 0)}, {"txHash", e.value("tx_hash", "")},
                        {"programId", prog}, {"programName", programName(prog)}, {"selector", sel}, {"selectorName", selectorName(sel)},
                        {"dataHex", lez::hex(lez::Bytes(data.begin(), data.end()))}};
            if (printable(data)) row["text"] = data;
            fresh.push_back(row);
        }
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        m_eventsFrom = to + 1;
    }
    if (fresh.empty()) return;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mtx);
        m_events.insert(m_events.end(), fresh.begin(), fresh.end());
        if (m_events.size() > kEventsKept) m_events.erase(m_events.begin(), m_events.begin() + long(m_events.size() - kEventsKept));
    }
    stampHops(fresh);
    if (*m_alive) onLoop([this] { publish(); });
}

// A step's hops are drawn at once from the actors' state diff; when the step's transaction
// finalizes, its acv_hop_ events arrive, in execution order, the same order as the hops: stamp
// each hop with the block that finalized it and the event's own sentence.
void LezActorsModuleImpl::stampHops(const std::vector<json>& fresh) {
    std::map<std::string, std::vector<const json*>> byTx;
    for (const auto& e : fresh) if (e.value("selector", "") == "6163765f686f705f") byTx[e.value("txHash", "")].push_back(&e);
    if (byTx.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    for (auto& [id, j] : m_jobs) {
        auto it = byTx.find(j.txHash);
        if (it == byTx.end() || j.txHash.empty() || !j.result.is_object()) continue;
        json evs = json::array();
        for (const json* e : it->second) evs.push_back({{"block", (*e)["block"]}, {"text", e->value("text", "")}});
        j.result["hopEvents"] = evs;
        j.result["finalizedBlock"] = (*it->second.front())["block"];
        if (j.result.contains("hops") && j.result["hops"].is_array()) {
            auto& hops = j.result["hops"];
            for (size_t i = 0; i < hops.size() && i < evs.size(); i++) {
                hops[i]["finalizedBlock"] = evs[i]["block"];
                hops[i]["eventText"] = evs[i]["text"];
            }
        }
    }
}

// filter: {"program": "<id or name>", "selector": "<hex>", "limit": N}. The newest last.
std::string LezActorsModuleImpl::events(std::string filterJson) {
    json f = json::parse(unquote(filterJson), nullptr, false);
    if (!f.is_object()) f = json::object();
    std::string prog = f.value("program", ""), sel = f.value("selector", "");
    size_t limit = f.contains("limit") && f["limit"].is_number_unsigned() ? f["limit"].get<size_t>() : 300;
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    json out = json::array();
    size_t total = 0;
    for (const auto& e : m_events) {
        if (!prog.empty() && e.value("programId", "") != prog && e.value("programName", "") != prog) continue;
        if (!sel.empty() && e.value("selector", "") != sel) continue;
        total++;
        out.push_back(e);
    }
    if (out.size() > limit) out.erase(out.begin(), out.begin() + long(out.size() - limit));
    return ok({{"events", out}, {"matching", total}, {"indexerTip", m_indexerTip}, {"indexerUrl", indexerUrl()},
               {"available", m_chainState == "running" && lez::childAlive(m_indexerPid)}, {"error", m_eventsError}});
}
