#!/bin/bash
# drive.sh RUNTAG [continue]: launch SM2 via Steam, press Play, wait for the main menu, optionally press Enter (Continue)
S=/private/tmp/claude-501/-Users-colinlysik-Documents-BRAIN-FOOD-MAC-GAMING/eae202ad-6cee-43d5-8725-94b37696b668/scratchpad
DEV=/Users/colinlysik/Documents/BRAIN_FOOD/MAC_GAMING/tools/dev
G="$HOME/Library/Application Support/Layover/prefixes/steam/drive_c/users/colinlysik/Documents/Marvel's Spider-Man 2/Marvel's Spider-Man 2.log"
tag=$1; cont=$2
ts() { date +%H:%M:%S; }
cd $DEV
echo "$(ts) launching"; python3 run_in_prefix.py $S/launch-$tag.log start steam://rungameid/2651280 --no-shim >/dev/null 2>&1
for i in $(seq 1 30); do
  sleep 5; python3 run_in_prefix.py $S/winlist-$tag.log "Z:$S/winlist.exe" --no-shim >/dev/null 2>&1; sleep 2
  if grep -q "class='GameNxApp'" $S/winlist-$tag.log; then echo "$(ts) launcher up"; break; fi
  if grep -q "Steam Dialog\|class='#32770'" $S/winlist-$tag.log; then echo "$(ts) dialog: $(grep -m1 "Steam Dialog\|#32770" $S/winlist-$tag.log)"; fi
  : > $S/winlist-$tag.log
done
sleep 3; python3 run_in_prefix.py $S/sendkey-$tag.log "Z:$S/sendkey.exe" --focus GameNxApp ENTER --no-shim >/dev/null 2>&1; echo "$(ts) pressed Play"
start=$(date +%s)
for i in $(seq 1 60); do sleep 5; if grep -q "Processing Slot 1" "$G" 2>/dev/null && [ $(stat -f %m "$G") -ge $start ]; then echo "$(ts) menu reached"; break; fi; done
if [ "$cont" = "continue" ]; then sleep 15; python3 run_in_prefix.py $S/sendkey2-$tag.log "Z:$S/sendkey.exe" ENTER --no-shim >/dev/null 2>&1; echo "$(ts) pressed Enter at menu (Continue?)"; fi
echo "$(ts) done"
