// Layover d3d12shim: a d3d12.dll that sits in front of D3DMetal's. Three jobs, all opt-in by env:
//   LAYOVER_MANAGED_MB=<n>  Spider-Man 2 fix: shrink the game's "ManagedBuffer" GPU pool so its typed
//                           buffer views fit Metal's 2^28-element limit (see try_patch_budget).
//   LAYOVER_SKIN_MB=<n>     Spider-Man 2 T-pose fix: keep skinned models' GPU data in the first 1 GB of
//                           the pool (sm2_skin.c; default 256 MB, 0 = off; LAYOVER_SKIN_LOG=1 per model).
//   LAYOVER_TSSHIM=1        serve timestamp queries from the CPU clock (Highball's tsshim fix).
//   LAYOVER_D3D12_LOG=<win path>  log what the game asks of D3D12 (feature queries, pools, views,
//                           pipelines, command signatures, counters); LAYOVER_D3D12_VERBOSE=1 for all.
// Built from Highball's tsshim (MIT, gauthierpiarrette/highball-engine, d3dmetal-tsshim-20261005):
// same export forwarding and in-place vtable patching. HB_D3D12_REAL names D3DMetal's real
// d3d12.dll (default d3d12_d3dmetal.dll, laid out beside this file by `layover`).
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static HMODULE real_dll;
static HINSTANCE self;
static int tsshim, verbose, logging;
static UINT64 qpc_freq;
static FILE *log_file;
static CRITICAL_SECTION log_lock;

static FILE *log_out(void)
{
    if (!log_file) {
        char path[MAX_PATH];
        if (GetEnvironmentVariableA("LAYOVER_D3D12_LOG", path, sizeof path)) log_file = fopen(path, "a");
        if (!log_file) log_file = stderr;   // D3DMetal's own diagnostics go there too; empty for Steam-launched games
    }
    return log_file;
}
// LOG is the verbose trace (only with LAYOVER_D3D12_LOG); NOTE always writes (budget patch outcome, failures).
#define LOG(...) do { if (logging) NOTE(__VA_ARGS__); } while (0)
#define NOTE(...) do { EnterCriticalSection(&log_lock); FILE *f_ = log_out(); SYSTEMTIME st_; GetLocalTime(&st_); \
    fprintf(f_, "%02u:%02u:%02u.%03u [%lu] ", st_.wHour, st_.wMinute, st_.wSecond, st_.wMilliseconds, (unsigned long)GetCurrentThreadId()); \
    fprintf(f_, __VA_ARGS__); fputc('\n', f_); fflush(f_); LeaveCriticalSection(&log_lock); } while (0)

#include <stdarg.h>
void shim_note(const char *fmt, ...)
{
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    NOTE("%s", buf);
}
static int readable(const void *p, SIZE_T n);
int shim_readable(const void *p, SIZE_T n) { return readable(p, n); }
void sm2_skin_install(void);
void sm2_skin_stats(char *out, size_t cap);
void sm2_skin_note_copy(UINT64 doff, UINT64 n);

static HMODULE load_real(void)
{
    WCHAR path[1024]; DWORD n;
    if (real_dll) return real_dll;
    n = GetEnvironmentVariableW(L"HB_D3D12_REAL", path, 1024);
    if (n && n < 1024) real_dll = LoadLibraryW(path);
    else real_dll = LoadLibraryW(L"d3d12_d3dmetal.dll");
    if (real_dll == self) { LOG("the real d3d12.dll resolved to the shim itself"); real_dll = NULL; }
    if (!real_dll) LOG("cannot load the real D3DMetal d3d12.dll (error %lu)", GetLastError());
    return real_dll;
}

#define FORWARD(name) \
    static void *p_##name __attribute__((used)); \
    __asm__(".text\n.globl " #name "\n" #name ":\n\tjmp *p_" #name "(%rip)\n");
FORWARD(D3D12CoreCreateLayeredDevice)
FORWARD(D3D12CoreGetLayeredDeviceSize)
FORWARD(D3D12CoreRegisterLayers)
FORWARD(D3D12CreateRootSignatureDeserializer)
FORWARD(D3D12CreateVersionedRootSignatureDeserializer)
FORWARD(D3D12EnableExperimentalFeatures)
FORWARD(D3D12GetDebugInterface)
FORWARD(D3D12SerializeRootSignature)
FORWARD(D3D12SerializeVersionedRootSignature)
FORWARD(GetBehaviorValue)
#define RESOLVE(name) do { p_##name = (void *)GetProcAddress(real_dll, #name); if (!p_##name) LOG("real d3d12.dll lacks %s", #name); } while (0)

// ---- small helpers -------------------------------------------------------------------------
static void patch_slot(void **slot, void *hook, void **saved);
static int readable(const void *p, SIZE_T n);
static UINT64 fnv(const void *p, SIZE_T n) { const UINT8 *b = p; UINT64 h = 1469598103934665603ull; SIZE_T i; for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } return h; }
static void hexdump(char *out, size_t cap, const void *p, UINT n) { const UINT8 *b = p; UINT i; size_t o = 0; if (n > 96) n = 96; for (i = 0; i < n && o + 3 < cap; i++) o += snprintf(out + o, cap - o, "%02x", b[i]); }
static UINT64 res_width(ID3D12Resource *r) { D3D12_RESOURCE_DESC d; if (!r) return 0; r->lpVtbl->GetDesc(r, &d); return d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? d.Width : 0; }

// ---- counters dumped periodically --------------------------------------------------------
static ID3D12Resource *pool_res; static UINT64 pool_width; static volatile LONG64 pool_hw; static volatile LONG pool_oob, pool_copies;
static volatile LONG c_dispatch, c_draw, c_drawidx, c_execind, c_execind_sig, c_execlists, c_copybuf, c_setpso, c_dispatchmesh, c_srv_buf, c_uav_buf, c_srv_tex, c_uav_tex, c_pso_gfx, c_pso_cs, c_pso_gen, c_pso_fail, c_res_buf, c_res_tex, c_map, c_barrier;
static DWORD WINAPI stats_thread(LPVOID arg)
{
    for (;;) {
        Sleep(5000);
        if (pool_res) { char sk[300]; sm2_skin_stats(sk, sizeof sk); NOTE("pool: high-water %llu MB of %llu, copies %ld, past-end %ld | %s", (unsigned long long)(pool_hw >> 20), (unsigned long long)(pool_width >> 20), pool_copies, pool_oob, sk); }
        LOG("stats: execlists=%ld dispatch=%ld dispatchmesh=%ld draw=%ld drawidx=%ld execind=%ld copybuf=%ld setpso=%ld map=%ld barrier=%ld | views srv_buf=%ld uav_buf=%ld srv_tex=%ld uav_tex=%ld | pso gfx=%ld cs=%ld gen=%ld FAIL=%ld | res buf=%ld tex=%ld",
            c_execlists, c_dispatch, c_dispatchmesh, c_draw, c_drawidx, c_execind, c_copybuf, c_setpso, c_map, c_barrier, c_srv_buf, c_uav_buf, c_srv_tex, c_uav_tex, c_pso_gfx, c_pso_cs, c_pso_gen, c_pso_fail, c_res_buf, c_res_tex);
    }
    return 0;
}

