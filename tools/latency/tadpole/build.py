"""Builds the Tadpole's latency test firmware (tadpole_latency/): build/latency/tadpole_latency.uf2.

    python tools/latency/tadpole/build.py

Needs arduino-cli with earlephilhower's arduino-pico 3.6.3 in .toolchain/arduino (not fetched by
this script: arduino-cli from github.com/arduino/arduino-cli, then `core install rp2040:rp2040@3.6.3`
with the config file there). Paths with a space break the core's build, so a checkout in one is
built through the drive letter W: (as tools/build.sh makes it).
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FQBN = 'rp2040:rp2040:rpipico'


def main():
    root = Path('W:/') if ' ' in str(ROOT) and Path('W:/tools/latency/tadpole/build.py').exists() else ROOT
    if ' ' in str(root):
        sys.exit('the checkout is in a path with a space, and W: is not mapped to it: run tools/build.sh once')
    cli = root / '.toolchain/arduino/arduino-cli.exe'
    if not cli.exists():
        sys.exit(f'{cli} is missing (see this script\'s first lines)')
    out = root / 'build/latency/tadpole_build'
    res = subprocess.run([str(cli), '--config-file', str(cli.with_name('arduino-cli.yaml')), 'compile', '--fqbn', FQBN,
                          '--build-path', str(out), '--warnings', 'default',
                          str(root / 'tools/latency/tadpole/tadpole_latency')], capture_output=True, text=True)
    if res.returncode:
        sys.exit(f'arduino-cli failed:\n{res.stdout[-3000:]}{res.stderr[-3000:]}')
    uf2 = out / 'tadpole_latency.ino.uf2'
    dest = ROOT / 'build/latency/tadpole_latency.uf2'
    shutil.copy(uf2, dest)
    print(res.stdout.strip().splitlines()[-2:] if res.stdout.strip() else '')
    print(f'{dest} ({dest.stat().st_size} bytes)')


if __name__ == '__main__':
    main()
