#pragma once
// The Bedrock side of the local chain (ADR 0003): a Logos blockchain node and LEZ's indexer, run as
// child processes next to the in-process sequencer, so blocks finalize and program events are kept.
// Both binaries ride in the package's lib/ next to libcore_ffi (see flake.nix). The node config is
// LEZ's bedrock/ single-validator setup, ported to logos-blockchain 6b33abb (durations with units,
// consensus_excluded_keys, the metrics struct), on loopback ports derived from the chain port.
#include <dlfcn.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#include <fcntl.h>
#include <fstream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace lez {

inline const char* kNodeConfigTemplate = R"YAML(network:
  backend:
    swarm:
      host: 127.0.0.1
      port: @SWARM@
blend:
  non_ephemeral_signing_key_id: 86c8519f00178e9eb1fe5f4247e4bed77d4c9f6da2fb10e8a1fdd7ba6bc79fa0
  core:
    backend:
      listening_address: /ip4/127.0.0.1/udp/@BLEND@/quic-v1
    zk:
      secret_key_kms_id: 64249c75c2cb813578b75d05b215fc95f67cea5862fff047228183f22e63460e
cryptarchia:
  service:
    bootstrap:
      prolonged_bootstrap_period: 1s
  network:
    network:
      max_connected_peers_to_try_download: 16
      max_discovered_peers_to_try_download: 16
  leader:
    wallet:
      funding_pk: "2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26"
sdp:
  wallet:
    funding_pk: ed266e6e887b9b97059dc1aa1b7b2e19b934291753c6336a163fe4ebaa28e717
kms:
  backend:
    keys:
      2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26: !Zk 6c645cd4636d9c4c36a37a9aeabcaa3300000000000000000000000000000000
      64249c75c2cb813578b75d05b215fc95f67cea5862fff047228183f22e63460e: !Zk 83c851cf4436e8d2fdac33d56d2b235f66431be97e2a20bf241d431713dc720f
      ed266e6e887b9b97059dc1aa1b7b2e19b934291753c6336a163fe4ebaa28e717: !Zk 7364705cd4636d9c4c36a37a9aeabcaa00000000000000000000000000000000
      86c8519f00178e9eb1fe5f4247e4bed77d4c9f6da2fb10e8a1fdd7ba6bc79fa0: !Ed25519 5cd4636d9c4c36a37a9aeabcaa332c3ec796226af0af48a0b2e70167205af749
wallet:
  known_keys:
    2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26: "2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26"
    ed266e6e887b9b97059dc1aa1b7b2e19b934291753c6336a163fe4ebaa28e717: "ed266e6e887b9b97059dc1aa1b7b2e19b934291753c6336a163fe4ebaa28e717"
  voucher_master_key_id: 2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26

pow:
  auto_claim:
    tick:
      unit: seconds
      value: 300
    targets: []

api:
  backend:
    listen_address: 127.0.0.1:@API@
    cors_origins: []
    timeout: 30s
    max_body_size: 10485760
    max_concurrent_requests: 500

tracing:
  logger:
    stdout: true
    stderr: false
    file:
      directory: "./state/logs"
    otlp: null
    loki: null
    gelf: null
  tracing: None
  filter: !Env
    filters:
      "*": WARN
      logos_blockchain::chain: INFO
  metrics:
    otlp: null
    open_metrics:
      enabled: false
  console: None
  level: "INFO"
)YAML";

