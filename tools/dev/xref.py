# find rip-relative / rel32 references to a VA inside .text (heuristic: instruction ends right after disp32,
# or has 1/4-byte immediate after). Usage: xref.py VA
import sys, numpy as np, pe
tgt = pe.norm(sys.argv[1])
for name, v, vs, p, ps in pe.SECS:
    if name != ".text": continue
    buf = np.frombuffer(pe._data, dtype=np.uint8, count=ps, offset=p)
    hits = set()
    for tail in (0, 1, 4):   # bytes of immediate after disp32
        for sh in range(4):
            arr = buf[sh:sh + ((len(buf) - sh) // 4) * 4].view("<i4")
            idx = np.arange(len(arr)) * 4 + sh
            m = (idx + 4 + tail + arr.astype(np.int64)) == (tgt - pe.BASE - v)
            for k in idx[m]: hits.add(int(k) - 0)
    for k in sorted(hits):
        va = pe.BASE + v + k
        # disassemble a window before to find the instruction containing k
        start = va - 12
        try: ins = list(pe.md.disasm(pe.read(start, 24), start))
        except Exception: continue
        for i in ins:
            if i.address <= va < i.address + i.size and i.address + i.size >= va + 4:
                print(f"{i.address:x}: {i.mnemonic} {i.op_str}")
                break
