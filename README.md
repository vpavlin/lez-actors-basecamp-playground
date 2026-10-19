# LEZ Actors - a Basecamp playground

> **Playground / experiment.** An experimental Basecamp app to try out LEZ actor programs. Not a product: it may change or go away. Repo: `vpavlin/lez-actors-basecamp-playground`.

A Basecamp app for exploring and calling **LEZ v0.4 actor programs**: connect to a sequencer, see every
program's **actor state** on an account, **call** a program, send **native transfers**, and watch the
state change. Built on the Logos tech stack; not affiliated with Logos.

It has two tabs:

- **Guided** walks you through v0.4 with nothing to install: it starts a **local v0.4 chain inside the
  app**, imports the dev chain's funded accounts, **deploys LEZ's tutorial example programs** (built in)
  and runs each one's steps. Every step says what it should do, and the result shows each mentioned actor's
  state before and after, the fee, and, for a revert, the guest's panic message.
- **Explorer**: accounts, any program by id, raw calls (text/hex messages), native transfers, activity.
  It works against the built-in chain or any v0.4 sequencer you run yourself.

| Package | What |
|---|---|
| `lez_actors_module` (core) | LEZ's v0.4 wallet library (`core_ffi`) and an embedded single-node sequencer; JSON-RPC reads, actor-state decoding, calls, transfers, deploys, the guided examples |
| `lez_actors_ui` (view) | Guided + Explorer tabs |

What we learned about v0.4 while building it (the model, fees, the C API, building it, its traps):
[docs/LEZ-V04-NOTES.md](docs/LEZ-V04-NOTES.md).

## The Guided tab

1. **Start the local chain.** It's LEZ's sequencer, built into the core with `--features standalone,prove`:
   no Docker, no Bedrock node, no r0vm. Guests run without real proofs (`RISC0_DEV_MODE`). It listens on
   `127.0.0.1:3040` with a block every 3 s, and keeps its data in `~/.lez-actors/chain` until you
   **Reset** it. If something already answers on :3040, the app uses that instead.
2. **Create a wallet** (it signs and pays).
3. **Use the dev accounts**: the genesis-funded test keys from LEZ's Justfile. The 20 T one pays the fees.
4. **The examples** (LEZ `examples/program_deployment`, prebuilt in `core/examples/`):

   | Example | Steps |
   |---|---|
   | `hello_world` | write to its actor state on an account |
   | `hello_world_with_authorization` | unsigned (reverts, still charged), then signed (applies) |
   | `hello_world_with_move_function` | write, move bytes to a second account, move bytes the source doesn't hold (reverts), Append from outside (reverts) |
   | `simple_tail_call` | one program calling another (`hello_world`) on the same account |

   Steps run on two example accounts of their own, which never pay fees, so whether they sign is up
   to the step. `tail_call_with_pda` isn't included: core_ffi's C API can't send PDA admission
   evidence yet.

## Your own sequencer

The Explorer tab works with any v0.4 (LEZ `dev`) sequencer:

```sh
cargo build --release -p sequencer_service --features standalone   # in a LEZ dev checkout
sequencer_service setup <home> --config lez/sequencer/service/configs/debug/sequencer_config.json \
  --funding-public-key 2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26 --create-channel
RISC0_DEV_MODE=1 sequencer_service start <home>     # http://127.0.0.1:3040; needs r0vm 3.0.5 on PATH
```

In Explorer: connect, create or unlock a wallet, import a funded key, choose the payer, and add
programs by header id. A call that leaves the state unchanged **reverted**: it was still included and
charged. Transfers are judged from both balances, since the credit can land a block or two later.

## Build

```sh
nix build .#core_ffi                     # LEZ dev's v0.4 wallet library (pinned in flake.nix)
(cd core && nix build .#lgx-portable)    # lez_actors_module
(cd ui && nix build .#lgx-portable)      # lez_actors_ui
```

Tests: `core/test/run-tests.sh` (codec and example-message unit tests), `scripts/smoke-wizard.sh` (headless:
the Guided path end to end, on the built-in chain), `scripts/smoke.sh` (headless, Explorer calls against a
sequencer on :3040).

The embedded chain comes from a small patch to core_ffi (`patches/0001-core-ffi-local-chain.patch`,
[docs/adr/0002](docs/adr/0002-embedded-local-chain.md)).

Why the wallet goes through `core_ffi` and not `lez_core`: [docs/adr/0001](docs/adr/0001-wallet-through-core-ffi.md).

## Licence

MIT OR Apache-2.0.
