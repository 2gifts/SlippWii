"""Checks the loader's C VCDIFF decoder (Nintendont/loader/source/vcdiff.c)
against tools/vcdiff.py: every Slippi menu patch, applied to the disc's file,
must give the same bytes, and its CRC-32 must be zlib's. Damaged patches and
a wrong disc file must be refused; a same-size wrong file decodes (the
patches have no checksums), so the loader's manifest CRC-32 must differ.

    python tools/test_vcdiff_c.py --iso Melee.iso [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import slippi_files  # noqa: E402
import vcdiff  # noqa: E402

MAIN = r'''
#include <stdio.h>
#include <stdlib.h>
#include "vcdiff.h"
static unsigned char *load(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
	b = malloc(*n ? *n : 1);
	if (fread(b, 1, *n, f) != *n) { fclose(f); return NULL; }
	fclose(f);
	return b;
}
int main(int argc, char **argv)
{
	size_t sn, dn;
	unsigned char *s = load(argv[1], &sn), *d = load(argv[2], &dn), *o;
	const char *err = "?";
	long n;
	FILE *f;
	if (!s || !d) return 3;
	n = vcdiff_target_size(d, dn, &err);
	if (n < 0) { printf("error: %s\n", err); return 1; }
	o = malloc(n ? n : 1);
	if (vcdiff_decode(s, sn, d, dn, o, (size_t)n, &err) != 0) { printf("error: %s\n", err); return 1; }
	printf("%08lx\n", vcdiff_crc32(vcdiff_crc32(0, o, n / 2), o + n / 2, n - n / 2));
	f = fopen(argv[3], "wb"); fwrite(o, 1, n, f); fclose(f);
	return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--iso', required=True)
    ap.add_argument('--cc', default='gcc')
    args = ap.parse_args()
    disc = slippi_files.open_disc(args.iso)
    failures = 0

    def check(ok, text):
        nonlocal failures
        print(f'{"ok  " if ok else "FAIL"} {text}')
        failures += not ok

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'main.c').write_text(MAIN)
        exe = tmp / 'vcdiff_test.exe'
        src = ROOT / 'Nintendont/loader/source'
        subprocess.run([args.cc, '-O2', '-Wall', '-Werror', '-I', str(src), str(tmp / 'main.c'), str(src / 'vcdiff.c'),
                        '-o', str(exe)], check=True)

        def run(source, delta):
            """(None, output bytes, its C CRC-32) or (error text, None, None)"""
            (tmp / 's').write_bytes(source)
            (tmp / 'd').write_bytes(delta)
            res = subprocess.run([str(exe), str(tmp / 's'), str(tmp / 'd'), str(tmp / 'o')], capture_output=True, text=True)
            if res.returncode:
                return res.stdout.strip() or f'exit {res.returncode}', None, None
            return None, (tmp / 'o').read_bytes(), int(res.stdout.strip(), 16)

        for name, sha in slippi_files.GAME_FILES.items():
            if not name.endswith('.diff'):
                continue
            delta = slippi_files.fetch(f'GameFiles/GALE01/{name}', sha, slippi_files.CACHE / 'GameFiles' / name)
            source = disc.file(name[:-5])
            want = vcdiff.decode(source, delta)
            err, got, crc = run(source, delta)
            check(err is None and got == want and crc == zlib.crc32(want),
                  f'{name}: {len(want)} bytes, CRC-32 {crc if crc is None else f"{crc:08x}"}' + (f' ({err})' if err else ''))
            if name != 'MnMaAll.usd.diff':
                continue
            err, got, crc = run(bytes(x ^ 0x5A for x in source), delta)
            check(err is None and crc != zlib.crc32(want), 'a same-size wrong file: a different CRC-32 (the manifest catches it)')
            for label, s, d in (('another disc file', disc.file('MnMaAll.dat'), delta),
                                ('a cut patch', source, delta[:len(delta) // 2]),
                                ('not a patch', source, b'hello world')):
                err, _, _ = run(s, d)
                check(err is not None, f'refuses {label}: {err or "(accepted!)"}')
    print('PASS' if not failures else f'{failures} FAILED')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
