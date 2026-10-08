# d3d12.dll logging wrapper in front of D3DMetal's (see d3d12.c). Marked as a Wine builtin so a
# WINEDLLPATH_PREPEND overlay dir can hold it ahead of D3DMetal's own d3d12.dll.
CC = x86_64-w64-mingw32-gcc
all: d3d12.dll
d3d12.dll: d3d12.c sm2_skin.c d3d12.def
	$(CC) -shared -O2 -Wall -Wno-unused-function -static -static-libgcc -o $@ d3d12.c sm2_skin.c d3d12.def -ldxguid
	python3 -c "p='$@'; b=bytearray(open(p,'rb').read()); b[0x40:0x51]=b'Wine builtin DLL\0'; open(p,'wb').write(b)"
clean:
	rm -f d3d12.dll
