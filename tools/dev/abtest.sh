#!/bin/bash
# abtest.sh LABEL: launch SM2 via the running Steam, reach the main menu, settle 60 s, collect 3 fps readings + CPU + threads, quit.
# Needs winlist.exe and sendkey.exe (build from this folder with x86_64-w64-mingw32-gcc ... -luser32) in $LAYOVER_SCRATCH
# (default /tmp/layover-dev); Steam must be running and logged on. Hold the display awake with `caffeinate -d -u` while measuring.
S=${LAYOVER_SCRATCH:-/tmp/layover-dev}
R=/Users/colinlysik/Documents/BRAIN_FOOD/MAC_GAMING
G="$HOME/Library/Application Support/Layover/prefixes/steam/drive_c/users/colinlysik/Documents/Marvel's Spider-Man 2/Marvel's Spider-Man 2.log"
label=$1; ts() { date +%H:%M:%S; }
cd $R && ./layover play 2651280 >/dev/null 2>&1
$S/drive2.sh $label >/dev/null
P=$(pgrep -f "Spider-Man2.exe" | head -1)
echo "$(ts) [$label] menu reached; settling 60 s"
sleep 60
menu=$(date +%H:%M:%S)
cpus=""; thr=""
for i in $(seq 1 12); do
  sleep 20; c=$(ps -o %cpu= -p $P | tr -d ' '); cpus="$cpus $c"
  t=$(ps -M -p $P | awk 'NR>1 && NF==6 {print $2}' | sort -rn | head -2 | tr '\n' '/'); thr="$thr $t"
  n=$(grep -a "fps:" "$G" | awk -F'fps: ' -v m="$menu" '{t=substr($1,1,8); if (t>m) print $NF}' | wc -l)
  [ "$n" -ge 3 ] && break
done
echo "$(ts) [$label] fps: $(grep -a "fps:" "$G" | awk -F'fps: ' -v m="$menu" '{t=substr($1,1,8); if (t>m) printf "%s ", $NF}')"
echo "$(ts) [$label] cpu:$cpus"
echo "$(ts) [$label] top2 threads:$thr"
L=$(ls -t "$HOME/Library/Application Support/Layover/logs/"d3d12-*.log | head -1)
grep -E "fence spin|frame limiter:" "$L" | sed -E 's/^[0-9:.]+ \[[0-9]+\] //' | cut -c1-70
grep "stats:" "$L" | tail -1 | sed -E 's/^([0-9:.]+) \[[0-9]+\] stats: (presents=[0-9]+ \(paced [0-9]+\) fence-polls=[0-9]+ \(slept [0-9]+\)).*/\1 \2/'
cd $R/tools/dev && python3 run_in_prefix.py $S/tk.log taskkill /IM Spider-Man2.exe --no-shim >/dev/null 2>&1
for i in $(seq 1 30); do pgrep -f "Spider-Man2.exe" >/dev/null || break; sleep 1; done
echo "$(ts) [$label] game closed"
sleep 20
