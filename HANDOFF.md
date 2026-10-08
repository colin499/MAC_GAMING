# Handoff notes (2026-10-08, end of third session)

Read this first if you are a new agent (or future me) picking up Layover.

## Third session (evening): what changed, what was learned, what is next

**State of the game:** runs stably through Steam with the stock 2046 MB pool; characters still T-pose.
No shim knobs are active (no `d3d12shim.env` files). The shim DLL in the engine overlay and in
`tools/d3d12shim.dll` is the new build from this session (inert without knobs).

**Environment facts that cost an hour:**
- Wine processes started from an agent/terminal session cannot see `~/Documents` at all (macOS
  folder access on the Wine binary), so the game said "Unable to write to the game's user folder"
  and Steam cloud sync failed with I/O errors. Fix in place: the prefix's `users/colinlysik/Documents`
  is now a REAL folder (the old symlink was renamed to `Documents.orig-link`) holding a copy of
  `Marvel's Spider-Man 2/` (saves, prefs, log). Steam cloud sync works again. The game's log and
  minidumps are now under the prefix, not `~/Documents`. Decide later whether to keep this.
- Launch the game through the running Steam (`wine start steam://rungameid/2651280`, as
  `run_in_prefix.py ... start steam://rungameid/2651280 --no-shim`), then drive the windows with
  `winlist.exe`/`sendkey.exe` (Enter on "Steam Dialog" dismisses the launch notice; Enter on the
  `GameNxApp` launcher = Play). The direct `Spider-Man2.exe` launch fails on the user-folder dialog.
