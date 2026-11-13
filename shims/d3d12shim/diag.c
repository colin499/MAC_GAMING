// Layover d3d12shim diagnostics: eyes and shader dumps for games that run where no screen capture is allowed.
//
//   LAYOVER_SHOT_DIR=<win path>   in-game screenshots. Whenever a file named `request` exists in that folder the
//                                 next presented frame is saved there as shot-<time>.bmp (the swapchain's back
//                                 buffer, halved in each direction when wider than 2000 px) and the request is
//                                 removed. LAYOVER_SHOT_EVERY=<seconds> also saves one periodically.
//                                 `layover shot` drops the request and converts the result to JPEG.
//   LAYOVER_DUMP_SHADERS=<win path>  every distinct shader the game hands to a pipeline is written there as
//                                 <stage>-<fnv1 hash>.dxbc (stage vs/ps/gs/hs/ds/cs/as/ms). Offline: `dxc -dumpbin`.
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void shim_note(const char *fmt, ...);
UINT64 pool_fnv1(const void *p, SIZE_T n);
int shim_readable(const void *p, SIZE_T n);

// ---------------------------------------------------------------- shader dump
static char dump_dir[MAX_PATH]; static int dump_on;
#define SEEN_CAP (1u << 16)
static UINT64 seen[SEEN_CAP]; static CRITICAL_SECTION seen_lock; static volatile LONG dumped, dump_failed;

void diag_shader(const void *code, SIZE_T len, const char *stage)
{
    UINT64 h; UINT i; char path[MAX_PATH + 64]; FILE *f; int is_new = 0;
    if (!dump_on || !code || len < 32 || len > (64u << 20) || !shim_readable(code, 32) || memcmp(code, "DXBC", 4)) return;
    h = pool_fnv1(code, len); if (!h) h = 1;
    EnterCriticalSection(&seen_lock);
    for (i = (UINT)(h & (SEEN_CAP - 1)); ; i = (i + 1) & (SEEN_CAP - 1)) {
        if (seen[i] == h) break;
        if (!seen[i]) { if (dumped < (LONG)(SEEN_CAP - 256)) { seen[i] = h; is_new = 1; } break; }
    }
    LeaveCriticalSection(&seen_lock);
    if (!is_new) return;
    snprintf(path, sizeof path, "%s\\%s-%016llx.dxbc", dump_dir, stage, (unsigned long long)h);
    f = fopen(path, "wb");
    if (!f) { if (InterlockedIncrement(&dump_failed) <= 3) shim_note("shader dump: cannot write %s", path); return; }
    fwrite(code, 1, len, f); fclose(f);
    InterlockedIncrement(&dumped);
}
// a pipeline-state stream: subobject types 1..6 = VS PS DS HS GS CS, 24 = AS, 25 = MS (payload = D3D12_SHADER_BYTECODE at +8)
SIZE_T pool_stream_sub_size(UINT type);
void diag_shader_stream(const void *stream, SIZE_T size)
{
    static const char *names[] = { 0, "vs", "ps", "ds", "hs", "gs", "cs" };
    const UINT8 *b = stream; SIZE_T off = 0;
    if (!dump_on || !b) return;
    while (off + 4 <= size) {
        UINT type = *(const UINT *)(b + off); SIZE_T sub = pool_stream_sub_size(type);
        if (!sub || size - off < sub) return;
        if ((type >= 1 && type <= 6) || type == 24 || type == 25) {
            const void *code; UINT64 len; memcpy(&code, b + off + 8, 8); memcpy(&len, b + off + 16, 8);
            diag_shader(code, (SIZE_T)len, type == 24 ? "as" : type == 25 ? "ms" : names[type]);
        }
        off += sub;
    }
}
int diag_dumping(void) { return dump_on; }
long diag_dumped(void) { return dumped; }

