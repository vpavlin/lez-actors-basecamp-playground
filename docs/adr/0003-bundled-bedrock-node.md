# ADR 0003: the local chain follows a real Bedrock node, bundled with the app

**Status:** accepted, 2026-10-26. Changes ADR 0002's `standalone` choice.

## Context

Program events (`Response::event`) are stored by the sequencer only for **finalized** blocks
(`lez/sequencer/core/src/lib.rs`, the finalization update). ADR 0002's chain is `standalone`: a mock
Bedrock that never finalizes past genesis, so the app could never show an event. Its conversation
view traces hops through actor state instead.

Finality needs a Bedrock node. The 2026-10-20 trial with LEZ's pinned node (`3926a532`) finalized, then
stopped for good after a few epochs. The 2026-10-23 soak found why (docs/bedrock-soak-2026-10-23.md):
LEZ turns off the zone SDK's pending-tx expiry (`stale_refund_slots: 0`), so an inscription
underfunded by a per-epoch storage-price step is never replaced.

## Options

1. **Point the app at an external stack** (a node, a sequencer and an indexer the user runs).
   Explorer already can; the Guided tab's "nothing to install" would be gone.
2. **Bundle the node and the indexer** in the core package and run them as child processes next to
   the in-process sequencer.

## Decision

Option 2.
- The root flake builds the Bedrock node (logos-blockchain `6b33abb3`, `--features testing`) and
  LEZ's `indexer_service`, and puts both in core_ffi's `lib/`, which the package carries.
- `patches/0001` moves LEZ to logos-blockchain `6b33abb3` and sets `stale_refund_slots: 30` (the
  zone SDK default). `patches/0002` is ADR 0002's `local-chain` feature without `standalone`.
- `lez_bedrock.hpp` holds the node config (LEZ's single-validator `bedrock/` setup, ported to the
  `6b33abb3` config format) and starts the children:
  - the node on chain port +1 (API), +2 (swarm) and +3 (blend), started first; the sequencer
    waits for its API;
  - the indexer on chain port +4, with `event_filter: archival`, started once the sequencer runs.
- Their data and logs live under `~/.lez-actors/chain/` (`node/`, `indexer/`, `node.log`,
  `indexer.log`), so Reset wipes all three. Stop and Reset stop the children (SIGTERM, then
  SIGKILL after 5 s). A host that crashed leaves them running: the next start kills them by pid
  file, and only if the pid still runs our binary.
- A 0.3.x chain home (standalone) can't be resumed against a node: its checkpoint names a channel
  the node doesn't have. On first start it's moved to `chain.standalone-0.3`.

## Consequences

- Blocks finalize about one block behind the head, and the indexer answers `getEvents` on
  `localChainStatus().indexerUrl`.
- The package grows by two binaries (node ~90 MB, indexer ~40 MB).
- Three more loopback ports. `LEZ_ACTORS_CHAIN_PORT` moves all of them.
- Two patches to carry until LEZ moves its logos-blockchain pin and fixes `stale_refund_slots`.
- An idle chain still has a ~30-slot gap at some epoch boundaries, while a stuck inscription expires
  and is rebuilt. Upstream fixes for that are listed in the soak report.