- Wine reports builtin DLLs under `C:\windows\system32`, so the shim's new settings file
  `d3d12shim.env` (KEY=VALUE lines, read in DllMain, only sets variables that are unset) must be
  placed in the prefix's `drive_c/windows/system32/`, not beside the DLL in the overlay. This is how
  knobs reach a Steam-launched game (Steam's environment cannot be changed after launch).

**Shim additions (shims/d3d12shim/d3d12.c):** settings file (above); `LAYOVER_SUBHEAP_MB=<n>`
rewrites the four class-subheap sizes (32/32/96/416 MB) that the DX12 buffer manager reserves at
pool start, in both setup functions; the logpoint byte-flip is now serialized by a lock (the crash
at the logpoint address was a write to a code page another thread had just set back to read-only).
Logpoints on the allocation hot path are still unusable: with the lock the game stalls (zero GPU
work for minutes); without it the game crashes within a minute. Use them only on rare paths.

**Measurements (single ManagedBuffer manager, object at exe+0xc0dc960, menu load, 6459 allocations,
log in tools/dev/alloc-log-menu-2026-10-08.txt.gz, parser tools/dev/analyze2.py):**
- caller exe+0x2b24936 (ModelManager / "Model Subset Gpu Registry", hash-named buffers): 2158
  allocations, 532 MB, 444 of them end above 1 GB. All memloc 0, format 0. Size mix: 1177 < 64 KB,
  444 in 64-256 KB, 357 in 256 KB-1 MB, 180 >= 1 MB. Nothing at the call distinguishes skinned
  from static meshes.
- caller exe+0x2e3d09e ("Heap Set", name "Asset"): 1362 allocations, 65 MB, 286 above 1 GB.
- R16_UINT (format 57) allocations: 489, 2.2 MB total, all below 660 MB. Not involved.
- Pool high-water with the stock layout: 1656 MB after the save loads (stable, no past-end copies).
  With LAYOVER_SUBHEAP_MB=64 the four reserved subheaps shrink (16/16/64 MB effective), the game
  still runs stably, high-water is still 1656 MB, and the user still sees T-pose: compaction of the
  reserved region does not move the skinned data below 1 GB.
- D3DMetal side is closed: `D3DMDevice::GetTextureBufferSizeLimit` uses a fixed
  `kMaxTexBufferSize`; raw-load emulation exists only for RGB32 (`EnableRGB32TypedBuffersWA`).

**The remaining plan (unchanged in substance, better targeted):** skinned-character meshes are a
small fraction of the pool but are interleaved with static geometry by the same allocator. The fix
is to recognise skinned models one level above the allocation call (the ModelManager code around
exe+0x2b24700..0x2b24940 that builds a model's GPU data; the model object is in rbp there, with
byte flags at +0x184 and +0x1a4 worth checking) and give their buffers a reserved region below
1 GB, leaving static geometry free to use the rest of the 2046 MB. Verify by eye at the main menu
(T-poses with the stock layout), then a save load and a few minutes of play.

**Tools:** tools/dev/pe.py (capstone/pefile disassembler with string annotation; `python3 pe.py
VA [nbytes]`), xref.py (rel32/rip-relative reference finder), ctx.py (call-site context),
analyze2.py (allocation-log parser). Make a venv with `pip install capstone pefile numpy`.

## START HERE: the one open task is Spider-Man 2's T-pose, and here is the plan

**What you inherit.** The game runs on Layover (Wine + D3DMetal). Characters are frozen in their bind
pose. The cause is known and proven (section "ROOT CAUSE FOUND" below): the game puts all geometry
in one 2046 MB pool and reads skinning inputs through 4-byte *typed* buffer views over the whole
pool; Metal hardware caps such a view at 2^28 elements = the first 1 GB, so anything placed above
1 GB reads zeros. Shrinking the pool to 1 GB (the shim can do it) animates everything but the city
load needs more than 1 GB and the game crashes. Every other shortcut has been tried and is listed
under "Where it stands"; do not repeat them.

**The plan (path 1): keep the 2046 MB pool, make the typed-read data land in its first 1 GB.**
The engine's `D3DBufferManager` sub-allocates the pool itself, so the placement is decided by game
code we can patch in memory from `shims/d3d12shim` (it already patches the game's budget table and
hooks D3DMetal's vtables; it has an INT3 logpoint facility and a D3D12 call trace). Steps:

1. Map the allocator. Set logpoints (env `LAYOVER_LOGPOINTS=rva,rva,...`, hex RVAs relative to the
   exe base 0x140000000; `LAYOVER_LOGPOINTS_MAX=200` to log more hits) on:
   - `0x2c5b8b0` AllocateBuffer(mgr, D3DHeapAlloc* req, DXGI_FORMAT fmt=r8d, data=r9, name=[rsp+0x28]
     (logged as s5), DataCopy, MemoryLocation) - the public entry; logs the FORMAT and NAME of each
     allocation. Expect names like the g_GlobalSrv_* view names or asset names.
   - `0x2c59720` AllocSmall(request*) where rcx -> {mgr @0, subheap ptr @8, size ptr @0x10,
     align ptr @0x18}; it returns the pool offset in rax. Log entry and the return (set a logpoint on
     the `ret` at the end of the function, or on its callers' return addresses) to learn offset per
     (format, name).
   - `0x2c5a0d0` subheap creation (mgr, size=edx, flags=r8d, r9d) -> `0x2c5b3b0`; subheaps are the
     per-class regions carved from the pool (list at mgr+0xb78, count mgr+0xb80).
   Run the game (see "Tooling" for the direct launch; the user presses Play and Continue), load the
   save, and correlate: which formats/names get offsets above 0x40000000 (1 GB) and which subheap
   they come from. Pool offsets are also visible from the shim's CopyBufferRegion tracker
   (`pool high-water mark` lines; add per-copy logging if offsets are needed).
2. Pick the steering patch. Likely options, cheapest first:
   a. If skinned-mesh vertex data has its own subheap class: pre-create / enlarge that class's
      subheaps at init so they are carved first (low offsets). A logpoint at 0x2c5b3b0 shows the
      creation order and sizes; the shim could call the game's own subheap-create function early.
   b. If classes are mixed: hook AllocSmall and, for requests whose format is a 4-byte typed one
      (DXGI 28/24/42 or whatever step 1 shows), retry from a reserved low region (reserve the first
      N MB at pool creation by issuing a dummy allocation that is freed into a private free list).
   c. Fallback: two pools. Patch the pool-creation path (0x142c5ae84, CreateHeap at 0x1431dd14c)
      so the typed SRVs/UAVs are created over a second 1 GB buffer and typed allocations go there.
      Harder: GPU virtual addresses must stay consistent for raw views.
3. Verify with the user: pool stays 2046 MB in the shim log, no "limiting size" effect on skinned
   data (characters animate), save loads, 5+ minutes of play, suit switch still fine.

Everything you need to run this loop: `tools/dev/run_in_prefix.py` (direct launch with the shim and
env; put `steam_appid.txt` containing 2651280 beside Spider-Man2.exe first and remove it after),
`tools/dev/sendkey.c` (Enter = Play on the launcher window, class GameNxApp), `tools/dev/minidump.py`
(crash registers + stack from ~/Documents/Marvel's Spider-Man 2/*.mdmp), `objdump -d --start-address`
on the 194 MB exe (~30 s per call), and the shim's `LAYOVER_D3D12_LOG=Z:\path` trace. The user sits at
the machine, presses Play/Continue when asked and reports "T-pose" or "animates" by eye; screenshots
from this session capture the wrong Space. Keep Steam running (the wineserver is up with msync OFF;
the launch script copies that). Each run costs 2-3 minutes.

Traps: the vramcap dxgi wrapper crashes the game (don't use; the shim's LAYOVER_VRAM_MB replaced it);
a `ps | grep` pattern that matches your own shell's command line kills your shell; a stray Enter at
the main menu hits Quit; the game's crash dialog needs `crs-handler.exe` killed too.

## State right now

- `layover` works end to end on this Mac (M4 Pro, 48 GB, macOS 15.7.9): setup, Steam sign-in,
  game download, controller detection, offline "flight" mode.
- Engine in use: `~/Library/Application Support/Layover/engines/sikarugir10.0_6` =
  Highball's default engine manifest `x64-sikarugir10.0_6-r19` (Gcenx Wine 10.0 Sikarugir build +
  Highball patches for msync/winemac/audio + Sikarugir Template runtime with D3DMetal 3.0 + Highball DXMT fork).
  `layover setup` assembles it from pinned, sha256-verified public GitHub assets.
- Verified with a mingw-built DirectX test program (`d3dtest`, not in repo): dxmt → "Apple M4 Pro" FL 11_0,
  d3dmetal → "AMD Compatibility Mode" + D3D12 device OK, wined3d → fake GeForce fallback.
- Steam signs in within ~15 s on this engine with NO web-helper wrapper. The wrapper (`wrapper/`) is kept
  as an opt-in (`layover config webhelper_wrapper on`) because the first engine needed it.
- Marvel's Spider-Man 2 (AppID 2651280, DirectX-12-only, 108 GB installed) **runs** as of 2026-10-08 13:35:
  the user saw the game on screen under D3DMetal after the two start-up fixes below (DX12 detection,
  D3DMetal identity). First start is slow (shader compile; D3DMetal caches under
  ~/Library/Caches/d3dm/Spider-Man2.exe). D3DMetal logs a few unsupported calls (pipeline-statistics
  queries, EnumerateMetaCommands, R32G32B32 typed buffers) that are harmless.
- Two older engine folders may still exist under `engines/` (`wine-staging-11.18`, `cx26.3`, ~2.3 GB);
  `layover setup` deletes them when Steam is not running.

## Next steps

0. Spider-Man 2 T-pose: root cause found, fix blocked; see the section below for the two strategies.
   The d3d12 shim is installed by `layover setup`/every launch and inert unless games.2651280.env sets
   LAYOVER_MANAGED_MB / LAYOVER_VRAM_MB / LAYOVER_LOGPOINTS / LAYOVER_D3D12_LOG / LAYOVER_TSSHIM.
   Dev tools: tools/dev (run_in_prefix.py direct launch, sendkey.c, winlist.c, minidump.py).
1. Spider-Man 2 runs; next: play-test performance/stability, fps_cap, controller, offline mode.
   `./layover play "spider"` auto-detects DX12 → D3DMetal and restarts Steam with that renderer.
   - If Steam shows a "controller recommended" interstitial and the launch hangs, Highball's
     `SteamLaunchNotice.swift` documents a workaround: mark notices as seen in the user's
     `localconfig.vdf` under `UserLocalConfigStore/WebStorage` while Steam is closed.
   - If the game shows nothing: `./layover renderer "spider" dxmt` is not an option (DX12); try
     `./layover config fps_cap 60`, check `~/Library/Application Support/Layover/logs`, and
     Steam's own logs in the prefix under `Steam/logs`.
2. Xbox controller: user has not paired one yet. `./layover controller` / `--test`.
3. Pre-flight: `./layover flight --go` (closes Steam, flips WantsOfflineMode in loginusers.vdf for both
   Windows and Mac Steam, relaunches offline).
4. Nice-to-haves not done: single Dock icon / proper .app bundle, external-drive Steam library support,
   D3DMetal timestamp shim (Highball's `d3dmetal-tsshim`, needs `apd12.dll` patch), per-game launch
   without restarting Steam (Highball launches game exes directly with their own env).

## Hard-won facts (don't re-learn these)

- Renderer selection = `WINEDLLPATH_PREPEND` overlay dirs (CrossOver feature present in this build).
  Dropping DLLs into the prefix's system32 does NOT beat the engine's copy. Wine still needs a
  placeholder file in system32 for a builtin to be found at all.
- D3DMetal needs `CX_D3DMETALPATH` and DYLD fallback paths pointing at `renderers/d3dmetal/external`;
  the `*.so` next to its DLLs are symlinks to `libd3dshared.dylib` and must stay symlinks.
- Gcenx's "Game Porting Toolkit 3.0-3" binary is Wine 7.7; its D3DMetal shims import the old
  `ntdll.__wine_unix_call` and do not load on Wine 10/11. Current Steam does not boot on Wine 7.7.
- Highball's CrossOver-26.3 Wine 11 engine (`x64-crossover26.3-r23`): Steam's login window never
  appears (their notes call it an open Wine 11 problem). Networking there was fine; it is CEF.
- On plain WineHQ Staging 11.18, Steam's window is black unless steamwebhelper runs with
  `--disable-gpu --single-process` (wrapper) AND Steam is launched with `-noverifyfiles
  -nobootstrapupdate -skipinitialbootstrap -norepairfiles` (else it restores its own helper).
  Steam ignores `-cef-single-process` nowadays.
- Two Steam clients on one account → "Session Replaced" → installs fail with "no internet".
- The agent host app has no Screen Recording permission: `screencapture`/ScreenCaptureKit return the
  wrong Space or TCC errors. Verify via Steam's logs (`webhelper.txt` "WasHidden 0" = window shown,
  `connection_log.txt` "Logged On") and the user's eyes.
- Terminal.app lacks Files & Folders access to Documents; anything Layover needs at runtime must be
  copied under `~/Library/Application Support/Layover` (the wrapper exe is, see `tools/`).
- `brew install mingw-w64` (1.5 GB) is installed; use it for tiny Windows test programs.
- Spider-Man 2's second start-up dialog, "No installed graphics card has been detected ... monitor
  connected", is an OK/Cancel WARNING (MB_OKCANCEL; OK continues) from the game's "Querying GPU
  Driver Info" step: it builds PCI\VEN_xxxx&DEV_xxxx&SUBSYS_...&REV_.. from the DXGI adapter desc
  and looks for that device's driver entry in HKLM\System\CurrentControlSet (Enum\PCI →
  Control\Class\{4d36e968-...}\0000 → DriverVersion). Wine registers the real GPU there
  (VEN_106B&DEV_03F1 "Apple M4 Pro", DriverVersion 31.0.10.1000) while D3DMetal's DXGI reports
  VEN_1002&DEV_66AF "AMD Compatibility Mode", so the lookup fails. Fix: D3DMetal honours
  D3DM_VENDOR_ID / D3DM_DEVICE_ID / D3DM_DEVICE_DESCRIPTION (hex strings); Layover now sets them
  from system.reg (`registered_gpu()`, config `d3dmetal_identity` = apple|amd). With the Apple
  identity the game creates its device, loads Streamline/FidelityFX/XeSS and the PlayStation pad
  library and continues; the DirectX 12 check (D3D12CreateDevice at FL 11_0) passes either way.
  Found with WINEDEBUG=+relay (RelayInclude in HKCU\Software\Wine\Debug), objdump of the function
  around the MessageBoxW return address, and the exe's own log strings.
- shims/amd_ags is a replacement for AMD's AGS 6.1 runtime (builtin-signed, forced with
  WINEDLLOVERRIDES amd_ags_x64=b under D3DMetal). It was built while chasing the dialog; it was
  not the cause but it does make agsInitialize succeed (verified with a test exe) and is kept for
  the `amd` identity. D3DMetal writes its diagnostics to stderr as "[D3DMetal:LOG:...]" lines.
- Direct launches for debugging: put `steam_appid.txt` (2651280) next to Spider-Man2.exe and run
  the exe with `wine` under wine_env() while Steam is up; no Steam restart or cloud-sync click
  per iteration. Remove the file afterwards.
- One Steam-launched start (13:29) sat idle at 736 MB for 3+ minutes with no window, every thread
  parked in a wait, before data loading began; the next starts (direct and via Steam) reached 4 GB
  within 25 s. Not understood; if it recurs, `layover quit` and relaunch.
- Steam shows a "cloud sync failed" prompt for a game whose previous process was killed (every
  `layover quit` while a game runs); the next launch waits for a click on it.
- DX12 auto-detection must scan WHOLE executables: Spider-Man2.exe is 194 MB and its `d3d12.dll`
  import string sits at byte ~140 M. A 64 MB cap missed it, Steam started under DXMT, and the game
  showed "DirectX 12 support not detected ... Apple M4 Pro". The `D3D12/` Agility SDK folder and
  `*_dx12.dll` files are also treated as DX12 markers now. Scanning a half-downloaded game yields
  nothing, which is fine because unknown results are not cached.

- Spider-Man 2 writes its own log to `~/Documents/Marvel's Spider-Man 2/Marvel's Spider-Man 2.log`
  (the prefix's Documents is a symlink to the real ~/Documents). It records the adapter, every
  graphics setting, and a `[Render] Working set ... fps:` line once a minute. Read this before
  Layover's own logs: D3DMetal's stderr is empty on Steam-launched runs. The game's prefs live in
  `~/Documents/Marvel's Spider-Man 2/<steamid>/prefs-autosave.save` (binary; change them in-game).
- First play session (2026-10-08 13:36): menu 86 fps, then 30 → 10 → 10 → 22 → 39 → 43 fps per minute
  through the opening cinematic and first gameplay at the game's defaults (1728x1117 fullscreen,
  preset High, RT off, FSR "Dynamic" upscale with a 30 fps dynamic-resolution target, VSync on at
  120 Hz). ioreg showed the GPU at 83-99 % and one game thread pegged at ~98 % CPU: GPU-bound, with
  the render thread spinning. D3DMetal's shader cache (`$TMPDIR/../C/d3dm/Spider-Man2.exe/
  shaders.cache`, 255 MB after the session, bytecode/stage caches still growing at 13:42) was
  being filled the whole time, so the first run of any scene also stalls on pipeline compiles.
  Fixes are in-game: preset Medium, fixed FSR Quality instead of Dynamic, dynamic-res target off or
  60, VSync off (or `layover config fps_cap 30` for even pacing via D3DM_MAX_FPS).
- The game logs `[NxStorage] DirectStorage initialization failed` (dstorage.dll, after "Graphics
  device supports GPU decompression") and falls back to Win32 I/O + CPU decompression under
  Rosetta. Not diagnosed yet; a candidate cause for streaming hitches/pop-in. D3DMetal also
  reports "Is UMA: No" and no tiled resources.
- D3DMetal 3.0 env knobs (from `strings` on the framework): D3DM_ALLOW_HOOKING, D3DM_BOUNDS_CHECK,
  D3DM_DEVICE_DESCRIPTION/ID/REVISION/SUBSYS, D3DM_VENDOR_ID, D3DM_DXIL_PROCESS_DEBUG_INFORMATION,
  D3DM_ENABLE_ASYNC_COMMIT, D3DM_ENABLE_METALFX, D3DM_NVNGX_PATH, D3DM_EXE_OVERRIDE,
  D3DM_FLUSH_POS_INF_TO_NAN, D3DM_FORCE_RTZ_TEXWRITE, D3DM_IGNORE_D3D11_RENDER_BARRIERS,
  D3DM_LOD_BIAS, D3DM_MIN_LOD_CLAMP, D3DM_MULTITHREADED_INTERFACE_ENABLE, D3DM_NO_WINDOW,
  D3DM_NOT_IMPLEMENTED, D3DM_POSITION_INVARIANCE, D3DM_RETAIN_REFERENCES, D3DM_SAMPLE_NAN_TO_ZERO,
  D3DM_SHOW_HUD_STATS, D3DM_SUPPORT_DXR, D3DM_WAIT_ON_RESET. Untested except the identity ones;
  D3DM_SHOW_HUD_STATS and D3DM_ENABLE_METALFX (+ D3DM_NVNGX_PATH, DLSS→MetalFX?) are the ones to try.

## Spider-Man 2 T-pose: ROOT CAUSE FOUND (2026-10-08 ~16:30, second session)

Symptom: skinned characters stuck in their bind pose under D3DMetal (all of them on every launch by
the end of the first session; intermittent at first; CodeWeavers' tip page says switching suits
brings animation back).

### Cause
The game (Nixxes/Insomniac engine, build v2.810.0.0) creates ONE 2046 MB default-heap buffer, its
"ManagedBuffer" pool (vertex/index/skin data, joint remaps ...), and at start-up creates typed
buffer views over the WHOLE pool: SRVs R32G32B32A32_UINT, R8G8B8A8_UNORM, R16G16B16A16_SINT,
R16G16B16A16_FLOAT, R10G10B10A2_UNORM, R32_UINT, R32G32_UINT, a RAW view, and UAVs R32_UINT,
R16_UINT, R32G32B32A32_UINT (shim log, run1). A typed buffer is a Metal texture buffer, whose width
is capped at 2^28 = 268,435,456 elements. The 4-byte views over 2046 MB have 536 M elements and the
R16_UINT UAV 1073 M, so D3DMetal clamps them ("Texture buffer size larger than device limit,
limiting size") to the first 1 GB (512 MB for R16). Any mesh whose vertex data the game's
sub-allocator places above the clamp reads zeros for its RGBA8 weights/indices and RGB10A2
normals -> bind pose. Allocation placement explains the intermittency and the suit-switch cure.
A/B with the user's eyes: pool 2046 MB -> menu characters T-pose; pool 1022 MB -> they animate.

### Fix mechanism (works; stability still open, see below)
`shims/d3d12shim/d3d12.c` = our d3d12.dll in front of D3DMetal's, derived from Highball's tsshim
(same export forwarding + in-place vtable patching of ID3D12Device / command list / queue; the
timestamp serving is kept behind LAYOVER_TSSHIM=1). With LAYOVER_MANAGED_MB=<n> it rewrites the
game's named-budget table in memory before the pool is created. Facts about that table:
- The size comes from `D3DBufferManager` looking up "ManagedBuffer" (string at 0x145d63210) via a
  CRC32-keyed table at exe+0xc3d9838: entries 0x20 bytes {name ptr @0, ?, hash @0xc, value @0x10,
  shared ptr @0x18}; count at table+8; the table pointer may be stored as a negative offset relative
  to the table (the game's accessor at 0x14309ef30). Only two code sites reference the name
  (0x142c5aea0 pool creation, 0x142c59ed5 the [mgr+0xbb4]!=0 re-read path); no other hard-coded
  2046 MB in .text. Other budgets seen: TextureAlloc 2858, AssetHeap 1548, InitAllocator 1050,
  XMemGpuWC 2750, ModelSkinMatrixBuffer 32, Physics 512 (dump in the shim log).
- The shim patches at D3D12CreateDevice (also retried from CreateCommandQueue/Signature/QueryHeap),
  only in Spider-Man2.exe, only when the entry's name string is "ManagedBuffer"; it refuses values
  outside 256..2046 MB. With 1022 the heap is 1022 MB and every 4-byte view has 267,911,168
  elements (< 2^28); the R16_UINT UAV is still clamped at 512 MB.
- `layover` installs the overlay (renderers/d3d12shim/wine: d3d12.dll + d3d12_d3dmetal.dll copy of
  D3DMetal's + x86_64-unix/d3d12_d3dmetal.so symlink + system32 placeholder) via
  install_d3d12_shim() and sets WINEDLLOVERRIDES d3d12=b;d3d12_d3dmetal=b plus GAME_FIXES env
  (LAYOVER_MANAGED_MB=1022) whenever D3DMetal is the renderer (config `d3d12_shim`).

### Where it stands (17:15): fix mechanism proven, full fix blocked by a hardware limit
Runs with the user watching (pool size via LAYOVER_MANAGED_MB; "system-memory mode" = LAYOVER_VRAM_MB=2048,
which makes the engine print "Managed Buffer Location: System Memory", put the pool in a CPU-visible
custom heap and create a SEPARATE 120 MB "Managed UAV buffer" for GPU-written data, so the R32_UINT /
R16_UINT UAVs then fit):

| pool | GPU-write UAVs | save load | characters |
| 2046 (stock) | on the pool, clamped | fine | T-pose |
| 1535 | on the pool, clamped | fine | T-pose |
| 1022 / 1024 (also with Very Low LOD, also system-memory mode) | fit | crash 10-30 s in (3x same site) | animate (menu) |
| 2046, system-memory mode | separate, fit | fine, 1-3 min of play | T-pose |
| 4092, system-memory mode | separate, fit | fine | T-pose (8-byte views now clamped too) |

Conclusions:
- The T-pose comes from the skinning INPUT side: a 4-byte typed SRV over the pool (RGBA8 weights/
  indices, RGB10A2 or R32_UINT; the engine's view names are MainVBPosView/MainVBNrmTanView/
  MainColorVBView/MainPaintVBView/GlobalManagedBufferRaw; SkinVBPosView/SkinVBNrmTanView are the
  skinned outputs). Static geometry renders fine above 1 GB, so it is fetched raw or through 8-byte views.
- The pool cannot be <= 1 GB: with 1024 the shim's high-water tracker shows the pool fully spanned at
  the main menu and the city load then corrupts memory (crash at exe+0x2942416 in a joint-remap builder
  reading a skeleton object full of 0xAAAA/0x5556 vertex-like data, or exe+0x3081a60 in a binary search
  over a garbage table). No OOM text reaches the game log: the allocator's failure branches print
  through 0x143084b40 ("D3DManagedBufferAuditPrintReport disabled."), not the log file.
- Metal's 2^28-element texture-buffer limit is HARDWARE: with -[MTLTextureDescriptorInternal
  validateWithDevice:] swizzled to a no-op, a 2^29-wide texture buffer is created but reads above
  element 2^28 return 0 and widths that are not a power of two break in-range reads too (scratch
  mtltest/texbuf.swift). D3DMetal queries -[MTLDevice maxTextureBufferWidth] (268435456) in
  D3DMDevice::GetTextureBufferSizeLimit and clamps in D3DMBuffer::GetView. The D3DMetal binary HAS
  symbols (nm works): IRCompilerSupportRGB32TypedBuffers / IRCompilerSupportUnAlignedTypedBuffers
  are Apple shader-converter options; no generic "typed buffer via raw loads" emulation exists.
- The game's named budgets are NOT derived from the DXGI video-memory figures (same table with a 4 GB
  cap). The in-game LOD/crowd/hair settings do not shrink the pool need below 1 GB.

### What a full fix needs (pick one)
1. Steer the typed-read data into the first 1 GB of a 2046 MB pool: the engine's D3DBufferManager
   allocates per request with a DXGI_FORMAT and a name (AllocateBuffer(D3DHeapAlloc*, DXGI_FORMAT,
   size, data, name, DataCopy, MemoryLocation)); AllocSmall (0x142c59720) takes a request struct
   {mgr, preferred subheap ptr, size ptr, align ptr}: < 64 KB -> small region at mgr+0x540 (when
   [mgr+0xbb9]), else mgr->vtbl[+0x30] (carve from the pool), else the subheap list [mgr+0xb78]/
   [mgr+0xb80]. Subheaps are created by 0x142c5a0d0 -> 0x142c5b3b0(mgr, subheap, size, flags)
   (failure logs "D3D Managed Buffer was unable to allocate %d bytes for SubHeap"). If 4-byte-typed
   allocations use their own subheap class, pre-creating that class's subheaps at init (low offsets)
   would pin them under 1 GB. Use the shim's logpoints (LAYOVER_LOGPOINTS=rva,rva; LAYOVER_LOGPOINTS_MAX)
   to log rcx/rdx/r8/r9 and the 5th stack arg as a string at those entries first.
2. Move the typed window instead of the data: patch D3DMetal's GetView so an oversized view's texture
   starts at (size - 1 GB) and the descriptor's textureViewOffsetInElements wraps negative (the
   converter adds it to the index). Covers [1 GB, 2 GB) only; content streamed into holes below 1 GB
   would T-pose again, so it needs (1) anyway.
3. Ask Apple: D3DMetal needs typed buffer views above 2^28 elements emulated through raw loads.

### Tooling built this session (scratchpad tools/, disposable; the shim source is in the repo)
- tools/run.py: direct launch of any exe in the prefix with D3DMetal + the shim; --env K=V; it copies
  the running wineserver's WINEMSYNC (Steam was started with msync off at 13:53, so a mismatched
  launch fails with "msync_init Failed to open msync shared memory").
- The game shows a LAUNCHER window first (class GameNxApp, 792x447, idle in NtUserWaitMessage until
  Play). tools/sendkey.exe presses keys via SendInput inside Wine (needs the window foreground;
  `--focus GameNxApp` + SetForegroundWindow did not find the fullscreen window); Enter = Play.
  A stray Enter at the main menu once hit "Quit".
- tools/winlist.exe lists visible Wine windows (class/title/children) - the way to see dialogs.
- `screencapture` captures the wrong Space when the game is fullscreen; `osascript` key events are
  not authorized for this host app. The user sits at the machine and reports by eye.
- Minidumps (~/Documents/Marvel's Spider-Man 2/*.mdmp) parse with a 40-line python (streams 4/5/6)
  for registers + return addresses; objdump -d --start-address on the 194 MB exe takes ~30 s.
- shims/vramcap (dxgi wrapper) DOES reach the game but the game then crashes at Play inside Wine's
  RtlVirtualUnwind2 (ntdll+0x38314) whenever it is loaded. Superseded: the d3d12 shim's
  LAYOVER_VRAM_MB patches D3DMetal's shared IDXGIAdapter vtable in place at DLL load (the game resolves
  CreateDXGIFactory dynamically, so IAT hooks miss) and works.
- Logging mode of the shim (LAYOVER_D3D12_LOG=<win path>, LAYOVER_D3D12_VERBOSE=1): feature
  queries (hex), pools, views >= 2^24 elements or RGB32, pipelines (failures), command signatures,
  ExecuteIndirect, 5-second counters. Findings from it: no PSO failures, no mesh shaders, no
  stream PSOs; the RGB32 typed SRV is an 8 MB buffer (699,050 x float3), unrelated; command
  signatures are plain DRAW/DRAW_INDEXED/DISPATCH; timestamp heap 1 M entries, frequency 60.

## Useful references

- Highball (MIT): github.com/gauthierpiarrette/highball — `spike/engines/*.json` manifests,
  `Sources/HighballKit/Bottle.swift` (env construction), `SteamRestart.swift`, `SteamLaunchNotice.swift`.
- notpop/steam-on-m1-wine (MIT): the web-helper wrapper source and Steam-on-Wine notes.
- DXMT: github.com/3Shain/dxmt (wiki "Installation Guide for Geeks"), dxmt.report for per-game issues.
- Sikarugir engines: github.com/Sikarugir-App/Engines releases; runtime: github.com/Sikarugir-App/Wrapper.