// ---------------------------------------------------------------- screenshots
static char shot_dir[MAX_PATH]; static int shot_on; static UINT shot_every_ms; static DWORD shot_last; static volatile LONG shot_busy, shot_count, shot_tick;
static ID3D12CommandQueue *shot_queue;

void diag_swapchain_queue(IUnknown *dev)
{
    ID3D12CommandQueue *q = NULL;
    if (!shot_on || !dev || shot_queue) return;
    if (SUCCEEDED(dev->lpVtbl->QueryInterface(dev, &IID_ID3D12CommandQueue, (void **)&q)) && q) { shot_queue = q; shim_note("screenshots: armed on queue %p, folder %s", (void *)q, shot_dir); }
}

static void put16(UINT8 *p, UINT v) { p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); }
static void put32(UINT8 *p, UINT v) { p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); p[2] = (UINT8)(v >> 16); p[3] = (UINT8)(v >> 24); }

static int save_bmp(const char *path, const UINT8 *src, UINT pitch, UINT w, UINT h, DXGI_FORMAT fmt)
{
    UINT step = w > 2000 ? 2 : 1, ow = w / step, oh = h / step, row = (ow * 3 + 3) & ~3u, x, y; UINT8 hdr[54] = {0}, *line; FILE *f;
    f = fopen(path, "wb"); if (!f) return 0;
    line = calloc(1, row); if (!line) { fclose(f); return 0; }
    hdr[0] = 'B'; hdr[1] = 'M'; put32(hdr + 2, 54 + row * oh); put32(hdr + 10, 54); put32(hdr + 14, 40); put32(hdr + 18, ow); put32(hdr + 22, oh); put16(hdr + 26, 1); put16(hdr + 28, 24); put32(hdr + 34, row * oh);
    fwrite(hdr, 1, 54, f);
    for (y = 0; y < oh; y++) {   // BMP rows go bottom-up
        const UINT8 *s = src + (SIZE_T)pitch * ((oh - 1 - y) * step);
        for (x = 0; x < ow; x++) {
            const UINT8 *p = s + (SIZE_T)x * step * 4; UINT8 r, g, b;
            if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || fmt == DXGI_FORMAT_B8G8R8X8_UNORM) { b = p[0]; g = p[1]; r = p[2]; }
            else if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) { UINT v; memcpy(&v, p, 4); r = (UINT8)((v & 1023) >> 2); g = (UINT8)(((v >> 10) & 1023) >> 2); b = (UINT8)(((v >> 20) & 1023) >> 2); }
            else { r = p[0]; g = p[1]; b = p[2]; }
            line[x * 3] = b; line[x * 3 + 1] = g; line[x * 3 + 2] = r;
        }
        fwrite(line, 1, row, f);
    }
    free(line); fclose(f);
    return 1;
}

