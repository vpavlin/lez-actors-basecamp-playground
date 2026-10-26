#!/usr/bin/env bash
# every 60 s: node slot/lib, last finalized L2 block, hello_event events in the indexer, InvalidParent count
S=~/lez-actors/bedrock-soak
while true; do
  ni=$(curl -s -m5 http://127.0.0.1:18180/cryptarchia/info | python3 -c 'import json,sys; d=json.load(sys.stdin)["cryptarchia_info"]; print("slot",d["slot"],"lib_slot",d["lib_slot"],"height",d["height"])' 2>/dev/null)
  fin=$(grep -a "finalized" $S/seq.log | tail -1 | grep -o "finalized [0-9.=]*" )
  head=$(grep -a "Publishing block" $S/seq.log | tail -1 | grep -o "block [0-9]*")
  ev=$(curl -s -m10 -X POST -H 'content-type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"getEvents","params":[{"from_block":0}]}' http://127.0.0.1:8879 | python3 -c 'import json,sys; r=json.load(sys.stdin).get("result",[]); h=[e for e in r if e["selector"]=="98abe0ff895d39c5"]; print("events",len(r),"hello",len(h),"last_block",max([e["block_id"] for e in h],default=None))' 2>/dev/null)
  ip=$(grep -a -c "InvalidParent" $S/node.log); rm_=$(grep -a -c "removed)" $S/node.log)
  echo "$(date -u +%H:%M:%S) $ni | seq $head $fin | $ev | InvalidParent=$ip removedLines=$rm_"
  sleep 60
done
