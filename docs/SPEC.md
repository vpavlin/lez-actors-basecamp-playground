# Basecamp app spec: "LEZ Actors" — a v0.4 actor-program explorer & caller

**Status:** draft for builder agents. 2026-10-19.
**Origin:** reframes the chain-interaction half of jzaki's `jz-progs/prog-tui` (a local TUI) as a Basecamp mini-app. The build/sequencer-lifecycle half of prog-tui stays a CLI/TUI; it cannot run in Basecamp's sandbox.

## 0. What this is and isn't

**Is:** a Basecamp `ui_qml` app + a `core` module that connects to a LEZ v0.4 sequencer over JSON-RPC, lists deployed actor programs, shows each program's **actor state** on an account, **calls** a program, does **native transfers**, and watches state update. A block-explorer-plus-contract-caller for LEZ actor programs.

**Is not:**
- a toolchain. No `cargo`/`just`/docker, no building guests, no starting/resetting a sequencer. Basecamp plugins are sandboxed. Those stay in prog-tui / a CLI.
- a SPEL client. v0.4 has no SPEL support, so there is no IDL / `spel-client-gen` path. The module hand-builds transactions and decodes `actor_states` itself (port `prog-tui/src/rpc.rs` + the `prog_cli` tx-building logic).

**Reference implementation to port from:** `jz-progs/prog-tui` — `rpc.rs` (AccountView, actor_states decode, getAccount/getLastBlockId/getFeeState/checkHealth), `jobs.rs` (deploy/call/transfer flows and the revert check), and LEZ `examples/program_deployment/src/bin/prog_cli.rs` (how a v0.4 transaction with a fee is built and signed).

## 1. Hard constraints / things to confirm first (a spike before building)

1. **No v0.4 testnet.** Target a local standalone sequencer (`http://127.0.0.1:3040` by default). The app must take an RPC URL. The user runs the sequencer out-of-band (prog-tui or `just run-sequencer-standalone`).
2. **Wallet / signing.** Calls and transfers need a payer that signs and pays a fee. Decide the module's wallet story:
   - **Preferred:** depend on the `lez_core` wallet module (as the AMM module does) and reuse its key management + `send_*_transaction`. Confirm `lez_core` works against a v0.4 (`dev`) sequencer, not just v0.2.x — this is the main unknown.
   - **Fallback:** the module links the v0.4 wallet/tx-building lib directly (what `prog_cli` uses).
   - A spike must answer: can a Basecamp core module submit a v0.4 transaction with a `FeeDeclaration` and a payer today? Everything else is easy; this is the risk.
3. **Transaction shape.** v0.4 messages are `borsh`, programs are addressed by their **deployed header account id** (program loader), and each public user tx carries a fee. Confirm against LEZ `dev` at the commit jz-progs pins.
4. **No guest build in-app.** Deploying a program means submitting an already-built `.bin` (optional, see §3.5) or just calling already-deployed ones. If deploy-in-app is dropped for v1, the app only needs a program's **header account id** typed/pasted in.

## 2. Module API (`lez_actors_module`, type: core, universal)

Every method returns the stable envelope (`{status, error, ...}`), like the AMM module. Hex ids in/out.

| Method | Args | Returns | Notes |
|---|---|---|---|
| `setEndpoint(request)` | `{rpcUrl}` | `{status}` | Also settable via env `LEZ_RPC_URL`. |
| `health()` | — | `{status, blockHeight, baseFeeExec, baseFeeStor}` | `checkHealth` + `getLastBlockId` + `getFeeState`. |
| `getAccount(accountIdHex)` | — | `{status, nonce, balance, actorStates:[{programId, programName?, hex, text?}]}` | Port `AccountView::from_json`; balance = native-token actor state (program id all-zero), LE u128; `text?` when printable. |
| `resolveProgram(headerIdHex)` | — | `{status, programId, exists}` | Confirm a deployed program (header account owned by the program loader). |
| `callProgram(request)` | `{programIdHex, accountIdHex, messageHex, payerHex, sign?}` | `{status, txHash, included, stateBefore, stateAfter, reverted?, revertReason?}` | Submit, wait for inclusion, re-read the target actor state. A tx that leaves the state unchanged reverted (still charged). Surface the reason when the sequencer log is reachable; otherwise "reverted, reason unavailable". |
| `transfer(request)` | `{fromHex, toHex, amount, toPkHex?}` | `{status, txHash, moved, fee, reverted?, revertReason?}` | `auth-transfer`. Credit is async (a Cast) — poll both balances up to ~60 s. Pre-check: sender in wallet, sender holds amount + fee reserve, recipient on-chain or in wallet (else needs `toPkHex`). |
| `listKnownPrograms()` | — | `{status, programs:[{name, headerIdHex, builtin}]}` | Persisted in module state: the built-ins (native token, loader, clock, fee, bridge) + whatever the user has added via `addProgram`. |
| `addProgram(request)` | `{name, headerIdHex}` | `{status}` | Register a deployed program by header id so the UI can label its actor states. |
| `feeReserveEstimate()` | — | `{status, reserve}` | `(gasLimit × baseFeeExec) + (bytes × baseFeeStor)`; port `fee_reserve_estimate`. |

