# A finalizing LEZ stack, outside the app

The configs and scripts behind [docs/bedrock-soak-2026-10-23.md](../docs/bedrock-soak-2026-10-23.md):
a Bedrock node, LEZ's sequencer (without `standalone`) and LEZ's indexer (`event_filter: archival`),
each its own process, on loopback.

| File | What |
|---|---|
| `node-config.yaml`, `deployment-settings.yaml` | LEZ's `bedrock/` single-validator node, ported to logos-blockchain `6b33abb3` (durations with units, `consensus_excluded_keys`, the `metrics` struct). API on 18180, swarm 3100, blend 3500 |
| `seq-config.json` | LEZ's debug sequencer config: node on 18180, a block every 3 s, gossip on loopback |
| `indexer.json` | LEZ's debug indexer config with the node on 18180 |
| `run-stack.sh` | a fresh stack (node -> sequencer on 3140 -> indexer on 8879), a wallet with the dev keys, and the event soak |
| `monitor.sh` | once a minute: node slot, finalized L2 block, `hello_event` events in the indexer, `InvalidParent` count |

The binaries come from a LEZ checkout with `patches/0001` applied (and, for the soak, `patches/extra/soak-tools-hello-event.patch`: the `hello_event` guest and
`run_event_soak`), built with `cargo build --release -p sequencer_service
-p indexer_service -p wallet -p program_deployment`, and a logos-blockchain `6b33abb3` checkout built
with `cargo build --release -p logos-blockchain-node --features testing`. The scripts point at the
build directories used on 2026-10-23: set `LEZ_BIN` and `BEDROCK_NODE`. Run it from a work directory,
not from here (it deletes the stack's state in that directory on each start):

```sh
mkdir -p /tmp/lez-stack/node && cp node-config.yaml deployment-settings.yaml /tmp/lez-stack/node/
cp seq-config.json indexer.json run-stack.sh monitor.sh /tmp/lez-stack/
SOAK_DIR=/tmp/lez-stack LEZ_BIN=... BEDROCK_NODE=... /tmp/lez-stack/run-stack.sh   # NO_SOAK=1: idle
```

The app itself doesn't use these files: its core module writes the same configs from
`core/src/lez_bedrock.hpp` and runs the bundled binaries ([docs/adr/0003](../docs/adr/0003-bundled-bedrock-node.md)).