static void take_shot(IDXGISwapChain *sc)
{
    IDXGISwapChain3 *sc3 = NULL; ID3D12Resource *bb = NULL, *rb = NULL; ID3D12Device *dev = NULL; ID3D12CommandAllocator *ca = NULL; ID3D12GraphicsCommandList *cl = NULL; ID3D12Fence *fence = NULL;
    D3D12_RESOURCE_DESC desc, bd; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows = 0; UINT64 row_size = 0, total = 0; D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_BARRIER br; D3D12_TEXTURE_COPY_LOCATION src, dst;
    HANDLE ev = NULL; void *map = NULL; char path[MAX_PATH + 64]; SYSTEMTIME st; UINT idx = 0; const char *why = "?"; HRESULT hr = S_OK; D3D12_RANGE rr;
    why = "no IDXGISwapChain3"; if (FAILED(hr = sc->lpVtbl->QueryInterface(sc, &IID_IDXGISwapChain3, (void **)&sc3)) || !sc3) goto out;
    idx = sc3->lpVtbl->GetCurrentBackBufferIndex(sc3);
    why = "GetBuffer"; if (FAILED(hr = sc3->lpVtbl->GetBuffer(sc3, idx, &IID_ID3D12Resource, (void **)&bb)) || !bb) goto out;
    why = "GetDevice"; if (FAILED(hr = bb->lpVtbl->GetDevice(bb, &IID_ID3D12Device, (void **)&dev)) || !dev) goto out;
    bb->lpVtbl->GetDesc(bb, &desc);
    why = "format"; if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && desc.Format != DXGI_FORMAT_B8G8R8X8_UNORM && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM)) { hr = (HRESULT)desc.Format; goto out; }
    dev->lpVtbl->GetCopyableFootprints(dev, &desc, 0, 1, 0, &fp, &rows, &row_size, &total);
    memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_READBACK;
    memset(&bd, 0, sizeof bd); bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    why = "readback buffer"; if (FAILED(hr = dev->lpVtbl->CreateCommittedResource(dev, &hp, 0, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb)) || !rb) goto out;
    why = "allocator"; if (FAILED(hr = dev->lpVtbl->CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca)) || !ca) goto out;
    why = "command list"; if (FAILED(hr = dev->lpVtbl->CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl)) || !cl) goto out;
    memset(&br, 0, sizeof br); br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = bb; br.Transition.Subresource = 0; br.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->lpVtbl->ResourceBarrier(cl, 1, &br);
    memset(&src, 0, sizeof src); src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    memset(&dst, 0, sizeof dst); dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    cl->lpVtbl->CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; br.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->lpVtbl->ResourceBarrier(cl, 1, &br);
    why = "Close"; if (FAILED(hr = cl->lpVtbl->Close(cl))) goto out;
    { ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl }; shot_queue->lpVtbl->ExecuteCommandLists(shot_queue, 1, lists); }
    why = "fence"; if (FAILED(hr = dev->lpVtbl->CreateFence(dev, 0, 0, &IID_ID3D12Fence, (void **)&fence)) || !fence) goto out;
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    why = "Signal"; if (FAILED(hr = shot_queue->lpVtbl->Signal(shot_queue, fence, 1))) goto out;
    fence->lpVtbl->SetEventOnCompletion(fence, 1, ev);
    why = "GPU wait (3 s)"; if (WaitForSingleObject(ev, 3000) != WAIT_OBJECT_0) { hr = E_FAIL; goto out; }
    rr.Begin = 0; rr.End = (SIZE_T)total;
    why = "Map"; if (FAILED(hr = rb->lpVtbl->Map(rb, 0, &rr, &map)) || !map) goto out;
    GetLocalTime(&st);
    snprintf(path, sizeof path, "%s\\shot-%02d%02d%02d-%03ld.bmp", shot_dir, st.wHour, st.wMinute, st.wSecond, InterlockedIncrement(&shot_count));
    why = "write"; if (!save_bmp(path, (const UINT8 *)map + fp.Offset, fp.Footprint.RowPitch, fp.Footprint.Width, fp.Footprint.Height, desc.Format)) { hr = E_FAIL; rr.End = 0; rb->lpVtbl->Unmap(rb, 0, &rr); goto out; }
    rr.End = 0; rb->lpVtbl->Unmap(rb, 0, &rr);
    shim_note("screenshot: %s (%ux%u fmt %u, back buffer %u)", path, fp.Footprint.Width, fp.Footprint.Height, (unsigned)desc.Format, idx);
    why = NULL;
out:
    if (why) shim_note("screenshot failed at %s (hr 0x%lx)", why, (unsigned long)hr);
    if (ev) CloseHandle(ev);
    if (fence) fence->lpVtbl->Release(fence);
    if (cl) cl->lpVtbl->Release(cl);
    if (ca) ca->lpVtbl->Release(ca);
    if (rb) rb->lpVtbl->Release(rb);
    if (dev) dev->lpVtbl->Release(dev);
    if (bb) bb->lpVtbl->Release(bb);
    if (sc3) sc3->lpVtbl->Release(sc3);
}

