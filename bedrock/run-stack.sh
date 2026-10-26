#!/usr/bin/env bash
# Fresh finalizing LEZ stack for the soak: Bedrock node -> sequencer (no standalone) -> indexer
# (archival) -> wallet -> event soak. Ports: node 18180, seq 3140 (+metrics 9141), indexer 8879.
set -e
S=${SOAK_DIR:-$(cd "$(dirname "$0")" && pwd)}; B=${LEZ_BIN:-/mnt/build/lez-bump/release}; NODE=${BEDROCK_NODE:-/mnt/build/bedrock-node-9786/release/logos-blockchain-node}
cd $S; rm -rf node/state seq idx wallet/storage.json; mkdir -p idx wallet
export SOAK_MARK=wb-bedrock-soak
cd node; POL_PROOF_DEV_MODE=true setsid nohup $NODE node-config.yaml --deployment deployment-settings.yaml > ../node.log 2>&1 < /dev/null & echo $! > ../node.pid; cd ..
for i in $(seq 60); do curl -s -m2 http://127.0.0.1:18180/cryptarchia/info >/dev/null && break; sleep 1; done
$B/sequencer_service setup $S/seq --config $S/seq-config.json --funding-public-key 2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26 --create-channel > setup.log 2>&1
RUST_LOG=info,kameo=warn RISC0_DEV_MODE=1 setsid nohup $B/sequencer_service start $S/seq --rpc-listen-address 127.0.0.1:3140 --metrics-address 127.0.0.1:9141 > seq.log 2>&1 < /dev/null & echo $! > seq.pid
sleep 20
RUST_LOG=info setsid nohup $B/indexer_service indexer.json -p 8879 -d $S/idx > idx.log 2>&1 < /dev/null & echo $! > idx.pid
export LEE_WALLET_HOME_DIR=$S/wallet LEE_WALLET_PASSWORD=soak-test
for k in 7f273098f25b71e6c005a9519f2678da8d1c7f01f6a27778e2d9948abdf901fb f434f8741720014586ae43356d2aec6257da086222f604ddb75d69733b86fc4c; do $B/wallet account import public --private-key $k > /dev/null; done
TARGET=$($B/wallet account new public | grep -o "Public/[A-Za-z0-9]*" | head -1 | cut -d/ -f2)
echo "target $TARGET"
setsid nohup ./monitor.sh > monitor.log 2>&1 < /dev/null & echo $! > monitor.pid
[ -n "$NO_SOAK" ] || setsid nohup $B/run_event_soak /mnt/build/lez-bump/riscv-guest/example_program_deployment_methods/example_program_deployment_programs/riscv32im-risc0-zkvm-elf/release/hello_event.bin $TARGET 2RHZhw9h534Zr3eq2RGhQete2Hh667foECzXPmSkGni2 ${SOAK_MIN:-50} 20 > soak.log 2>&1 < /dev/null & echo $! > soak.pid
echo started
