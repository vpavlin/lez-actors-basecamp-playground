# LEZ v0.4 (dev): working notes

What we had to find out to build this playground against LEZ v0.4, the actor model (LEZ `dev`
at `f6afdd285d274b3406b93877b16d85fd849c2df3`). It's written so the work can be redone after LEZ
moves. Anything marked *(verified)* was run against a real local v0.4 sequencer.

## 1. Why not lez_core

The Logos `lez_core` Basecamp module still targets LEZ v0.3.1. Its transactions use the v0.3
format, which a v0.4 sequencer can't read. Porting `lez_core` is upstream work. Instead, this app
links LEZ's own C library, `lez/core_ffi`, built from the pinned LEZ commit (ADR 0001).

## 2. The v0.4 model in one page

- **Transaction.** `TransactionMessage<PublicExecution{ root: RootCall{ to: Actor, message }, fee:
  Option<FeeDeclaration> }>` plus `public_context` and `admission_evidence`.
- **Actor.** An actor is an (account, program) pair. One account holds **one actor state per
  program**: `getAccount` returns `{nonce, data:{actor_states:{<program b58>: [bytes]}}}`. The
  native token balance is just the native-token program's actor state (16 bytes, LE u128).
- **Mentions.** Every actor a call touches is mentioned in the transaction: the root plus every
  actor a tail call reaches. Each mention says whether that account **authorizes**, i.e. signs.
  Programs see this as `input.is_authorized`.
- **Fees.** Every user public tx needs a `FeeDeclaration`, and the **payer signs**. LEZ refuses a
  payer that is a *non-signing* mention: "Fee payer is a non-signing account of this transaction".
  So a mention of the payer must always authorize *(verified)*.
- **Reverts are included and charged.** A guest panic (e.g. `assert!(input.is_authorized)`) still
  puts the tx in a block and still takes the fee; only the state is unchanged. The RPC gives no
  revert flag. We tell a revert by comparing the actor state before and after *(verified)*.
- **IDs.** All ids are 32 bytes, base58 on the wire. The native token program is 32 zero bytes
  (`1111…1`). The program loader is 32 × `0xFE`
  (`JAQ5MVHbCkSYRzXunsrNuM2m1LS859PGveHfoYPAmcvZ`). Built-in guests sit at
  `SHA-256("/LEE-BuiltinProgram/v1/AccountId" + name)`: clock, fee, bridge, bridge_lock,
  cross_zone_inbox, cross_zone_outbox, ping_sender, ping_receiver, sequencer_stake, wrapped_token.
- **Messages are borsh.** `Program::serialize_message` is plain `borsh::to_vec`. Some examples:
  - Native transfer `Message::Transfer{to, amount}` is variant byte `0`, then the 32-byte id,
    then a LE u128.
  - hello_world takes a `Vec<u8>`: u32 LE length, then the bytes.
  - hello_world_with_move_function takes an enum: `0` = Write(Vec), `1` = MoveData{data: Vec, to:
    32 bytes}, `2` = Append(Vec).
  - simple_tail_call takes an `Actor`: the 32-byte account id, then the 32-byte program id.
- **Fee reserve.** The wallet reserves gas_limit (2,000,000) × base_fee_exec plus about 600 bytes
  × base_fee_stor. On the dev chain both base fees are 8, which gives 16,004,800 raw units. An
  account needs amount + reserve to send. The fee actually charged is far lower (4–6 k raw units
  per simple tx) *(verified)*.
- **Credits are async.** A transfer's credit can land a block after inclusion. Judge a transfer by
  polling balances (we poll for up to 60 s), not by inclusion alone.

## 3. Sequencer JSON-RPC (what we use)

`getAccount(b58)` returns the account object, or null if it's not on chain. Also used:
`getLastBlockId`, `getFeeState` (`base_fee_exec`, `base_fee_stor`), and `checkHealth` (`null` =
ok). It's plain HTTP POST JSON-RPC on the sequencer's RPC port (3040 in the dev configs).

## 4. core_ffi (the C ABI)

- Header: `lez/core_ffi/core_ffi.h` (checked in). Function prefixes are `wallet_ffi_*`,
  `sequencer_ffi_*` and `primitives_ffi_*`. Free returned strings with
  `primitives_ffi_free_cstring`; there is no `wallet_ffi_free_string`.
- Wallet lifecycle:
  - `wallet_ffi_create_new(config, storage, password)` returns `{wallet, mnemonic}`.
  - `wallet_ffi_open(config, storage, password, &handle)`. A wrong password is
    `FfiOperationError_WrongPassword` (25).
- Sending: `wallet_ffi_send_generic_public_transaction(handle, mentions, n, root_index, msg, len,
  payer*, &result)`, where `FfiAccountMention = {identity, program_account_id, authorizes,
  address_openings, address_openings_size}`. Get `identity` from
  `wallet_ffi_resolve_public_account(id, needs_sign, &identity)`.
- Inclusion: `wallet_ffi_poll_transaction_status(handle, FfiBytes32 hash, &bool)`.
- Deploy: `wallet_ffi_program_loader_deploy(handle, header*, segments*, n, elf, len, immutable,
  payer*, &result)`. The header and segments are fresh wallet accounts, and the header's id
  becomes the program id. The segment count must match what the loader expects for the ELF
  (hello_world, ~300 KB, took 3). A wrong count is refused *before* sending, so trying 1..8 is
  cheap *(verified)*.
- **Errors.** Return codes are coarse (`InternalError` = 99 covers many causes). The real reason
  only goes to **stderr** (`print_error`). Read the host log, or capture stderr.
