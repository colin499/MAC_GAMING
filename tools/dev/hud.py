#!/usr/bin/env python3
"""hud.py [SECONDS] [LOG]: frame pacing of the running game from the Metal HUD lines in the launch log
(MTL_HUD_LOG_ENABLED=1 prints them on the game's stderr = logs/steam-*.log). Line format:
metal-HUD: <first frame>,<?>,<memory MB>,<present interval ms>,<GPU ms>, ... one pair per frame."""
import sys, re, glob, os, datetime
secs = int(sys.argv[1]) if len(sys.argv) > 1 else 30
logs = sorted(glob.glob(os.path.expanduser("~/Library/Application Support/Layover/logs/steam-*.log")), key=os.path.getmtime)
path = sys.argv[2] if len(sys.argv) > 2 else logs[-1]
now = datetime.datetime.now(); iv = []; gpu = []; mem = 0
for line in open(path, errors="replace"):
    m = re.match(r"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+ .*metal-HUD: (\d+),([\d.]+),([\d.]+),(.*)", line)
    if not m: continue
    t = datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
    if (now - t).total_seconds() > secs: continue
    v = [float(x) for x in m.group(5).strip().split(",") if x]
    iv += v[0::2]; gpu += v[1::2]; mem = float(m.group(4))
if not iv: print("no HUD frames in the last", secs, "s of", path); sys.exit(1)
s = sorted(iv); g = sorted(gpu); n = len(s)
pct = lambda a, p: a[min(len(a) - 1, int(len(a) * p))]
print(f"{n} frames / {sum(iv)/1000:.0f} s: {1000*n/sum(iv):.1f} fps | frame ms median {pct(s,.5):.1f} p90 {pct(s,.9):.1f} p99 {pct(s,.99):.1f} max {s[-1]:.0f} | over 50 ms: {sum(1 for x in iv if x>50.5)} | GPU ms median {pct(g,.5):.1f} p90 {pct(g,.9):.1f} | mem {mem:.0f} MB")