// ---- the fake timestamp query heap (Highball) ---------------------------------------------
typedef struct FakeHeap { const ID3D12QueryHeapVtbl *lpVtbl; LONG ref; UINT count; UINT64 *values; ID3D12Device *device; } FakeHeap;
static const ID3D12QueryHeapVtbl fake_heap_vtbl;
static int is_fake(ID3D12QueryHeap *h) { return h && h->lpVtbl == &fake_heap_vtbl; }
static HRESULT STDMETHODCALLTYPE fh_QueryInterface(ID3D12QueryHeap *iface, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ID3D12Object) || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
        || IsEqualGUID(riid, &IID_ID3D12Pageable) || IsEqualGUID(riid, &IID_ID3D12QueryHeap)) { InterlockedIncrement(&((FakeHeap *)iface)->ref); *out = iface; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fh_AddRef(ID3D12QueryHeap *iface) { return InterlockedIncrement(&((FakeHeap *)iface)->ref); }
static ULONG STDMETHODCALLTYPE fh_Release(ID3D12QueryHeap *iface) { FakeHeap *h = (FakeHeap *)iface; ULONG r = InterlockedDecrement(&h->ref); if (!r) { h->device->lpVtbl->Release(h->device); free(h->values); free(h); } return r; }
static HRESULT STDMETHODCALLTYPE fh_GetPrivateData(ID3D12QueryHeap *iface, REFGUID guid, UINT *size, void *data) { if (size) *size = 0; return (HRESULT)0x887A0002L; }
static HRESULT STDMETHODCALLTYPE fh_SetPrivateData(ID3D12QueryHeap *iface, REFGUID guid, UINT size, const void *data) { return S_OK; }
static HRESULT STDMETHODCALLTYPE fh_SetPrivateDataInterface(ID3D12QueryHeap *iface, REFGUID guid, const IUnknown *data) { return S_OK; }
static HRESULT STDMETHODCALLTYPE fh_SetName(ID3D12QueryHeap *iface, LPCWSTR name) { return S_OK; }
static HRESULT STDMETHODCALLTYPE fh_GetDevice(ID3D12QueryHeap *iface, REFIID riid, void **out) { FakeHeap *h = (FakeHeap *)iface; return h->device->lpVtbl->QueryInterface(h->device, riid, out); }
static const ID3D12QueryHeapVtbl fake_heap_vtbl = { fh_QueryInterface, fh_AddRef, fh_Release, fh_GetPrivateData, fh_SetPrivateData, fh_SetPrivateDataInterface, fh_SetName, fh_GetDevice };

#define RING_BYTES (8u << 20)
typedef struct Ring { ID3D12Device *device; ID3D12Resource *buffer; UINT8 *cpu; volatile LONG64 cursor; } Ring;
static Ring rings[4];
static CRITICAL_SECTION ring_lock;
static Ring *ring_for(ID3D12Device *dev)
{
    Ring *r = NULL; int i;
    EnterCriticalSection(&ring_lock);
    for (i = 0; i < 4 && !r; i++) if (rings[i].device == dev) r = &rings[i];
    for (i = 0; i < 4 && !r; i++) if (!rings[i].device) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, RING_BYTES, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
        ID3D12Resource *buf = NULL; void *cpu = NULL;
        if (SUCCEEDED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&buf))
            && SUCCEEDED(buf->lpVtbl->Map(buf, 0, NULL, &cpu))) {
            rings[i].device = dev; dev->lpVtbl->AddRef(dev); rings[i].buffer = buf; rings[i].cpu = cpu; rings[i].cursor = 0; r = &rings[i];
            LOG("tsshim: upload ring of %u bytes created", RING_BYTES);
        } else { if (buf) buf->lpVtbl->Release(buf); LOG("tsshim: upload ring creation failed"); }
        break;
    }
    LeaveCriticalSection(&ring_lock);
    return r;
}
static UINT64 now_ticks(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (UINT64)c.QuadPart; }

// ---- originals -------------------------------------------------------------------------------
#define DECL(ret, name, ...) static ret (STDMETHODCALLTYPE *real_##name)(__VA_ARGS__);
DECL(HRESULT, CreateQueryHeap, ID3D12Device *, const D3D12_QUERY_HEAP_DESC *, REFIID, void **)
DECL(HRESULT, CheckFeatureSupport, ID3D12Device *, D3D12_FEATURE, void *, UINT)
DECL(HRESULT, CreateCommittedResource, ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)
DECL(HRESULT, CreatePlacedResource, ID3D12Device *, ID3D12Heap *, UINT64, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)
DECL(HRESULT, CreateReservedResource, ID3D12Device *, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)
DECL(HRESULT, CreateHeap, ID3D12Device *, const D3D12_HEAP_DESC *, REFIID, void **)
DECL(void, CreateShaderResourceView, ID3D12Device *, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE)
DECL(void, CreateUnorderedAccessView, ID3D12Device *, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE)
DECL(HRESULT, CreateGraphicsPipelineState, ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **)
DECL(HRESULT, CreateComputePipelineState, ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)
DECL(HRESULT, CreatePipelineState, ID3D12Device2 *, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **)
DECL(HRESULT, CreateCommandSignature, ID3D12Device *, const D3D12_COMMAND_SIGNATURE_DESC *, ID3D12RootSignature *, REFIID, void **)
DECL(HRESULT, CreateRootSignature, ID3D12Device *, UINT, const void *, SIZE_T, REFIID, void **)
DECL(HRESULT, CreateCommandQueue, ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **)
DECL(HRESULT, CreateCommandList, ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **)
DECL(HRESULT, CreateCommandList1, ID3D12Device4 *, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void **)
DECL(HRESULT, GetDeviceRemovedReason, ID3D12Device *)
DECL(void, EndQuery, ID3D12GraphicsCommandList *, ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT)
DECL(void, BeginQuery, ID3D12GraphicsCommandList *, ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT)
DECL(void, ResolveQueryData, ID3D12GraphicsCommandList *, ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource *, UINT64)
DECL(void, Dispatch, ID3D12GraphicsCommandList *, UINT, UINT, UINT)
DECL(void, DrawInstanced, ID3D12GraphicsCommandList *, UINT, UINT, UINT, UINT)
DECL(void, DrawIndexedInstanced, ID3D12GraphicsCommandList *, UINT, UINT, UINT, INT, UINT)
DECL(void, ExecuteIndirect, ID3D12GraphicsCommandList *, ID3D12CommandSignature *, UINT, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64)
DECL(void, CopyBufferRegion, ID3D12GraphicsCommandList *, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64, UINT64)
DECL(void, SetPipelineState, ID3D12GraphicsCommandList *, ID3D12PipelineState *)
DECL(void, ResourceBarrier, ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *)
DECL(void, DispatchMesh, ID3D12GraphicsCommandList6 *, UINT, UINT, UINT)
DECL(void, ExecuteCommandLists, ID3D12CommandQueue *, UINT, ID3D12CommandList *const *)
DECL(HRESULT, GetTimestampFrequency, ID3D12CommandQueue *, UINT64 *)
DECL(HRESULT, GetClockCalibration, ID3D12CommandQueue *, UINT64 *, UINT64 *)

// ---- Spider-Man 2 managed-buffer budget ----------------------------------------------------------
// The game's D3DBufferManager sizes its "ManagedBuffer" (one 2046 MB pool with typed buffer views
// over the whole thing) from a table of named budgets keyed by CRC32 of the name. Metal caps a typed
// buffer view at 2^28 elements, so D3DMetal clamps those views and GPU writes/reads above the clamp
// are lost: skinned characters whose data lands there stay in their bind pose. LAYOVER_MANAGED_MB
// rewrites the budget before the pool is created. Table address is for build v2.810.0.0.
static int budget_done;
static int readable(const void *p, SIZE_T n) { MEMORY_BASIC_INFORMATION mbi; return VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (const UINT8 *)p + n <= (const UINT8 *)mbi.BaseAddress + mbi.RegionSize; }
static void try_patch_budget(const char *where)
{
    char v[32], exepath[MAX_PATH]; UINT8 *exe, *tbl, *entries; int count, i, hit = 0; const char *base;
    if (budget_done) return;
    if (!GetEnvironmentVariableA("LAYOVER_MANAGED_MB", v, sizeof v)) { budget_done = 1; return; }
    GetModuleFileNameA(NULL, exepath, sizeof exepath); base = strrchr(exepath, '\\'); base = base ? base + 1 : exepath;
    if (_stricmp(base, "Spider-Man2.exe")) { budget_done = 1; return; }   // the table address below is this game's
    exe = (UINT8 *)GetModuleHandleA(NULL); tbl = exe + 0xc3d9838;
    if (!readable(tbl, 16)) { LOG("budget table at %p not readable (%s)", tbl, where); return; }
    { INT64 raw = *(INT64 *)tbl; entries = raw < 0 ? tbl + (int)raw : (UINT8 *)raw; }   // the game's accessor: negative = offset from the table itself
    count = *(int *)(tbl + 8);
    if (!entries || count <= 0 || count > 4096 || !readable(entries, (SIZE_T)count * 0x20)) { LOG("budget table not ready (%s): entries=%p count=%d", where, entries, count); return; }
    for (i = 0; i < count; i++) {
        UINT8 *e = entries + i * 0x20; UINT32 hash = *(UINT32 *)(e + 0xc); UINT64 val = *(UINT64 *)(e + 0x10);
        const char *name = *(const char **)e; char nm[64] = "?";
        if (name && readable(name, 64)) { int k; for (k = 0; k < 63 && name[k] >= 0x20 && name[k] < 0x7f; k++) nm[k] = name[k]; nm[k] = 0; }
        LOG("budget[%d] hash=%08x value=%llu (%llu MB) q0=%016llx q18=%016llx name?=%s", i, hash, (unsigned long long)val, (unsigned long long)(val >> 20), (unsigned long long)*(UINT64 *)e, (unsigned long long)*(UINT64 *)(e + 0x18), nm);
        if (!strcmp(nm, "ManagedBuffer")) {
            UINT64 want = (UINT64)atoi(v) << 20; DWORD old;
            if (want < (256ull << 20) || want > (4096ull << 20)) { NOTE("LAYOVER_MANAGED_MB=%s ignored (must be 256..4096)", v); budget_done = 1; return; }
            if (VirtualProtect(e + 0x10, 8, PAGE_READWRITE, &old)) { *(UINT64 *)(e + 0x10) = want; VirtualProtect(e + 0x10, 8, old, &old); }
            else *(UINT64 *)(e + 0x10) = want;
            NOTE("ManagedBuffer budget entry %d patched: %llu MB -> %llu MB (%s)", i, (unsigned long long)(val >> 20), (unsigned long long)(want >> 20), where);
            hit = 1;
        }
    }
    if (hit) budget_done = 1; else NOTE("no ManagedBuffer entry among %d budgets (%s); game build changed? fix not applied", count, where);
}

