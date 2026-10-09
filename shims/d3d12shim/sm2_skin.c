// Spider-Man 2 skinned-model steering (LAYOVER_SKIN_MB, default 256; 0 disables).
//
// The game keeps every model's GPU data in one 2046 MB "ManagedBuffer" pool and skins characters
// with compute shaders that read the model data through 4-byte typed buffer views over the whole
// pool. Metal caps such a view at 2^28 elements = the first 1 GB of the pool, so a skinned model
// whose blob lands above 1 GB reads zeros and stays in its bind pose (T-pose). Static geometry is
// read raw and does not care.
//
// Fix: give skinned models their own region in the first 1 GB. The engine's own machinery does the
// work; we only decide placement:
//   * D3DBufferManager::CreateSubHeap (exe+0x2c5a0d0) carves a region out of the pool bottom-up.
//     The game creates one 526 MB subheap early (impostors; their data is read through the typed
//     views as well, so it must stay below 1 GB: measured peak use 193 MB over 3 hours). Right before
//     that call we create ours and shrink the impostor heap to 16 MB, so the first 1 GB is: [0,132)
//     small allocations, [132,1008) our shared heap (skinned models + impostor data, redirected in the
//     AllocateBuffer hook while the heap keeps LAYOVER_SKIN_RESERVE_MB free for skinned models),
//     [1008,1024) the game's own impostor heap (fallback). Static geometry (peak ~690 MB) goes above
//     1 GB where raw reads work fine. Impostor frees name their heap as owner; the FreeBuffer hook
//     clears that so the record is resolved by address.
//   * Model loading pre-allocates the GPU blob before the data is read (one AllocateBuffer per
//     model, nothing there says "skinned"). The loader DMA's the asset's GPU segment into a CPU
//     mirror of the pool at the blob's offset. When the data has arrived, the model handler
//     (exe+0x2b24750: (mgr, model, slot)) parses the CPU segment, registers subsets from the blob's
//     address, and only THEN uploads mirror -> GPU (D3DBufferManager::UploadToGpu, DataCopy 2).
//     We hook its entry: the "Model Built" section (tag 0x283d0383) starts with a 64-bit flags word
//     whose bit 1 means skinned (the game tests exactly this for its sphere-collision fallback). If
//     the blob ends above 1 GB, allocate a new one from our subheap (AllocateBuffer with that heap as
//     the preferred D3DHeapAlloc), memcpy the mirror bytes, free the old record (deferred, like the
//     game does) and patch the slot: record pointer at slot+0x90, mirror address at [slot+0x28]+0x18.
//     Everything downstream reads those two fields.
//   * Frees resolve the owning allocator by address range, so relocated blobs free back into our heap.
// Build v2.810.0.0 addresses; every hook checks the expected bytes before patching.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void shim_note(const char *fmt, ...);
int shim_readable(const void *p, SIZE_T n);
#define SNOTE shim_note

typedef struct Regs { UINT64 rflags, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax; } Regs;
typedef void (*HookFn)(Regs *);

static UINT8 *exe_base;
static UINT8 *tramp_mem; static size_t tramp_used, tramp_cap;

static int tramp_init(void)
{
    INT64 d;
    if (tramp_mem) return 1;
    // the exe sits at 0x140000000; jmp rel32 needs the trampolines within +-2 GB of it
    for (d = 0x1000000; d < 0x7f000000; d += 0x1000000) {
        void *p = VirtualAlloc(exe_base - d, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) { tramp_mem = p; tramp_cap = 0x10000; return 1; }
    }
    for (d = 0x10000000; d < 0x7f000000; d += 0x1000000) {
        void *p = VirtualAlloc(exe_base + d, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) { tramp_mem = p; tramp_cap = 0x10000; return 1; }
    }
    return 0;
}

