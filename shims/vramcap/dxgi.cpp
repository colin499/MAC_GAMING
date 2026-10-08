// Layover: dxgi.dll wrapper that caps the video memory DXGI reports.
//
// D3DMetal reports Metal's recommended working set (75% of RAM, 36 GB on a 48 GB Mac) as
// dedicated video memory and twice that as the memory budget. Games size their GPU pools from
// those numbers; pools that large can exceed Metal's limits (D3DMetal then logs "Texture buffer
// size larger than device limit, limiting size").
//
// Every export is forwarded to D3DMetal's own dxgi.dll, which lives next to this file under the
// name VRAMCAP_REAL_DXGI (default dxgi_d3dm.dll; Wine needs a system32 placeholder of that name).
// Factories returned to the game get their vtable pointer swapped to a patched copy whose
// EnumAdapters* return adapters that are patched the same way, so GetDesc/GetDesc1/2/3 and
// QueryVideoMemoryInfo never report more than VRAMCAP_MB (default 8192). The framework's own
// vtables are never written (Wine refuses VirtualProtect on memory it did not map).
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// vtable slots (C++ layout). Factory: IUnknown 0-2, IDXGIObject 3-6, EnumAdapters 7, EnumAdapters1 12,
// EnumAdapterByLuid 26, EnumAdapterByGpuPreference 29 (IDXGIFactory7 has 32 slots).
// Adapter: GetDesc 8, GetDesc1 10, GetDesc2 11, QueryVideoMemoryInfo 14, GetDesc3 18 (19 slots).
enum { F_EnumAdapters = 7, F_EnumAdapters1 = 12, F_EnumAdapterByLuid = 26, F_EnumAdapterByGpuPreference = 29, F_SLOTS = 32 };
enum { A_GetDesc = 8, A_GetDesc1 = 10, A_GetDesc2 = 11, A_QueryVideoMemoryInfo = 14, A_GetDesc3 = 18, A_SLOTS = 19 };

static HMODULE real, self;
static FILE *logf;
static UINT64 cap_bytes = 8192ull << 20;
static CRITICAL_SECTION lock;

static void logmsg(const char *fmt, ...) {
    if (!logf) {
        const char *p = getenv("VRAMCAP_LOG");
        logf = p ? fopen(p, "a") : stderr;
        if (!logf) logf = stderr;
    }
    va_list ap; va_start(ap, fmt);
    fprintf(logf, "[vramcap %lu] ", (unsigned long)GetCurrentProcessId()); vfprintf(logf, fmt, ap); fprintf(logf, "\n"); fflush(logf);
    va_end(ap);
}

static HMODULE load_real() {
    if (real) return real;
    const char *p = getenv("VRAMCAP_REAL_DXGI");
    if (!p) p = "dxgi_d3dm.dll";
    real = LoadLibraryA(p);
    if (real && real == self) { logmsg("real dxgi resolved to this wrapper; refusing"); real = NULL; }
    logmsg("real dxgi %s -> %p (err %lu)", p, (void *)real, real ? 0 : GetLastError());
    return real;
}

// ---- patched vtable copies, one per original vtable ----
struct VtCopy { void **orig; void **copy; VtCopy *next; };
static VtCopy *adapter_copies, *factory_copies;
static VtCopy *factory_copy_of(void *obj);

static VtCopy *find_copy(VtCopy *list, void **orig) { for (; list; list = list->next) if (list->orig == orig) return list; return NULL; }
static VtCopy *make_copy(VtCopy **list, void **orig, size_t n) {
    VtCopy *c = find_copy(*list, orig);
    if (c) return c;
    c = (VtCopy *)calloc(1, sizeof *c);
    c->orig = orig;
    c->copy = (void **)calloc(n, sizeof(void *));
    memcpy(c->copy, orig, n * sizeof(void *));
    c->next = *list; *list = c;
    return c;
}
#define ORIG(obj, T, slot) ((T)(find_copy_for(obj)->orig[slot]))

