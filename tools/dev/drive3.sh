#!/bin/bash
# drive3.sh TAG [SECONDS]: unattended Spider-Man 2 run through `layover play` (so the watchdog runs too):
# press Play on the game's launcher, wait for the main menu, press Enter (Continue = the last save), let
# it run SECONDS (default 180), then close the game with `layover kill`. Needs sendkey.exe and winlist.exe
# in ~/Library/Application Support/Layover/tools (build: x86_64-w64-mingw32-gcc -O2 -o X.exe X.c -luser32).
# Output: timestamps on stdout; the shim log is the newest logs/d3d12-*.log, the watchdog's logs/watch.log.
T="$HOME/Library/Application Support/Layover/tools"
LOGS="$HOME/Library/Application Support/Layover/logs"
DEV=/Users/colinlysik/Documents/BRAIN_FOOD/MAC_GAMING/tools/dev
G="$HOME/Library/Application Support/Layover/prefixes/steam/drive_c/users/colinlysik/Documents/Marvel's Spider-Man 2/Marvel's Spider-Man 2.log"
tag=${1:-run}; secs=${2:-180}
S=${LAYOVER_SCRATCH:-/tmp/layover-dev}; mkdir -p "$S"
ts() { date +%H:%M:%S; }
winlist() { : > "$S/winlist-$tag.log"; python3 "$DEV/run_in_prefix.py" "$S/winlist-$tag.log" "Z:$T/winlist.exe" --no-shim >/dev/null 2>&1; sleep 2; }
cd "$DEV"
echo "$(ts) layover play"
/Users/colinlysik/Documents/BRAIN_FOOD/MAC_GAMING/layover play 2651280 2>&1 | sed "s/^/$(ts) play: /"
start=$(date +%s)
for i in $(seq 1 36); do
  sleep 5; winlist
  if grep -q "class='GameNxApp'" "$S/winlist-$tag.log"; then echo "$(ts) launcher up"; break; fi
  if grep -q "Steam Dialog\|class='#32770'" "$S/winlist-$tag.log"; then echo "$(ts) dialog: $(grep -m1 "Steam Dialog\|#32770" "$S/winlist-$tag.log")"; fi
done
for k in 1 2 3 4; do
  sleep 3; python3 run_in_prefix.py "$S/sendkey-$tag.log" "Z:$T/sendkey.exe" --focus GameNxApp --post ENTER --no-shim >/dev/null 2>&1; echo "$(ts) pressed Play ($k)"
  sleep 15; winlist
  grep -q "class='GameNxApp'" "$S/winlist-$tag.log" || break
done
for i in $(seq 1 60); do sleep 5; if grep -q "Processing Slot 1" "$G" 2>/dev/null && [ "$(stat -f %m "$G")" -ge "$start" ]; then echo "$(ts) menu reached"; break; fi; done
sleep 15; python3 run_in_prefix.py "$S/sendkey2-$tag.log" "Z:$T/sendkey.exe" ENTER --no-shim >/dev/null 2>&1; echo "$(ts) pressed Enter at the menu (Continue)"
echo "$(ts) running for $secs s"
sleep "$secs"
echo "$(ts) closing the game"
/Users/colinlysik/Documents/BRAIN_FOOD/MAC_GAMING/layover kill 2>&1 | sed "s/^/$(ts) kill: /"
echo "$(ts) done; shim log: $(ls -t "$LOGS"/d3d12-*.log | head -1)"
