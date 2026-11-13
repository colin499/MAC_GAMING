#!/usr/bin/env python3
"""typed2raw.py IN.ll OUT.ll SPEC [SPEC...]: rewrite typed buffer reads in `dxc -dumpbin` text into raw reads.

Why: D3DMetal backs a typed buffer view (Buffer<T>) with a Metal texture buffer, which stops at 2^28
elements. A game that views a 2 GB pool through a 4-byte typed view reads zeros above 1 GB. A raw view
(ByteAddressBuffer) is a plain Metal buffer with no such limit, so the shim creates the wide view raw and
swaps in a copy of each shader whose `Load(i)` on that register became a raw load at byte i*4 plus the
format's decode. Same bytes, same values.

SPEC = <space>:<register>:<format>[:<raw space>:<raw register>], format one of
    u32       Buffer<uint>   over R32_UINT            -> the word itself
    rgba8     Buffer<float4> over R8G8B8A8_UNORM      -> bytes / 255
    rgb10a2   Buffer<float4> over R10G10B10A2_UNORM   -> 10,10,10 bits / 1023, 2 bits / 3
With <raw space>:<raw register> the loads are redirected to a raw view of the same buffer that the shader
already declares there (Spider-Man 2 binds its whole mesh pool raw at t97,space1 next to the typed views):
the typed register is left alone, so nothing has to change on the D3D12 side and a shader that was not
rewritten keeps working exactly as before. Without it the register itself becomes a raw buffer and the
view bound there must be created raw.
Only SRVs. Exit code 0 = rewritten, 3 = none of the registers is used by this shader (OUT not written),
1 = the shader uses a register in a way this tool does not know (OUT not written; do not swap it).
The output still has to go through `dxa` (assemble) and `dxv` (validate + sign).
Written for Layover; the u32 case follows macprotips/SpiderMan2Fix's public-domain recipe.
"""
import re
import struct
import sys


def fconst(x):
    """An LLVM `float` literal: the float32 value written as a 64-bit hex double."""
    f = struct.unpack("<f", struct.pack("<f", x))[0]
    return "0x%016X" % struct.unpack("<Q", struct.pack("<d", f))[0]


DECODE = {  # format -> per component (shift, mask, scale)
    "rgba8": [(0, 255, 1 / 255.0), (8, 255, 1 / 255.0), (16, 255, 1 / 255.0), (24, 255, 1 / 255.0)],
    "rgb10a2": [(0, 1023, 1 / 1023.0), (10, 1023, 1 / 1023.0), (20, 1023, 1 / 1023.0), (30, 3, 1 / 3.0)],
}
ID = r"%[A-Za-z0-9_.]+"


class Unsupported(Exception):
    pass


