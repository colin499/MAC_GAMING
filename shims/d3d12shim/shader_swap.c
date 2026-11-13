// Layover d3d12shim shader swap: replace a game's shaders, by hash, with fixed copies built on this Mac.
//
// Why: D3DMetal backs a typed buffer view with a Metal texture buffer, capped at 2^28 elements. A game that
// reads a big pool through 4-byte typed views (Spider-Man 2: vertex paint via R8G8B8A8_UNORM, impostor
// normals via R10G10B10A2_UNORM over a 2 GB pool) gets zeros for everything stored above 1 GB: ground and
// buildings lose their painted material blends. The same shaders also have a raw view of the pool, so
// `layover shaderfix` (tools/typed2raw.py + dxc/dxa/dxv) builds copies that read the same bytes through the
// raw view, and this module hands D3DMetal the copy whenever the game creates a pipeline with the original.
// Shaders without a raw view (the impostor shaders that draw mid-distance city blocks) read the typed register
// in place; the shim then creates that view raw (pool_fix.c, LAYOVER_RAW_FORMATS).
//
//   LAYOVER_SHADER_SWAP=<win dir>     replacements: <fnv1 hash of the ORIGINAL>.dxil; <hash>.skip marks a
//                                     shader that was looked at and needs nothing
//   LAYOVER_SHADER_PENDING=<win dir>  originals that match LAYOVER_SHADER_WATCH but have no replacement yet
//                                     are written here as <hash>.dxbc for the next `layover shaderfix`
//   LAYOVER_SHADER_WATCH=1:77,1:95    registers (space:register) whose users need a replacement, found in
//                                     the shader's PSV0 resource table
// A shader without a replacement is used unchanged, exactly as before.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void shim_note(const char *fmt, ...);
UINT64 pool_fnv1(const void *p, SIZE_T n);
int shim_readable(const void *p, SIZE_T n);

typedef struct { UINT64 hash; void *data; SIZE_T size; volatile LONG used; } Swap;
static Swap *table; static int n_table, n_skip; static UINT64 *skips;
static char swap_dir[MAX_PATH], pending_dir[MAX_PATH]; static int swap_on, pending_on;
static struct { UINT32 space, reg; } watch[16]; static int n_watch;
#define SEEN_CAP 8192
static UINT64 seen[SEEN_CAP]; static CRITICAL_SECTION seen_lock;
static volatile LONG c_swapped, c_failed, c_pending, c_unfixed;

static int cmp_swap(const void *a, const void *b) { UINT64 x = ((const Swap *)a)->hash, y = ((const Swap *)b)->hash; return x < y ? -1 : x > y; }
static int cmp_u64(const void *a, const void *b) { UINT64 x = *(const UINT64 *)a, y = *(const UINT64 *)b; return x < y ? -1 : x > y; }

static void *read_file(const char *path, SIZE_T *size)
{
    FILE *f = fopen(path, "rb"); long n; void *d = NULL;
    *size = 0; if (!f) return NULL;
    if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t)n))) { if (fread(d, 1, (size_t)n, f) == (size_t)n) *size = (SIZE_T)n; else { free(d); d = NULL; } }
    fclose(f);
    return d;
}

