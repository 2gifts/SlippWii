"""Runs the loader's settings file code (LoadNinCFG, UpdateNinCFG, SaveNinCFG in
Nintendont/loader/source/global.c) on a PC, against a real file: what the menu saves
must come back on the next start, and an old or damaged file must not.

    python tools/test_loader_settings.py [--cc "python -m ziglang cc"]
"""
import argparse
import re
import shlex
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GLOBAL = ROOT / 'Nintendont/loader/source/global.c'

PRELUDE = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "CommonConfig.h"

typedef unsigned int UINT;
typedef int FRESULT;
typedef FILE *FIL;
#define FR_OK 0
#define FA_READ 1
#define FA_WRITE 2
#define FA_OPEN_EXISTING 4
#define FA_OPEN_ALWAYS 8
#define gprintf(...) ((void)0)

static NIN_CFG cfg_memory;
NIN_CFG *ncfg = &cfg_memory;
static int flushes;

/* The game's drive is the boot drive in these tests: both saves go to one file. */
static const char *GetRootDevice(void) { return "sd"; }
static void FlushDevices(void) { flushes++; }
static const char *local(const char *path)
{
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}
static int f_open_char(FIL *f, const char *path, int mode)
{
	const char *name = local(path);
	if (mode & FA_OPEN_EXISTING) {
		*f = fopen(name, "rb");
	} else {
		*f = fopen(name, "r+b");
		if (!*f)
			*f = fopen(name, "w+b");
	}
	return *f ? FR_OK : 4;
}
static int f_read(FIL *f, void *buf, UINT n, UINT *got) { *got = (UINT)fread(buf, 1, n, *f); return FR_OK; }
static int f_write(FIL *f, const void *buf, UINT n, UINT *done) { *done = (UINT)fwrite(buf, 1, n, *f); return FR_OK; }
static int f_close(FIL *f) { fclose(*f); return FR_OK; }
static long f_size(FIL *f)
{
	long here = ftell(*f), size;
	fseek(*f, 0, SEEK_END);
	size = ftell(*f);
	fseek(*f, here, SEEK_SET);
	return size;
}
static int f_expand(FIL *f, long size, int opt) { (void)f; (void)size; (void)opt; return FR_OK; }
'''

MAIN = r'''
void UpdateNinCFG();
bool LoadNinCFG(void);
void SaveNinCFG(void);

