#!/usr/bin/env bash
# Headless smoke test: lez_actors_module under logosctl against a local LEZ v0.4 (dev) sequencer.
#   scripts/smoke.sh <lez_actors_module.lgx> [rpcUrl]
# Needs: logosctl 0.3.x (LOGOSCTL), a running standalone sequencer (default http://127.0.0.1:3040)
# with the dev genesis accounts. Creates a throwaway session + wallet under $SMOKE_DIR.
set -u
LGX=${1:?the lez_actors_module .lgx}
RPC=${2:-http://127.0.0.1:3040}
L=${LOGOSCTL:-logosctl}
BASE=${SMOKE_DIR:-/tmp/lez-actors-smoke}
# the dev chain's funded genesis accounts (LEZ Justfile: wallet-import-test-accounts)
KEY_A=7f273098f25b71e6c005a9519f2678da8d1c7f01f6a27778e2d9948abdf901fb
KEY_B=f434f8741720014586ae43356d2aec6257da086222f604ddb75d69733b86fc4c
pass=0; fail=0
check() { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "  ok   $2"; else fail=$((fail+1)); echo "  FAIL $2"; fi; }
ctl() { HOME="$BASE/home" LOGOSCTL_CONFIG_DIR="$BASE/cfg" LEZ_RPC_URL="$RPC" QT_QPA_PLATFORM=offscreen timeout 60 "$L" "$@"; }
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

[ -d "$BASE/cfg" ] && ctl daemon stop >/dev/null 2>&1
rm -rf "$BASE"; mkdir -p "$BASE/home"
ctl daemon start --detach >/dev/null
ctl install "$LGX" -y >/dev/null
ctl module load lez_actors_module >/dev/null
sleep 2

echo "== health"
H=$(call health); echo "  $H" | cut -c1-200
[ "$(echo "$H" | field status)" = ok ] && [ "$(echo "$H" | field reachable)" = True ]; check $? "the sequencer answers (block height + base fees)"

echo "== wallet"
W=$(call createWallet str:smoke-pass)
[ "$(echo "$W" | field status)" = ok ] && [ -n "$(echo "$W" | field mnemonic)" ]; check $? "createWallet returns a recovery phrase"
for k in $KEY_A $KEY_B; do I=$(call importKey str:$k); [ "$(echo "$I" | field status)" = ok ]; check $? "importKey ${k:0:8}…"; done
A=$(call listAccounts); echo "  $A" | cut -c1-300
# a new wallet comes with one empty account of its own; the two imported ones are the funded ones
FUNDED=$(echo "$A" | python3 -c 'import json,sys; print(" ".join(a["accountId"] for a in json.load(sys.stdin).get("accounts",[]) if a.get("onChain") and int(a.get("balance","0"))>0))')
set -- $FUNDED
[ "$#" = 2 ]; check $? "the two imported genesis accounts are funded on chain"
ACC_A=$1; ACC_B=$2
NEW=$(call newAccount | field accountId); [ -n "$NEW" ] && [ "$NEW" != None ]; check $? "newAccount gives a fresh id"
P=$(call setPayer str:$ACC_B); [ "$(echo "$P" | field status)" = ok ]; check $? "setPayer B"

echo "== reads"
G=$(call getAccount str:$ACC_A); echo "  $G" | cut -c1-240
[ "$(echo "$G" | field onChain)" = True ] && [ "$(echo "$G" | field actorStates.0.programName)" = "native token" ]; check $? "getAccount: on chain, native-token actor state labelled"
G2=$(call getAccount str:$NEW); [ "$(echo "$G2" | field onChain)" = False ]; check $? "a fresh account is off-chain"
R=$(call resolveProgram str:JAQ5MVHbCkSYRzXunsrNuM2m1LS859PGveHfoYPAmcvZ); [ "$(echo "$R" | field builtin)" = True ]; check $? "resolveProgram knows the program loader"
F=$(call feeReserveEstimate); [ "$(echo "$F" | field status)" = ok ]; check $? "feeReserveEstimate: $(echo "$F" | field reserve)"

echo "== transfer A -> fresh account, B pays the fee"
BAL_A0=$(call getAccount str:$ACC_A | field balance); BAL_B0=$(call getAccount str:$ACC_B | field balance)
T=$(call transfer "str:{\"from\":\"$ACC_A\",\"to\":\"$NEW\",\"amount\":\"1000\"}"); echo "  $T"
JOB=$(echo "$T" | field jobId)
[ -n "$JOB" ] && [ "$JOB" != None ]; check $? "transfer starts a job"
STATE=""
for i in $(seq 1 60); do J=$(call job str:$JOB); STATE=$(echo "$J" | field state); case "$STATE" in applied|reverted|failed|unknown) break;; esac; sleep 2; done
echo "  job: $J" | cut -c1-400
[ "$STATE" = applied ]; check $? "the transfer is applied (judged from the balances, not the inclusion)"
BAL_A1=$(call getAccount str:$ACC_A | field balance); BAL_B1=$(call getAccount str:$ACC_B | field balance); BAL_N=$(call getAccount str:$NEW | field balance)
python3 -c "import sys; a0,a1,b0,b1,n=map(int,sys.argv[1:]); sys.exit(0 if a0-a1==1000 and n==1000 and b0>b1 else 1)" "$BAL_A0" "$BAL_A1" "$BAL_B0" "$BAL_B1" "$BAL_N"
check $? "A -1000, fresh +1000, B paid the fee ($((BAL_B0-BAL_B1)))"

