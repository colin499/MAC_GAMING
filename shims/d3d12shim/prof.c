// prof.c: "where does the GPU time go" by ablation (LAYOVER_PROF=1; off = no cost, nothing hooked changes).
// There is no per-pass GPU timer we can read under D3DMetal without Xcode, but the Metal HUD log gives GPU ms
// per frame (MTL_HUD_LOG_ENABLED=1, tools/dev/hud.py). So: hold the game in a steady scene, switch one group
// of work off for a few seconds through the live file, and read how far GPU ms falls.
//   live.env lines (next to fps=N; read once a second by the shim):
//     skip=<bits>    1 Dispatch  2 DrawInstanced  4 DrawIndexedInstanced  8 ExecuteIndirect   (0 = nothing)
//     skipcs=<n>     skip the Dispatches of compute pipeline number n (-1 = none); numbers are in the report
//     skipgx=<n>     skip Dispatches with exactly n thread groups in x (0 = none)
//   Every 5 s the log gets "prof:" lines: the compute pipelines by dispatches per frame, with the shader's
//   FNV-1a hash (the same hash the verbose log prints) and its size.
// The picture is wrong while something is skipped; that is the point. Never leave it on for play.
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void shim_note(const char *fmt, ...);

#define PROF_MAX 4096
typedef struct { void *pso; unsigned long long hash; unsigned len; volatile LONG n; volatile LONG64 groups; } ProfCs;
static ProfCs cs[PROF_MAX]; static volatile LONG cs_n;
static int prof_on; static DWORD prof_tls;
static volatile LONG skip_bits, skip_cs = -1, skip_gx;
static volatile LONG skipped;

int prof_active(void) { return prof_on; }
void prof_install(void)
{
    char v[16];
    if (!GetEnvironmentVariableA("LAYOVER_PROF", v, sizeof v) || atoi(v) == 0) return;
    prof_tls = TlsAlloc(); prof_on = 1;
    shim_note("prof: ablation profiler ON (live file: skip=<bits> skipcs=<n> skipgx=<n>); not for play");
}
void prof_cs_created(void *pso, unsigned long long hash, unsigned len)
{
    LONG i;
    if (!prof_on || !pso) return;
    i = InterlockedIncrement(&cs_n) - 1;
    if (i >= PROF_MAX) return;
    cs[i].hash = hash; cs[i].len = len; cs[i].pso = pso;
}
void prof_setpso(void *pso)
{
    LONG i, n;
    if (!prof_on) return;
    n = cs_n; if (n > PROF_MAX) n = PROF_MAX;
    for (i = n - 1; i >= 0; i--) if (cs[i].pso == pso) break;      // a few hundred entries; only with the profiler on
    TlsSetValue(prof_tls, (void *)(INT_PTR)(i + 1));
}
int prof_dispatch(unsigned x, unsigned y, unsigned z)              // 1 = skip this Dispatch
{
    INT_PTR i;
    if (!prof_on) return 0;
    i = (INT_PTR)TlsGetValue(prof_tls) - 1;
    if (i >= 0) { InterlockedIncrement(&cs[i].n); InterlockedAdd64(&cs[i].groups, (LONG64)x * y * z); }
    if ((skip_bits & 1) || (skip_cs >= 0 && i == skip_cs) || (skip_gx && (LONG)x == skip_gx)) { InterlockedIncrement(&skipped); return 1; }
    return 0;
}
int prof_skip(int bit) { if (prof_on && (skip_bits & bit)) { InterlockedIncrement(&skipped); return 1; } return 0; }
void prof_live(const char *line)
{
    int n;
    if (!prof_on) return;
    if (sscanf(line, "skip=%d", &n) == 1 && n != skip_bits) { shim_note("prof: skip bits %ld -> %d", skip_bits, n); skip_bits = n; }
    if (sscanf(line, "skipcs=%d", &n) == 1 && n != skip_cs) { shim_note("prof: skip compute pipeline %ld -> %d", skip_cs, n); skip_cs = n; }
    if (sscanf(line, "skipgx=%d", &n) == 1 && n != skip_gx) { shim_note("prof: skip dispatches with x groups %ld -> %d", skip_gx, n); skip_gx = n; }
}
void prof_report(long frames)                                      // from the 5 s stats thread; frames = presents in the window
{
    LONG n, i, k, cnt[PROF_MAX]; LONG64 grp[PROF_MAX]; LONG sk;
    if (!prof_on || frames <= 0) return;
    n = cs_n; if (n > PROF_MAX) n = PROF_MAX;
    for (i = 0; i < n; i++) { cnt[i] = InterlockedExchange(&cs[i].n, 0); grp[i] = InterlockedExchange64(&cs[i].groups, 0); }
    sk = InterlockedExchange(&skipped, 0);
    if (sk) shim_note("prof: %ld calls skipped in this window (%.1f per frame)", sk, (double)sk / frames);
    for (k = 0; k < 24; k++) {
        LONG best = -1;
        for (i = 0; i < n; i++) if (cnt[i] > 0 && (best < 0 || cnt[i] > cnt[best])) best = i;
        if (best < 0 || cnt[best] * 4 < frames) break;             // under a quarter of a dispatch per frame: noise
        shim_note("prof: cs #%ld hash %016llx %u bytes: %.1f dispatches/frame, %.0f groups/dispatch", best, cs[best].hash, cs[best].len, (double)cnt[best] / frames, (double)grp[best] / cnt[best]);
        cnt[best] = 0;
    }
}
