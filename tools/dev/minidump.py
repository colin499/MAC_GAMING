"""Print the crashing thread's registers and the return addresses on its stack from a game minidump.
usage: python3 minidump.py "~/Documents/Marvel's Spider-Man 2/<file>.mdmp"  (defaults to the newest)"""
import struct, glob, os, sys
path = sys.argv[1] if len(sys.argv) > 1 else sorted(glob.glob(os.path.expanduser("~/Documents/Marvel's Spider-Man 2/*.mdmp")))[-1]
d = open(os.path.expanduser(path), "rb").read(); print(os.path.basename(path), len(d), "bytes")
sig, ver, nstreams, stream_rva = struct.unpack_from("<4sIII", d, 0)
mods = []; mems = []; exc = None
for i in range(nstreams):
    t, size, rva = struct.unpack_from("<III", d, stream_rva + i * 12)
    if t == 4:   # ModuleListStream
        n = struct.unpack_from("<I", d, rva)[0]
        for j in range(n):
            base, msize, cks, ts, name_rva = struct.unpack_from("<QIIII", d, rva + 4 + j * 108)
            ln = struct.unpack_from("<I", d, name_rva)[0]
            mods.append((base, msize, os.path.basename(d[name_rva + 4: name_rva + 4 + ln].decode("utf-16le"))))
    if t == 6:   # ExceptionStream
        tid, _, code, flags, rec, addr = struct.unpack_from("<IIIIQQ", d, rva)
        nparams = struct.unpack_from("<I", d, rva + 32)[0]; params = struct.unpack_from("<15Q", d, rva + 40)
        ctx_size, ctx_rva = struct.unpack_from("<II", d, rva + 160); exc = (tid, code, addr, params[:nparams], ctx_rva)
    if t == 5:   # MemoryListStream
        n = struct.unpack_from("<I", d, rva)[0]
        for j in range(n):
            start, dsize, drva = struct.unpack_from("<QII", d, rva + 4 + j * 16); mems.append((start, dsize, drva))
tid, code, addr, params, ctx_rva = exc
def mod_of(a):
    for base, size, name in mods:
        if base <= a < base + size: return "%s+0x%x" % (name, a - base)
print("exception 0x%x at %s (0x%x) params %s thread 0x%x" % (code, mod_of(addr), addr, [hex(p) for p in params], tid))
names = ["Rax","Rcx","Rdx","Rbx","Rsp","Rbp","Rsi","Rdi","R8","R9","R10","R11","R12","R13","R14","R15","Rip"]
vals = struct.unpack_from("<17Q", d, ctx_rva + 0x78)   # CONTEXT: integer registers start at 0x78, Rip at 0xf8
print(" ".join("%s=%x" % (n, v) for n, v in zip(names, vals)))
rsp = vals[4]
for start, dsize, drva in mems:
    if start <= rsp < start + dsize:
        mem = d[drva: drva + dsize]; off = rsp - start
        for k in range(off, min(off + 0x800, dsize - 8), 8):
            v = struct.unpack_from("<Q", mem, k)[0]; m = mod_of(v)
            if m: print("[rsp+0x%x] = %s" % (k - off, m))
