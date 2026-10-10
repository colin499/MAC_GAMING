#!/bin/bash
# ab2.sh: A/B of the fence-wait relief (off, then mode 3) using abtest.sh; edits games.2651280.env in config.json and restores it.
S=${LAYOVER_SCRATCH:-/tmp/layover-dev}
C="$HOME/Library/Application Support/Layover/config.json"
setenv() { python3 - "$C" "$1" <<'PY'
import json,sys
p,v=sys.argv[1],sys.argv[2]; c=json.load(open(p)); g=c["games"].setdefault("2651280",{}); e=g.setdefault("env",{})
e.pop("LAYOVER_FENCE_SLEEP",None); e.pop("LAYOVER_FENCE_SPIN_US",None)
if v!="": e["LAYOVER_FENCE_SLEEP"]=v; e["LAYOVER_FENCE_SPIN_US"]="300"
if not e: g.pop("env",None)
json.dump(c,open(p,"w"),indent=2)
PY
}
setenv 0; $(dirname "$0")/abtest.sh off3
setenv 3; $(dirname "$0")/abtest.sh event3b
setenv ""
echo "ALL DONE"
