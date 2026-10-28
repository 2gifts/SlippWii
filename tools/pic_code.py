"""Builds position independent PowerPC code from assembly glue and C, for
Gecko C2 codes that run wherever the code list sits (the Wii's boot codes,
Dolphin's code area): tools/sweep/autoplay/ and tools/css_delay/.

The glue puts its entry code in section START and the replaced instruction
in section TAIL; the C goes between. The build fails on any absolute
relocation, data section or library call, so the C keeps its floats, colours
and strings in a struct the glue lays out.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / '.toolchain/opt/devkitpro/devkitPPC/bin'
CFLAGS = ['-O2', '-mcpu=750', '-meabi', '-mno-sdata', '-G', '0', '-fno-jump-tables', '-ffreestanding',
          '-fno-builtin', '-fno-pic', '-fno-common', '-fno-asynchronous-unwind-tables', '-fno-tree-loop-distribute-patterns',
          '-nostdlib', '-Wall', '-Werror']
RELATIVE = {'R_PPC_REL24', 'R_PPC_REL14', 'R_PPC_LOCAL24PC', 'R_PPC_REL14_BRTAKEN', 'R_PPC_REL14_BRNTAKEN'}
LINK = """
SECTIONS {
  . = 0;
  .text : { *(START) *(.text .text.*) *(TAIL) }
  /DISCARD/ : { *(.comment) *(.note*) *(.gnu*) *(.eh_frame) }
}
"""


def tool(name):
    return str(BIN / f'powerpc-eabi-{name}.exe')


def run(*cmd):
    res = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if res.returncode:
        sys.exit(f'{cmd[0]} failed:\n{res.stdout}{res.stderr}')
    return res.stdout


def check_object(obj):
    """No absolute relocations and nothing outside .text."""
    sections = run(tool('readelf'), '-S', '-W', obj)
    for line in sections.splitlines():
        parts = line.split()
        for name in ('.data', '.bss', '.rodata', '.sdata', '.sbss', '.sdata2', '.got'):
            if any(p == name or p.startswith(name + '.') for p in parts):
                idx = [i for i, p in enumerate(parts) if p == name or p.startswith(name + '.')][0]
                size = int(parts[idx + 4], 16) if len(parts) > idx + 4 else 0
                if size:
                    sys.exit(f'{obj.name}: section {parts[idx]} ({size} bytes): not position independent')
    relocs = run(tool('readelf'), '-r', '-W', obj)
    for line in relocs.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2].startswith('R_PPC_') and parts[2] not in RELATIVE:
            sys.exit(f'{obj.name}: absolute relocation {parts[2]} ({" ".join(parts[3:])})')


def build(glue, sources, start='.pic_start', tail='.pic_tail', defines=()):
    """(code bytes, {symbol: offset}); `defines`: -D names for the C"""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        objs = [tmp / 'glue.o']
        run(tool('as'), '-mregnames', '-mgekko', '-o', objs[0], glue)
        for i, src in enumerate(sources):
            objs.append(tmp / f'c{i}.o')
            run(tool('gcc'), *CFLAGS, *(f'-D{d}' for d in defines), '-c', '-o', objs[-1], src)
        for obj in objs:
            check_object(obj)
        elf, raw, ld = tmp / 'a.elf', tmp / 'a.bin', tmp / 'link.ld'
        ld.write_text(LINK.replace('START', start).replace('TAIL', tail))
        run(tool('ld'), '-T', ld, '-o', elf, *objs)
        run(tool('objcopy'), '-O', 'binary', '-j', '.text', elf, raw)
        syms = {l.split()[2]: int(l.split()[0], 16) for l in run(tool('nm'), elf).splitlines() if len(l.split()) == 3}
        code = bytearray(raw.read_bytes())
        if syms.get('tail') != len(code) - 4:
            sys.exit(f'{glue}: `tail` is not the last instruction (section {tail} misplaced)')
        return code, syms