// Detour at exe+rva: jmp rel32 -> trampoline { save regs; handler(&regs); restore; original n bytes; jmp back }.
// The first n bytes at the site must be whole, position-independent instructions (checked against `expect`).
static int install_hook(UINT64 rva, const UINT8 *expect, int n, HookFn handler, const char *name)
{
    static const UINT8 pro[] = { 0x50,0x51,0x52,0x53,0x55,0x56,0x57, 0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57, 0x9C,
                                 0x48,0x89,0xE1, 0x48,0x89,0xE3, 0x48,0x83,0xE4,0xF0, 0x48,0x83,0xEC,0x20, 0x48,0xB8 };
    static const UINT8 epi[] = { 0xFF,0xD0, 0x48,0x89,0xDC, 0x9D, 0x41,0x5F,0x41,0x5E,0x41,0x5D,0x41,0x5C,0x41,0x5B,0x41,0x5A,0x41,0x59,0x41,0x58, 0x5F,0x5E,0x5D,0x5B,0x5A,0x59,0x58 };
    UINT8 *site = exe_base + rva, *t; UINT64 h = (UINT64)(UINT_PTR)handler; INT64 rel; DWORD old; size_t need = sizeof pro + 8 + sizeof epi + n + 5;
    if (!shim_readable(site, n) || memcmp(site, expect, n)) { SNOTE("hook %s at exe+0x%llx: unexpected code bytes, not installed (game build changed?)", name, (unsigned long long)rva); return 0; }
    if (!tramp_init() || tramp_used + need + 16 > tramp_cap) { SNOTE("hook %s: no trampoline memory", name); return 0; }
    t = tramp_mem + ((tramp_used + 15) & ~(size_t)15);
    memcpy(t, pro, sizeof pro); memcpy(t + sizeof pro, &h, 8); memcpy(t + sizeof pro + 8, epi, sizeof epi);
    memcpy(t + sizeof pro + 8 + sizeof epi, site, n);
    { UINT8 *j = t + sizeof pro + 8 + sizeof epi + n; rel = (INT64)(site + n) - (INT64)(j + 5); j[0] = 0xE9; memcpy(j + 1, &rel, 4); }
    tramp_used = (t - tramp_mem) + need;
    rel = (INT64)t - (INT64)(site + 5);
    if (rel != (INT32)rel) { SNOTE("hook %s: trampoline out of reach", name); return 0; }
    if (!VirtualProtect(site, n, PAGE_EXECUTE_READWRITE, &old)) { SNOTE("hook %s: VirtualProtect failed (%lu)", name, GetLastError()); return 0; }
    { UINT8 patch[16]; patch[0] = 0xE9; memcpy(patch + 1, &rel, 4); memset(patch + 5, 0x90, n - 5); memcpy(site, patch, n); }
    VirtualProtect(site, n, old, &old); FlushInstructionCache(GetCurrentProcess(), site, n);
    SNOTE("hook %s installed at exe+0x%llx (trampoline %p)", name, (unsigned long long)rva, (void *)t);
    return 1;
}

// ---- game functions and objects (v2.810.0.0) ----
#define RVA_MGR            0xc0dc960   // the one D3DBufferManager (ManagedBuffer)
#define RVA_ALLOCATE       0x2c59d80   // IABuffer *D3DBufferManager::AllocateBuffer(D3DHeapAlloc *, DXGI_FORMAT, UINT size, const void *data, const char *name, DataCopy, MemoryLocation)
#define RVA_CREATE_SUBHEAP 0x2c5a0d0   // D3DHeapAlloc *CreateSubHeap(mgr, UINT size, UINT entries, UINT tag)
#define RVA_FREE_DEFERRED  0x2c5a800   // FreeBufferDeferred(mgr, D3DHeapAlloc *owner_or_null, IABuffer **rec)  (rec is cleared)
#define RVA_FREE           0x2c5a860   // FreeBuffer(mgr, owner_or_null, IABuffer **rec)
#define RVA_FIND_SECTION   0x30a07a0   // SectionEntry *FindSection(const void *dat1, UINT tag): {tag, offset, size}
#define RVA_MODEL_ARRIVED  0x2b24750   // model handler: data arrived (mgr, model, slot)
#define RVA_IMPOSTOR_HEAP  0xbf2dd80   // qword: the impostor system's D3DHeapAlloc* (set right after its CreateSubHeap; used as alloc target and free owner)
#define TAG_MODEL_BUILT    0x283d0383u
#define TAG_MODEL_SKIN     0xc5354b60u  // "Model Skin Data": present (with joints 0x15df9d3b and skin batches 0xdcc88a19) exactly on GPU-skinned models
#define MGR_MIRROR_BASE    0xb20       // qword: CPU mirror of the pool (allocation addresses are mirror addresses)
#define REC_BASE 8
#define REC_FMT 0x10
#define REC_OFF 0x14
#define REC_FLAG 0x18
#define REC_SIZE 0x1c
#define SLOT_RES 0x28                  // -> { cpu data ptr @0, cpu size @8, ?, mirror addr of the GPU blob @0x18, gpu size @0x20 }
#define SLOT_REC 0x90                  // IABuffer *
#define LIMIT_1G (1024ull << 20)

