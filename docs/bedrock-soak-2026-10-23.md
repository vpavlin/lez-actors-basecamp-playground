# A finalizing local LEZ stack: why finality stopped, and the fix

**For:** the LEZ team and the playground. **From:** the LEZ Actors playground
(vpavlin/lez-actors-basecamp-playground). Follows [bedrock-trial-2026-10-20.md](bedrock-trial-2026-10-20.md).

**Versions:** LEZ `dev` `f6afdd28`; logos-blockchain (Bedrock node and the zone SDK the sequencer
links) moved from `3926a532` to `9786cd8f` and then to `6b33abb3` (master, 2026-10-23).

## Result

- LEZ builds against logos-blockchain `6b33abb3` with no source changes beyond the dependency
  pins. `integration_tests` has to leave the workspace: it needs the old testing framework.
- **The bump alone doesn't fix the stall.** Both `9786cd8f` and `6b33abb3` stop finalizing at an
  epoch boundary, the same way as on 10-20.
- **The cause is in LEZ's sequencer config:** `stale_refund_slots: 0`
  (`lez/sequencer/actors/bedrock/src/actor.rs`, commit `766b1733`). With the zone SDK's default
  of 30, the stack recovers by itself within ~30 slots.
- With that one-line change on `6b33abb3`:
  - 50 minutes (~29 epoch boundaries) with a program call every 20 s: all 150 calls' events came
    back from the indexer's `getEvents`, and finality stayed 1-2 blocks behind the head;
  - 30+ minutes idle: 17 underfunded inscriptions, each one dropped and rebuilt, and finality
    kept going.

## What happens

1. The node's **storage gas price** moves once per epoch (`update_storage_market`, ledger
   `cryptarchia/mod.rs`): ±12.5 %, rounded up with `div_ceil`. On a fresh chain it starts at 1,
   so each step is +1: 1 → 2 → 3 … → 16 over 16 epochs. At these prices one step is +100 % to
   +6 %.
2. The zone SDK **pre-builds and pipelines** inscriptions with a fee for the price at build time
   plus the priority fee (~12 % over cost here: `balance=7466`, cost 6666 at price 5).
3. At the boundary where the price steps from 5 to 6, an inscription built a slot earlier costs
   7870 and carries 7466: the leader skips it with `InsufficientBalance`.
4. Every later inscription names that one as its parent, so they're all `InvalidParent`.
5. **With `stale_refund_slots: 0`** the zone SDK never expires a pending inscription ("Zero
   disables the pending-tx expiry"), so nothing replaces the skipped one: finality stops for good,
   and a restart restores the same pending chain from the checkpoint.
6. **With 30**, after 30 slots `shed_expired` drops the stuck entries ("Pending tx … expired
   unmined; orphaned"), and the sequencer rebuilds from the channel tip with current fees.

Logs of every run are on atlas, in `~/lez-actors/bedrock-soak/run*/`; the node log has
`tx balance check balance=… total_gas_cost=… storage_gas_price=…` at `logos_blockchain::ledger=DEBUG`.

| Run | logos-blockchain | `stale_refund_slots` | Load | Result |
|---|---|---|---|---|
| 1 | 9786cd8f | 0 | idle (the deploy failed) | stalled after ~6 min, at an epoch boundary |
| 2 | 6b33abb3 | 0 | idle (calls failed: "Incorrect fee") | stalled after ~2 min, at the 2nd boundary |
| 3 | 6b33abb3 | 30 | a call every 20 s, 50 min | OK: 150/150 events, no `InsufficientBalance` |
| 4 | 6b33abb3 | 0 | a call every 20 s, 15 min | OK (no inscription happened to be underfunded) |
| 5 | 6b33abb3 | 30 | idle, 30+ min | OK: 17 `InsufficientBalance`, 28 expired and rebuilt |
| 6 | 6b33abb3 | 30 | the app's own chain (0.4.0 under logosctl 0.3.1: in-process sequencer, bundled node and indexer), a call every 20 s, 35 min (2026-10-26) | OK: 105/105 events, the newest within 2-3 blocks of the head |

Run 4 shows the stall isn't certain on every boundary. It needs an inscription built just
before a price step, which on an idle chain happens within a few epochs.

## Suggested upstream fixes

- **LEZ:** set `stale_refund_slots` (the zone SDK default is 30), or make it a sequencer config
  field. The "TODO: set a sensible value" next to it is this bug.
- **Zone SDK:** at an epoch boundary, re-price pending inscriptions that the new storage price
  underfunds, rather than waiting out the expiry window.
- **Ledger:** with `div_ceil` at small prices, the per-epoch step is +1 unit (up to +100 %), far
  more than the 12.5 % the storage market describes. The fee headroom the zone SDK leaves can't
  cover that.

## Config changes for the 6b33abb node

LEZ's `bedrock/node-config.yaml` and `deployment-settings.yaml` were written for `3926a532`.
The newer node refuses them:

- durations need units (#3851): `api.backend.timeout: 30s`, `cryptarchia.service.bootstrap.prolonged_bootstrap_period: 1s`,
  `time.slot_duration: 1s`;
- `tracing.metrics` is a struct (`otlp: null`, `open_metrics: {enabled: false}`), no longer `None`;
- `cryptarchia.faucet_pk` became `consensus_excluded_keys: [<key>]`.

The ported files are in [../bedrock/](../bedrock/), with the scripts that ran the soak
(`run-stack.sh`, `monitor.sh`).

## Other notes

- `getEvents` takes `{from_block, to_block}` or `{tx_hash}`, and refuses a span over 1000 blocks
  ("getEvents block span 1016 exceeds the maximum of 1000"): page through a long chain.
- A v0.4 program call needs a fee: `examples/program_deployment`'s `run_hello_world` builds a
  bare `PublicTransaction` and the sequencer answers "Incorrect fee"; the wallet's
  `send_pub_tx_paid_by` works.
- `program_deployment::deploy_program` counts segments from the `.bin` size, but the loader wants
  `ceil(user ELF / 96 KiB)`: "Program bytecode splits into 3 segment(s) but 4 were supplied".
