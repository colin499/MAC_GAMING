# ctx.py CALLSITE [nback] : print the function-ish context before a call site: string refs in the window and the last N instructions
import sys, pe
site = pe.norm(sys.argv[1]); nback = int(sys.argv[2]) if len(sys.argv) > 2 else 30
win = 0x900
lines = pe.dis(site - win, stop=site + 0x30)
# resync: find the index of the line at the site (capstone may be misaligned at the window start; search for exact address)
idx = next((k for k, l in enumerate(lines) if l.startswith(f"{site:x}:")), None)
if idx is None:
    # try smaller windows
    for w in (0x700, 0x500, 0x300, 0x100, 0x40):
        lines = pe.dis(site - w, stop=site + 0x30); idx = next((k for k, l in enumerate(lines) if l.startswith(f"{site:x}:")), None)
        if idx is not None: break
strs = [l for l in lines[:idx] if "= '" in l or "= \"" in l or 'L"' in l or "L'" in l]
print(f"--- site {site:x}; string refs in window:")
for l in strs[-12:]: print("   ", l)
print("--- last instructions:")
for l in lines[max(0, idx - nback): idx + 3]: print("   ", l)