inline const char* kDeploymentSettings = R"YAML(eras:
  0:
    blend:
      common:
        num_blend_layers: 1
        minimum_network_size: 30
        network_absorption_in_rounds: 2
        data_replication_factor: 1
      core:
        scheduler:
          cover:
            message_frequency_per_round: 1.0
          delayer:
            maximum_release_delay_in_rounds: 1
        target_peering_degree: 4
        verification_rate_per_second: 156
        edge_node_send_deadline_in_rounds: 1
        core_handshake_deadline_in_rounds: 2
        activity_threshold_sensitivity: 1
    cryptarchia:
      epoch_config:
        epoch_stake_distribution_stabilization: 3
        epoch_period_nonce_buffer: 3
        epoch_period_nonce_stabilization: 4
      security_param: 5
      slot_activation_coeff:
        numerator: 1
        denominator: 2
      learning_rate: 0.5
      uncle_reference_window_in_block: 12
      pow_config:
        blend:
          base_difficulty: 19
          target_transactions_per_block: 512
          max_step: 2
          damping_num: 1
          damping_den_offset: 1
        reward:
          reward_pool_genesis: 1000000000
          epoch_reward_genesis: 1000000
          minimum_difficulty: 19
          ema_smoothing_factor: 9
          ema_smoothing_precision: 10
          target_claims_per_block: 100
          rate_num: 0
          rate_den: 1
          target_claim_per_block: 1
          pow_share: 0
          share_den: 100
          slot_window: 100
      sdp_config:
        service_params:
          BN:
            inactivity_period: 2
            epoch: 0
        min_stake:
          threshold: 1
          timestamp: 0
      consensus_excluded_keys:
        - c2a6a4a0981d5bdcf8ddeb8d7934fd8c5510efeb1053f613b45871670b6f7b19
    time:
      slot_duration: 1s
genesis_block:
  header:
    slot: 0
    parent_block: '0000000000000000000000000000000000000000000000000000000000000000'
    body_root: '2d0c91f1530d5771115f23a8fde6e9bc575ae56e271f23c55841c17ce1beaa45'
    proof_of_leadership:
      proof: '0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
      entropy_contribution: '0000000000000000000000000000000000000000000000000000000000000000'
      leader_key: '0000000000000000000000000000000000000000000000000000000000000000'
      voucher_cm: '0000000000000000000000000000000000000000000000000000000000000000'
  signature: '00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
  uncle_headers: []
  transactions:
  - mantle_tx:
      ops:
      - opcode: 0
        payload:
          inputs: []
          outputs:
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 1000000
            pk: '2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26'
          - value: 100000
            pk: '6b2bcd3029fba573cff0c332dc4de7430faf5e261383d693d8dbb5b97665660a'
          - value: 1000000000000
            pk: ed266e6e887b9b97059dc1aa1b7b2e19b934291753c6336a163fe4ebaa28e717
          - value: 18446744073709551615
            pk: c2a6a4a0981d5bdcf8ddeb8d7934fd8c5510efeb1053f613b45871670b6f7b19
      - opcode: 17
        payload:
          channel_id: '0000000000000000000000000000000000000000000000000000000000000000'
          inscription: '05302e322e3123766c6a2d2ddf918544bca603c5a291c7dd1b902d6769ff4b00021506780e075c06051a'
          parent: '0000000000000000000000000000000000000000000000000000000000000000'
          signer: '0000000000000000000000000000000000000000000000000000000000000000'
      - opcode: 32
        payload:
          service_type: BN
          locators:
          - /ip4/65.109.51.37/udp/3400/quic-v1
          provider_id: '59c662860b737f4e2515599adb3434856db8070b373a449ff66955ad3da6b473'
          zk_id: '6b2bcd3029fba573cff0c332dc4de7430faf5e261383d693d8dbb5b97665660a'
          service_note_id: '67e2ec02c536a82f0eb80800fcfb8f2d66c5423d57cd081dfb0f237234f6ac26'
    ops_proofs:
    - !ZkSig
      pi_a: '0000000000000000000000000000000000000000000000000000000000000000'
      pi_b: '00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
      pi_c: '0000000000000000000000000000000000000000000000000000000000000000'
    - !Ed25519Sig '00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
    - !ZkAndEd25519Sigs
      zk_sig:
        pi_a: '0000000000000000000000000000000000000000000000000000000000000000'
        pi_b: '00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
        pi_c: '0000000000000000000000000000000000000000000000000000000000000000'
      ed25519_sig: '00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
)YAML";

// LEZ's debug indexer config with event_filter archival (every program event kept)
inline std::string indexerConfig(int nodeApiPort) {
    return "{\n \"consensus_info_polling_interval\": \"1s\",\n \"bedrock_config\": { \"addr\": \"http://127.0.0.1:" + std::to_string(nodeApiPort) +
           "\" },\n \"channel_id\": \"0101010101010101010101010101010101010101010101010101010101010101\",\n \"allow_chain_reset\": true,\n \"event_filter\": \"archival\"\n}\n";
}

