#!/usr/bin/env bash
# Headless smoke test of the Guided tab's path: lez_actors_module under logosctl starts its own
# in-process v0.4 chain, imports the dev accounts, deploys every example and runs every step,
# checking each step's outcome against what it says it does.
#   scripts/smoke-wizard.sh <lez_actors_module.lgx>
# Needs logosctl 0.3.x (LOGOSCTL) and a free 127.0.0.1:$CHAIN_PORT (default 3041; if something
# answers there, the module uses it instead of its own chain, and this test says so).
set -u
LGX=${1:?the lez_actors_module .lgx}
L=${LOGOSCTL:-logosctl}
BASE=${SMOKE_DIR:-/tmp/lez-actors-smoke-wizard}
pass=0; fail=0
check() { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "  ok   $2"; else fail=$((fail+1)); echo "  FAIL $2"; fi; }
# its own port, so it never touches another local sequencer on LEZ's usual 3040 (prog-tui's)
PORT=${CHAIN_PORT:-3041}
ctl() { HOME="$BASE/home" LOGOSCTL_CONFIG_DIR="$BASE/cfg" QT_QPA_PLATFORM=offscreen LEZ_ACTORS_CHAIN_PORT=$PORT LEZ_RPC_URL=http://127.0.0.1:$PORT timeout 90 "$L" "$@"; }
res() { python3 -c 'import json,sys
raw=sys.stdin.read().strip().splitlines(); d=json.loads(raw[-1]) if raw else {}
r=d.get("result",d)
for _ in range(3):
    if isinstance(r,str):
        try: r=json.loads(r)
        except Exception: break
print(json.dumps(r))'; }
call() { local m=$1; shift; ctl call lez_actors_module "$m" "$@" -j 2>/dev/null | res; }
field() { python3 -c 'import json,sys; d=json.loads(sys.stdin.read()); v=d
for k in sys.argv[1].split("."):
    v = v[int(k)] if isinstance(v,list) else (v.get(k) if isinstance(v,dict) else None)
print(v if not isinstance(v,(dict,list)) else json.dumps(v))' "$1"; }
waitjob() { local J S; for i in $(seq 1 90); do J=$(call job str:$1); S=$(echo "$J" | field state); case "$S" in applied|reverted|failed|unknown) break;; esac; sleep 2; done; echo "$J"; }

[ -d "$BASE/cfg" ] && ctl daemon stop >/dev/null 2>&1
rm -rf "$BASE"; mkdir -p "$BASE/home"
ctl daemon start --detach >/dev/null
HOME="$BASE/home" LOGOSCTL_CONFIG_DIR="$BASE/cfg" QT_QPA_PLATFORM=offscreen LEZ_ACTORS_CHAIN_PORT=$PORT timeout 600 "$L" install "$LGX" -y >/dev/null || echo "install failed"
ctl module load lez_actors_module >/dev/null
sleep 2

echo "== the local chain"
S=$(call startLocalChain); echo "  $S" | cut -c1-200
for i in $(seq 1 60); do ST=$(call localChainStatus | field state); case "$ST" in running|failed|external) break;; esac; sleep 1; done
[ "$ST" = running ]; check $? "the in-process chain is running (state: $ST $(call localChainStatus | field error))"
H=$(call health); [ "$(echo "$H" | field reachable)" = True ]; check $? "it answers JSON-RPC on 127.0.0.1:$PORT (block $(echo "$H" | field blockHeight))"
# ADR 0003: a real Bedrock node and an indexer run next to it
CS=$(call localChainStatus)
[ "$(echo "$CS" | field nodeRunning)" = True ]; check $? "the bundled Bedrock node runs ($(echo "$CS" | field nodeUrl))"
[ "$(echo "$CS" | field indexerRunning)" = True ]; check $? "the bundled indexer runs ($(echo "$CS" | field indexerUrl))"
IDX=$(echo "$CS" | field indexerUrl)
evq() { curl -s -m10 -X POST -H 'content-type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getEvents\",\"params\":[{\"from_block\":$1}]}" "$IDX"; }
for i in $(seq 1 60); do N=$(evq 0 | python3 -c 'import json,sys; print(len(json.load(sys.stdin).get("result",[])))' 2>/dev/null); [ "${N:-0}" -gt 0 ] && break; sleep 2; done
[ "${N:-0}" -gt 0 ]; check $? "blocks finalize: the indexer has the genesis events ($N)"

echo "== wallet + dev accounts"
W=$(call createWallet str:wizard-pass); [ "$(echo "$W" | field status)" = ok ]; check $? "createWallet"
D=$(call useDevAccounts); echo "  $D" | cut -c1-200
[ "$(echo "$D" | field status)" = ok ]; check $? "useDevAccounts imports the funded dev accounts"
A=$(call listAccounts)
echo "$A" | python3 -c 'import json,sys; a=json.load(sys.stdin)["accounts"]; p=[x for x in a if x["accountId"]=="2RHZhw9h534Zr3eq2RGhQete2Hh667foECzXPmSkGni2"]; sys.exit(0 if p and int(p[0]["balance"])>0 else 1)'
check $? "the 20 T dev account is on chain and funded"
[ "$(echo "$A" | field payer)" = 2RHZhw9h534Zr3eq2RGhQete2Hh667foECzXPmSkGni2 ]; check $? "it is the payer"

echo "== the examples"
for ex in hello_world hello_world_with_authorization hello_world_with_move_function simple_tail_call actor_conversation; do
  J=$(call deployExample str:$ex); JOB=$(echo "$J" | field jobId)
  J=$(waitjob "$JOB"); echo "  deploy $ex: $(echo "$J" | field state) $(echo "$J" | field error | cut -c1-200)"
  [ "$(echo "$J" | field state)" = applied ]; check $? "deploy $ex"
done
E=$(call listExamples)
echo "$E" | python3 -c 'import json,sys; e=json.load(sys.stdin)["examples"]; sys.exit(0 if all(x["deployed"] for x in e) else 1)'; check $? "listExamples shows all five deployed"

steps() { echo "$E" | python3 -c 'import json,sys; [print(x["name"], s["id"], s["expect"]) for x in json.load(sys.stdin)["examples"] for s in x["steps"]]'; }
while read -r ex step expect; do
  C=$(call runExampleStep "str:{\"example\":\"$ex\",\"step\":\"$step\"}"); JOB=$(echo "$C" | field jobId)
  if [ -z "$JOB" ] || [ "$JOB" = None ]; then check 1 "$ex/$step: $(echo "$C" | field error)"; continue; fi
  J=$(waitjob "$JOB")
  ST=$(echo "$J" | field state); AE=$(echo "$J" | field result.asExpected); WHY=$(echo "$J" | field result.revertReason)
  echo "  $ex/$step -> $ST ${WHY:+($WHY)} $(echo "$J" | field error | cut -c1-160)"
  [ "$ST" = "$expect" ] && [ "$AE" = True ]; check $? "$ex/$step is $expect, as it says"
  if [ "$ex/$step" = actor_conversation/ask ]; then
    echo "$J" | python3 -c 'import json,sys; h=json.load(sys.stdin)["result"].get("hops",[]); print("   hops:", *[x["text"][:100] for x in h], sep="\n     "); sys.exit(0 if len(h)==2 and all(x["sameTx"] for x in h) and h[0]["text"].startswith("->") and h[1]["text"].startswith("<-") else 1)'
    check $? "ask: two hops (request ->, reply <-) on the one actor, in the one transaction"
  fi
  if [ "$ex/$step" = actor_conversation/ping ]; then
    echo "$J" | python3 -c 'import json,sys; r=json.load(sys.stdin)["result"]; h=r.get("hops",[]); print("   ping hops:", *[("same tx  " if x["sameTx"] else "LATER    ")+x["text"][:80] for x in h], sep="\n     "); sys.exit(0 if len(h)==2 and all(x["sameTx"] for x in h) and h[1]["text"].startswith("<- Pong") else 1)'
    check $? "ping to a declared actor: Pong delivered in the same transaction (promoted)"
  fi
  if [ "$expect" = reverted ]; then echo "$WHY" | grep -q "fee was charged" && ! echo "$WHY" | grep -q "no guest panic"; check $? "$ex/$step: the revert reason is the guest's panic message"; fi
done < <(steps)

# the move really moved: account 1 empty under the move program, account 2 holds the bytes
EA=$(call snapshot | field exampleAccounts)
A2=$(echo "$EA" | python3 -c 'import json,sys; print(json.load(sys.stdin)[1])')
MP=$(echo "$E" | python3 -c 'import json,sys; print([x["programId"] for x in json.load(sys.stdin)["examples"] if x["name"]=="hello_world_with_move_function"][0])')
G=$(call getAccount str:$A2)
echo "$G" | python3 -c 'import json,sys; g=json.load(sys.stdin); s=[a for a in g["actorStates"] if a["programId"]==sys.argv[1]]; sys.exit(0 if s and s[0].get("text","").startswith("move me") else 1)' "$MP"
check $? "example account 2 holds the moved bytes under hello_world_with_move_function"

L2=$(call localChainLog str:0); [ "$(echo "$L2" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["lines"]))')" -gt 0 ]; check $? "localChainLog returns the chain's log"

echo "== program events (the finalized record)"
for i in $(seq 1 45); do EV=$(call events 'str:{"selector":"6163765f686f705f"}'); N=$(echo "$EV" | field matching); [ "${N:-0}" -ge 4 ] && break; sleep 2; done
echo "  $(echo "$EV" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("matching"), "hop events, finalized up to block", d.get("indexerTip")); [print("   ", e["block"], e.get("selectorName"), "|", e.get("text","")[:90]) for e in d.get("events",[])[-4:]]')"
[ "${N:-0}" -ge 4 ]; check $? "events() has the actor_conversation hops' events once their blocks finalize ($N)"
SNAP=$(call snapshot)
echo "$SNAP" | python3 -c 'import json,sys
jobs=[j for j in json.load(sys.stdin)["jobs"] if j["request"].get("example")=="actor_conversation" and j["request"].get("step")=="ask" and j["state"]=="applied"]
h=jobs[0]["result"]["hops"] if jobs else []
sys.exit(0 if h and all("finalizedBlock" in x and x.get("eventText") for x in h) else 1)'
check $? "the ask step's hops are stamped finalized, each with its event's text"
echo "$SNAP" | python3 -c 'import json,sys
jobs=[j for j in json.load(sys.stdin)["jobs"] if j["request"].get("example")=="actor_conversation" and j["request"].get("step")=="ask" and j["state"]=="applied"]
h=jobs[0]["result"]["hops"] if jobs else []
ok=bool(h) and all(x.get("block")==x.get("finalizedBlock") for x in h)
ok or print("   hops:", [(x.get("block"), x.get("finalizedBlock")) for x in h])
sys.exit(0 if ok else 1)'
check $? "a hop's block (the sequencer's) is the block its event finalized in"
echo "$(call events 'str:{"program":"actor_conversation","selector":"0000000000000000"}')" | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)["matching"]==0 else 1)'
check $? "events() filters by program and selector"

echo "== stop + restart keeps the chain"
call stopLocalChain >/dev/null
for i in $(seq 1 30); do ST=$(call localChainStatus | field state); [ "$ST" = stopped ] && break; sleep 1; done
[ "$ST" = stopped ]; check $? "stopLocalChain"
call startLocalChain >/dev/null
for i in $(seq 1 60); do ST=$(call localChainStatus | field state); case "$ST" in running|failed) break;; esac; sleep 1; done
E2=$(call listExamples)
echo "$E2" | python3 -c 'import json,sys; e=json.load(sys.stdin)["examples"]; sys.exit(0 if all(x["deployed"] for x in e) else 1)'; check $? "after a restart the deployed examples are still there"

ctl daemon stop >/dev/null
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