// ---- Spider-Man 2 allocation steering (LAYOVER_SUBHEAP_MB) --------------------------------------
// The ManagedBuffer manager reserves four class subheaps at the bottom of the 2046 MB pool: 32, 32,
// 96 and 416 MB (= 576 MB), and only a small part is ever used, so skinned-mesh data is carved ABOVE
// them and climbs past the 1 GB typed-view limit -> T-pose. Shrinking the big two (96 + 416 MB) pulls
// the carve region down so skinned data lands in the first 1 GB while the pool keeps its full size
// (no exhaustion crash). The immediates "mov edx, 0x06000000 / 0x1a000000" precede the subheap-create
// calls in the two D3DBufferManagerDX12 class-setup functions (build v2.810.0.0). LAYOVER_SUBHEAP_MB
// is the new size (MB) for the 416 MB class; the 96 MB class is scaled to a quarter of it.
static int subheap_done;
static void patch_one_imm(UINT8 *at, UINT32 was, UINT32 now, const char *tag)
{
    DWORD old;
    if (!readable(at, 4) || *(UINT32 *)at != was) { NOTE("subheap imm %s at %p not the expected %#x (found %#x); skipped", tag, at, was, readable(at, 4) ? *(UINT32 *)at : 0); return; }
    if (VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) { *(UINT32 *)at = now; VirtualProtect(at, 4, old, &old); FlushInstructionCache(GetCurrentProcess(), at, 4); NOTE("subheap imm %s: %u MB -> %u MB", tag, was >> 20, now >> 20); }
}
static void try_patch_subheaps(const char *where)
{
    char v[32], exepath[MAX_PATH]; UINT8 *exe; const char *base; UINT32 big, small;
    if (subheap_done) return;
    if (!GetEnvironmentVariableA("LAYOVER_SUBHEAP_MB", v, sizeof v)) { subheap_done = 1; return; }
    GetModuleFileNameA(NULL, exepath, sizeof exepath); base = strrchr(exepath, '\\'); base = base ? base + 1 : exepath;
    if (_stricmp(base, "Spider-Man2.exe")) { subheap_done = 1; return; }
    big = (UINT32)atoi(v) << 20; if (big < (16u << 20) || big > (416u << 20)) { NOTE("LAYOVER_SUBHEAP_MB=%s ignored (16..416)", v); subheap_done = 1; return; }
    small = big / 4; if (small < (16u << 20)) small = (16u << 20);
    exe = (UINT8 *)GetModuleHandleA(NULL);
    // operand is the 4 bytes right after the 0xba (mov edx, imm32) opcode
    patch_one_imm(exe + 0x2cbacce + 1, 0x06000000, small, "fn1.96M");
    patch_one_imm(exe + 0x2cbad05 + 1, 0x1a000000, big,   "fn1.416M");
    patch_one_imm(exe + 0x2cbb339 + 1, 0x06000000, small, "fn2.96M");
    patch_one_imm(exe + 0x2cbb389 + 1, 0x1a000000, big,   "fn2.416M");
    subheap_done = 1;
}

// ---- VRAM cap (LAYOVER_VRAM_MB): what DXGI tells the game about video memory ---------------------
// D3DMetal reports Metal's recommended working set (75% of RAM) as dedicated video memory and twice
// that as the budget; the game sizes its memory budgets from it. We patch the exe's import table for
// CreateDXGIFactory* so the first factory the game creates gets its adapter vtable patched in place
// (D3DMetal shares one vtable per class, so every adapter the game enumerates afterwards is covered).
#include <dxgi1_6.h>
static UINT64 vram_cap;
typedef HRESULT (STDMETHODCALLTYPE *PFN_AGetDesc)(IDXGIAdapter *, DXGI_ADAPTER_DESC *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_AGetDesc1)(IDXGIAdapter1 *, DXGI_ADAPTER_DESC1 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_AGetDesc2)(IDXGIAdapter2 *, DXGI_ADAPTER_DESC2 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_AGetDesc3)(IDXGIAdapter4 *, DXGI_ADAPTER_DESC3 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_AQVMI)(IDXGIAdapter3 *, UINT, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO *);
static PFN_AGetDesc real_AGetDesc; static PFN_AGetDesc1 real_AGetDesc1; static PFN_AGetDesc2 real_AGetDesc2; static PFN_AGetDesc3 real_AGetDesc3; static PFN_AQVMI real_AQVMI;
static SIZE_T capsz(SIZE_T v) { return v > vram_cap ? (SIZE_T)vram_cap : v; }
#define CAPDESC(d) do { (d)->DedicatedVideoMemory = capsz((d)->DedicatedVideoMemory); (d)->DedicatedSystemMemory = capsz((d)->DedicatedSystemMemory); (d)->SharedSystemMemory = capsz((d)->SharedSystemMemory); } while (0)
static HRESULT STDMETHODCALLTYPE hook_AGetDesc(IDXGIAdapter *a, DXGI_ADAPTER_DESC *d) { HRESULT hr = real_AGetDesc(a, d); if (SUCCEEDED(hr) && d) CAPDESC(d); return hr; }
static HRESULT STDMETHODCALLTYPE hook_AGetDesc1(IDXGIAdapter1 *a, DXGI_ADAPTER_DESC1 *d) { HRESULT hr = real_AGetDesc1(a, d); if (SUCCEEDED(hr) && d) CAPDESC(d); return hr; }
static HRESULT STDMETHODCALLTYPE hook_AGetDesc2(IDXGIAdapter2 *a, DXGI_ADAPTER_DESC2 *d) { HRESULT hr = real_AGetDesc2(a, d); if (SUCCEEDED(hr) && d) CAPDESC(d); return hr; }
static HRESULT STDMETHODCALLTYPE hook_AGetDesc3(IDXGIAdapter4 *a, DXGI_ADAPTER_DESC3 *d) { HRESULT hr = real_AGetDesc3(a, d); if (SUCCEEDED(hr) && d) CAPDESC(d); return hr; }
static HRESULT STDMETHODCALLTYPE hook_AQVMI(IDXGIAdapter3 *a, UINT node, DXGI_MEMORY_SEGMENT_GROUP seg, DXGI_QUERY_VIDEO_MEMORY_INFO *i)
{
    HRESULT hr = real_AQVMI(a, node, seg, i);
    if (SUCCEEDED(hr) && i) { if (i->Budget > vram_cap) i->Budget = vram_cap; if (i->AvailableForReservation > vram_cap / 2) i->AvailableForReservation = vram_cap / 2; if (i->CurrentUsage > vram_cap) i->CurrentUsage = vram_cap; }
    return hr;
}
static int adapter_patched;
static void patch_adapter_vtables(IDXGIFactory *f)
{
    IDXGIAdapter *a = NULL; void *p;
    if (adapter_patched || !f || FAILED(f->lpVtbl->EnumAdapters(f, 0, &a)) || !a) return;
    patch_slot((void **)&a->lpVtbl->GetDesc, (void *)hook_AGetDesc, (void **)&real_AGetDesc);
    if (SUCCEEDED(a->lpVtbl->QueryInterface(a, &IID_IDXGIAdapter1, &p)) && p) { IDXGIAdapter1 *a1 = p; patch_slot((void **)&a1->lpVtbl->GetDesc1, (void *)hook_AGetDesc1, (void **)&real_AGetDesc1); patch_slot((void **)&a1->lpVtbl->GetDesc, (void *)hook_AGetDesc, (void **)&real_AGetDesc); a1->lpVtbl->Release(a1); }
    if (SUCCEEDED(a->lpVtbl->QueryInterface(a, &IID_IDXGIAdapter2, &p)) && p) { IDXGIAdapter2 *a2 = p; patch_slot((void **)&a2->lpVtbl->GetDesc2, (void *)hook_AGetDesc2, (void **)&real_AGetDesc2); a2->lpVtbl->Release(a2); }
    if (SUCCEEDED(a->lpVtbl->QueryInterface(a, &IID_IDXGIAdapter3, &p)) && p) { IDXGIAdapter3 *a3 = p; patch_slot((void **)&a3->lpVtbl->QueryVideoMemoryInfo, (void *)hook_AQVMI, (void **)&real_AQVMI); a3->lpVtbl->Release(a3); }
    if (SUCCEEDED(a->lpVtbl->QueryInterface(a, &IID_IDXGIAdapter4, &p)) && p) { IDXGIAdapter4 *a4 = p; patch_slot((void **)&a4->lpVtbl->GetDesc3, (void *)hook_AGetDesc3, (void **)&real_AGetDesc3); a4->lpVtbl->Release(a4); }
    a->lpVtbl->Release(a);
    adapter_patched = 1;
    NOTE("DXGI adapter vtable patched: video memory capped at %llu MB", (unsigned long long)(vram_cap >> 20));
}
typedef HRESULT (WINAPI *PFN_CDF)(REFIID, void **); typedef HRESULT (WINAPI *PFN_CDF2)(UINT, REFIID, void **);
static PFN_CDF real_CDF, real_CDF1; static PFN_CDF2 real_CDF2;
static HRESULT WINAPI hook_CDF(REFIID riid, void **out) { HRESULT hr = real_CDF(riid, out); if (SUCCEEDED(hr) && out && *out) patch_adapter_vtables(*out); return hr; }
static HRESULT WINAPI hook_CDF1(REFIID riid, void **out) { HRESULT hr = real_CDF1(riid, out); if (SUCCEEDED(hr) && out && *out) patch_adapter_vtables(*out); return hr; }
static HRESULT WINAPI hook_CDF2(UINT flags, REFIID riid, void **out) { HRESULT hr = real_CDF2(flags, riid, out); if (SUCCEEDED(hr) && out && *out) patch_adapter_vtables(*out); return hr; }
static void hook_exe_iat(const char *dll, const char *name, void *hook, void **saved)
{
    UINT8 *base = (UINT8 *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base; IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        if (_stricmp((const char *)(base + imp->Name), dll)) continue;
        IMAGE_THUNK_DATA *orig = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk), *cur = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; orig->u1.AddressOfData; orig++, cur++) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData);
            if (strcmp((const char *)n->Name, name)) continue;
            DWORD old; if (VirtualProtect(&cur->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) { *saved = (void *)cur->u1.Function; cur->u1.Function = (ULONGLONG)hook; VirtualProtect(&cur->u1.Function, sizeof(void *), old, &old); NOTE("IAT hook %s!%s installed", dll, name); }
            return;
        }
    }
    NOTE("IAT hook %s!%s: import not found", dll, name);
}
static void install_vram_cap(void)
{
    char v[32];
    if (!GetEnvironmentVariableA("LAYOVER_VRAM_MB", v, sizeof v) || atoi(v) <= 0) return;
    vram_cap = (UINT64)atoi(v) << 20;
    hook_exe_iat("dxgi.dll", "CreateDXGIFactory", (void *)hook_CDF, (void **)&real_CDF);
    hook_exe_iat("dxgi.dll", "CreateDXGIFactory1", (void *)hook_CDF1, (void **)&real_CDF1);
    hook_exe_iat("dxgi.dll", "CreateDXGIFactory2", (void *)hook_CDF2, (void **)&real_CDF2);
    // The game resolves CreateDXGIFactory dynamically, so also patch the shared adapter vtable right
    // now through a factory of our own (D3DMetal hands out one adapter object/vtable to everyone).
    {
        HMODULE dxgi = LoadLibraryA("dxgi.dll"); PFN_CDF cdf1 = dxgi ? (PFN_CDF)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL; IDXGIFactory *f = NULL;
        if (cdf1 && SUCCEEDED(cdf1(&IID_IDXGIFactory1, (void **)&f)) && f) { patch_adapter_vtables(f); f->lpVtbl->Release(f); }
        else NOTE("VRAM cap: could not create a DXGI factory (dxgi %p)", (void *)dxgi);
    }
}

