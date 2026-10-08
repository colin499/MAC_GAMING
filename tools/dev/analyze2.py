import re, sys, collections
L = open(sys.argv[1], errors="replace").read().splitlines()
# entry 0x2c59d80: rcx=mgr, r8=fmt, r9=size, s6=name, s8=memloc. inner 0x2c59b56: rax[14]=offset, rax[1c]=rsize
rxe = re.compile(r"\[(\d+)\] logpoint exe\+0x2c59d80 hit \d+: rcx=([0-9a-f]+) rdx=([0-9a-f]+) r8=([0-9a-f]+) r9=([0-9a-f]+) rax=[0-9a-f]+ ret=exe\+0x([0-9a-f]+) s5='[^']*' s6='([^']*)' s7=([0-9a-f]+) s8=([0-9a-f]+)")
rxi = re.compile(r"\[(\d+)\] logpoint exe\+0x2c59b56 .*rax\[14\]=([0-9a-f]+) rax\[1c\]=([0-9a-f]+)")
pend = {}; allocs = []
for l in L:
    m = rxe.search(l)
    if m:
        tid=m.group(1); pend[tid]=dict(mgr=int(m.group(2),16), fmt=int(m.group(4),16)&0xffffffff, size=int(m.group(5),16)&0xffffffff, ret=m.group(6), name=m.group(7), memloc=int(m.group(9),16)&0xffffffff); continue
    m = rxi.search(l)
    if m:
        tid=m.group(1); p=pend.pop(tid,None)
        if p: allocs.append(dict(p, off=int(m.group(2),16), rsize=int(m.group(3),16)))
print("total paired allocs:", len(allocs))
bymgr = collections.defaultdict(list)
for a in allocs: bymgr[a["mgr"]].append(a)
print("\nmanagers (ptr: count, total MB, maxend MB):")
for mgr, al in sorted(bymgr.items(), key=lambda x:-sum(a['size'] for a in x[1])):
    tot=sum(a['size'] for a in al); mx=max(a['off']+a['size'] for a in al)
    print(f"  {mgr:#x}: n={len(al)} total={tot/2**20:.1f} MB maxend={mx/2**20:.1f} MB")
# the ManagedBuffer mgr = the one with the largest maxend (spans the 2 GB pool)
mgr = max(bymgr, key=lambda m: max(a['off']+a['size'] for a in bymgr[m]))
al = bymgr[mgr]
print(f"\n=== ManagedBuffer mgr {mgr:#x}: {len(al)} allocs ===")
MB=2**20
# group by caller (ret) -> count, MB, how much above 1GB
g=collections.defaultdict(lambda:[0,0,0,0])
for a in al:
    k=a['ret']; x=g[k]; x[0]+=1; x[1]+=a['size']; x[3]+=(a['off']+a['size']>1024*MB); x[2]=max(x[2],a['off']+a['size'])
print("by caller (ret): n, totalMB, maxendMB, countAbove1G")
for k,x in sorted(g.items(),key=lambda y:-y[1][1]): print(f"  ret exe+0x{k}: n={x[0]} tot={x[1]/MB:.1f} maxend={x[2]/MB:.1f} above1G={x[3]}")
# names
g2=collections.defaultdict(lambda:[0,0,0])
for a in al:
    nm=a['name'] if not a['name'].startswith('[') else '[hash]'; x=g2[nm]; x[0]+=1; x[1]+=a['size']; x[2]+=(a['off']+a['size']>1024*MB)
print("\nby name: n, totalMB, countAbove1G")
for k,x in sorted(g2.items(),key=lambda y:-y[1][1])[:25]: print(f"  {k!r}: n={x[0]} tot={x[1]/MB:.1f} above1G={x[2]}")
print("\nmemloc split:", collections.Counter(a['memloc'] for a in al))
print("fmt split:", collections.Counter(a['fmt'] for a in al).most_common(6))
