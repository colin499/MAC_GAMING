# Rebuild the AGS replacement with Homebrew's mingw-w64 (brew install mingw-w64).
# The DLL is marked as a Wine builtin so WINEDLLOVERRIDES "amd_ags_x64=b" picks it over the
# copy a game ships next to its executable.
CXX ?= /opt/homebrew/bin/x86_64-w64-mingw32-g++
all: amd_ags_x64.dll
amd_ags_x64.dll: amd_ags_x64.cpp amd_ags.h
	$(CXX) -O2 -std=c++14 -shared -static -static-libgcc -static-libstdc++ -o $@ amd_ags_x64.cpp -ldxgi -luser32
	python3 -c "p='$@'; b=bytearray(open(p,'rb').read()); b[0x40:0x51]=b'Wine builtin DLL\0'; open(p,'wb').write(b)"
clean:
	rm -f amd_ags_x64.dll
