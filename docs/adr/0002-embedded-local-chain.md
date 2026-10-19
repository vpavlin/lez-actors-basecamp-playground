# ADR 0002: the local chain runs inside the core module

**Status:** accepted, 2026-10-19

## Context

A newcomer who wants to see v0.4 behave has to: build LEZ's sequencer, install r0vm 3.0.5, set up a
genesis home, start it, find funded keys, build the example guests with the RISC Zero toolchain, deploy
them, then call them. That's most of the effort, and none of it is v0.4 itself. The app should do it.

## Options

1. **Download a prebuilt sequencer + r0vm** and run them as child processes. That's two more
   artifacts per platform to build, host, pin and verify; the app would run downloaded binaries; and
   the sequencer binary needs system libraries (libpcsclite, a recent glibc).
2. **Embed the sequencer in the core library.** core_ffi already contains LEZ's sequencer
   (`sequencer_ffi_start_sequencer`), and the wallet library is linked anyway.

## Decision

Option 2. A small patch (`patches/0001-core-ffi-local-chain.patch`, applied by the root flake):
- adds a `local-chain` feature to core_ffi, turning on sequencer_service's `rpc` (the JSON-RPC
  server, so the wallet talks to it like any sequencer), `standalone` (a mock Bedrock: no node, no
  Docker) and `prove` (in-process guest execution: no r0vm);
- adds `sequencer_ffi_local_setup(home, template, funding_key)`, which does what `sequencer_service
  setup --create-channel` does, and `sequencer_ffi_start_local(home, port)`, which serves RPC on
  127.0.0.1:port. Upstream binds port 0 and keeps RPC off.

The example guests are embedded too (`core/examples/*.bin`, about 300 KB each), so deploying needs no
toolchain. The module tees its stdout/stderr (`LogTap`) to show the chain's log and to read revert
reasons (guest panics) and the wallet library's error details, which are reported nowhere else.

## Consequences

- No downloads; the chain is as portable as the core library. The library grows by the sequencer
  (RPC server, mock Bedrock, RocksDB).
- The chain runs in the module's host process: stopping Basecamp stops it, and its data stays in
  `~/.lez-actors/chain`.
- Port 3040 is fixed. If it's taken, the app uses whatever answers there.
- The patch has to be carried when the LEZ pin moves (two features, ~100 lines). Upstreaming the two
  functions would remove it.
