#!/bin/bash
# Builds Microsoft's DirectX Shader Compiler tools (dxc, dxa, dxv) for this Mac into
# ~/Library/Application Support/Layover/tools/dxc. `layover sm2-shaders` uses them to rewrite four of
# Spider-Man 2's shaders (typed pool access -> raw; see shims/d3d12shim/pool_fix.c and HANDOFF.md).
# Pinned to the DXC commit macprotips/SpiderMan2Fix verified its shaders with, so the results can be
# checked against their hashes. Needs the Xcode command line tools; installs cmake and ninja with brew.
#   tools/dev/build-dxc.sh            (30-60 minutes the first time; about 6 GB of build files)
set -euo pipefail
ROOT="$HOME/Library/Application Support/Layover"
DXC_COMMIT=6e0fab36dc9ac8c423e1b08fed3aeb0e739d9e54
OUT="$ROOT/tools/dxc"
WORK="${DXC_WORK:-$ROOT/tools/dxc-build}"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
mkdir -p "$ROOT/logs"
exec > >(tee -a "$ROOT/logs/build-dxc.log") 2>&1   # everything also lands in logs/build-dxc.log for diagnosis
echo "=== build-dxc $(date) ==="

for t in cmake ninja; do command -v "$t" >/dev/null || brew install "$t"; done
mkdir -p "$WORK"
if [[ ! -d "$WORK/src/.git" ]]; then
    git init -q "$WORK/src"
    git -C "$WORK/src" remote add origin https://github.com/microsoft/DirectXShaderCompiler.git
fi
git -C "$WORK/src" fetch -q --depth 1 origin "$DXC_COMMIT"
git -C "$WORK/src" checkout -q FETCH_HEAD
git -C "$WORK/src" submodule update -q --init --depth 1 --recursive

cmake -S "$WORK/src" -B "$WORK/build" -G Ninja -C "$WORK/src/cmake/caches/PredefinedParams.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DSPIRV_WERROR=OFF -DCLANG_FORMAT_EXE=OFF -DHLSL_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF -DENABLE_SPIRV_CODEGEN=OFF -DSPIRV_BUILD_TESTS=OFF
ninja -C "$WORK/build" dxc dxa dxv

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/licenses"
for tool in dxc dxa dxv; do
    cp "$(cd "$WORK/build/bin" && pwd -P)/$(readlink "$WORK/build/bin/$tool" || echo "$tool")" "$OUT/bin/$tool"
done
cp "$WORK/build/lib/libdxcompiler.dylib" "$OUT/lib/"
[[ -f "$WORK/build/lib/libdxil.dylib" ]] && cp "$WORK/build/lib/libdxil.dylib" "$OUT/lib/"
cp "$WORK/src/LICENSE.TXT" "$OUT/licenses/DXC-LICENSE.txt"
cp "$WORK/src/ThirdPartyNotices.txt" "$OUT/licenses/DXC-ThirdPartyNotices.txt"
echo "$DXC_COMMIT" > "$OUT/COMMIT"
"$OUT/bin/dxc" --version
du -sh "$OUT"
echo
echo "DXC tools built. Next: ./layover sm2-shaders   (then play; the shim log shows 'pool fix: ON')"
echo "The build folder $WORK can be deleted afterwards."