// ---- adapter hooks ----
static VtCopy *adapter_copy_of(void *obj) { void **vt = *(void ***)obj; for (VtCopy *c = adapter_copies; c; c = c->next) if (c->copy == vt) return c; return NULL; }
static SIZE_T capv(SIZE_T v) { return v > cap_bytes ? (SIZE_T)cap_bytes : v; }
#define CAP3(d) do { (d)->DedicatedVideoMemory = capv((d)->DedicatedVideoMemory); (d)->DedicatedSystemMemory = capv((d)->DedicatedSystemMemory); (d)->SharedSystemMemory = capv((d)->SharedSystemMemory); } while (0)

typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc)(void *, DXGI_ADAPTER_DESC *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc1)(void *, DXGI_ADAPTER_DESC1 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc2)(void *, DXGI_ADAPTER_DESC2 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc3)(void *, DXGI_ADAPTER_DESC3 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_QVMI)(void *, UINT, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO *);

static HRESULT STDMETHODCALLTYPE h_GetDesc(void *a, DXGI_ADAPTER_DESC *d) { VtCopy *c = adapter_copy_of(a); HRESULT hr = ((PFN_GetDesc)c->orig[A_GetDesc])(a, d); if (SUCCEEDED(hr) && d) { logmsg("GetDesc on %p: %llu MB", a, (unsigned long long)(d->DedicatedVideoMemory >> 20)); CAP3(d); } return hr; }
static HRESULT STDMETHODCALLTYPE h_GetDesc1(void *a, DXGI_ADAPTER_DESC1 *d) { VtCopy *c = adapter_copy_of(a); HRESULT hr = ((PFN_GetDesc1)c->orig[A_GetDesc1])(a, d); if (SUCCEEDED(hr) && d) { logmsg("GetDesc1 on %p: %llu MB", a, (unsigned long long)(d->DedicatedVideoMemory >> 20)); CAP3(d); } return hr; }
static HRESULT STDMETHODCALLTYPE h_GetDesc2(void *a, DXGI_ADAPTER_DESC2 *d) { VtCopy *c = adapter_copy_of(a); HRESULT hr = ((PFN_GetDesc2)c->orig[A_GetDesc2])(a, d); if (SUCCEEDED(hr) && d) { logmsg("GetDesc2 on %p: %llu MB", a, (unsigned long long)(d->DedicatedVideoMemory >> 20)); CAP3(d); } return hr; }
static HRESULT STDMETHODCALLTYPE h_GetDesc3(void *a, DXGI_ADAPTER_DESC3 *d) { VtCopy *c = adapter_copy_of(a); HRESULT hr = ((PFN_GetDesc3)c->orig[A_GetDesc3])(a, d); if (SUCCEEDED(hr) && d) { logmsg("GetDesc3 on %p: %llu MB", a, (unsigned long long)(d->DedicatedVideoMemory >> 20)); CAP3(d); } return hr; }
static HRESULT STDMETHODCALLTYPE h_QVMI(void *a, UINT node, DXGI_MEMORY_SEGMENT_GROUP seg, DXGI_QUERY_VIDEO_MEMORY_INFO *i) {
    VtCopy *c = adapter_copy_of(a);
    HRESULT hr = ((PFN_QVMI)c->orig[A_QueryVideoMemoryInfo])(a, node, seg, i);
    if (SUCCEEDED(hr) && i) {
        logmsg("QueryVideoMemoryInfo on %p seg %u: budget %llu MB", a, (unsigned)seg, (unsigned long long)(i->Budget >> 20));
        if (i->Budget > cap_bytes) i->Budget = cap_bytes;
        if (i->AvailableForReservation > cap_bytes / 2) i->AvailableForReservation = cap_bytes / 2;
    }
    return hr;
}