typedef void *(*PFN_Allocate)(void *mgr, void *heap, UINT fmt, UINT size, const void *data, const char *name, UINT datacopy, UINT memloc);
typedef void *(*PFN_CreateSubHeap)(void *mgr, UINT size, UINT entries, UINT tag);
typedef void (*PFN_FreeDeferred)(void *mgr, void *owner, void **rec);
typedef const UINT8 *(*PFN_FindSection)(const void *dat1, UINT tag);
static PFN_Allocate g_alloc; static PFN_CreateSubHeap g_create_subheap; static PFN_FreeDeferred g_free_deferred; static PFN_FindSection g_find_section;
static UINT8 *g_mgr;
static void *skin_heap; static UINT64 skin_heap_lo, skin_heap_hi;   // our subheap and its pool-offset range
static UINT skin_mb = 876; static UINT impostor_mb = 16; static UINT reserve_mb = 48; static int plug_on = 0; static int skin_log;
static volatile LONG imp_redirected, imp_overflow;
static void *plug;   // temporary block filling [skin heap end, 1 GB) while the game creates its impostor subheap, so that heap lands above 1 GB
static void *imp_heap; static UINT64 imp_lo, imp_hi, imp_req; static volatile LONG64 imp_live, imp_peak, skin_peak; static volatile LONG imp_allocs;
static volatile LONG st_models, st_skinned, st_moved, st_fail, st_already_low, st_freed; static volatile LONG64 st_live;
static CRITICAL_SECTION skin_lock; static int in_create;

static int in_skin_heap(UINT64 off) { return skin_heap && off >= skin_heap_lo && off < skin_heap_hi; }
// exact live bytes inside our heap, from the manager's record table (index allocator at mgr+0x510: capacity @+0x18; records at [mgr+0x538], 0x20 bytes, byte +0x18 = live)
static UINT64 skin_live_exact(void)
{
    int n = *(int *)(g_mgr + 0x528), i; UINT8 *tab = *(UINT8 **)(g_mgr + 0x538); UINT64 b = 0;
    if (!tab || n <= 0 || n > 1000000 || !shim_readable(tab, (SIZE_T)n * 0x20)) return (UINT64)st_live;
    for (i = 0; i < n; i++) { UINT8 *rec = tab + (SIZE_T)i * 0x20; if (rec[0x18] && in_skin_heap(*(UINT32 *)(rec + REC_OFF))) b += *(UINT32 *)(rec + REC_SIZE); }
    return b;
}

static void create_skin_heap(const char *why)
{
    UINT8 *h; UINT64 base, mirror;
    if (skin_heap || in_create || !skin_mb) return;
    in_create = 1;
    h = g_create_subheap(g_mgr, skin_mb << 20, 0x8000, 0);
    in_create = 0;
    if (!h) { SNOTE("skin heap: CreateSubHeap(%u MB) failed (%s); skinned models will not be steered", skin_mb, why); skin_mb = 0; return; }
    base = *(UINT64 *)(h + 0x4d0); mirror = *(UINT64 *)(g_mgr + MGR_MIRROR_BASE);
    skin_heap_lo = base - mirror; skin_heap_hi = skin_heap_lo + ((UINT64)skin_mb << 20); skin_heap = h;
    SNOTE("skin heap: %u MB subheap at pool offset [%llu MB, %llu MB) (%s)%s", skin_mb, (unsigned long long)(skin_heap_lo >> 20), (unsigned long long)(skin_heap_hi >> 20), why,
          skin_heap_hi > LIMIT_1G ? " WARNING: extends past 1 GB" : "");
}