int main(int argc, char **argv)
{
	FILE *f;
	if (!strcmp(argv[1], "save")) {	/* what the menu does: settings changed, then saved */
		memset(ncfg, 0, sizeof(NIN_CFG));
		ncfg->Magicbytes = 0x01070CF6;
		ncfg->Version = NIN_CFG_VERSION;
		ncfg->Config = NIN_CFG_NETWORK | NIN_CFG_SLIPPI_REPLAYS | NIN_CFG_FORCE_WIDE;
		ncfg->MeleeCodeOptions[0] = 2;
		ncfg->MeleeCodeOptions[MELEE_CODES_LAG_REDUCTION_OPTION_ID] = 1;	/* off, chosen */
		ncfg->ReplaysLED = 1;
		strcpy(ncfg->GamePath, "/games/SSBM/game.iso");
		SaveNinCFG();
		return flushes == 1 ? 0 : 1;
	}
	/* load: prints what a start would use */
	(void)argc;
	memset(ncfg, 0x55, sizeof(NIN_CFG));
	printf("%d %x %x %u %u %s %u %u\n", LoadNinCFG(), ncfg->Version, ncfg->Config, ncfg->MeleeCodeOptions[0],
		ncfg->ReplaysLED, ncfg->GamePath, (unsigned)sizeof(NIN_CFG),
		ncfg->MeleeCodeOptions[MELEE_CODES_LAG_REDUCTION_OPTION_ID]);
	(void)f;
	return 0;
}
'''


def function(source, signature):
    """One function's text, up to the closing brace in the first column."""
    m = re.search(r'^' + re.escape(signature) + r'\s*\n\{.*?^\}', source, re.M | re.S)
    if not m:
        sys.exit(f'{GLOBAL.name}: no {signature}')
    return m.group(0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--cc', default='python -m ziglang cc -target x86_64-windows-gnu')
    args = ap.parse_args()
    source = GLOBAL.read_text().replace('\r\n', '\n')
    code = PRELUDE + '\n'.join(function(source, s) for s in ('void UpdateNinCFG()', 'bool LoadNinCFG(void)',
                                                             'void SaveNinCFG(void)')) + MAIN
    failures = 0

    def check(name, ok, detail=''):
        nonlocal failures
        print(('ok    ' if ok else 'FAIL  ') + name + (f': {detail}' if detail and not ok else ''))
        failures += not ok

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'test.c').write_text(code)
        exe = tmp / 'settings_test.exe'
        subprocess.run(shlex.split(args.cc) + ['-O1', '-w', '-I', str(ROOT / 'Nintendont/common/include'),
                                               str(tmp / 'test.c'), '-o', str(exe)], check=True)
        cfg = tmp / 'slippwii_nincfg.bin'

        def load():
            out = subprocess.run([str(exe), 'load'], cwd=tmp, capture_output=True, text=True, check=True).stdout.split()
            return {'loaded': out[0] == '1', 'version': int(out[1], 16), 'config': int(out[2], 16),
                    'ucf': int(out[3]), 'led': int(out[4]), 'path': out[5], 'size': int(out[6]), 'lag': int(out[7])}

        check('no file: not loaded', not (subprocess.run([str(exe), 'load'], cwd=tmp, capture_output=True,
                                                         text=True).stdout.startswith('1')))
        subprocess.run([str(exe), 'save'], cwd=tmp, check=True)
        good = cfg.read_bytes()
        got = load()
        check('saved settings come back', got['loaded'], str(got))
        check('  replays and the other switches', got['config'] & (1 << 14) and got['config'] & (1 << 13)
              and got['config'] & (1 << 4), str(got))
        check('  options, LED and the game', got['ucf'] == 2 and got['led'] == 1 and
              got['path'] == '/games/SSBM/game.iso', str(got))
        check('the file is the whole struct', len(good) == got['size'], f'{len(good)} of {got["size"]}')
        check('the polling drift fix turned off stays off', got['lag'] == 1, str(got))

        subprocess.run([str(exe), 'save'], cwd=tmp, check=True)
        check('saving again over it', load()['loaded'] and cfg.read_bytes() == good)

        cfg.write_bytes(good[:100])
        check('a cut-off file is refused', not load()['loaded'])
        cfg.write_bytes(b'\0\0\0\0' + good[4:])
        check('a damaged file is refused', not load()['loaded'])
        cfg.write_bytes(good[:4] + struct.pack('<I', 0x7F) + good[8:])
        check('an unknown version is refused', not load()['loaded'])

        # The previous version: the same size, UCF 0.84's old values mapped to the new ones.
        old = bytearray(good)
        old[4:8] = struct.pack('<I', 0xD)
        old[288:292] = struct.pack('<I', 4)
        cfg.write_bytes(old)
        got = load()
        check('version 0xD is brought up to date', got['loaded'] and got['version'] == 0xF and got['ucf'] == 2, str(got))

        # The last version: its Off was the old default, its other choices stay.
        lag = 288 + 3 * 4
        for before, after, name in ((1, 2, 'off'), (0, 2, 'unset'), (2, 2, 'on'), (3, 3, 'PDF + 1/2F')):
            old = bytearray(good)
            old[4:8] = struct.pack('<I', 0xE)
            old[lag:lag + 4] = struct.pack('<I', before)
            cfg.write_bytes(old)
            got = load()
            check(f'version 0xE, polling drift fix {name}: now {after}', got['loaded'] and got['version'] == 0xF
                  and got['lag'] == after, str(got))
    print('RESULT:', 'all passed' if not failures else f'{failures} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