def rewrite(ir, specs):
    """specs: [(space, reg, fmt)]. Returns (text, stats) or raises Unsupported; (None, None) when nothing matches."""
    lines = ir.split("\n")
    stats = {"handles": 0, "loads": 0}
    handle_fmt = {}      # annotated handle value -> fmt
    any_meta = False
    redirect = {}        # annotated typed handle -> (created raw handle, fmt)
    for spec in specs:
        space, reg, fmt = spec[:3]
        if len(spec) == 5:
            # redirect mode: loads on (space, reg) go to the raw view the shader already has at spec[3:5]
            tb = re.compile(r"^\s*(%s) = call %%dx\.types\.Handle @dx\.op\.createHandleFromBinding\(i32 217, %%dx\.types\.ResBind \{ i32 %d, i32 %d, i32 %d, i8 0 \}" % (ID, reg, reg, space))
            rb = re.compile(r"^\s*(%s) = call %%dx\.types\.Handle @dx\.op\.createHandleFromBinding\(i32 217, %%dx\.types\.ResBind \{ i32 %d, i32 %d, i32 %d, i8 0 \}" % (ID, spec[4], spec[4], spec[3]))
            typed = {tb.match(l).group(1) for l in lines if tb.match(l)}
            if not typed:
                continue
            any_meta = True
            raws = [(i, rb.match(l).group(1)) for i, l in enumerate(lines) if rb.match(l)]
            if not raws:
                raise Unsupported("no raw view at t%d,space%d to redirect to" % (spec[4], spec[3]))
            first_def = next(i for i, l in enumerate(lines) if l.startswith("define "))
            first_label = next((i for i, l in enumerate(lines) if i > first_def and l.startswith("; <label>:")), len(lines))
            if raws[0][0] > first_label:
                raise Unsupported("raw handle is not created in the entry block")
            an = re.compile(r"^\s*(%s) = call %%dx\.types\.Handle @dx\.op\.annotateHandle\(i32 216, %%dx\.types\.Handle (%s), %%dx\.types\.ResourceProperties \{ i32 (\d+), i32 (\d+) \}" % (ID, ID))
            for l in lines:
                m = an.match(l)
                if m and m.group(2) in typed:
                    if m.group(3) != "10":
                        raise Unsupported("handle for t%d,space%d annotated as kind %s" % (reg, space, m.group(3)))
                    redirect[m.group(1)] = (raws[0][1], fmt)
                    stats["handles"] += 1
            continue
        # 1. resource metadata: {id, type* undef, !"", space, reg, 1, kind 10 (TypedBuffer), sample count 0, !{elem type}}
        meta = re.compile(r'^(!\d+ = !\{i32 \d+, %%[^*]+\* undef, !"[^"]*", i32 %d, i32 %d, i32 1, )i32 10, i32 0, !\d+\}$' % (space, reg))
        hit = [i for i, l in enumerate(lines) if meta.match(l)]
        if not hit:
            continue
        any_meta = True
        lines[hit[0]] = meta.match(lines[hit[0]]).group(1) + "i32 11, i32 0, null}"
        # 2. handles created for the binding
        if space == 0 and reg == 0:
            bind = re.compile(r"^\s*(%s) = call %%dx\.types\.Handle @dx\.op\.createHandleFromBinding\(i32 217, %%dx\.types\.ResBind (?:zeroinitializer|\{ i32 0, i32 0, i32 0, i8 0 \})" % ID)
        else:
            bind = re.compile(r"^\s*(%s) = call %%dx\.types\.Handle @dx\.op\.createHandleFromBinding\(i32 217, %%dx\.types\.ResBind \{ i32 %d, i32 %d, i32 %d, i8 0 \}" % (ID, reg, reg, space))
        created = {bind.match(l).group(1) for l in lines if bind.match(l)}
        if not created:
            if any("@dx.op.createHandle(" in l for l in lines):
                raise Unsupported("old-style createHandle")
            # declared but never used: the metadata edit alone is right
            continue
        # 3. annotations {10, <component type/count>} -> {11, 0}
        annot = re.compile(r"^(\s*(%s) = call %%dx\.types\.Handle @dx\.op\.annotateHandle\(i32 216, %%dx\.types\.Handle (%s), %%dx\.types\.ResourceProperties )\{ i32 (\d+), i32 (\d+) \}(\).*)$" % (ID, ID))
        for i, l in enumerate(lines):
            m = annot.match(l)
            if not m or m.group(3) not in created:
                continue
            if m.group(4) != "10":
                raise Unsupported("handle for t%d,space%d annotated as kind %s" % (reg, space, m.group(4)))
            tail = re.sub(r"TypedBuffer<[^>]*>", "ByteAddressBuffer", m.group(6))
            lines[i] = m.group(1) + "{ i32 11, i32 0 }" + tail
            handle_fmt[m.group(2)] = fmt
            stats["handles"] += 1
        # a created handle used without annotateHandle (should not happen in SM 6.6 output)
        for l in lines:
            for c in created:
                if re.search(r"@dx\.op\.(?:bufferLoad|rawBufferLoad|getDimensions)\.?\w*\(i32 \d+, %%dx\.types\.Handle %s[,)]" % re.escape(c), l):
                    raise Unsupported("handle used without annotateHandle")
    if not any_meta:
        return None, None
    # 4. loads
    load = re.compile(r"^(\s*)(%s) = call %%dx\.types\.ResRet\.(i32|f32) @dx\.op\.bufferLoad\.(?:i32|f32)\(i32 68, %%dx\.types\.Handle (%s), i32 ([^,]+), i32 undef\)(.*)$" % (ID, ID))
    extract = re.compile(r"^(\s*)(%s) = extractvalue %%dx\.types\.ResRet\.f32 (%s), (\d)\s*$" % (ID, ID))
    out = []
    f32_loads = {}   # ResRet value -> (word value, fmt)
    n = 0
    need_i32_decl = False
    for l in lines:
        m = load.match(l)
        if m and (m.group(4) in handle_fmt or m.group(4) in redirect):
            ind, val, ty, h, idx = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
            n += 1
            if h in redirect:
                raw, fmt = redirect[h]
                out.append(f"{ind}%t2r{n}h = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle {raw}, %dx.types.ResourceProperties {{ i32 11, i32 0 }})  ; AnnotateHandle(res,props)  resource: ByteAddressBuffer")
                h = f"%t2r{n}h"
            else:
                fmt = handle_fmt[h]
            stats["loads"] += 1
            need_i32_decl = True
            out.append(f"{ind}%t2r{n}i = shl i32 {idx}, 2")
            if fmt == "u32":
                if ty != "i32":
                    raise Unsupported("u32 view loaded as " + ty)
                out.append(f"{ind}{val} = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle {h}, i32 %t2r{n}i, i32 undef, i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)")
            else:
                if ty != "f32":
                    raise Unsupported(fmt + " view loaded as " + ty)
                out.append(f"{ind}%t2r{n}r = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle {h}, i32 %t2r{n}i, i32 undef, i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)")
                out.append(f"{ind}%t2r{n}w = extractvalue %dx.types.ResRet.i32 %t2r{n}r, 0")
                f32_loads[val] = (f"%t2r{n}w", fmt, n)
            continue
        m = extract.match(l)
        if m and m.group(3) in f32_loads:
            ind, val, comp = m.group(1), m.group(2), int(m.group(4))
            word, fmt, k = f32_loads[m.group(3)]
            if comp > 3:
                raise Unsupported("status component of a typed load is used")
            shift, mask, scale = DECODE[fmt][comp]
            tag = f"%t2r{k}c{comp}_{len(out)}"
            src = word
            if shift:
                out.append(f"{ind}{tag}s = lshr i32 {word}, {shift}")
                src = tag + "s"
            if shift + mask.bit_length() < 32:
                out.append(f"{ind}{tag}m = and i32 {src}, {mask}")
                src = tag + "m"
            out.append(f"{ind}{tag}f = uitofp i32 {src} to float")
            out.append(f"{ind}{val} = fmul fast float {tag}f, {fconst(scale)}")
            continue
        out.append(l)
    # any other use of a rewritten f32 load result (passed whole somewhere) is not handled
    for v in f32_loads:
        pat = re.compile(re.escape(v) + r"(?![A-Za-z0-9_.])")
        if any(pat.search(l) for l in out):
            raise Unsupported("typed load result used other than by extractvalue")
    for h in list(handle_fmt) + list(redirect):
        pat = re.compile(r"@dx\.op\.(?!rawBufferLoad|annotateHandle)\w+(?:\.\w+)?\([^)]*%dx\.types\.Handle " + re.escape(h) + r"(?![A-Za-z0-9_.])")
        if any(pat.search(l) for l in out):
            raise Unsupported("rewritten handle has a use other than a load")
    # 5. declarations: rawBufferLoad.i32 present; bufferLoad.* dropped when no longer called
    if need_i32_decl and not any(l.startswith("declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(") for l in out):
        attr = None
        for l in out:
            m = re.match(r"^declare %dx\.types\.ResRet\.\w+ @dx\.op\.(?:bufferLoad|rawBufferLoad)\.\w+\(.*\) (#\d+)$", l)
            if m:
                attr = m.group(1)
                break
        if attr is None:
            raise Unsupported("no load declaration to copy attributes from")
        at = max(i for i, l in enumerate(out) if l.startswith("declare "))
        out[at + 1:at + 1] = ["", "; Function Attrs: nounwind readonly", f"declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) {attr}"]
    for ty in ("i32", "f32"):
        decl = f"declare %dx.types.ResRet.{ty} @dx.op.bufferLoad.{ty}("
        if not any(f"call %dx.types.ResRet.{ty} @dx.op.bufferLoad.{ty}(" in l for l in out):
            for i, l in enumerate(out):
                if l.startswith(decl):
                    lo = i - 1 if i and out[i - 1].startswith("; Function Attrs") else i
                    del out[lo:i + 1]
                    break
    if handle_fmt:
        set_raw_flag(out)
    if not stats["loads"] and redirect:
        return None, None
    return "\n".join(out), stats


