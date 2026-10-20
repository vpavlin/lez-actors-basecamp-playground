#pragma once
// LezActorsModuleImpl - "LEZ Actors" as a Logos CORE module (universal authoring).
//
// A block-explorer-plus-caller for LEZ v0.4 (the actor model): it connects to a sequencer over
// JSON-RPC, shows each program's actor state on an account, calls programs and does native
// transfers. Transactions are built, signed and paid through LEZ's own v0.4 wallet library
// (lez/core_ffi, linked as an external library): lez_core still targets v0.3.1, whose
// transactions a v0.4 sequencer can't read (docs/adr/0001-wallet-through-core-ffi.md).
//
// Rules (logos-basecamp-module skill):
//  - public methods return std::string (a JSON envelope {status, error?, ...});
//  - at most 4 string args; structured requests pass as ONE JSON string;
//  - no trailing // comments on a declaration line (the interface generator drops the method);
//  - ASCII only in this header;
//  - nothing that blocks longer than a few seconds runs on the module's thread: calls and
//    transfers are background jobs (jobId now, outcome in job()/snapshot() and stateChanged).
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include "logos_module_context.h"
#include "lez_codec.hpp"
#include "lez_logtap.hpp"

class QTimer;
struct WalletHandle;
struct SequencerServiceFFI;

class LezActorsModuleImpl : public LogosModuleContext {
public:
    ~LezActorsModuleImpl() override;

    // --- state ---
    std::string snapshot();
    std::string setEndpoint(std::string rpcUrl);
    std::string health();
    std::string feeReserveEstimate();

    // --- wallet (LEZ v0.4 wallet storage, password-encrypted) ---
    std::string walletStatus();
    std::string createWallet(std::string password);
    std::string unlockWallet(std::string password);
    std::string importKey(std::string privateKeyHex);
    std::string newAccount();
    std::string listAccounts();
    std::string setPayer(std::string accountId);
    std::string setSelected(std::string accountId);

    // --- chain reads ---
    std::string getAccount(std::string accountId);
    std::string resolveProgram(std::string headerId);

    // --- programs the UI can label ---
    std::string listKnownPrograms();
    std::string addProgram(std::string requestJson);
    std::string removeProgram(std::string headerId);

    // --- transactions (background jobs) ---
    std::string callProgram(std::string requestJson);
    std::string transfer(std::string requestJson);
    std::string job(std::string jobId);

    // --- a local v0.4 chain inside this module (the wizard) ---
    std::string localChainStatus();
    std::string startLocalChain();
    std::string stopLocalChain();
    std::string resetLocalChain();
    std::string localChainLog(std::string sinceSeq);
    std::string useDevAccounts();

    // --- the guided examples (LEZ's program-deployment tutorial programs, embedded) ---
    std::string listExamples();
    std::string deployExample(std::string name);
    std::string runExampleStep(std::string requestJson);

protected:
    void onContextReady() override;

logos_events:
    void stateChanged(const std::string& snapshotJson);

private:
    struct Program { std::string name; lez::Id id{}; bool builtin = false; };
    struct Job {
        std::string id, kind, state = "queued", error, txHash;
        lez::json request, result;
        long long createdMs = 0, finishedMs = 0;
    };

    // data dir, persisted settings
    void setupDataDir();
    void loadSettings();
    void saveSettings();
    void writeWalletConfig();
    std::string walletConfigPath() const;
    std::string walletStoragePath() const;

    // the worker thread: the only place the wallet library is called
    std::string runOnWorker(std::function<lez::json()> fn, int timeoutMs);
    void enqueue(std::function<void()> fn);
    void workerLoop();
    void runCall(Job& j);
    void runTransfer(Job& j);
    void runDeploy(Job& j);
    void runExample(Job& j);
    void attachFlow(Job& j, const std::string& ex, const std::string& step, const lez::Id& a, const lez::Id& b, const lez::Id& prog);
    std::vector<std::string> newLines(const lez::Bytes& before, const lez::Bytes& after);
    uint64_t blockOfTx(const std::string& txHashHex);
    std::string sendPublic(std::vector<std::pair<lez::Id, std::pair<lez::Id, bool>>> actors, const lez::Bytes& message,
                           const lez::Id& payer, bool* included, std::string* err);
    std::string revertReason(long long logSeqBefore);
    std::vector<std::string> ensureExampleAccounts(std::string* err);
    std::optional<lez::Id> exampleProgram(const std::string& name);
    void startChainThread(bool reset);
    std::string startJob(const std::string& kind, const lez::json& request);
    void finishJob(const std::string& id, std::function<void(Job&)> update);

    // chain
    std::optional<lez::AccountView> fetchAccount(const lez::Id& id, std::string* err, bool* onChain);
    lez::json describeAccount(const lez::Id& id, const std::optional<lez::AccountView>& v, bool onChain);
    std::string programName(const std::string& programB58);
    bool walletHolds(const lez::Id& id);

    void publish();
    void onLoop(std::function<void()> fn);
    lez::json buildSnapshot();

    std::recursive_mutex m_mtx;
    std::string m_dataDir;
    std::string m_rpcUrl = "http://127.0.0.1:3040";
    std::string m_payer, m_selected;
    std::vector<Program> m_programs;
    std::map<std::string, Job> m_jobs;
    std::vector<std::string> m_jobOrder;
    long long m_jobSeq = 0;

    // wallet (touched only on the worker thread, except the handle pointer under m_mtx)
    WalletHandle* m_wallet = nullptr;
    std::vector<std::string> m_accounts;
    // kept while the wallet is unlocked, only to reopen it when the sequencer address changes
    // (the wallet library reads that address at open)
    std::string m_walletPw;
    std::vector<std::string> m_helperAccounts;     // deploy header/segment accounts: not listed
    std::vector<std::string> m_exampleAccounts;    // "example account 1/2"
    std::map<std::string, std::string> m_examplePrograms;   // example name -> deployed program id

    // the local chain
    lez::LogTap m_tap;
    SequencerServiceFFI* m_chain = nullptr;
    std::string m_chainState = "stopped";   // stopped | starting | running | stopping | failed | external
    std::string m_chainError;
    std::thread m_chainThread;

    QTimer* m_loop = nullptr;
    std::thread m_worker;
    std::mutex m_qMtx;
    std::condition_variable m_qCv;
    std::deque<std::function<void()>> m_queue;
    std::atomic<bool> m_stop{false};
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
