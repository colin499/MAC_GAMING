# Handoff notes (2026-10-08, end of first session)

Read this first if you are a new agent (or future me) picking up Layover.

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

## Useful references

- Highball (MIT): github.com/gauthierpiarrette/highball — `spike/engines/*.json` manifests,
  `Sources/HighballKit/Bottle.swift` (env construction), `SteamRestart.swift`, `SteamLaunchNotice.swift`.
- notpop/steam-on-m1-wine (MIT): the web-helper wrapper source and Steam-on-Wine notes.
- DXMT: github.com/3Shain/dxmt (wiki "Installation Guide for Geeks"), dxmt.report for per-game issues.
- Sikarugir engines: github.com/Sikarugir-App/Engines releases; runtime: github.com/Sikarugir-App/Wrapper.