- **No public PDA admission evidence** in the generic send. Calls that need
  `PublicAccountEvidence::Pda` (the `tail_call_with_pda` example) can't be built through the C API
  as of this commit.
- **In-process sequencer.** `sequencer_ffi_start_sequencer(runtime*, config_path)` /
  `sequencer_ffi_stop_sequencer`. core_ffi builds sequencer_service **without** its `rpc` feature
  and binds `127.0.0.1:0`, so out of the box an embedded sequencer is reachable only through the
  FFI query functions, not by the wallet. sequencer_service features: `rpc` (JSON-RPC server),
  `standalone` (mock Bedrock: no Bedrock node or Docker), and `prove` (in-process guest execution:
  **no r0vm binary needed**). `SequencerHandle::addr()` gives the bound RPC address when `rpc` is
  on.

## 5. Building core_ffi with Nix

See `flake.nix` at the repo root. The traps:
- LEZ's `rust-toolchain.toml` pins Rust 1.98.1 (needed for the `bit_width` APIs; 1.96 fails). LEZ's
  own locked `rust-overlay` predates 1.98.1, so `fromRustupToolchainFile` fails with "Stable 1.98.1
  is not available". Use your own `rust-overlay` input.
- Inherit `logos-nix`, `crane`, `logos-blockchain-circuits` and `rust-rapidsnark` from
  `lez.inputs`.
- Set `LBC_ROOT_DIR`, `RAPIDSNARK_LIB_DIR`, `LIBCLANG_PATH`, a writable `HOME`, and
  `RECURSION_SRC_PATH`. The last one is the risc0-circuit-recursion zip, pre-fetched from the
  Cargo.lock version, because the build script can't download inside the sandbox.
- Built-in guest ELFs are prebuilt in `artifacts/lez/programs/*.bin`, so **no RISC Zero toolchain
  is needed** to build core_ffi or the sequencer.
- Output: `libcore_ffi.so` (~150 MB unstripped) plus the header. Drop `libcore_ffi.a`.
- In the Basecamp core module, wire it with the builder's `externalLibInputs` and
  `external_libraries: [{name: core_ffi}]` in metadata.json. Quote `"${METADATA_JSON}"` in CMake:
  a `;` in the description otherwise splits the JSON.

## 6. A local v0.4 chain (the standalone binary, without Docker)

```sh
cargo build --release -p sequencer_service --features standalone      # from the LEZ checkout
sequencer_service setup <home> \
  --config lez/sequencer/service/configs/debug/sequencer_config.json \
  --funding-public-key 2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26 \
  --create-channel
RISC0_DEV_MODE=1 sequencer_service start <home>                        # RPC on :3040
```

- Built without `prove`, the binary executes guests via **r0vm 3.0.5**, which must be on PATH
  (`rzup install r0vm 3.0.5`). Without it, genesis panics with a bare "No such file or directory".
- The LEZ integration tests and the tutorial assume a Bedrock node in Docker. `--features
  standalone` avoids that.
- `setup` only writes files: the config with `home` set, `channel_signing_key` (created if
  missing) and `bedrock_funding_public_key`.
- The dev genesis funds two public accounts:

  | Private key | Account | Balance |
  |---|---|---|
  | `7f273098f25b71e6c005a9519f2678da8d1c7f01f6a27778e2d9948abdf901fb` | `CbgR6tj5kWx5oziiFptM7jMvrQeYY3Mzaao6ciuhSr2r` | 10 T |
  | `f434f8741720014586ae43356d2aec6257da086222f604ddb75d69733b86fc4c` | `2RHZhw9h534Zr3eq2RGhQete2Hh667foECzXPmSkGni2` | 20 T |

  These are the LEZ Justfile's `wallet-import-test-accounts` keys, public dev keys and never
  real ones.
- Guest stdout/stderr, including panic messages such as `Missing required authorization`, goes to
  the sequencer's stderr. That's the only place a revert reason appears.

## 7. Example programs (`examples/program_deployment`)

Build: `cargo risczero build --manifest-path examples/program_deployment/methods/guest/Cargo.toml`
(needs the RISC Zero toolchain). The output `.bin` files are about 300 KB each.

| Program | What it shows | How to call it |
|---|---|---|
| hello_world | appends the message to its own actor state | root (acct, prog), msg `Vec<u8>` |
| hello_world_with_authorization | `is_authorized`: reverts unless the account signs | as above; sign on/off |
| hello_world_with_move_function | moves bytes between two accounts' actor states; the source must hold exactly the moved bytes; only the program itself may Append | MoveData mentions [source, destination] |
| simple_tail_call | program-to-program call: sends "Hello from tail call" to a callee actor | mentions [(acct, tail_call), (acct, callee)], msg = callee Actor |
| tail_call_with_pda | PDA authorization granted by the caller | needs `PublicAccountEvidence::Pda`, which isn't in the C API (§4) |

## 8. Basecamp-side lessons from this app

- A headless `logosctl` smoke test can't catch view bugs. Two broke this app in real Basecamp 0.3.1:
  - `short` is a reserved word in QML's JavaScript, so a function named `short` fails to
    compile the whole view;
  - a nested `ColumnLayout` with only `preferredWidth` still fills, and its implicit width
    squeezed its sibling column to zero.

  Always do an Xvfb GUI pass.
- Long-running chain operations (send, poll, deploy) run on a worker thread as jobs (`jobId` now,
  outcome in `job()`). Never block the module's thread.
