"""Builds and runs the rollback engine's PC test (rb_test.c) with Zig's C
compiler (pip install ziglang).

    python tools/rollback/test/run.py [frames] [seed]
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def data_list():
    """glue.s's region and exclude list (`data:`)"""
    text = (HERE.parent / 'glue.s').read_text()
    body = text[text.index('\ndata:'):text.index('# end of the list')]
    words = [int(w, 16) for line in body.splitlines() if line.strip().startswith('.long')
             for w in re.findall(r'0x[0-9A-Fa-f]+|\b0\b', line.split('#')[0])]
    return words + [0, 0]  # the terminator


def main():
    words = data_list()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'data.inc').write_text('static const u32 DATA_LIST[] = {' + ', '.join(f'0x{w:08X}u' for w in words) + '};\n')
        exe = tmp / 'rb_test.exe'
        cc = [sys.executable, '-m', 'ziglang', 'cc', '-std=gnu99', '-O2', '-g', '-Wall', '-Werror',
              '-Wno-int-to-pointer-cast', '-Wno-pointer-to-int-cast', '-Wno-unused-function',
              '-fno-strict-aliasing', '-DRB_VERIFY', '-I', str(tmp), '-o', str(exe), str(HERE / 'rb_test.c')]
        r = subprocess.run(cc, capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout + r.stderr)
        sys.exit(subprocess.run([str(exe), *sys.argv[1:]]).returncode)


if __name__ == '__main__':
    main()
