# ✈ Layover

Play Windows Steam games on your Apple Silicon MacBook, including offline on a plane,
with an Xbox controller. Our own launcher, built from free and open components.
No CrossOver, no subscriptions, nothing needs an admin password.

## What it is made of

| Piece | What it does | Source |
|---|---|---|
| Wine 10.0 (Sikarugir build by Gcenx) + Highball's msync, focus and audio fixes | Runs x86_64 Windows programs on macOS via Rosetta 2 (LGPL) | github.com/Sikarugir-App/Engines, github.com/gauthierpiarrette/highball-engine |
| DXMT (Highball fork) | Translates Direct3D 10/11 to Metal; what most games use | github.com/3Shain/dxmt |
| D3DMetal 3.0 | Apple's Direct3D 11/12 → Metal, from the Game Porting Toolkit; needed for DirectX-12-only games. Non-commercial license. | shipped in the Sikarugir runtime bundle |
| Runtime bundle | GStreamer (video), MoltenVK, gnutls and friends | github.com/Sikarugir-App/Wrapper |
| Windows Steam client | Downloaded from Valve and installed into the Wine prefix | steampowered.com |
| AMD AGS replacement (`shims/amd_ags/`) | 100 KB stand-in for AMD's `amd_ags_x64.dll`. D3DMetal poses as an AMD GPU, so games that bundle AGS (Spider-Man 2, other Nixxes ports) call it; the real one needs AMD's driver and fails, which the game reports as "No installed graphics card". Installed into the D3DMetal overlay and forced with `WINEDLLOVERRIDES amd_ags_x64=b`. | Layover's own, against AMD's MIT header |
| Web-helper wrapper (`wrapper/`, optional) | 150 KB shim that starts Steam's Chromium UI in single-process mode; needed on plain WineHQ builds, off by default on this engine (`layover config webhelper_wrapper on`) | from notpop/steam-on-m1-wine (MIT) |

Everything is installed under `~/Library/Application Support/Layover`.
Delete that folder (or run `layover uninstall`) and it is gone.

## Quick start

```bash
cd ~/Documents/BRAIN_FOOD/MAC_GAMING
./layover setup        # one time, needs Wi-Fi: ~440 MB of downloads, about two minutes to install
./layover steam        # opens Windows Steam; first launch self-updates (1–3 min) then shows sign-in
```

**No Terminal needed after setup:** `./layover app` (setup does this too) puts **Layover.app** in
`~/Applications`. Double-click it, or Cmd-Space → "Layover": a dialog lists your installed Windows
games; pick one and it launches, starting Steam first if needed. Drag it to the Dock to keep it there.
`Layover.command` still opens the text menu in Terminal.

Sign in to Steam, install games as usual (they download into the Wine prefix),
then:

```bash
./layover games                 # what is installed (Windows prefix + native Mac Steam)
./layover play "elden ring"     # launch by name or AppID
./layover install 1245620       # start a download by AppID
```

To run `layover` from anywhere: `ln -s ~/Documents/BRAIN_FOOD/MAC_GAMING/layover /opt/homebrew/bin/layover`

## Before a flight

Steam's offline mode only works for games that have already been launched once
online (that caches the license, config and shaders). So, while you still have Wi-Fi:

```bash
./layover flight               # checklist: login remembered, games ready, controller, battery, disk
./layover flight --go          # switches Steam to offline mode and launches it
```

`flight --go` closes Steam if it is open, flips the saved *WantsOfflineMode* flag for
both Windows Steam and native Mac Steam, and starts Windows Steam offline.
After landing: `./layover flight --online` (or Steam menu → Go Online).

## Xbox controller

```bash
./layover controller           # detect; prints pairing steps and opens Bluetooth settings
./layover controller --test    # live button/stick test (uses Apple's GameController framework)
```

* **USB-C cable**: just plug it in. macOS 15 has a native driver; approve the prompt
  the first time. Charges the pad too.