// pre-hook on CreateSubHeap: make ours before the game's first (the 526 MB impostor heap)
static void on_create_subheap(Regs *r)
{
    if (skin_heap || in_create) return;
    SNOTE("game CreateSubHeap(size %llu MB, entries %#llx, tag %#llx) from exe+0x%llx; creating the skin heap first", (unsigned long long)(r->rdx >> 20), (unsigned long long)r->r8, (unsigned long long)r->r9, (unsigned long long)(((UINT64 *)(r + 1))[0] - (UINT64)exe_base));
    EnterCriticalSection(&skin_lock); create_skin_heap("before the game's first subheap"); LeaveCriticalSection(&skin_lock);
    imp_req = r->rdx & 0xffffffff;
    if (plug_on && skin_heap && skin_heap_hi < LIMIT_1G) {
        UINT64 want = LIMIT_1G - skin_heap_hi;
        plug = g_alloc(g_mgr, NULL, 0, (UINT)want, NULL, "LayoverPlug", 2, 0);
        if (plug) SNOTE("plug: %llu MB at pool offset %u MB so the impostor subheap goes above 1 GB", (unsigned long long)(want >> 20), *(UINT32 *)((UINT8 *)plug + REC_OFF) >> 20);
        else SNOTE("plug allocation of %llu MB failed; impostor subheap will stay low", (unsigned long long)(want >> 20));
    }
    if (impostor_mb && imp_req > ((UINT64)impostor_mb << 20)) { r->rdx = (UINT64)impostor_mb << 20; SNOTE("impostor subheap capped: %llu MB -> %u MB (LAYOVER_IMPOSTOR_MB)", (unsigned long long)(imp_req >> 20), impostor_mb); imp_req = r->rdx; }
}

