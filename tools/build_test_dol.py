"""Builds a Wii test DOL that logs to this PC, then puts the repo's files back.

    python tools/build_test_dol.py NAME [build_online_boot.py options, e.g. --rollback]

Writes build/test/loader-NAME.dol: the kernel config's log_host is this PC
(LOG_HOST: this PC's address and port, e.g. 192.168.1.20:51450) with any extra config.ini lines in
TEST_CONFIG ("key=value;key=value", e.g. "rollback=1;test_lag_frames=2"), and
the online boot codes are built with the given options. Boot it and record the log with:

    python tools/online_selftest.py watch --boot --wii-ip WII_IP --dol build/test/loader-NAME.dol
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NIN = ROOT / 'Nintendont'
INC = NIN / 'kernel/slippi_online/so_test_config.inc'
BOOT = NIN / 'kernel/gecko/g_online_boot.bin'
DOL = NIN / 'loader/loader.dol'
DIST = ROOT / 'dist/sd/apps/SlippWii/boot.dol'


def main():
    if len(sys.argv) < 2 or sys.argv[1].startswith('-'):
        sys.exit(__doc__)
    name, options = sys.argv[1], sys.argv[2:]
    host = os.environ.get('LOG_HOST') or sys.exit('set LOG_HOST to this PC address:port (e.g. 192.168.1.20:51450)')
    out = ROOT / f'build/test/loader-{name}.dol'
    saved = {p: p.read_bytes() for p in (INC, BOOT, DOL, DIST) if p.exists()}
    try:
        extra = ''.join(f'{kv.strip()}\\n' for kv in os.environ.get('TEST_CONFIG', '').split(';') if kv.strip())
        INC.write_text(f'/* TEST BUILD ({name}): the Wii log goes to this PC */\n'
                       f'"log_host={host}\\nstatus_beacon=1\\n{extra}"\n')
        print(INC.read_text().strip())
        subprocess.run([sys.executable, str(ROOT / 'tools/build_online_boot.py'), *options], check=True, cwd=ROOT)
        r = subprocess.run(['sh', 'tools/build.sh'], capture_output=True, text=True, cwd=ROOT)
        if r.returncode:
            sys.exit(f'build failed:\n{r.stdout[-3000:]}{r.stderr[-3000:]}')
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(DOL, out)
    finally:
        for p, b in saved.items():
            p.write_bytes(b)
    print(f'{out} ({out.stat().st_size} bytes); the repo files are restored')


if __name__ == '__main__':
    main()
