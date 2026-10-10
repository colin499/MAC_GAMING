#!/bin/bash
# drive2.sh TAG: game already asked to start via `layover play`; press Play on the launcher, wait for the main menu
# Needs winlist.exe and sendkey.exe (build from this folder with x86_64-w64-mingw32-gcc ... -luser32) in $LAYOVER_SCRATCH
# (default /tmp/layover-dev); Steam must be running and logged on. Hold the display awake with `caffeinate -d -u` while measuring.
S=${LAYOVER_SCRATCH:-/tmp/layover-dev}
DEV=/Users/colinlysik/Documents/BRAIN_FOOD/MAC_GAMING/tools/dev
G="$HOME/Library/Application Support/Layover/prefixes/steam/drive_c/users/colinlysik/Documents/Marvel's Spider-Man 2/Marvel's Spider-Man 2.log"
tag=$1; ts() { date +%H:%M:%S; }
cd $DEV
start=$(date +%s)
for i in $(seq 1 30); do
  sleep 5; : > $S/winlist-$tag.log; python3 run_in_prefix.py $S/winlist-$tag.log "Z:$S/winlist.exe" --no-shim >/dev/null 2>&1; sleep 2
  if grep -q "class='GameNxApp'" $S/winlist-$tag.log; then echo "$(ts) launcher up"; break; fi
  if grep -q "Steam Dialog\|class='#32770'" $S/winlist-$tag.log; then echo "$(ts) dialog: $(grep -m1 "Steam Dialog\|#32770" $S/winlist-$tag.log)"; fi
done
for k in 1 2 3 4; do
  sleep 3; python3 run_in_prefix.py $S/sendkey-$tag.log "Z:$S/sendkey.exe" --focus GameNxApp --post ENTER --no-shim >/dev/null 2>&1; echo "$(ts) pressed Play ($k)"
  sleep 15; : > $S/winlist-$tag.log; python3 run_in_prefix.py $S/winlist-$tag.log "Z:$S/winlist.exe" --no-shim >/dev/null 2>&1; sleep 2
  grep -q "class='GameNxApp'" $S/winlist-$tag.log || break
done
for i in $(seq 1 60); do sleep 5; if grep -q "Processing Slot 1" "$G" 2>/dev/null && [ $(stat -f %m "$G") -ge $start ]; then echo "$(ts) menu reached"; break; fi; done
echo "$(ts) done"