// pre-hook on the model data-arrived handler (rcx = ModelManager, rdx = model, r8 = slot)
static void on_model_arrived(Regs *r)
{
    UINT8 *slot = (UINT8 *)r->r8, *res, *cpu, *rec, *nrec; const UINT8 *sec; UINT64 flags, off, size, mirror, noff; void *old; char nm[80] = "";
    LONG nth = InterlockedIncrement(&st_models); int dbg = skin_log && nth <= 12;
    if (!slot || !shim_readable(slot, 0x98)) { if (dbg) SNOTE("arrival %ld: slot %p unreadable", nth, (void *)slot); return; }
    res = *(UINT8 **)(slot + SLOT_RES); rec = *(UINT8 **)(slot + SLOT_REC);
    if (!res || !rec || !shim_readable(res, 0x28) || !shim_readable(rec, 0x20)) { if (dbg) SNOTE("arrival %ld: res %p rec %p", nth, (void *)res, (void *)rec); return; }
    cpu = *(UINT8 **)res;
    if (!cpu || !shim_readable(cpu, 0x10) || !shim_readable(cpu, 0x10 + 12 * *(UINT16 *)(cpu + 0xc))) { if (dbg) SNOTE("arrival %ld: cpu %p unreadable", nth, (void *)cpu); return; }
    sec = g_find_section(cpu, TAG_MODEL_BUILT);
    if (dbg) { char hx[80]; int k; for (k = 0; k < 24; k++) sprintf(hx + 2 * k, "%02x", cpu[k]); SNOTE("arrival %ld: rec off %u size %u cpu %p [%s] sections %u sec %p", nth, *(UINT32 *)(rec + REC_OFF), *(UINT32 *)(rec + REC_SIZE), (void *)cpu, hx, *(UINT16 *)(cpu + 0xc), (const void *)sec); }
    if (!sec || !shim_readable(sec, 12)) return;
    { UINT32 so = *(const UINT32 *)(sec + 4); if (!shim_readable(cpu + so, 8)) return; flags = *(const UINT64 *)(cpu + so);
      if (dbg) SNOTE("arrival %ld: Model Built at +%u size %u flags %016llx", nth, so, *(const UINT32 *)(sec + 8), (unsigned long long)flags); }
    if (skin_log >= 2 && nth <= 6000) {   // survey mode: name, flags, size, section tags
        char tags[400] = "", path[200] = ""; int k, n = *(UINT16 *)(cpu + 0xc); size_t o = 0; const UINT8 *m = (const UINT8 *)r->rdx;
        for (k = 0; k < n && o < sizeof tags - 12; k++) o += snprintf(tags + o, sizeof tags - o, "%08x,", *(const UINT32 *)(cpu + 0x10 + 12 * k));
        if (m && shim_readable(m, 0x18)) { const char *q = *(const char **)(m + 0x10); if (q && shim_readable(q, 200)) { for (k = 0; k < 199 && q[k] >= 0x20 && q[k] < 0x7f; k++) path[k] = q[k]; path[k] = 0; } }
        SNOTE("survey flags=%016llx hdr=%08x%08x%08x%08x%08x%08x size=%u off=%u tags=%s name=%s", (unsigned long long)flags,
              *(const UINT32 *)(cpu + *(const UINT32 *)(sec + 4) + 8), *(const UINT32 *)(cpu + *(const UINT32 *)(sec + 4) + 12), *(const UINT32 *)(cpu + *(const UINT32 *)(sec + 4) + 16), *(const UINT32 *)(cpu + *(const UINT32 *)(sec + 4) + 20), *(const UINT32 *)(cpu + *(const UINT32 *)(sec + 4) + 24), *(const UINT32 *)(cpu + *(const UINT32 *)(sec + 4) + 28),
              *(UINT32 *)(rec + REC_SIZE), *(UINT32 *)(rec + REC_OFF), tags, path);
    }
    if (!g_find_section(cpu, TAG_MODEL_SKIN)) return;   // not skinned (the Model Built flags do not say it at this point)
    InterlockedIncrement(&st_skinned);
    off = *(UINT32 *)(rec + REC_OFF); size = *(UINT32 *)(rec + REC_SIZE); mirror = *(UINT64 *)(g_mgr + MGR_MIRROR_BASE);
    { const UINT8 *m = (const UINT8 *)r->rdx; if (skin_log && m && shim_readable(m, 0x18)) { const char *p = *(const char **)(m + 0x10); if (p && shim_readable(p, 64)) { int k; for (k = 0; k < 63 && p[k] >= 0x20 && p[k] < 0x7f; k++) nm[k] = p[k]; nm[k] = 0; } } }
    if (off + size <= LIMIT_1G) { InterlockedIncrement(&st_already_low); if (skin_log) SNOTE("skinned model %s: %llu KB at %llu MB already below 1 GB", nm, (unsigned long long)(size >> 10), (unsigned long long)(off >> 20)); return; }
    if (!skin_heap) { EnterCriticalSection(&skin_lock); create_skin_heap("first skinned model"); LeaveCriticalSection(&skin_lock); }
    if (!skin_heap) { InterlockedIncrement(&st_fail); return; }
    nrec = g_alloc(g_mgr, skin_heap, *(UINT32 *)(rec + REC_FMT), (UINT)size, NULL, "LayoverSkinnedModel", 2, 0);
    if (!nrec) { LONG k = InterlockedIncrement(&st_fail); if (k <= 20 || skin_log) SNOTE("skinned model %s: %llu KB at %llu MB could not be moved (skin heap full?)", nm, (unsigned long long)(size >> 10), (unsigned long long)(off >> 20)); return; }
    noff = *(UINT32 *)(nrec + REC_OFF);
    if (noff + size > LIMIT_1G) SNOTE("skinned model %s: new offset %llu MB is still above 1 GB!", nm, (unsigned long long)(noff >> 20));
    memcpy((UINT8 *)mirror + noff, (UINT8 *)mirror + off, (size_t)size);
    *(UINT8 **)(slot + SLOT_REC) = nrec;
    *(UINT64 *)(res + 0x18) = mirror + noff;
    old = rec; g_free_deferred(g_mgr, NULL, &old);
    InterlockedIncrement(&st_moved); { LONG64 v = InterlockedExchangeAdd64(&st_live, (LONG64)size) + (LONG64)size; if (v > skin_peak) skin_peak = v; }
    if (skin_log) SNOTE("skinned model %s: %llu KB moved %llu MB -> %llu MB", nm, (unsigned long long)(size >> 10), (unsigned long long)(off >> 20), (unsigned long long)(noff >> 20));
}

