#!/bin/bash
# bench.sh TAG "Name=val Name=val ..." [SECONDS] [WARMUP]: unattended Spider-Man 2 frame-rate measurement.
# Writes the given Graphics registry dwords, launches through `layover play`, presses Play and Continue (the
# autosave), waits WARMUP s (default 60), lifts the frame cap (live.env fps=0), measures SECONDS (default 90)
# from the shim's 5 s "frame:" lines, closes everything with `layover kill` and puts the save folder back from
# BACKUP (env, required: Continue + kill would otherwise leave a newer autosave). The registry values STAY as
# written: set the ones you want to keep with the last run. Prints one RESULT line.
ROOT="$HOME/Library/Application Support/Layover"
T="$ROOT/tools"; LOGS="$ROOT/logs"
REPO=/Users/colinlysik/Documents/BRAIN_FOOD/MAC_GAMING; DEV=$REPO/tools/dev
DOCS="$ROOT/prefixes/steam/drive_c/users/colinlysik/Documents/Marvel's Spider-Man 2"
G="$DOCS/Marvel's Spider-Man 2.log"
tag=${1:?tag}; vals=${2:-}; secs=${3:-90}; warm=${4:-60}
: "${BACKUP:?set BACKUP to a copy of the save folder}"
S=${LAYOVER_SCRATCH:-/tmp/layover-dev}; mkdir -p "$S"
ts() { date +%H:%M:%S; }
winlist() { : > "$S/winlist-$tag.log"; python3 "$DEV/run_in_prefix.py" "$S/winlist-$tag.log" "Z:$T/winlist.exe" --no-shim >/dev/null 2>&1; sleep 2; }
cd "$DEV"
if [ -n "$vals" ]; then
python3 - "$vals" <<'E'
import importlib.machinery, importlib.util, sys
ld = importlib.machinery.SourceFileLoader('layover_mod', '/Users/colinlysik/Documents/BRAIN_FOOD/MAC_GAMING/layover')
m = importlib.util.module_from_spec(importlib.util.spec_from_loader('layover_mod', ld)); ld.exec_module(m)
cfg = m.load_config(); key = r"HKCU\Software\Insomniac Games\Marvel's Spider-Man 2\Graphics"
for pair in sys.argv[1].split():
    n, v = pair.split('='); m.reg_add(cfg, key, n, int(v), 'REG_DWORD')
cur = m.reg_query_dwords(cfg, key)
print('registry:', ' '.join('%s=%s' % (p.split('=')[0], cur.get(p.split('=')[0])) for p in sys.argv[1].split()))
E
fi
echo "$(ts) layover play"
"$REPO/layover" play 2651280 2>&1 | sed "s/^/play: /"
start=$(date +%s)
for i in $(seq 1 36); do
  sleep 5; winlist
  if grep -q "class='GameNxApp'" "$S/winlist-$tag.log"; then echo "$(ts) launcher up"; break; fi
done
for k in 1 2 3 4; do
  sleep 3; python3 run_in_prefix.py "$S/sendkey-$tag.log" "Z:$T/sendkey.exe" --focus GameNxApp --post ENTER --no-shim >/dev/null 2>&1; echo "$(ts) pressed Play ($k)"
  sleep 15; winlist
  grep -q "class='GameNxApp'" "$S/winlist-$tag.log" || break
done
for i in $(seq 1 60); do sleep 5; if grep -q "Processing Slot 1" "$G" 2>/dev/null && [ "$(stat -f %m "$G")" -ge "$start" ]; then echo "$(ts) menu reached"; break; fi; done
sleep 15; python3 run_in_prefix.py "$S/sendkey2-$tag.log" "Z:$T/sendkey.exe" ENTER --no-shim >/dev/null 2>&1; echo "$(ts) pressed Enter at the menu (Continue)"
sleep "$warm"
echo "fps=${CAP:-0}" > "$ROOT/live.env"; sleep 3
LOG=$(ls -t "$LOGS"/d3d12-*.log | head -1)
t0=$(date +%H:%M:%S); echo "$t0 measuring $secs s at cap ${CAP:-0} (0 = none) ($LOG)"
[ -n "$SHOT" ] && "$REPO/layover" shot "$S/shot-$tag.jpg" >/dev/null 2>&1
pid=$(pgrep -f "Spider-Man2.exe" | head -1); : > "$S/samples-$tag.log"
end=$(( $(date +%s) + secs ))
while [ "$(date +%s)" -lt "$end" ]; do   # every 10 s: GPU utilisation, the game's CPU %, its five busiest threads
  gpu=$(ioreg -r -c IOAccelerator -d 1 | grep -oE '"Device Utilization %"=[0-9]+' | head -1 | cut -d= -f2)
  cpu=$(ps -o pcpu= -p "$pid" | tr -d ' ')
  thr=$(ps -M -p "$pid" 2>/dev/null | awk 'NR==2 {print $4} NR>2 {print $1}' | sort -rn | head -5 | tr '\n' ' ')
  echo "$(ts) gpu=$gpu cpu=$cpu threads: $thr" >> "$S/samples-$tag.log"
  sleep 10
done
t1=$(date +%H:%M:%S)
awk '{split($2,a,"=");split($3,b,"=");g+=a[2];c+=b[2];n++} END{if(n) printf "SAMPLES: GPU %.0f%% busy, game CPU %.0f%% (n=%d)\n", g/n, c/n, n}' "$S/samples-$tag.log"; tail -2 "$S/samples-$tag.log"
python3 "$DEV/hud.py" "$secs" 2>&1 | sed 's/^/HUD: /'
echo "fps=60" > "$ROOT/live.env"
grep -E "Upscale quality|Shadow Quality|VSync:|ConvertScreenToRenderRes|	Resolution" "$G" | cut -c27- | sort -u | tr '\n' ';'; echo
python3 - "$LOG" "$t0" "$t1" "$tag" <<'E'
import re, sys, statistics as st
log, t0, t1, tag = sys.argv[1:]
fps = []; gap = []; fw = []; ex = []
for l in open(log, errors='replace'):
    m = re.match(r'(\d\d:\d\d:\d\d)\.\d+ \[\d+\] frame: ([\d.]+) fps .*gap max ([\d.]+) ms \| execlists (\d+) avg ([\d.]+) max ([\d.]+) ms \| fence wait max ([\d.]+)', l)
    if m and t0 < m[1] <= t1:
        fps.append(float(m[2])); gap.append(float(m[3])); fw.append(float(m[7])); ex.append(float(m[5]))
if fps:
    print('RESULT %s: n=%d fps mean %.1f min %.1f max %.1f | worst frame gap %.0f ms (median of 5 s maxima %.0f) | fence wait max %.0f ms'
          % (tag, len(fps), st.mean(fps), min(fps), max(fps), max(gap), st.median(gap), max(fw)))
else:
    print('RESULT %s: no frame lines between %s and %s' % (tag, t0, t1))
E
echo "$(ts) closing the game"
"$REPO/layover" kill 2>&1 | sed "s/^/kill: /"
sleep 2
rsync -a --delete "$BACKUP/" "$DOCS/76561198640460568/" && echo "$(ts) saves restored from $BACKUP"
echo "$(ts) done"