echo "== guard rails"
X=$(call transfer "str:{\"from\":\"$ACC_A\",\"to\":\"$NEW\",\"amount\":\"1.5\"}"); [ "$(echo "$X" | field status)" = error ]; check $? "a fractional amount is refused, not trimmed"
X=$(call transfer "str:{\"from\":\"$NEW\",\"to\":\"$ACC_A\",\"amount\":\"999999999999999999\"}"); [ "$(echo "$X" | field status)" = error ]; check $? "a sender without the amount + fee reserve is refused before signing: $(echo "$X" | field error | cut -c1-80)"

if [ -n "${HELLO_PROGRAM:-}" ]; then
  echo "== call hello_world ($HELLO_PROGRAM) on the fresh account"
  call addProgram "str:{\"name\":\"hello_world\",\"programId\":\"$HELLO_PROGRAM\"}" >/dev/null
  C=$(call callProgram "str:{\"programId\":\"$HELLO_PROGRAM\",\"accountId\":\"$NEW\",\"message\":\"Hola mundo!\",\"encoding\":\"text\"}"); JOB=$(echo "$C" | field jobId)
  for i in $(seq 1 60); do J=$(call job str:$JOB); STATE=$(echo "$J" | field state); case "$STATE" in applied|reverted|failed|unknown) break;; esac; sleep 2; done
  echo "  job: $J" | cut -c1-400
  [ "$STATE" = applied ] && [ "$(echo "$J" | field result.stateAfterText)" = "Hola mundo!" ]; check $? "hello_world wrote \"Hola mundo!\" to its actor state"
  G=$(call getAccount str:$NEW); echo "$G" | grep -q '"programName": "hello_world"'; check $? "the account shows a hello_world actor state, labelled"
  # the payer's own account, sign off: LEZ refuses a non-signing payer, so the payer's mention must sign
  C=$(call callProgram "str:{\"programId\":\"$HELLO_PROGRAM\",\"accountId\":\"$ACC_B\",\"message\":\"payer here\",\"encoding\":\"text\",\"sign\":false}"); JOB=$(echo "$C" | field jobId)
  for i in $(seq 1 60); do J=$(call job str:$JOB); STATE=$(echo "$J" | field state); case "$STATE" in applied|reverted|failed|unknown) break;; esac; sleep 2; done
  echo "  job: $J" | cut -c1-300
  [ "$STATE" = applied ] && [ "$(echo "$J" | field result.signedAsPayer)" = True ]; check $? "a call on the payer's own account is applied (the payer signs)"
fi

if [ -n "${AUTH_PROGRAM:-}" ]; then
  echo "== hello_world_with_authorization ($AUTH_PROGRAM): unsigned must revert, signed must apply"
  call addProgram "str:{\"name\":\"hello_auth\",\"programId\":\"$AUTH_PROGRAM\"}" >/dev/null
  waitjob() { local J S; for i in $(seq 1 60); do J=$(call job str:$1); S=$(echo "$J" | field state); case "$S" in applied|reverted|failed|unknown) break;; esac; sleep 2; done; echo "$J"; }
  C=$(call callProgram "str:{\"programId\":\"$AUTH_PROGRAM\",\"accountId\":\"$NEW\",\"message\":\"no signature\",\"encoding\":\"text\",\"sign\":false}")
  J=$(waitjob "$(echo "$C" | field jobId)"); echo "  job: $J" | cut -c1-300
  [ "$(echo "$J" | field state)" = reverted ] && [ "$(echo "$J" | field result.reverted)" = True ]; check $? "an unsigned call is reported REVERTED (included and charged, state unchanged), not applied"
  C=$(call callProgram "str:{\"programId\":\"$AUTH_PROGRAM\",\"accountId\":\"$NEW\",\"message\":\"signed\",\"encoding\":\"text\",\"sign\":true}")
  J=$(waitjob "$(echo "$C" | field jobId)"); echo "  job: $J" | cut -c1-300
  [ "$(echo "$J" | field state)" = applied ] && [ "$(echo "$J" | field result.stateAfterText)" = "signed" ]; check $? "the signed call is applied"
fi

ctl daemon stop >/dev/null
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