static void resolve_imp_heap(void)
{
    int i, n = *(int *)(g_mgr + 0xb80); UINT8 **list = *(UINT8 ***)(g_mgr + 0xb78);
    if (imp_heap || !skin_heap || !list || n < 1 || !shim_readable(list, n * 8)) return;
    for (i = 0; i < n; i++) if (list[i] && list[i] != skin_heap && shim_readable(list[i], 0x4f8)) {
        imp_heap = list[i]; imp_lo = *(UINT64 *)(list[i] + 0x4d0) - *(UINT64 *)(g_mgr + MGR_MIRROR_BASE); imp_hi = imp_lo + imp_req;
        SNOTE("impostor subheap %p at pool offset [%llu MB, %llu MB)", imp_heap, (unsigned long long)(imp_lo >> 20), (unsigned long long)(imp_hi >> 20));
        // Point the impostor system at our heap instead. Its deferred frees are processed by the manager's
        // frame function, which frees straight through the owner it recorded (bypassing FreeBuffer and our
        // hook); with the owner being our heap, allocation and free both land in the same allocator.
        { void **g = (void **)(exe_base + RVA_IMPOSTOR_HEAP);
          if (shim_readable(g, 8) && *g == imp_heap) { DWORD old; if (VirtualProtect(g, 8, PAGE_READWRITE, &old)) { *g = skin_heap; VirtualProtect(g, 8, old, &old); SNOTE("impostor heap pointer redirected to the shared heap"); } else SNOTE("impostor heap pointer: VirtualProtect failed"); }
          else SNOTE("impostor heap pointer global does not hold the heap (%p); redirect by request instead", *g); }
        return;
    }
}
// pre-hook on AllocateBuffer (rcx mgr, rdx heap, r8 fmt, r9 size): impostor-heap accounting
typedef void (*PFN_Free)(void *mgr, void *owner, void **rec);
static void on_alloc(Regs *r)
{
    if (!imp_heap) { resolve_imp_heap(); if (imp_heap && plug) { void *p = plug; plug = NULL; ((PFN_Free)(exe_base + RVA_FREE))(g_mgr, NULL, &p); SNOTE("plug released"); } }
    if (imp_heap && (void *)r->rdx == imp_heap) {
        UINT64 size = r->r9 & 0xffffffff; LONG64 v = InterlockedExchangeAdd64(&imp_live, (LONG64)size) + (LONG64)size; LONG k = InterlockedIncrement(&imp_allocs); if (v > imp_peak) imp_peak = v;
        // impostor data is read through the typed views too: serve it from the shared low heap while that keeps a reserve for skinned models
        if (skin_heap && (k % 16 == 0 || (UINT64)st_live + size + ((UINT64)reserve_mb << 20) > ((UINT64)skin_mb << 20))) st_live = (LONG64)skin_live_exact();   // resync the estimate (failed requests are never freed)
        if (skin_heap && (UINT64)st_live + size + ((UINT64)reserve_mb << 20) <= ((UINT64)skin_mb << 20)) {
            r->rdx = (UINT64)(UINT_PTR)skin_heap; InterlockedIncrement(&imp_redirected); { LONG64 w = InterlockedExchangeAdd64(&st_live, (LONG64)size) + (LONG64)size; if (w > skin_peak) skin_peak = w; }
        } else { LONG o = InterlockedIncrement(&imp_overflow); if (o <= 10) SNOTE("impostor request of %llu KB not redirected: shared heap live %lld MB", (unsigned long long)(size >> 10), (long long)(st_live >> 20)); }
        if (skin_log >= 2 && k <= 400) { UINT64 *sp = (UINT64 *)(r + 1); const char *nm = (const char *)sp[6]; char n[64] = ""; int i;
            if (nm && shim_readable(nm, 64)) { for (i = 0; i < 63 && nm[i] >= 0x20 && nm[i] < 0x7f; i++) n[i] = nm[i]; n[i] = 0; }
            SNOTE("impostor alloc %ld: size %llu fmt %llu data %p datacopy %llu memloc %llu ret exe+0x%llx name '%s'", k, (unsigned long long)size, (unsigned long long)(r->r8 & 0xffffffff), (void *)sp[5], (unsigned long long)sp[7], (unsigned long long)sp[8], (unsigned long long)(sp[0] - (UINT64)exe_base), n); }
    }
}