**Out of scope for v1 (document as "later"):** `deployProgram(binPath)` (needs the program-loader segment/header flow and a readable `.bin`; add once §1.4 is decided), events, private accounts.

## 3. QML views (one tab, three panes — mirror prog-tui's layout)

1. **Connect bar:** RPC URL field + Connect; shows block height, base fees, wallet account (payer). A red state when the sequencer is unreachable with a one-line "start a local sequencer" hint.
2. **Accounts pane:** the wallet's accounts; ★ payer, ▶ selected; balance or "off-chain". Buttons: New account, Set payer, Set selected, Transfer.
3. **Account detail:** selected account's nonce, balance, and a row per program's actor state, labelled by program name (from `listKnownPrograms`), shown as text when printable, hex otherwise. This is the heart of it — it makes the actor model visible.
4. **Programs pane:** known programs (built-in dimmed/read-only; user-added normal). Select → Call form.
5. **Call form:** program + target account (prefilled with the selected account) + a message field. For the five examples, prefill the message like prog-tui does (`hello` = text bytes; `move-data`; `tail`/`pda` with a prefilled callee). Generic programs: a raw hex/text message box. Enter sends; the result shows state-before → state-after and, on no change, "reverted: <reason>".

### 3.5 Deploy (optional, later)
If kept: a path/file field for a built `.bin`, submit through the loader, register the returned header id. Gate on the §1.4 decision. For v1, "paste a deployed program's header id" (`addProgram`) is enough to be useful.

## 4. UX rules (from the M1 Basecamp review — don't repeat those bugs)

- **"Submitted" is not "succeeded".** Every call/transfer verifies against chain state and distinguishes included-and-applied from included-and-reverted. Never show a success toast for a reverted tx. Show the tx hash and the real outcome.
- **Decimals / units:** native balances are raw units; label them as such or format consistently. Don't silently trim typed amounts.
- **Refresh after every action;** don't show a launch-time balance next to a fresh pane.
- **Async credits:** a transfer's credit may land a block or two later; poll, don't declare failure early.
- **Locale:** format numbers consistently regardless of the host locale.

## 5. Build / deliverables for the agents

- `lez_actors_module` (core, universal): C++ adapter + the tx/RPC logic (port rpc.rs + prog_cli tx-building). Depends on `lez_core` if §1.2 confirms it works on v0.4.
- `lez_actors_ui` (ui_qml): the views above, calling the module via `logos.callModule` asynchronously, rendering peer/program bytes as plain text.
- A README: run a local v0.4 sequencer (point at prog-tui/jz-progs), set the RPC URL, add a deployed program by header id, call it.
- Tests: the module's decode/encode (actor_states, LE u128 balance, borsh message) as unit tests; a headless `logosctl`/`logoscore` smoke against a local sequencer; a real-Basecamp UI pass (isolated `--user-dir`, screenshots) before shipping.

## 6. Recommendation

Build v1 as **call + inspect + transfer against a local v0.4 sequencer**, programs added by header id, no in-app deploy or build. That is the whole value (seeing and driving actor state) with none of the sandbox-blocked parts, and it carries over to a v0.4 testnet unchanged. Do the §1.2 wallet/signing spike first — it is the one thing that can sink the whole app.
