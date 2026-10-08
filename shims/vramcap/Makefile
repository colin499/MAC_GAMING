# dxgi.dll wrapper that caps the video memory D3DMetal's DXGI reports (see dxgi.cpp).
# Marked as a Wine builtin so it can live in a WINEDLLPATH_PREPEND overlay ahead of D3DMetal's.
CXX ?= x86_64-w64-mingw32-g++
all: dxgi.dll
dxgi.dll: dxgi.cpp
	$(CXX) -O2 -std=c++14 -shared -static -static-libgcc -static-libstdc++ -o $@ dxgi.cpp -ldxguid
	python3 -c "p='$@'; b=bytearray(open(p,'rb').read()); b[0x40:0x51]=b'Wine builtin DLL\0'; open(p,'wb').write(b)"
clean:
	rm -f dxgi.dll
