# ADR 0001 - Sign and pay through LEZ's v0.4 wallet library (core_ffi), not lez_core

Status: accepted (0.1.0). Recorded 2026-10-19.

## Context
The app has to build, sign and pay LEZ v0.4 (actor-model) transactions: every user public transaction
carries a `FeeDeclaration` inside the signed message, and the payer must sign. The spec preferred the
`lez_core` wallet module, as the AMM module uses it, if it works on a v0.4 (`dev`) sequencer.

A spike (2026-10-19) found:
- `lez_core` (logos-blockchain/logos-execution-zone-module) wraps LEZ v0.3.1's `wallet-ffi` on `main`;
  no branch targets `dev`. v0.4's public message is a different structure
  (`TransactionMessage<PublicExecution{root: RootCall{to: Actor, message}, fee}>` plus admission
  evidence), and the FFI's `wallet_ffi_send_generic_public_transaction` kept its name but changed its
  arguments (`root_mention`, `message` instead of `instruction_data`, `program_account_id`). A
  v0.3.1-built `lez_core` cannot produce a transaction a v0.4 sequencer accepts.
- On `dev`, `wallet-ffi` was replaced by `lez/core_ffi`, but `dev`'s `flake.nix` still builds the old
  crate, so the Nix package `lez_core` consumes is broken there.
- Linking `core_ffi` directly works: on a standalone `dev` sequencer a native transfer A to C with a
  separate payer B (co-signing only the fee) was included and applied (A -1000, C +1000, B -fee).

## Decision
- The core module links `core_ffi` as an external library. The repo-root flake builds it from a pinned
  LEZ `dev` commit with LEZ's own inputs and build environment (circuits, rapidsnark, the pre-fetched
  risc0 recursion zkr), `-p core_ffi`, shipping `core_ffi.h`.
- All wallet-library calls run on one worker thread in the module. Calls and transfers are background
  jobs (they wait for inclusion and for async credits, up to a minute), so no module call blocks past
  the IPC timeout.
- Chain reads (`getAccount`, `getLastBlockId`, `getFeeState`, `checkHealth`) use the sequencer's
  JSON-RPC directly, so they work before the wallet is unlocked.

## Consequences
- The app has its own wallet (its own keys, in its data dir), not the one other LEZ apps share. When
  `lez_core` gains v0.4 support, moving onto it lets apps share one wallet; the module's wallet calls
  are in one place to make that a contained change.
- Bumping LEZ is a pin change in the root flake plus a rebuild (~30 min, ~1000 crates).
- https sequencers (a future testnet) need a TLS-capable RPC client; v1 is http only (local).