// pre-hook on FreeBuffer: account for relocated blobs going away (rcx mgr, rdx owner, r8 -> rec)
static void on_free(Regs *r)
{
    UINT8 **pp = (UINT8 **)r->r8, *rec;
    if (!skin_heap || !pp || !shim_readable(pp, 8)) return;
    rec = *pp; if (!rec || !shim_readable(rec, 0x20) || !*(UINT8 *)(rec + REC_FLAG)) return;
    if (in_skin_heap(*(UINT32 *)(rec + REC_OFF))) {
        InterlockedIncrement(&st_freed); InterlockedExchangeAdd64(&st_live, -(LONG64)*(UINT32 *)(rec + REC_SIZE));
        { static volatile LONG nf; LONG k = InterlockedIncrement(&nf); if (skin_log && k <= 12) SNOTE("free in shared heap %ld: owner %p rec %p off %u MB size %u KB ret exe+0x%llx", k, (void *)r->rdx, (void *)rec, *(UINT32 *)(rec + REC_OFF) >> 20, *(UINT32 *)(rec + REC_SIZE) >> 10, (unsigned long long)(((UINT64 *)(r + 1))[0] - (UINT64)exe_base)); }
        if (r->rdx && (void *)r->rdx != skin_heap) r->rdx = 0;   // the impostor code names its own heap as the owner; let FreeBuffer resolve ours by address
    }
    else if (imp_heap && *(UINT32 *)(rec + REC_OFF) >= imp_lo && *(UINT32 *)(rec + REC_OFF) < imp_hi) InterlockedExchangeAdd64(&imp_live, -(LONG64)*(UINT32 *)(rec + REC_SIZE));
}

// called from the shim's CopyBufferRegion hook for every upload into the pool: high-water marks inside our ranges
static volatile LONG64 imp_hw, skin_hw;
void sm2_skin_note_copy(UINT64 doff, UINT64 n)
{
    UINT64 end = doff + n;
    if (imp_heap && doff >= imp_lo && doff < imp_hi) { LONG64 v = (LONG64)(end - imp_lo); if (v > imp_hw) imp_hw = v; }
    if (skin_heap && doff >= skin_heap_lo && doff < skin_heap_hi) { LONG64 v = (LONG64)(end - skin_heap_lo); if (v > skin_hw) skin_hw = v; }
}
// live bytes per pool region, from the manager's own record table (index allocator at mgr+0x510: capacity @+0x18, used @+0x1c; records at [mgr+0x538], 0x20 bytes, byte +0x18 = live)
static void region_usage(char *out, size_t cap)
{
    int n = *(int *)(g_mgr + 0x528), used = *(int *)(g_mgr + 0x52c), i; UINT8 *tab = *(UINT8 **)(g_mgr + 0x538);
    UINT64 b[5] = {0}; int c[5] = {0};
    if (!tab || n <= 0 || n > 1000000 || !shim_readable(tab, (SIZE_T)n * 0x20)) { snprintf(out, cap, "regions: n/a"); return; }
    for (i = 0; i < n; i++) {
        UINT8 *rec = tab + (SIZE_T)i * 0x20; UINT64 off, sz; int k;
        if (!rec[0x18]) continue;
        off = *(UINT32 *)(rec + REC_OFF); sz = *(UINT32 *)(rec + REC_SIZE);
        k = (skin_heap && off < skin_heap_lo) ? 0 : in_skin_heap(off) ? 1 : (imp_heap && off >= imp_lo && off < imp_hi) ? 3 : (imp_heap && off >= imp_hi) ? 4 : 2;
        b[k] += sz; c[k]++;
    }
    snprintf(out, cap, "live MB: small %llu (%d) skin %llu (%d) carve-low %llu (%d) impostor %llu (%d) carve-high %llu (%d) records %d/%d",
             (unsigned long long)(b[0] >> 20), c[0], (unsigned long long)(b[1] >> 20), c[1], (unsigned long long)(b[2] >> 20), c[2], (unsigned long long)(b[3] >> 20), c[3], (unsigned long long)(b[4] >> 20), c[4], used, n);
}
void sm2_skin_stats(char *out, size_t cap)
{
    if (!g_mgr) { out[0] = 0; return; }
    { char rg[300]; region_usage(rg, sizeof rg); shim_note("%s", rg); }
    snprintf(out, cap, "skin: models %ld skinned %ld moved %ld (live %lld MB, peak %lld, heap %u MB) freed %ld low-already %ld failed %ld skin-hw %lld heap-failed-bytes %llu alloc-internal: bytes %lld MB blocks %d freedesc %d | impostor: redirected %ld overflow %ld, own heap hw %lld MB of %llu failed-bytes %llu", st_models, st_skinned, st_moved, (long long)(st_live >> 20), (long long)(skin_peak >> 20), skin_mb, st_freed, st_already_low, st_fail, (long long)(skin_hw >> 20), (unsigned long long)(skin_heap ? *(UINT64 *)((UINT8 *)skin_heap + 0x4e0) : 0), (long long)(skin_heap ? *(INT64 *)((UINT8 *)skin_heap + 0xd0) >> 20 : 0), skin_heap ? *(int *)((UINT8 *)skin_heap + 0x114) : 0, skin_heap ? *(int *)((UINT8 *)skin_heap + 0x110) : 0, imp_redirected, imp_overflow, (long long)(imp_hw >> 20), (unsigned long long)(imp_req >> 20), (unsigned long long)(imp_heap ? *(UINT64 *)((UINT8 *)imp_heap + 0x4e0) : 0));
}

