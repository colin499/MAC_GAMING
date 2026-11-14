#!/bin/bash
# mbench.sh TAG "Name=val ...": GPU ms per frame at the MAIN MENU (a steady 3D scene, no save is loaded) from the Metal HUD log;
# needs `layover game 2651280 env "MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1"`. KEEP=1 leaves the game running. EXTRA is only a label.
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
sleep 25
python3 "$DEV/hud.py" 15 2>&1 | sed "s/^/MENU $tag [$vals] [$EXTRA]: /"
grep -E "Upscale quality|Upscale method|Shadow Quality|ConvertScreenToRenderRes|	Resolution" "$G" | cut -c27- | sort -u | tr '\n' ';'; echo
[ -n "$KEEP" ] || { "$REPO/layover" kill >/dev/null 2>&1; sleep 3; }