def set_raw_flag(lines):
    """Shader flag 0x10 (EnableRawAndStructuredBuffers) in the entry point's properties."""
    ep = next((re.match(r"!dx\.entryPoints = !\{(!\d+)", l) for l in lines if l.startswith("!dx.entryPoints = !{")), None)
    if not ep:
        raise Unsupported("no entry point list")
    i = next((i for i, l in enumerate(lines) if l.startswith(ep.group(1) + " = !{")), None)
    m = re.search(r", (!\d+|null)\}$", lines[i]) if i is not None else None
    if not m:
        raise Unsupported("entry point properties not found")
    if m.group(1) == "null":
        raise Unsupported("entry point has no properties list")
    j = next((j for j, l in enumerate(lines) if l.startswith(m.group(1) + " = !{")), None)
    if j is None:
        raise Unsupported("entry point properties not found")
    l = lines[j]
    f = re.search(r"\{i32 0, i64 (\d+)", l)
    if f:
        lines[j] = l[:f.start(1)] + str(int(f.group(1)) | 16) + l[f.end(1):]
    else:
        k = l.index("!{") + 2
        lines[j] = l[:k] + "i32 0, i64 16, " + l[k:]


def name_values(ir):
    """%N -> %vN and `; <label>:N` -> `vN:` inside functions, so DXC's assembler takes the edited text."""
    out, inside = [], False
    ren = lambda s: re.sub(r"%(\d+)(?![A-Za-z0-9_])", r"%v\1", s)
    for l in ir.split("\n"):
        if l.startswith("define "):
            inside = True
            out += [l, "v0:"]
            continue
        if inside and l == "}":
            inside = False
        if inside:
            m = re.match(r"^; <label>:(\d+)(.*)$", l)
            out.append(f"v{m.group(1)}:{ren(m.group(2))}" if m else ren(l))
        else:
            out.append(l)
    return "\n".join(out)


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    specs = []
    for s in argv[3:]:
        f = s.split(":")
        if len(f) not in (3, 5) or f[2] not in ("u32", "rgba8", "rgb10a2"):
            print("bad spec " + s)
            return 2
        specs.append((int(f[0]), int(f[1]), f[2]) + tuple(int(x) for x in f[3:]))
    ir = open(argv[1], encoding="utf-8", errors="surrogateescape").read()
    try:
        text, stats = rewrite(ir, specs)
    except Unsupported as e:
        print("unsupported: " + str(e))
        return 1
    if text is None:
        return 3
    open(argv[2], "w", encoding="utf-8", errors="surrogateescape").write(name_values(text))
    print(f"handles {stats['handles']} loads {stats['loads']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
