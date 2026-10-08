import pefile, capstone, sys, struct, functools
import os
EXE = os.environ.get("SM2_EXE", os.path.expanduser("~/Library/Application Support/Layover/prefixes/steam/drive_c/Program Files (x86)/Steam/steamapps/common/Marvel's Spider-Man 2/Spider-Man2.exe"))  # needs: pip install capstone pefile numpy
BASE = 0x140000000
_data = open(EXE, "rb").read()
_pe = pefile.PE(data=_data, fast_load=True)
SECS = [(s.Name.rstrip(b"\0").decode(), s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in _pe.sections]
def va2off(va):
    rva = va - BASE
    for n, v, vs, p, ps in SECS:
        if v <= rva < v + max(vs, ps): return p + (rva - v)
    raise ValueError(hex(va))
def read(va, n): o = va2off(va); return _data[o:o+n]
def u64(va): return struct.unpack_from("<Q", _data, va2off(va))[0]
def u32(va): return struct.unpack_from("<I", _data, va2off(va))[0]
def cstr(va, mx=200):
    try: b = read(va, mx)
    except ValueError: return None
    e = b.find(b"\0"); b = b[:e] if e >= 0 else b
    if len(b) >= 2 and all(0x20 <= c < 0x7f for c in b): return b.decode()
    return None
def wstr(va, mx=200):
    try: b = read(va, mx*2)
    except ValueError: return None
    s = b.decode("utf-16le", "replace"); e = s.find("\0"); s = s[:e] if e >= 0 else s
    if len(s) >= 2 and all(0x20 <= ord(c) < 0x7f for c in s): return "L" + repr(s)
    return None
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def norm(a):
    a = int(a, 16) if isinstance(a, str) else a
    return a + BASE if a < BASE else a
def dis(va, n_bytes=None, stop=None, until_ret=False, max_insn=4000):
    va = norm(va); out = []
    end = stop if stop else (va + n_bytes if n_bytes else va + 0x4000)
    code = read(va, end - va)
    for i in md.disasm(code, va):
        note = ""
        for op in i.operands:
            t = None
            if op.type == capstone.x86.X86_OP_IMM and i.mnemonic in ("call", "jmp") or (op.type == capstone.x86.X86_OP_IMM and i.mnemonic.startswith("j")): t = op.imm
            elif op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP: t = i.address + i.size + op.mem.disp
            if t:
                s = cstr(t) or wstr(t)
                if s: note += f"  ; {t:#x} = {s!r}"
                elif op.type == capstone.x86.X86_OP_MEM and i.mnemonic.startswith(("mov", "cmp", "lea")) :
                    try: note += f"  ; [{t:#x}]"
                    except Exception: pass
        out.append(f"{i.address:x}: {i.mnemonic} {i.op_str}{note}")
        if until_ret and i.mnemonic in ("ret", "int3") : break
        if len(out) >= max_insn: break
    return out
if __name__ == "__main__":
    a = sys.argv[1]; n = int(sys.argv[2], 0) if len(sys.argv) > 2 else None
    print("\n".join(dis(a, n_bytes=n, until_ret=(n is None))))