void shader_swap_install(void)
{
    char pat[MAX_PATH + 16], v[256]; WIN32_FIND_DATAA fd; HANDLE h; int cap = 0, scap = 0;
    InitializeCriticalSection(&seen_lock);
    if (GetEnvironmentVariableA("LAYOVER_SHADER_WATCH", v, sizeof v) >= 3) {
        char *p = v;
        while (*p && n_watch < 16) { unsigned s, r; if (sscanf(p, "%u:%u", &s, &r) == 2) { watch[n_watch].space = s; watch[n_watch].reg = r; n_watch++; } p = strchr(p, ','); if (!p) break; p++; }
    }
    if (GetEnvironmentVariableA("LAYOVER_SHADER_PENDING", pending_dir, sizeof pending_dir) >= 3 && n_watch) { CreateDirectoryA(pending_dir, NULL); pending_on = 1; }
    if (GetEnvironmentVariableA("LAYOVER_SHADER_SWAP", swap_dir, sizeof swap_dir) < 3) { if (pending_on) shim_note("shader swap: no replacement folder; shaders using the watched registers go to %s", pending_dir); return; }
    snprintf(pat, sizeof pat, "%s\\*.*", swap_dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *dot = strrchr(fd.cFileName, '.'); UINT64 hash; char *end;
            if (!dot || dot - fd.cFileName != 16) continue;
            hash = _strtoui64(fd.cFileName, &end, 16); if (end != dot) continue;
            if (!_stricmp(dot, ".skip")) {
                if (n_skip == scap) { scap = scap ? scap * 2 : 256; skips = realloc(skips, scap * sizeof *skips); }
                skips[n_skip++] = hash;
            } else if (!_stricmp(dot, ".dxil")) {
                char path[MAX_PATH * 2]; SIZE_T size; void *d;
                snprintf(path, sizeof path, "%s\\%s", swap_dir, fd.cFileName);
                d = read_file(path, &size);
                if (!d || size < 32 || memcmp(d, "DXBC", 4)) { free(d); continue; }
                if (n_table == cap) { cap = cap ? cap * 2 : 256; table = realloc(table, cap * sizeof *table); }
                table[n_table].hash = hash; table[n_table].data = d; table[n_table].size = size; table[n_table].used = 0; n_table++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (n_table) qsort(table, n_table, sizeof *table, cmp_swap);
    if (n_skip) qsort(skips, n_skip, sizeof *skips, cmp_u64);
    swap_on = n_table > 0;
    shim_note("shader swap: %d replacement shaders and %d no-change marks in %s; watching %d registers%s", n_table, n_skip, swap_dir, n_watch, pending_on ? "" : " (no pending folder)");
}

// does the shader's PSV0 part list a resource at one of the watched (space, register) pairs?
static int uses_watched(const UINT8 *d, SIZE_T len)
{
    UINT32 parts, i; int w;
    if (len < 36) return 0;
    memcpy(&parts, d + 28, 4);
    if (parts > 64 || 32 + (SIZE_T)parts * 4 > len) return 0;
    for (i = 0; i < parts; i++) {
        UINT32 off, size; SIZE_T k;
        memcpy(&off, d + 32 + i * 4, 4);
        if ((SIZE_T)off + 8 > len || memcmp(d + off, "PSV0", 4)) continue;
        memcpy(&size, d + off + 4, 4);
        if ((SIZE_T)off + 8 + size > len) return 0;
        for (w = 0; w < n_watch; w++) {
            UINT32 rec[3] = { watch[w].space, watch[w].reg, watch[w].reg };
            for (k = 0; k + 12 <= size; k += 4) if (!memcmp(d + off + 8 + k, rec, 12)) return 1;
        }
    }
    return 0;
}

// the replacement for a shader (NULL = use it as it is). Unknown shaders that use a watched register are saved for the builder.
const void *shader_swap_lookup(const void *code, SIZE_T len, SIZE_T *out_len)
{
    UINT64 h; int lo, hi;
    if ((!swap_on && !pending_on) || !code || len < 36 || len > (64u << 20)) return NULL;
    h = pool_fnv1(code, len);
    for (lo = 0, hi = n_table - 1; lo <= hi; ) { int mid = (lo + hi) / 2; if (table[mid].hash == h) { InterlockedIncrement(&table[mid].used); *out_len = table[mid].size; return table[mid].data; } if (table[mid].hash < h) lo = mid + 1; else hi = mid - 1; }
    if (!n_watch) return NULL;
    for (lo = 0, hi = n_skip - 1; lo <= hi; ) { int mid = (lo + hi) / 2; if (skips[mid] == h) return NULL; if (skips[mid] < h) lo = mid + 1; else hi = mid - 1; }
    {   // seen before in this run?
        UINT i; int is_new = 0;
        EnterCriticalSection(&seen_lock);
        for (i = (UINT)(h & (SEEN_CAP - 1)); ; i = (i + 1) & (SEEN_CAP - 1)) { if (seen[i] == h) break; if (!seen[i]) { seen[i] = h; is_new = 1; break; } }
        LeaveCriticalSection(&seen_lock);
        if (!is_new) return NULL;
    }
    if (!shim_readable(code, 36) || memcmp(code, "DXBC", 4) || !uses_watched(code, len)) return NULL;
    InterlockedIncrement(&c_unfixed);
    if (pending_on) {
        char path[MAX_PATH + 64]; FILE *f;
        snprintf(path, sizeof path, "%s\\%016llx.dxbc", pending_dir, (unsigned long long)h);
        if ((f = fopen(path, "wb"))) { fwrite(code, 1, len, f); fclose(f); InterlockedIncrement(&c_pending); }
    }
    return NULL;
}
void shader_swap_result(int ok) { if (ok) InterlockedIncrement(&c_swapped); else InterlockedIncrement(&c_failed); }
int shader_swap_active(void) { return swap_on || pending_on; }
void shader_swap_stats(char *out, size_t cap)
{
    int i, used = 0;
    if (!swap_on && !pending_on) { out[0] = 0; return; }
    for (i = 0; i < n_table; i++) if (table[i].used) used++;
    snprintf(out, cap, "shader-swap: %d replacements loaded, %d in use, pipelines swapped %ld, failed %ld | not fixed yet %ld (saved for the next build: %ld)", n_table, used, c_swapped, c_failed, c_unfixed, c_pending);
}
