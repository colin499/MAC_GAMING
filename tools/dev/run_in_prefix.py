"""Run a Windows exe in the Layover Steam prefix with the D3DMetal renderer plus the d3dlog shim.
usage: run.py LOGFILE EXE [args...] [--cwd DIR] [--env K=V ...] [--no-shim]"""
import importlib.machinery, importlib.util, os, sys, subprocess
loader = importlib.machinery.SourceFileLoader("layover", "/Users/colinlysik/Documents/BRAIN_FOOD/MAC_GAMING/layover")
spec = importlib.util.spec_from_loader("layover", loader); L = importlib.util.module_from_spec(spec); loader.exec_module(L)
cfg = L.load_config()
args = sys.argv[1:]
log = args.pop(0)
extra = {}; cwd = None; shim = True; exe_args = []
while args:
    a = args.pop(0)
    if a == "--cwd": cwd = args.pop(0)
    elif a == "--env": k, v = args.pop(0).split("=", 1); extra[k] = v
    elif a == "--no-shim": shim = False
    else: exe_args.append(a)
env = L.wine_env(cfg, game="2651280", extra={"LAYOVER_RENDERER": "d3dmetal"})
env["WINEDEBUG"] = extra.pop("WINEDEBUG", "err+all")
if shim:
    overlay = os.path.join(L.ENGINE_RENDERERS, "d3d12shim", "wine")
    env["WINEDLLPATH_PREPEND"] = overlay + ":" + env["WINEDLLPATH_PREPEND"]
    env["WINEDLLOVERRIDES"] += ";d3d12=b;d3d12_d3dmetal=b"
    env.setdefault("LAYOVER_D3D12_LOG", "Z:" + log.replace("/", "\\") + ".d3d")
if extra.pop("VRAMCAP", None):
    overlay = os.path.join(L.ENGINE_RENDERERS, "vramcap", "wine")
    env["WINEDLLPATH_PREPEND"] = overlay + ":" + env["WINEDLLPATH_PREPEND"]
    env["WINEDLLOVERRIDES"] += ";dxgi=b;dxgi_d3dm=b"
    env.setdefault("VRAMCAP_LOG", "Z:" + log.replace("/", "\\") + ".vram")
# match the msync mode of the wineserver that is already running for Steam
ws = subprocess.run(["bash", "-c", "ps eww -p $(pgrep -f 'engine/lib/wine/../../bin/wineserver' | head -1) 2>/dev/null | tr ' ' '\\n' | grep '^WINEMSYNC='"], capture_output=True, text=True).stdout.strip()
if ws: env["WINEMSYNC"] = ws.split("=", 1)[1]
env.update(extra)
out = open(log, "ab")
p = subprocess.Popen([L.WINE] + exe_args, env=env, stdout=out, stderr=subprocess.STDOUT, cwd=cwd, start_new_session=True)
print("pid", p.pid)
if os.environ.get("RUN_WAIT"): p.wait(); print("rc", p.returncode)