// ---- logpoints (LAYOVER_LOGPOINTS=rva,rva,...): INT3 at exe+rva, logged by a vectored handler -----
// Each hit logs the registers and the return address, then the original byte is restored, the thread
// single-steps over it and the INT3 goes back. Cheap way to see whether a code path (an OOM branch)
// runs, without a debugger.
#define MAX_LP 16
static struct { UINT8 *addr; UINT8 orig; volatile LONG hits; } lps[MAX_LP]; static int n_lp; static long lp_max = 50;
static DWORD lp_tls;
static CRITICAL_SECTION lp_lock; static int lp_lock_ready;
// Serialize every byte flip: without this, one thread can restore the page to read-only (its second
// VirtualProtect) while another thread's *a=b is still writing the code byte, which faults on the
// now read-only page and crashes the game. The single-byte write itself is atomic, so a thread that
// executes the address concurrently always sees a valid 0xCC or the original byte, never garbage.
static void lp_write(UINT8 *a, UINT8 b) { DWORD old; if (lp_lock_ready) EnterCriticalSection(&lp_lock); VirtualProtect(a, 1, PAGE_EXECUTE_READWRITE, &old); *a = b; VirtualProtect(a, 1, old, &old); FlushInstructionCache(GetCurrentProcess(), a, 1); if (lp_lock_ready) LeaveCriticalSection(&lp_lock); }
static LONG CALLBACK lp_handler(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord; DWORD code = ep->ExceptionRecord->ExceptionCode; int i;
    if (code == EXCEPTION_BREAKPOINT) {
        UINT8 *pc = (UINT8 *)c->Rip;
        for (i = 0; i < n_lp; i++) if (lps[i].addr == pc) {
            LONG h = InterlockedIncrement(&lps[i].hits); UINT64 *sp = (UINT64 *)c->Rsp; UINT8 *exe = (UINT8 *)GetModuleHandleA(NULL);
            if (h <= lp_max || (h % 1000) == 0) {
                char str[96] = "", str6[96] = "", ext[200] = ""; const char *sa = (const char *)sp[5], *sb = (const char *)sp[6];   // 5th/6th stack-passed args as strings, if they are
                if (sa && readable(sa, 96)) { int k; for (k = 0; k < 95 && sa[k] >= 0x20 && sa[k] < 0x7f; k++) str[k] = sa[k]; str[k] = 0; }
                if (sb && readable(sb, 96)) { int k; for (k = 0; k < 95 && sb[k] >= 0x20 && sb[k] < 0x7f; k++) str6[k] = sb[k]; str6[k] = 0; }
                { const UINT8 *ra = (const UINT8 *)c->Rax;   // fields of whatever rax points at (allocation record / subheap), if readable
                  if (ra && readable(ra, 0x20)) snprintf(ext, sizeof ext, " rax[10]=%x rax[14]=%x rax[1c]=%x", *(UINT32 *)(ra + 0x10), *(UINT32 *)(ra + 0x14), *(UINT32 *)(ra + 0x1c));
                  if (ra && readable(ra, 0x4f8)) snprintf(ext + strlen(ext), sizeof ext - strlen(ext), " rax[4d0]=%llx rax[4f0]=%x rax[4f4]=%x", (unsigned long long)*(UINT64 *)(ra + 0x4d0), *(UINT32 *)(ra + 0x4f0), *(UINT32 *)(ra + 0x4f4)); }
                NOTE("logpoint exe+0x%llx hit %ld: rcx=%llx rdx=%llx r8=%llx r9=%llx rax=%llx ret=exe+0x%llx s5='%s' s6='%s' s7=%llx s8=%llx%s", (unsigned long long)(pc - exe), h, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx, (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->Rax, (unsigned long long)(sp[0] - (UINT64)exe), str, str6, (unsigned long long)sp[7], (unsigned long long)sp[8], ext);
            }
            lp_write(pc, lps[i].orig); c->EFlags |= 0x100; TlsSetValue(lp_tls, (void *)(UINT_PTR)(i + 1));
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    } else if (code == EXCEPTION_SINGLE_STEP) {
        UINT_PTR k = (UINT_PTR)TlsGetValue(lp_tls);
        if (k) { lp_write(lps[k - 1].addr, 0xCC); TlsSetValue(lp_tls, NULL); c->EFlags &= ~0x100; return EXCEPTION_CONTINUE_EXECUTION; }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void install_logpoints(void)
{
    char v[512], *tok; UINT8 *exe = (UINT8 *)GetModuleHandleA(NULL);
    if (!GetEnvironmentVariableA("LAYOVER_LOGPOINTS", v, sizeof v)) return;
    InitializeCriticalSection(&lp_lock); lp_lock_ready = 1;
    lp_tls = TlsAlloc(); AddVectoredExceptionHandler(1, lp_handler);
    { char m[16]; if (GetEnvironmentVariableA("LAYOVER_LOGPOINTS_MAX", m, sizeof m)) lp_max = atol(m); }
    for (tok = strtok(v, ","); tok && n_lp < MAX_LP; tok = strtok(NULL, ",")) {
        UINT64 rva = strtoull(tok, NULL, 16); UINT8 *a = exe + rva;
        lps[n_lp].addr = a; lps[n_lp].orig = *a; lp_write(a, 0xCC); NOTE("logpoint %d at exe+0x%llx (orig byte %02x)", n_lp, (unsigned long long)rva, lps[n_lp].orig); n_lp++;
    }
}

// ---- device hooks ------------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE hook_CreateQueryHeap(ID3D12Device *dev, const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **out)
{
    FakeHeap *h; HRESULT hr;
    try_patch_budget("CreateQueryHeap"); try_patch_subheaps("CreateQueryHeap");
    if (desc) LOG("CreateQueryHeap type=%d count=%u", (int)desc->Type, desc->Count);
    if (!tsshim || !desc || (desc->Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP && desc->Type != D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP))
        return real_CreateQueryHeap(dev, desc, riid, out);
    if (!out) return S_FALSE;
    h = calloc(1, sizeof *h); if (!h) return E_OUTOFMEMORY;
    h->lpVtbl = &fake_heap_vtbl; h->ref = 1; h->count = desc->Count;
    h->values = calloc(desc->Count ? desc->Count : 1, sizeof(UINT64));
    h->device = dev; dev->lpVtbl->AddRef(dev);
    hr = fh_QueryInterface((ID3D12QueryHeap *)h, riid, out);
    fh_Release((ID3D12QueryHeap *)h);
    LOG("tsshim: timestamp query heap of %u entries served by the shim (hr 0x%08lx)", desc->Count, (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CheckFeatureSupport(ID3D12Device *dev, D3D12_FEATURE feature, void *data, UINT size)
{
    char in[200] = "", outb[200] = "";
    if (data) hexdump(in, sizeof in, data, size);
    HRESULT hr = real_CheckFeatureSupport(dev, feature, data, size);
    if (data) hexdump(outb, sizeof outb, data, size);
    LOG("CheckFeatureSupport feature=%d size=%u hr=0x%08lx in=%s out=%s", (int)feature, size, (unsigned long)hr, in, outb);
    return hr;
}

static const char *heap_name(D3D12_HEAP_TYPE t) { return t == 1 ? "default" : t == 2 ? "upload" : t == 3 ? "readback" : t == 4 ? "custom" : "?"; }
static void log_res(const char *what, const D3D12_RESOURCE_DESC *d, const D3D12_HEAP_PROPERTIES *hp, HRESULT hr, void *res)
{
    if (!d) return;
    int big = d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? d->Width >= (32ull << 20) : (d->Width * d->Height >= 4096 * 4096);
    if (d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) InterlockedIncrement(&c_res_buf); else InterlockedIncrement(&c_res_tex);
    if (!verbose && !big && SUCCEEDED(hr)) return;
    LOG("%s dim=%d fmt=%u %llux%ux%u mips=%u samples=%u flags=0x%x layout=%d heap=%s hr=0x%lx res=%p", what, (int)d->Dimension, (unsigned)d->Format,
        (unsigned long long)d->Width, (unsigned)d->Height, (unsigned)d->DepthOrArraySize, (unsigned)d->MipLevels, (unsigned)d->SampleDesc.Count, (unsigned)d->Flags, (int)d->Layout,
        hp ? heap_name(hp->Type) : "placed", (unsigned long)hr, res);
}
static HRESULT STDMETHODCALLTYPE hook_CreateCommittedResource(ID3D12Device *dev, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE *cv, REFIID riid, void **out)
{
    HRESULT hr = real_CreateCommittedResource(dev, hp, hf, d, st, cv, riid, out);
    log_res("CreateCommittedResource", d, hp, hr, out ? *out : NULL);
    return hr;
}
// The game's big pool: remember it so CopyBufferRegion can report the highest offset written into it
// (pool usage high-water mark) and any copy that lands past its end.
static HRESULT STDMETHODCALLTYPE hook_CreatePlacedResource(ID3D12Device *dev, ID3D12Heap *heap, UINT64 off, const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE *cv, REFIID riid, void **out)
{
    HRESULT hr = real_CreatePlacedResource(dev, heap, off, d, st, cv, riid, out);
    if (SUCCEEDED(hr) && d && d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d->Width >= (512ull << 20) && out && *out && !pool_res) {
        pool_res = (ID3D12Resource *)*out; pool_width = d->Width;
        NOTE("pool resource %p is %llu MB; tracking copies into it", (void *)pool_res, (unsigned long long)(d->Width >> 20));
    }
    log_res("CreatePlacedResource", d, NULL, hr, out ? *out : NULL);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hook_CreateReservedResource(ID3D12Device *dev, const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE *cv, REFIID riid, void **out)
{
    HRESULT hr = real_CreateReservedResource(dev, d, st, cv, riid, out);
    log_res("CreateReservedResource", d, NULL, hr, out ? *out : NULL);
    return hr;
}
static void log_backtrace(const char *why)
{
    void *frames[24]; USHORT n = RtlCaptureStackBackTrace(1, 24, frames, NULL), i;
    char line[1200]; size_t o = 0;
    HMODULE exe = GetModuleHandleA(NULL);
    for (i = 0; i < n && o < sizeof line - 60; i++) {
        HMODULE m = NULL; char name[64] = "?"; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m) { char path[MAX_PATH]; GetModuleFileNameA(m, path, sizeof path); const char *b = strrchr(path, '\\'); snprintf(name, sizeof name, "%s", b ? b + 1 : path); }
        o += snprintf(line + o, sizeof line - o, " %s+0x%llx", name, (unsigned long long)((UINT8 *)frames[i] - (UINT8 *)m));
    }
    LOG("backtrace (%s; exe base %p):%s", why, (void *)exe, line);
}
static HRESULT STDMETHODCALLTYPE hook_CreateHeap(ID3D12Device *dev, const D3D12_HEAP_DESC *d, REFIID riid, void **out)
{
    HRESULT hr = real_CreateHeap(dev, d, riid, out);
    if (d && d->SizeInBytes >= (1ull << 30)) log_backtrace("CreateHeap >= 1 GB");
    if (d && (verbose || d->SizeInBytes >= (64ull << 20) || FAILED(hr)))
        LOG("CreateHeap size=%llu MB type=%s flags=0x%x align=%llu hr=0x%lx heap=%p", (unsigned long long)(d->SizeInBytes >> 20), heap_name(d->Properties.Type), (unsigned)d->Flags, (unsigned long long)d->Alignment, (unsigned long)hr, out ? *out : NULL);
    return hr;
}
static void STDMETHODCALLTYPE hook_CreateShaderResourceView(ID3D12Device *dev, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    if (d && d->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
        InterlockedIncrement(&c_srv_buf);
        int interesting = d->Format == DXGI_FORMAT_R32G32B32_FLOAT || d->Format == DXGI_FORMAT_R32G32B32_UINT || d->Format == DXGI_FORMAT_R32G32B32_SINT || d->Buffer.NumElements >= (1u << 24);
        if (verbose || interesting)
            LOG("SRV buffer res=%p (width %llu) fmt=%u first=%llu num=%u stride=%u flags=%u", res, (unsigned long long)res_width(res), (unsigned)d->Format,
                (unsigned long long)d->Buffer.FirstElement, d->Buffer.NumElements, d->Buffer.StructureByteStride, (unsigned)d->Buffer.Flags);
    } else if (d) InterlockedIncrement(&c_srv_tex);
    real_CreateShaderResourceView(dev, res, d, h);
}
static void STDMETHODCALLTYPE hook_CreateUnorderedAccessView(ID3D12Device *dev, ID3D12Resource *res, ID3D12Resource *counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    if (d && d->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
        InterlockedIncrement(&c_uav_buf);
        int interesting = d->Format == DXGI_FORMAT_R32G32B32_FLOAT || d->Buffer.NumElements >= (1u << 24) || counter;
        if (verbose || interesting)
            LOG("UAV buffer res=%p (width %llu) fmt=%u first=%llu num=%u stride=%u flags=%u counter=%p", res, (unsigned long long)res_width(res), (unsigned)d->Format,
                (unsigned long long)d->Buffer.FirstElement, d->Buffer.NumElements, d->Buffer.StructureByteStride, (unsigned)d->Buffer.Flags, counter);
    } else if (d) InterlockedIncrement(&c_uav_tex);
    real_CreateUnorderedAccessView(dev, res, counter, d, h);
}
static HRESULT STDMETHODCALLTYPE hook_CreateGraphicsPipelineState(ID3D12Device *dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d, REFIID riid, void **out)
{
    HRESULT hr = real_CreateGraphicsPipelineState(dev, d, riid, out);
    InterlockedIncrement(&c_pso_gfx);
    if (FAILED(hr)) { InterlockedIncrement(&c_pso_fail); LOG("CreateGraphicsPipelineState FAILED hr=0x%lx vs=%llu ps=%llu gs=%llu hs=%llu ds=%llu rtv=%u fmt0=%u dsv=%u so=%u", (unsigned long)hr, d ? (unsigned long long)d->VS.BytecodeLength : 0, d ? (unsigned long long)d->PS.BytecodeLength : 0, d ? (unsigned long long)d->GS.BytecodeLength : 0, d ? (unsigned long long)d->HS.BytecodeLength : 0, d ? (unsigned long long)d->DS.BytecodeLength : 0, d ? d->NumRenderTargets : 0, d ? (unsigned)d->RTVFormats[0] : 0, d ? (unsigned)d->DSVFormat : 0, d ? d->StreamOutput.NumEntries : 0); }
    else if (verbose && d) LOG("CreateGraphicsPipelineState ok vs=%016llx ps=%016llx", (unsigned long long)fnv(d->VS.pShaderBytecode, d->VS.BytecodeLength), (unsigned long long)fnv(d->PS.pShaderBytecode, d->PS.BytecodeLength));
    return hr;
}
static HRESULT STDMETHODCALLTYPE hook_CreateComputePipelineState(ID3D12Device *dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC *d, REFIID riid, void **out)
{
    HRESULT hr = real_CreateComputePipelineState(dev, d, riid, out);
    InterlockedIncrement(&c_pso_cs);
    if (FAILED(hr)) { InterlockedIncrement(&c_pso_fail); LOG("CreateComputePipelineState FAILED hr=0x%lx cs=%llu bytes hash=%016llx", (unsigned long)hr, d ? (unsigned long long)d->CS.BytecodeLength : 0, d ? (unsigned long long)fnv(d->CS.pShaderBytecode, d->CS.BytecodeLength) : 0); }
    else if (verbose && d) LOG("CreateComputePipelineState ok cs=%llu bytes hash=%016llx pso=%p", (unsigned long long)d->CS.BytecodeLength, (unsigned long long)fnv(d->CS.pShaderBytecode, d->CS.BytecodeLength), out ? *out : NULL);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hook_CreatePipelineState(ID3D12Device2 *dev, const D3D12_PIPELINE_STATE_STREAM_DESC *d, REFIID riid, void **out)
{
    HRESULT hr = real_CreatePipelineState(dev, d, riid, out);
    InterlockedIncrement(&c_pso_gen);
    // walk the stream for subobject types: 13=MS, 14=AS
    char types[256] = ""; size_t o = 0;
    if (d && d->pPipelineStateSubobjectStream) {
        const UINT8 *p = d->pPipelineStateSubobjectStream, *end = p + d->SizeInBytes;
        static const UINT sz[] = { 8, 16, 16, 16, 16, 16, 16, 16, 16, 24, 4, 32, 8, 4, 4, 16, 36, 4, 8, 8, 8, 8, 16, 16, 8, 8, 8, 8 };
        while (p + 4 <= end) {
            UINT t = *(const UINT *)p; if (t >= sizeof sz / sizeof *sz) break;
            if (o < sizeof types - 8) o += snprintf(types + o, sizeof types - o, "%u,", t);
            UINT step = 8 + sz[t]; if (t == 9 /*stream output*/) step = 8 + 40; if (t == 11 /*input layout*/) step = 8 + 16; if (t == 16 /*view instancing*/) step = 8 + 24; if (t == 1 /*VS*/ || t == 2 || t == 3 || t == 4 || t == 5 || t == 6 || t == 13 || t == 14) step = 8 + 16;
            step = (step + 7) & ~7u; p += step;
        }
    }
    if (FAILED(hr)) { InterlockedIncrement(&c_pso_fail); LOG("CreatePipelineState(stream) FAILED hr=0x%lx size=%llu types=%s", (unsigned long)hr, d ? (unsigned long long)d->SizeInBytes : 0, types); }
    else if (verbose || strstr(types, "13,") || strstr(types, "14,")) LOG("CreatePipelineState(stream) ok size=%llu types=%s pso=%p", d ? (unsigned long long)d->SizeInBytes : 0, types, out ? *out : NULL);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hook_CreateCommandSignature(ID3D12Device *dev, const D3D12_COMMAND_SIGNATURE_DESC *d, ID3D12RootSignature *rs, REFIID riid, void **out)
{
    HRESULT hr = real_CreateCommandSignature(dev, d, rs, riid, out);
    try_patch_budget("CreateCommandSignature"); try_patch_subheaps("CreateCommandSignature");
    char args[400] = ""; size_t o = 0; UINT i;
    if (d) for (i = 0; i < d->NumArgumentDescs && o < sizeof args - 40; i++) {
        const D3D12_INDIRECT_ARGUMENT_DESC *a = &d->pArgumentDescs[i];
        static const char *names[] = { "DRAW", "DRAW_INDEXED", "DISPATCH", "VBV", "IBV", "CONST", "CBV", "SRV", "UAV", "DISPATCH_RAYS", "DISPATCH_MESH", "INCREMENTING_CONSTANT" };
        o += snprintf(args + o, sizeof args - o, "%s", a->Type < 12 ? names[a->Type] : "?");
        if (a->Type == D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW) o += snprintf(args + o, sizeof args - o, "(slot %u)", a->VertexBuffer.Slot);
        if (a->Type == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT) o += snprintf(args + o, sizeof args - o, "(root %u, off %u, n %u)", a->Constant.RootParameterIndex, a->Constant.DestOffsetIn32BitValues, a->Constant.Num32BitValuesToSet);
        if (a->Type == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW) o += snprintf(args + o, sizeof args - o, "(root %u)", a->ConstantBufferView.RootParameterIndex);
        if (a->Type == D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW) o += snprintf(args + o, sizeof args - o, "(root %u)", a->ShaderResourceView.RootParameterIndex);
        if (a->Type == D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW) o += snprintf(args + o, sizeof args - o, "(root %u)", a->UnorderedAccessView.RootParameterIndex);
        o += snprintf(args + o, sizeof args - o, " ");
    }
    LOG("CreateCommandSignature stride=%u n=%u rootsig=%p args=[%s] hr=0x%lx sig=%p", d ? d->ByteStride : 0, d ? d->NumArgumentDescs : 0, rs, args, (unsigned long)hr, out ? *out : NULL);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hook_CreateRootSignature(ID3D12Device *dev, UINT node, const void *blob, SIZE_T len, REFIID riid, void **out)
{
    HRESULT hr = real_CreateRootSignature(dev, node, blob, len, riid, out);
    if (FAILED(hr) || verbose) LOG("CreateRootSignature len=%llu hr=0x%lx", (unsigned long long)len, (unsigned long)hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hook_GetDeviceRemovedReason(ID3D12Device *dev)
{
    HRESULT hr = real_GetDeviceRemovedReason(dev);
    if (FAILED(hr)) LOG("GetDeviceRemovedReason -> 0x%lx", (unsigned long)hr);
    return hr;
}

// ---- command list hooks -----------------------------------------------------------------------
static void STDMETHODCALLTYPE hook_EndQuery(ID3D12GraphicsCommandList *list, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
{
    FakeHeap *h;
    if (!is_fake(heap)) { real_EndQuery(list, heap, type, index); return; }
    h = (FakeHeap *)heap; if (index < h->count) h->values[index] = now_ticks();
}
static void STDMETHODCALLTYPE hook_BeginQuery(ID3D12GraphicsCommandList *list, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
{
    if (!is_fake(heap)) real_BeginQuery(list, heap, type, index);
}
static void STDMETHODCALLTYPE hook_ResolveQueryData(ID3D12GraphicsCommandList *list, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource *dst, UINT64 offset)
{
    FakeHeap *h; Ring *r; UINT64 bytes, off; LONG64 pos; int tries;
    if (!is_fake(heap)) { real_ResolveQueryData(list, heap, type, start, count, dst, offset); return; }
    h = (FakeHeap *)heap; r = ring_for(h->device);
    if (!r || !dst || !count || start >= h->count) return;
    if (start + count > h->count) count = h->count - start;
    bytes = (UINT64)count * 8; if (bytes > RING_BYTES / 2) return;
    for (tries = 0; tries < 2; tries++) {
        pos = InterlockedExchangeAdd64(&r->cursor, (LONG64)bytes); off = (UINT64)pos % RING_BYTES;
        if (off + bytes <= RING_BYTES) break;
        InterlockedExchangeAdd64(&r->cursor, (LONG64)(RING_BYTES - off));
    }
    if (tries == 2) return;
    memcpy(r->cpu + off, h->values + start, (size_t)bytes);
    list->lpVtbl->CopyBufferRegion(list, dst, offset, r->buffer, off, bytes);
}
static void STDMETHODCALLTYPE hook_Dispatch(ID3D12GraphicsCommandList *l, UINT x, UINT y, UINT z) { InterlockedIncrement(&c_dispatch); real_Dispatch(l, x, y, z); }
static void STDMETHODCALLTYPE hook_DrawInstanced(ID3D12GraphicsCommandList *l, UINT a, UINT b, UINT c, UINT d) { InterlockedIncrement(&c_draw); real_DrawInstanced(l, a, b, c, d); }
static void STDMETHODCALLTYPE hook_DrawIndexedInstanced(ID3D12GraphicsCommandList *l, UINT a, UINT b, UINT c, INT d, UINT e) { InterlockedIncrement(&c_drawidx); real_DrawIndexedInstanced(l, a, b, c, d, e); }
static void STDMETHODCALLTYPE hook_CopyBufferRegion(ID3D12GraphicsCommandList *l, ID3D12Resource *d, UINT64 doff, ID3D12Resource *s, UINT64 soff, UINT64 n)
{
    InterlockedIncrement(&c_copybuf);
    if (pool_res && d == pool_res) {
        LONG64 end = (LONG64)(doff + n), prev;
        InterlockedIncrement(&pool_copies);
        sm2_skin_note_copy(doff, n);
        do { prev = pool_hw; if (end <= prev) break; } while (InterlockedCompareExchange64(&pool_hw, end, prev) != prev);
        if (end > prev && (end >> 26) != (prev >> 26)) NOTE("pool high-water mark %llu MB (copies into pool: %ld)", (unsigned long long)(end >> 20), pool_copies);
        if ((UINT64)end > pool_width) { LONG k = InterlockedIncrement(&pool_oob); if (k <= 10) NOTE("COPY PAST POOL END: dst offset %llu + %llu bytes > %llu MB pool (src %p+%llu)", (unsigned long long)doff, (unsigned long long)n, (unsigned long long)(pool_width >> 20), (void *)s, (unsigned long long)soff); }
    }
    if (pool_res && s == pool_res && soff + n > pool_width) { LONG k = InterlockedIncrement(&pool_oob); if (k <= 10) NOTE("COPY FROM PAST POOL END: src offset %llu + %llu", (unsigned long long)soff, (unsigned long long)n); }
    real_CopyBufferRegion(l, d, doff, s, soff, n);
}
static void STDMETHODCALLTYPE hook_SetPipelineState(ID3D12GraphicsCommandList *l, ID3D12PipelineState *p) { InterlockedIncrement(&c_setpso); real_SetPipelineState(l, p); }
static void STDMETHODCALLTYPE hook_ResourceBarrier(ID3D12GraphicsCommandList *l, UINT n, const D3D12_RESOURCE_BARRIER *b) { InterlockedIncrement(&c_barrier); real_ResourceBarrier(l, n, b); }
static void STDMETHODCALLTYPE hook_DispatchMesh(ID3D12GraphicsCommandList6 *l, UINT x, UINT y, UINT z) { InterlockedIncrement(&c_dispatchmesh); real_DispatchMesh(l, x, y, z); }
static struct { ID3D12CommandSignature *sig; LONG n; } sigs[64];
static void STDMETHODCALLTYPE hook_ExecuteIndirect(ID3D12GraphicsCommandList *l, ID3D12CommandSignature *sig, UINT max, ID3D12Resource *args, UINT64 aoff, ID3D12Resource *count, UINT64 coff)
{
    int i; LONG n = InterlockedIncrement(&c_execind);
    for (i = 0; i < 64; i++) {
        if (sigs[i].sig == sig) { if (InterlockedIncrement(&sigs[i].n) == 1) {} break; }
        if (!sigs[i].sig && InterlockedCompareExchangePointer((void *volatile *)&sigs[i].sig, sig, NULL) == NULL) { sigs[i].n = 1; LOG("ExecuteIndirect first use of sig=%p max=%u args=%p+%llu count=%p+%llu", sig, max, args, (unsigned long long)aoff, count, (unsigned long long)coff); break; }
    }
    if (n <= 20) LOG("ExecuteIndirect sig=%p max=%u args=%p+%llu count=%p+%llu", sig, max, args, (unsigned long long)aoff, count, (unsigned long long)coff);
    real_ExecuteIndirect(l, sig, max, args, aoff, count, coff);
}
static void STDMETHODCALLTYPE hook_ExecuteCommandLists(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *lists) { InterlockedIncrement(&c_execlists); real_ExecuteCommandLists(q, n, lists); }
static HRESULT STDMETHODCALLTYPE hook_GetTimestampFrequency(ID3D12CommandQueue *q, UINT64 *f)
{
    if (!tsshim) { HRESULT hr = real_GetTimestampFrequency(q, f); LOG("GetTimestampFrequency -> %llu hr=0x%lx", f ? (unsigned long long)*f : 0, (unsigned long)hr); return hr; }
    if (!f) return E_INVALIDARG; *f = qpc_freq; return S_OK;
}
static HRESULT STDMETHODCALLTYPE hook_GetClockCalibration(ID3D12CommandQueue *q, UINT64 *gpu, UINT64 *cpu)
{
    if (!tsshim) return real_GetClockCalibration(q, gpu, cpu);
    UINT64 t = now_ticks(); if (gpu) *gpu = t; if (cpu) *cpu = t; return S_OK;
}

// ---- patching ---------------------------------------------------------------------------------
static void patch_slot(void **slot, void *hook, void **saved)
{
    DWORD old;
    if (*slot == hook) return;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) old = 0;
    if (!*saved) *saved = *slot;
    *slot = hook;
    if (old) VirtualProtect(slot, sizeof(void *), old, &old);
}
#define PATCH(vt, name) patch_slot((void **)&(vt)->name, (void *)hook_##name, (void **)&real_##name)

static void patch_list_vtbl(void *iface, int gen)
{
    ID3D12GraphicsCommandListVtbl *vt = *(ID3D12GraphicsCommandListVtbl **)iface;
    PATCH(vt, EndQuery); PATCH(vt, BeginQuery); PATCH(vt, ResolveQueryData);
    PATCH(vt, Dispatch); PATCH(vt, DrawInstanced); PATCH(vt, DrawIndexedInstanced); PATCH(vt, ExecuteIndirect);
    PATCH(vt, CopyBufferRegion); PATCH(vt, SetPipelineState); PATCH(vt, ResourceBarrier);
    if (gen >= 6) { ID3D12GraphicsCommandList6Vtbl *v6 = (ID3D12GraphicsCommandList6Vtbl *)vt; PATCH(v6, DispatchMesh); }
}
static void patch_queue_vtbl(void *iface)
{
    ID3D12CommandQueueVtbl *vt = *(ID3D12CommandQueueVtbl **)iface;
    PATCH(vt, GetTimestampFrequency); PATCH(vt, GetClockCalibration); PATCH(vt, ExecuteCommandLists);
}
static void patch_lists_of(ID3D12Device *dev);
static HRESULT STDMETHODCALLTYPE hook_CreateCommandQueue(ID3D12Device *dev, const D3D12_COMMAND_QUEUE_DESC *d, REFIID riid, void **out)
{
    HRESULT hr = real_CreateCommandQueue(dev, d, riid, out);
    try_patch_budget("CreateCommandQueue");
    LOG("CreateCommandQueue type=%d prio=%d flags=%u hr=0x%lx q=%p", d ? (int)d->Type : -1, d ? d->Priority : 0, d ? (unsigned)d->Flags : 0, (unsigned long)hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out && *out) patch_queue_vtbl(*out);
    return hr;
}
static volatile LONG lists_seen[8];
static HRESULT STDMETHODCALLTYPE hook_CreateCommandList(ID3D12Device *dev, UINT node, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso, REFIID riid, void **out)
{
    HRESULT hr = real_CreateCommandList(dev, node, type, alloc, pso, riid, out);
    if ((unsigned)type < 8 && InterlockedIncrement(&lists_seen[type]) <= 3) LOG("CreateCommandList type=%d hr=0x%lx", (int)type, (unsigned long)hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hook_CreateCommandList1(ID3D12Device4 *dev, UINT node, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **out)
{
    HRESULT hr = real_CreateCommandList1(dev, node, type, flags, riid, out);
    if ((unsigned)type < 8 && InterlockedIncrement(&lists_seen[type]) <= 3) LOG("CreateCommandList1 type=%d hr=0x%lx", (int)type, (unsigned long)hr);
    return hr;
}

static void patch_device_vtbl(void *iface, int gen)
{
    ID3D12DeviceVtbl *vt = *(ID3D12DeviceVtbl **)iface;
    PATCH(vt, CreateQueryHeap); PATCH(vt, CheckFeatureSupport);
    PATCH(vt, CreateCommittedResource); PATCH(vt, CreatePlacedResource); PATCH(vt, CreateReservedResource); PATCH(vt, CreateHeap);
    PATCH(vt, CreateShaderResourceView); PATCH(vt, CreateUnorderedAccessView);
    PATCH(vt, CreateGraphicsPipelineState); PATCH(vt, CreateComputePipelineState); PATCH(vt, CreateCommandSignature); PATCH(vt, CreateRootSignature);
    PATCH(vt, CreateCommandQueue); PATCH(vt, CreateCommandList); PATCH(vt, GetDeviceRemovedReason);
    if (gen >= 2) { ID3D12Device2Vtbl *v2 = (ID3D12Device2Vtbl *)vt; PATCH(v2, CreatePipelineState); }
    if (gen >= 4) { ID3D12Device4Vtbl *v4 = (ID3D12Device4Vtbl *)vt; PATCH(v4, CreateCommandList1); }
}

static void patch_all(IUnknown *obj, const IID *const *ids, int n, void (*patch)(void *, int))
{
    int i; void *p;
    patch(obj, 0);
    for (i = 0; i < n; i++) {
        if (FAILED(obj->lpVtbl->QueryInterface(obj, ids[i], &p)) || !p) { LOG("  interface gen %d not supported", i + 1); continue; }
        patch(p, i + 1); ((IUnknown *)p)->lpVtbl->Release((IUnknown *)p);
    }
}
static void patch_lists_of(ID3D12Device *dev)
{
    static const IID *const list_ids[] = { &IID_ID3D12GraphicsCommandList1, &IID_ID3D12GraphicsCommandList2, &IID_ID3D12GraphicsCommandList3, &IID_ID3D12GraphicsCommandList4, &IID_ID3D12GraphicsCommandList5, &IID_ID3D12GraphicsCommandList6, &IID_ID3D12GraphicsCommandList7 };
    static const D3D12_COMMAND_LIST_TYPE types[] = { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY };
    int t;
    for (t = 0; t < 3; t++) {
        ID3D12CommandAllocator *alloc = NULL; ID3D12GraphicsCommandList *list = NULL; ID3D12CommandQueue *queue = NULL;
        D3D12_COMMAND_QUEUE_DESC qd = { types[t], 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
        if (SUCCEEDED(real_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue)) && queue) { patch_queue_vtbl(queue); queue->lpVtbl->Release(queue); }
        if (FAILED(dev->lpVtbl->CreateCommandAllocator(dev, types[t], &IID_ID3D12CommandAllocator, (void **)&alloc)) || !alloc) continue;
        if (SUCCEEDED(real_CreateCommandList(dev, 0, types[t], alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list)) && list) {
            LOG("probe list type %d: vtable %p", (int)types[t], *(void **)list);
            patch_all((IUnknown *)list, list_ids, sizeof list_ids / sizeof *list_ids, patch_list_vtbl);
            list->lpVtbl->Close(list); list->lpVtbl->Release(list);
        }
        alloc->lpVtbl->Release(alloc);
    }
}
static void patch_device(ID3D12Device *dev)
{
    static const IID *const dev_ids[] = { &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4, &IID_ID3D12Device5, &IID_ID3D12Device6, &IID_ID3D12Device7, &IID_ID3D12Device8, &IID_ID3D12Device9, &IID_ID3D12Device10 };
    LOG("device %p vtable %p", (void *)dev, *(void **)dev);
    patch_all((IUnknown *)dev, dev_ids, sizeof dev_ids / sizeof *dev_ids, patch_device_vtbl);
    patch_lists_of(dev);
    LOG("device patched (tsshim=%d, qpc %llu Hz)", tsshim, (unsigned long long)qpc_freq);
}

HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **out)
{
    typedef HRESULT (WINAPI *PFN)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    PFN fn = real_dll ? (PFN)GetProcAddress(real_dll, "D3D12CreateDevice") : NULL;
    HRESULT hr;
    if (!fn) return E_FAIL;
    hr = fn(adapter, level, riid, out);
    try_patch_budget("D3D12CreateDevice"); try_patch_subheaps("D3D12CreateDevice");
    LOG("D3D12CreateDevice adapter=%p level=0x%x out=%p hr=0x%lx -> %p", adapter, (unsigned)level, out, (unsigned long)hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12Device *dev = NULL; IUnknown *u = (IUnknown *)*out;
        if (SUCCEEDED(u->lpVtbl->QueryInterface(u, &IID_ID3D12Device, (void **)&dev)) && dev) { patch_device(dev); dev->lpVtbl->Release(dev); }
    }
    return hr;
}

// Settings can also come from a d3d12shim.env file beside this DLL (KEY=VALUE lines): a game started
// by Steam inherits Steam's environment, so per-run knobs cannot always be passed through env.
static void load_env_file(void)
{
    char path[MAX_PATH], line[1024]; FILE *f; char *p;
    if (!GetModuleFileNameA(self, path, sizeof path)) return;
    p = strrchr(path, '\\'); if (!p) return; strcpy(p + 1, "d3d12shim.env");
    f = fopen(path, "r"); if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        if (!eq || line[0] == '#') continue; *eq = 0;
        if (GetEnvironmentVariableA(line, NULL, 0) == 0) SetEnvironmentVariableA(line, eq + 1);
    }
    fclose(f);
}
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        LARGE_INTEGER f; char v[8];
        self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&log_lock);
        InitializeCriticalSection(&ring_lock);
        QueryPerformanceFrequency(&f); qpc_freq = (UINT64)f.QuadPart;
        load_env_file();
        tsshim = GetEnvironmentVariableA("LAYOVER_TSSHIM", v, sizeof v) == 1 && v[0] == '1';
        logging = GetEnvironmentVariableA("LAYOVER_D3D12_LOG", NULL, 0) > 0;
        verbose = GetEnvironmentVariableA("LAYOVER_D3D12_VERBOSE", v, sizeof v) == 1 && v[0] == '1';
        if (!load_real()) return FALSE;
        RESOLVE(D3D12CoreCreateLayeredDevice); RESOLVE(D3D12CoreGetLayeredDeviceSize); RESOLVE(D3D12CoreRegisterLayers);
        RESOLVE(D3D12CreateRootSignatureDeserializer); RESOLVE(D3D12CreateVersionedRootSignatureDeserializer);
        RESOLVE(D3D12EnableExperimentalFeatures); RESOLVE(D3D12GetDebugInterface); RESOLVE(D3D12SerializeRootSignature);
        RESOLVE(D3D12SerializeVersionedRootSignature); RESOLVE(GetBehaviorValue);
        install_vram_cap();
        install_logpoints();
        sm2_skin_install();
        { char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe); LOG("d3dlog loaded in %s (pid %lu) tsshim=%d verbose=%d", exe, (unsigned long)GetCurrentProcessId(), tsshim, verbose); }
        CreateThread(NULL, 0, stats_thread, NULL, 0, NULL);   // 5-second pool usage line always; the rest only when logging
    }
    return TRUE;
}
