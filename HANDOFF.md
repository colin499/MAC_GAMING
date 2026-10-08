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
- The user owns Marvel's Spider-Man 2 (AppID 2651280, DirectX-12-only, ~97 GB). It was still downloading
  (~81/97 GB) when this session ended. **Nobody has run a game yet.** That is the next milestone.
- Two older engine folders may still exist under `engines/` (`wine-staging-11.18`, `cx26.3`, ~2.3 GB);
  `layover setup` deletes them when Steam is not running.

## Next steps

1. When the download finishes: `./layover play "spider"` (auto-detects DX12 → D3DMetal, restarts Steam
   with that renderer). First launch compiles shaders; expect a dark screen for 1–2 min.
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

## Useful references

- Highball (MIT): github.com/gauthierpiarrette/highball — `spike/engines/*.json` manifests,
  `Sources/HighballKit/Bottle.swift` (env construction), `SteamRestart.swift`, `SteamLaunchNotice.swift`.
- notpop/steam-on-m1-wine (MIT): the web-helper wrapper source and Steam-on-Wine notes.
- DXMT: github.com/3Shain/dxmt (wiki "Installation Guide for Geeks"), dxmt.report for per-game issues.
- Sikarugir engines: github.com/Sikarugir-App/Engines releases; runtime: github.com/Sikarugir-App/Wrapper.
