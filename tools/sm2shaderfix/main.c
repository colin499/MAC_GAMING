/* sm2shaderfix: the native half of `layover sm2-shaders`. Rewrites four of Spider-Man 2's compute
 * shaders so their typed access to the game's 2 GB mesh pool becomes raw access (D3DMetal on M1-M4
 * drops typed buffer reads and writes past element 2^28; see shims/d3d12shim/pool_fix.c).
 * The rewrite itself (shaderfix.c) is vendored from macprotips/SpiderMan2Fix (public domain).
 * Microsoft's dxc/dxa/dxv do the disassembly, reassembly and signing; `layover` drives them.
 *
 *   sm2shaderfix hash <file>                         FNV-1 hash of a file (the shader hash scheme)
 *   sm2shaderfix extract <exe> <hash> <out.dxil>     copy the DXBC container with that hash out of the exe
 *   sm2shaderfix rewrite <in.ll> <out.ll> <space> <reg> srv|uav
 *                                                    typed access at (space, reg) -> raw, locals named
 * Exit status 0 on success; a message on stderr otherwise. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shaderfix.h"

static void *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb"); long n; void *d = NULL;
    *size = 0;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t)n + 1))) {
        if (fread(d, 1, (size_t)n, f) == (size_t)n) { *size = (size_t)n; ((char *)d)[n] = 0; } else { free(d); d = NULL; }
    }
    fclose(f);
    if (!d) fprintf(stderr, "cannot read %s\n", path);
    return d;
}

static int spit(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, size, f) != size || fclose(f)) { fprintf(stderr, "cannot write %s\n", path); return 0; }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "hash")) {
        size_t n; void *d = slurp(argv[2], &n);
        if (!d) return 1;
        printf("%016llx\n", (unsigned long long)sf_shader_hash(d, n));
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "extract")) {
        size_t n, off, sz; void *d = slurp(argv[2], &n); unsigned long long hash = strtoull(argv[3], NULL, 16);
        if (!d) return 1;
        if (!sf_find_container((const uint8_t *)d, n, hash, &off, &sz)) { fprintf(stderr, "no DXBC container with hash %016llx in %s (game build changed?)\n", hash, argv[2]); return 2; }
        if (!spit(argv[4], (const uint8_t *)d + off, sz)) return 1;
        printf("%zu bytes at offset %zu\n", sz, off);
        return 0;
    }
    if (argc >= 7 && !strcmp(argv[1], "rewrite")) {
        size_t n; char *ir = slurp(argv[2], &n), *raw, *named, err[256] = ""; int handles = 0, loads = 0, stores = 0, uav = !strcmp(argv[6], "uav");
        if (!ir) return 1;
        raw = uav ? sf_typed_to_raw_uav(ir, atoi(argv[4]), atoi(argv[5]), &handles, &loads, &stores, err, sizeof err)
                  : sf_typed_to_raw(ir, atoi(argv[4]), atoi(argv[5]), &handles, &loads, err, sizeof err);
        if (!raw) { fprintf(stderr, "rewrite failed: %s\n", err); return 3; }
        named = sf_name_values(raw);
        if (!named) { fprintf(stderr, "out of memory\n"); return 1; }
        if (!spit(argv[3], named, strlen(named))) return 1;
        printf("%d handle%s, %d load%s, %d store%s rewritten\n", handles, handles == 1 ? "" : "s", loads, loads == 1 ? "" : "s", stores, stores == 1 ? "" : "s");
        return 0;
    }
    fprintf(stderr, "usage: sm2shaderfix hash <file> | extract <exe> <hash> <out> | rewrite <in.ll> <out.ll> <space> <reg> srv|uav\n");
    return 64;
}
