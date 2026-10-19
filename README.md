# LEZ Actors - a Basecamp playground

> **Playground / experiment.** An experimental Basecamp app to try out LEZ actor programs. Not a product: it may change or go away. Repo: `vpavlin/lez-actors-basecamp-playground`.

A Basecamp app for exploring and calling **LEZ v0.4 actor programs**: connect to a sequencer, see every
program's **actor state** on an account, **call** a program, send **native transfers**, and watch the
state change. Built on the Logos tech stack; not affiliated with Logos.

It is the chain-interaction half of [jz-progs/prog-tui](https://github.com/jzaki/jz-progs) as a Basecamp
mini-app. Building guests and running a sequencer stay in prog-tui / a CLI: Basecamp plugins are sandboxed.

| Package | What |
|---|---|
| `lez_actors_module` (core) | JSON-RPC reads, actor-state decoding, calls and transfers signed and paid through LEZ's v0.4 wallet library (`core_ffi`) |
| `lez_actors_ui` (view) | One tab: connect bar, accounts, account detail (actor states), programs, call form, transfer form, activity |

## Run a local v0.4 sequencer

v0.4 is LEZ `dev` (the actor model); there is no tag or testnet yet.

```sh
git clone https://github.com/logos-blockchain/logos-execution-zone && cd logos-execution-zone   # branch dev
rzup install r0vm 3.0.5            # the executor the sequencer runs genesis with
RISC0_DEV_MODE=1 just run-sequencer-standalone     # listens on http://127.0.0.1:3040
```

Without `r0vm` 3.0.5 on PATH the sequencer panics at genesis with "No such file or directory".

## Use it

1. **Connect:** the sequencer URL (default `http://127.0.0.1:3040`). The bar shows the block height and base fees.
2. **Wallet:** create one (pick a password; write the recovery phrase down), or unlock it. On a dev chain,
   import a funded test account's private key (LEZ `Justfile`, `wallet-import-test-accounts`).
3. **Payer:** choose the account that pays fees (Pay with).
4. **Programs:** add a deployed program by its header account id (deploy with prog-tui or `wallet
   program-loader deploy`). Its actor states are then labelled with its name.
5. **Call:** program + account + message. `text` sends a borsh `Vec<u8>` of the text (what `hello_world`
   takes), `hex` the exact message bytes. The result shows the state before and after; a call that
   leaves it unchanged **reverted** (it was still included and charged; the reason is in the sequencer log).
6. **Transfer:** native token, raw units. The credit is an async message and can land a block or two
   later; the app waits up to a minute and judges the outcome from both balances.

## Build

```sh
nix build .#core_ffi                     # LEZ dev's v0.4 wallet library (pinned in flake.nix)
(cd core && nix build .#lgx-portable)    # lez_actors_module
(cd ui && nix build .#lgx-portable)      # lez_actors_ui
```

Tests: `core/test/run-tests.sh` (codec unit tests), `scripts/smoke.sh` (headless, against a local sequencer).

Why the wallet goes through `core_ffi` and not `lez_core`: [docs/adr/0001](docs/adr/0001-wallet-through-core-ffi.md).

## Licence

MIT OR Apache-2.0.