typedef HRESULT (STDMETHODCALLTYPE *PFN_QI)(void *, REFIID, void **);
static void patch_adapter(void *adapter);
static void patch_factory(void *factory);
static void fmt_guid(REFIID r, char *buf) { sprintf(buf, "%08lx-%04x-%04x", (unsigned long)r.Data1, r.Data2, r.Data3); }
static HRESULT STDMETHODCALLTYPE h_AdapterQI(void *a, REFIID riid, void **out) {
    HRESULT hr = ((PFN_QI)adapter_copy_of(a)->orig[0])(a, riid, out);
    char g[40]; fmt_guid(riid, g);
    if (SUCCEEDED(hr) && out && *out && *out != a) { logmsg("adapter QI %s -> different object %p (this %p)", g, *out, a); if (riid != IID_IUnknown && riid != IID_IDXGIObject) patch_adapter(*out); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_FactoryQI(void *f, REFIID riid, void **out) {
    HRESULT hr = ((PFN_QI)factory_copy_of(f)->orig[0])(f, riid, out);
    char g[40]; fmt_guid(riid, g);
    if (SUCCEEDED(hr) && out && *out && *out != f) { logmsg("factory QI %s -> different object %p (this %p)", g, *out, f); patch_factory(*out); }
    return hr;
}

static void patch_adapter(void *adapter) {
    if (!adapter) return;
    EnterCriticalSection(&lock);
    void **vt = *(void ***)adapter;
    if (!adapter_copy_of(adapter)) {
        VtCopy *c = make_copy(&adapter_copies, vt, A_SLOTS);
        c->copy[A_GetDesc] = (void *)h_GetDesc; c->copy[A_GetDesc1] = (void *)h_GetDesc1; c->copy[A_GetDesc2] = (void *)h_GetDesc2;
        c->copy[A_QueryVideoMemoryInfo] = (void *)h_QVMI; c->copy[A_GetDesc3] = (void *)h_GetDesc3; c->copy[0] = (void *)h_AdapterQI;
        *(void ***)adapter = c->copy;
        logmsg("adapter %p: vtable %p -> patched copy %p", adapter, (void *)vt, (void *)c->copy);
    }
    LeaveCriticalSection(&lock);
}

// ---- factory hooks ----
static VtCopy *factory_copy_of(void *obj) { void **vt = *(void ***)obj; for (VtCopy *c = factory_copies; c; c = c->next) if (c->copy == vt) return c; return NULL; }
typedef HRESULT (STDMETHODCALLTYPE *PFN_EnumAdapters)(void *, UINT, void **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_EnumAdapterByLuid)(void *, LUID, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_EnumAdapterByGpuPreference)(void *, UINT, DXGI_GPU_PREFERENCE, REFIID, void **);

static HRESULT STDMETHODCALLTYPE h_EnumAdapters(void *f, UINT i, void **out) { HRESULT hr = ((PFN_EnumAdapters)factory_copy_of(f)->orig[F_EnumAdapters])(f, i, out); if (SUCCEEDED(hr)) { logmsg("EnumAdapters(%u) -> %p", i, *out); patch_adapter(*out); } return hr; }
static HRESULT STDMETHODCALLTYPE h_EnumAdapters1(void *f, UINT i, void **out) { HRESULT hr = ((PFN_EnumAdapters)factory_copy_of(f)->orig[F_EnumAdapters1])(f, i, out); if (SUCCEEDED(hr)) { logmsg("EnumAdapters1(%u) -> %p", i, *out); patch_adapter(*out); } return hr; }
static HRESULT STDMETHODCALLTYPE h_EnumAdapterByLuid(void *f, LUID l, REFIID riid, void **out) { HRESULT hr = ((PFN_EnumAdapterByLuid)factory_copy_of(f)->orig[F_EnumAdapterByLuid])(f, l, riid, out); if (SUCCEEDED(hr)) { logmsg("EnumAdapterByLuid -> %p", *out); patch_adapter(*out); } return hr; }
static HRESULT STDMETHODCALLTYPE h_EnumAdapterByGpuPreference(void *f, UINT i, DXGI_GPU_PREFERENCE p, REFIID riid, void **out) { HRESULT hr = ((PFN_EnumAdapterByGpuPreference)factory_copy_of(f)->orig[F_EnumAdapterByGpuPreference])(f, i, p, riid, out); if (SUCCEEDED(hr)) { logmsg("EnumAdapterByGpuPreference(%u) -> %p", i, *out); patch_adapter(*out); } return hr; }

static void patch_factory(void *factory) {
    if (!factory) return;
    EnterCriticalSection(&lock);
    void **vt = *(void ***)factory;
    if (!factory_copy_of(factory)) {
        VtCopy *c = make_copy(&factory_copies, vt, F_SLOTS);
        c->copy[F_EnumAdapters] = (void *)h_EnumAdapters; c->copy[F_EnumAdapters1] = (void *)h_EnumAdapters1;
        c->copy[F_EnumAdapterByLuid] = (void *)h_EnumAdapterByLuid; c->copy[F_EnumAdapterByGpuPreference] = (void *)h_EnumAdapterByGpuPreference; c->copy[0] = (void *)h_FactoryQI;
        *(void ***)factory = c->copy;
        logmsg("factory %p: vtable %p -> patched copy %p", factory, (void *)vt, (void *)c->copy);
    }
    LeaveCriticalSection(&lock);
}

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID, void **);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void **);
typedef HRESULT (WINAPI *PFN_DXGIGetDebugInterface1)(UINT, REFIID, void **);
typedef HRESULT (WINAPI *PFN_DXGIDeclareAdapterRemovalSupport)(void);
typedef HRESULT (WINAPI *PFN_DXGID3D10CreateDevice)(HMODULE, IDXGIFactory *, IDXGIAdapter *, UINT, const void *, UINT, void **);
typedef HRESULT (WINAPI *PFN_DXGID3D10RegisterLayers)(const void *, UINT);
#define GETREAL(name) static PFN_##name p_##name; if (!p_##name) { load_real(); p_##name = real ? (PFN_##name)GetProcAddress(real, #name) : NULL; } if (!p_##name) { logmsg("%s: real export missing", #name); return E_FAIL; }