inline std::string nodeConfig(int api, int swarm, int blend) {
    std::string s = kNodeConfigTemplate;
    auto sub = [&](const std::string& k, int v) { for (size_t p; (p = s.find(k)) != std::string::npos;) s.replace(p, k.size(), std::to_string(v)); };
    sub("@API@", api); sub("@SWARM@", swarm); sub("@BLEND@", blend);
    return s;
}

// The directory libcore_ffi was loaded from: the package's lib/, where the node and indexer are.
inline std::string coreFfiLibDir(const void* anySymbol) {
    Dl_info info{};
    if (!dladdr(anySymbol, &info) || !info.dli_fname) return "";
    std::string p = info.dli_fname;
    auto slash = p.rfind('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

// A child process: cwd, extra env, stdout+stderr appended to logPath. Returns the pid, or -1.
// (Not PR_SET_PDEATHSIG: that fires when the spawning THREAD exits. A host that crashed leaves
// its children; the next start kills them by pid file, see killStale.)
inline pid_t spawnChild(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd,
                        const std::vector<std::pair<std::string, std::string>>& env, const std::string& logPath) {
    if (access(exe.c_str(), X_OK) != 0) return -1;
    // everything is built before fork(): the host is multithreaded, so the child only makes
    // async-signal-safe calls until exec
    std::vector<std::string> argStore{exe}; argStore.insert(argStore.end(), args.begin(), args.end());
    std::vector<char*> argv; for (auto& a : argStore) argv.push_back(a.data()); argv.push_back(nullptr);
    std::vector<std::string> envStore;
    for (char** e = environ; e && *e; ++e) {
        std::string kv = *e; bool overridden = false;
        for (const auto& [k, v] : env) if (kv.rfind(k + "=", 0) == 0) overridden = true;
        if (!overridden) envStore.push_back(kv);
    }
    for (const auto& [k, v] : env) envStore.push_back(k + "=" + v);
    std::vector<char*> envp; for (auto& e : envStore) envp.push_back(e.data()); envp.push_back(nullptr);
    pid_t pid = fork();
    if (pid != 0) return pid;
    if (chdir(cwd.c_str()) != 0) _exit(126);
    int fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
    execve(exe.c_str(), argv.data(), envp.data());
    _exit(127);
}

inline void writePidFile(const std::string& path, pid_t pid) { std::ofstream(path, std::ios::trunc) << pid << "\n"; }

// A child a crashed host left behind: the pid in `pidFile`, if that process still runs `exe`.
inline void killStale(const std::string& pidFile, const std::string& exe) {
    std::ifstream f(pidFile);
    long pid = 0;
    if (!(f >> pid) || pid <= 1) return;
    std::ifstream cmd("/proc/" + std::to_string(pid) + "/cmdline");
    std::string argv0;
    std::getline(cmd, argv0, '\0');
    if (argv0 != exe) return;   // not ours (or gone): never signal a stranger
    kill(pid_t(pid), SIGTERM);
    for (int i = 0; i < 50 && kill(pid_t(pid), 0) == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (kill(pid_t(pid), 0) == 0) kill(pid_t(pid), SIGKILL);
}

// Something accepts TCP connections on 127.0.0.1:port.
inline bool portOpen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(uint16_t(port)); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    close(fd);
    return ok;
}

inline bool childAlive(pid_t pid) {
    if (pid <= 0) return false;
    int st = 0;
    return waitpid(pid, &st, WNOHANG) == 0;
}

// SIGTERM, then SIGKILL after `graceMs`; reaps it.
inline void stopChild(pid_t& pid, int graceMs = 5000) {
    if (pid <= 0) return;
    int st0 = 0;
    if (waitpid(pid, &st0, WNOHANG) != 0) { pid = 0; return; }   // gone (or reaped): never signal a reused pid
    kill(pid, SIGTERM);
    for (int waited = 0; waited < graceMs; waited += 100) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) != 0) { pid = 0; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);
    pid = 0;
}

}  // namespace lez