// called on the game's present thread right before the real Present
void diag_present(IDXGISwapChain *sc)
{
    char req[MAX_PATH + 16]; int want = 0; DWORD now;
    if (!shot_on || !shot_queue) return;
    if (InterlockedIncrement(&shot_tick) % 20) return;
    now = GetTickCount();
    snprintf(req, sizeof req, "%s\\request", shot_dir);
    if (GetFileAttributesA(req) != INVALID_FILE_ATTRIBUTES) { DeleteFileA(req); want = 1; }
    else if (shot_every_ms && now - shot_last >= shot_every_ms) want = 1;
    if (!want || InterlockedExchange(&shot_busy, 1)) return;
    shot_last = now;
    take_shot(sc);
    InterlockedExchange(&shot_busy, 0);
}

void diag_install(void)
{
    char v[32];
    InitializeCriticalSection(&seen_lock);
    if (GetEnvironmentVariableA("LAYOVER_DUMP_SHADERS", dump_dir, sizeof dump_dir) >= 3) { CreateDirectoryA(dump_dir, NULL); dump_on = 1; shim_note("shader dump: every distinct shader goes to %s", dump_dir); }
    if (GetEnvironmentVariableA("LAYOVER_SHOT_DIR", shot_dir, sizeof shot_dir) >= 3) {
        CreateDirectoryA(shot_dir, NULL); shot_on = 1; shot_last = GetTickCount();
        if (GetEnvironmentVariableA("LAYOVER_SHOT_EVERY", v, sizeof v) >= 1 && atoi(v) > 0) shot_every_ms = (UINT)atoi(v) * 1000;
    }
}

// ---------------------------------------------------------------- view / texture survey (LAYOVER_VIEW_SURVEY=1)
// One log line per distinct kind of texture view and texture the game creates: which formats are written by
// compute, which views cast a resource to another format.
static int survey_on = -1; static UINT64 sv_seen[4096]; static CRITICAL_SECTION sv_lock;
static int sv_new(UINT64 key)
{
    UINT i; int is_new = 0;
    if (survey_on < 0) { char v[8]; InitializeCriticalSection(&sv_lock); survey_on = GetEnvironmentVariableA("LAYOVER_VIEW_SURVEY", v, sizeof v) >= 1 && v[0] != '0'; }
    if (!survey_on) return 0;
    key = key * 0x9E3779B97F4A7C15ull + 1; if (!key) key = 1;
    EnterCriticalSection(&sv_lock);
    for (i = (UINT)(key >> 40) & 4095; ; i = (i + 1) & 4095) { if (sv_seen[i] == key) break; if (!sv_seen[i]) { sv_seen[i] = key; is_new = 1; break; } }
    LeaveCriticalSection(&sv_lock);
    return is_new;
}
void diag_view(int kind, ID3D12Resource *res, unsigned fmt, unsigned dim)
{
    D3D12_RESOURCE_DESC d; UINT64 key;
    if (survey_on == 0 || !res) return;
    res->lpVtbl->GetDesc(res, &d);
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return;
    key = ((UINT64)kind << 60) ^ ((UINT64)fmt << 50) ^ ((UINT64)d.Format << 40) ^ ((UINT64)dim << 36) ^ ((UINT64)d.Width << 20) ^ ((UINT64)d.Height << 6) ^ ((UINT64)d.DepthOrArraySize << 2) ^ d.MipLevels ^ ((UINT64)d.Flags << 30);
    if (sv_new(key)) shim_note("survey %s view fmt %u dim %u on texture fmt %u %llux%ux%u mips %u flags 0x%x", kind == 'U' ? "UAV" : "SRV", fmt, dim, (unsigned)d.Format, (unsigned long long)d.Width, d.Height, (unsigned)d.DepthOrArraySize, (unsigned)d.MipLevels, (unsigned)d.Flags);
}
