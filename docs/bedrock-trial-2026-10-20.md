# LEZ dev + a local Bedrock node: finality stops after ~25 minutes

**For:** the LEZ team. **From:** the LEZ Actors playground (vpavlin/lez-actors-basecamp-playground).
**Versions:** LEZ `dev` at `f6afdd285d274b3406b93877b16d85fd849c2df3` (the `dev` head as of
2026-10-20), and Bedrock `logos-blockchain` at `3926a532b9d96d62747da064161a115a7c0c5a3e` (LEZ's pin),
built with `--features testing`.

## Setup

One host, nothing else on the network:
- Bedrock: `bedrock/node-config.yaml` and `bedrock/deployment-settings.yaml` from LEZ, run natively
  with `POL_PROOF_DEV_MODE=true`. Only the ports changed: API 18180, swarm 3100, blend 3500.
- The sequencer: `sequencer_service` built without `standalone`. `setup` used
  `configs/debug/sequencer_config.json` with `node_url` http://127.0.0.1:18180,
  `block_create_timeout` 3s, `--funding-public-key 2e03b2ef…0d26 --create-channel`. Started with
  `RISC0_DEV_MODE=1`.
- The indexer: `configs/debug/indexer_config.json` pointed at the same node, `event_filter: archival`.

## What works

- Blocks finalize about one block behind the head: "Channel update: … finalized N..=N".
- `getEvents` on the indexer returns program events (`Response::event`) from finalized blocks,
  with block id and tx hash.
- `getBlock` carries `bedrock_status` (the last byte of the borsh block: 0 Pending, 1 Safe,
  2 Finalized).

## What breaks

After about 25 minutes (here at 11:23:46 UTC, slot 7002400, which is also an epoch transition
70023 → 70024), finality stops for good:

- The sequencer keeps producing L2 blocks (2,500+ later) and keeps submitting inscriptions.
- The Bedrock leader removes every one of them: `proposed block … with 0 transactions (N removed)`.
  With `logos_blockchain::chain::leader=TRACE`, the reason is
  `ChannelVerificationError(InvalidParent { parent: …, actual: 6917c051… })`, thousands of times.
  There were also 3 `InsufficientBalance` rejections.
- The channel tip on Bedrock (`GET /channel/0101…`) stays at `tip_message 6917c051…`,
  `tip_slot 7002397`. Its `posting_timeframe` and `posting_timeout` read 0, although the genesis
  `channel_params` set 300 and 25 (possibly unrelated).
- The funding wallet is fine: 45 M across 48 notes.
- The sequencer logs `WARN … Skipping turn: Channel moved to sequence N while a block on sequence
  N-1 was being built` repeatedly.
- Restarting the sequencer doesn't recover. It logs `Restoring from checkpoint: 2205 pending txs`
  and resubmits the same broken chain. Ten minutes later finality still hasn't moved.

**Our reading:** the zone SDK pipelines inscriptions, each one's parent being the previous
pending one. The inscription right after the channel tip was lost (possibly the
`InsufficientBalance` ones: a fee note spent twice around the epoch change), so every later
pending inscription has a parent Bedrock never saw, forever. Nothing re-bases the pending chain
on the actual tip, and a restart restores it from the checkpoint as it was.

## Probably already fixed in Bedrock

`logos-blockchain` has 43 commits after `3926a53`. Several touch exactly this:
- `583cb0dc` feat(zone-sdk): track channel notes through BlockStates (#3828)
- `3789f5da` feat(zone-sdk): keep the channel lineage per block in BlockStates (#3833)
- `c706cf27` fix(zone-sdk): retire pending entries by message id on resume backfill (#3826)
- `d81ba5aa` fix(zone-sdk): backfill an unseen reported tip before adopting it (#3825)
- `6a868119` fix(zone-sdk): classify re-mirrored txs one at a time (#3824)

## Bumping LEZ to the newest Bedrock (9786cd8f)

See the end of this file for how that went.