extern "C" {
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **out) { GETREAL(CreateDXGIFactory); HRESULT hr = p_CreateDXGIFactory(riid, out); if (SUCCEEDED(hr)) { logmsg("CreateDXGIFactory -> %p", *out); patch_factory(*out); } return hr; }
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **out) { GETREAL(CreateDXGIFactory1); HRESULT hr = p_CreateDXGIFactory1(riid, out); if (SUCCEEDED(hr)) { logmsg("CreateDXGIFactory1 -> %p", *out); patch_factory(*out); } return hr; }
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **out) { GETREAL(CreateDXGIFactory2); HRESULT hr = p_CreateDXGIFactory2(flags, riid, out); if (SUCCEEDED(hr)) { logmsg("CreateDXGIFactory2(%u) -> %p", flags, *out); patch_factory(*out); } return hr; }
__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void **out) { GETREAL(DXGIGetDebugInterface1); return p_DXGIGetDebugInterface1(flags, riid, out); }
__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void) { GETREAL(DXGIDeclareAdapterRemovalSupport); return p_DXGIDeclareAdapterRemovalSupport(); }
__declspec(dllexport) HRESULT WINAPI DXGID3D10CreateDevice(HMODULE m, IDXGIFactory *f, IDXGIAdapter *a, UINT flags, const void *fl, UINT n, void **dev) { GETREAL(DXGID3D10CreateDevice); return p_DXGID3D10CreateDevice(m, f, a, flags, fl, n, dev); }
__declspec(dllexport) HRESULT WINAPI DXGID3D10RegisterLayers(const void *layers, UINT n) { GETREAL(DXGID3D10RegisterLayers); return p_DXGID3D10RegisterLayers(layers, n); }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&lock);
        const char *mb = getenv("VRAMCAP_MB");
        if (mb && atoi(mb) > 0) cap_bytes = (UINT64)atoi(mb) << 20;
        logmsg("loaded, cap %llu MB", (unsigned long long)(cap_bytes >> 20));
    }
    return TRUE;
}