* **Bluetooth**: hold the Pair button until the Xbox light flashes fast, then
  System Settings → Bluetooth → Connect. Bluetooth is allowed on planes.
* Inside Windows Steam: Settings → Controller → enable Xbox controller support.
  Steam Input then handles mapping for every game.

## Graphics backends

```bash
./layover renderer                        # show the choices and per-game settings
./layover renderer default auto           # auto | dxmt | d3dmetal | wined3d
./layover config d3dmetal_identity apple  # apple (default) | amd: what D3DMetal calls the GPU
./layover renderer "cyberpunk" wined3d    # force one game (Steam restarts if needed)
```

`auto` (the default) looks at each installed game's executable: DirectX 12 games get
D3DMetal, everything else gets DXMT. When Steam is launched without naming a game, it runs
with D3DMetal if any installed game needs it, so launching from Steam's own UI works for
all of them. The renderer is chosen when Steam starts; `layover play` restarts Steam
automatically if a game needs a different one. If a game shows nothing or crashes on
launch, try the other Metal renderer first, then `wined3d`.

Under D3DMetal the GPU is reported as the adapter Wine registered in the prefix (vendor 0x106B,
"Apple M4 Pro") rather than D3DMetal's stock "AMD Compatibility Mode". Spider-Man 2 cross-checks
the DXGI adapter against the registry's display-adapter driver entry and otherwise stops with
"No installed graphics card has been detected"; the Apple identity makes that lookup succeed.
`./layover config d3dmetal_identity amd` restores the AMD identity for games that insist on a
known desktop vendor (Layover's AGS replacement then stands in for AMD's driver library).

Other settings (`./layover config`): `msync` (faster sync, on), `metal_hud` (FPS overlay), `fps_cap`
(cap frame rate, e.g. 60, for cooler and quieter play on battery),
`advertise_avx` (some engines refuse to start without it, on), `retina` (high-res mode),
`steam_args` (extra flags for steam.exe).

## Honest limitations

* **Anti-cheat multiplayer games** (EAC, BattlEye, Vanguard) will not run. Nothing on macOS fixes this.
* **Performance** is below a Windows PC. DX11 games through DXMT are generally good on an M4 Pro;
  DX12 games vary a lot.
* **Steam's UI** runs with GPU rendering off (`-cef-disable-gpu`), so store pages are a bit
  slower than on Windows; games are unaffected. If the Steam window ever comes up black, turn
  on the single-process wrapper with `./layover config webhelper_wrapper on` (then Steam's
  self-verification is skipped so the wrapper survives; use `./layover update-steam` for updates).
* Apple has said Rosetta 2 goes away after macOS 27. Everything here needs Rosetta.
  (That is also true of CrossOver, Whisky and every other Wine-based option.)

## Troubleshooting

```bash
./layover doctor      # health check
./layover logs        # last launch log; Steam's own logs are in the prefix under Steam/logs
./layover kill        # stop Steam and everything in the Windows prefix
./layover update-steam  # let the Steam client update itself, then re-install the wrapper
./layover winecfg     # Wine's settings window (drives, audio, Windows version)
./layover setup --reinstall
```

Compatibility lookups: codeweavers.com/compatibility (same Wine code), applegamingwiki.com,
and dxmt.report for DXMT-specific game notes.

## Layout

```
layover            the program (Python 3, no dependencies beyond macOS)
wrapper/           steamwebhelper wrapper: prebuilt .exe plus C source (rebuilds with brew's mingw-w64)
shims/amd_ags/     AMD AGS replacement DLL: prebuilt .dll plus C++ source and AMD's header (make rebuilds it)
Layover.command    double-click launcher for the menu
~/Library/Application Support/Layover/
  engines/sikarugir10.0_6/  engine/ (Wine), frameworks/ (runtime), renderers/ (dxmt, d3dmetal)
  prefixes/steam/  the Windows environment; games live in drive_c/Program Files (x86)/Steam/steamapps
  downloads/       cached archives (checksums pinned in `layover`)
  logs/            launch logs (last 20 kept)
  config.json      your settings
```