void sm2_skin_install(void)
{
    static const UINT8 e_movrbx[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };   // mov [rsp+0x10], rbx
    char v[32], exepath[MAX_PATH]; const char *base;
    GetModuleFileNameA(NULL, exepath, sizeof exepath); base = strrchr(exepath, '\\'); base = base ? base + 1 : exepath;
    if (_stricmp(base, "Spider-Man2.exe")) return;
    if (GetEnvironmentVariableA("LAYOVER_SKIN_MB", v, sizeof v)) skin_mb = (UINT)atoi(v);
    if (!skin_mb) { SNOTE("LAYOVER_SKIN_MB=0: skinned-model steering off"); return; }
    if (skin_mb < 32 || skin_mb > 892) { SNOTE("LAYOVER_SKIN_MB=%u out of range (32..892); using 876", skin_mb); skin_mb = 876; }
    if (GetEnvironmentVariableA("LAYOVER_IMPOSTOR_MB", v, sizeof v)) impostor_mb = (UINT)atoi(v);   // 0 = leave the game's 526 MB alone
    if (GetEnvironmentVariableA("LAYOVER_PLUG", v, sizeof v)) plug_on = atoi(v) != 0;
    if (GetEnvironmentVariableA("LAYOVER_SKIN_RESERVE_MB", v, sizeof v)) reserve_mb = (UINT)atoi(v);
    skin_log = GetEnvironmentVariableA("LAYOVER_SKIN_LOG", v, sizeof v) >= 1 ? atoi(v) : 0;
    InitializeCriticalSection(&skin_lock);
    exe_base = (UINT8 *)GetModuleHandleA(NULL);
    g_mgr = exe_base + RVA_MGR;
    g_alloc = (PFN_Allocate)(exe_base + RVA_ALLOCATE); g_create_subheap = (PFN_CreateSubHeap)(exe_base + RVA_CREATE_SUBHEAP);
    g_free_deferred = (PFN_FreeDeferred)(exe_base + RVA_FREE_DEFERRED); g_find_section = (PFN_FindSection)(exe_base + RVA_FIND_SECTION);
    if (!install_hook(RVA_MODEL_ARRIVED, e_movrbx, 5, on_model_arrived, "model-data-arrived")) { g_mgr = NULL; return; }
    install_hook(RVA_CREATE_SUBHEAP, e_movrbx, 5, on_create_subheap, "CreateSubHeap");
    install_hook(RVA_FREE, e_movrbx, 5, on_free, "FreeBuffer");
    { static const UINT8 e_alloc[] = { 0x44, 0x89, 0x4C, 0x24, 0x20 }; install_hook(RVA_ALLOCATE, e_alloc, 5, on_alloc, "AllocateBuffer"); }
    SNOTE("skinned-model steering on: skin heap %u MB (LAYOVER_SKIN_MB), impostor heap cap %u MB (LAYOVER_IMPOSTOR_MB, 0 = none), plug %d (LAYOVER_PLUG), per-model log %d (LAYOVER_SKIN_LOG)", skin_mb, impostor_mb, plug_on, skin_log);
}
