"""Checks that nothing in the linked kernel uses memcpy's return value.

    python tools/check_kernel_memcpy.py [Nintendont/kernel/kernel.elf]

The kernel's memcpy (Nintendont/kernel/utils_asm.S) returns the END of the destination, where C
says the start. The kernel's own code is compiled with -fno-builtin-memcpy, so the compiler never
leans on the standard value; a library compiled without it may. BearSSL did (x509_minimal.c kept
the server's public key at the returned address), which failed every TLS handshake on a Wii and
none on a PC. tools/build.sh runs this after linking: a use found fails the build.

How: in the disassembly, after each call of memcpy, the instructions up to the next branch or
call (or the first that overwrites r0) are looked at for one that reads r0. That covers what a
compiler makes of "p = memcpy(...)" and of its own reuse of the value; it is a net, not a proof.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OBJDUMP = ROOT / '.toolchain/opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump.exe'
R0 = re.compile(r'\br0\b')
NO_WRITE = ('cmp', 'cmn', 'tst', 'teq', 'str', 'push')  # their first operand is read, not written


def uses(elf):
    """[(function, instruction)] that read memcpy's return value."""
    lines = subprocess.run([str(OBJDUMP), '-d', str(elf)], capture_output=True, text=True, check=True).stdout.splitlines()
    found, func = [], '?'
    for i, line in enumerate(lines):
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            func = m.group(1)
            continue
        if not re.search(r'\tblx?\t[0-9a-f]+ <memcpy>', line):
            continue
        for ins in lines[i + 1:i + 16]:
            parts = ins.split('\t')
            if len(parts) < 4:
                break
            mnem, ops = parts[2], parts[3].split('@')[0].strip()
            if mnem.startswith('b') and not mnem.startswith('bic'):
                break  # a branch or a call: what follows is another story
            first, _, rest = ops.partition(',')
            if mnem.startswith(('ldm', 'pop')):
                # a load of several registers: the base is read, the list written
                if mnem.startswith('ldm') and R0.search(first):
                    found.append((func, ins.strip()))
                elif not R0.search(rest if mnem.startswith('ldm') else ops):
                    continue
                break
            reads_first = mnem.startswith(NO_WRITE) or mnem.startswith('stm')
            if R0.search(rest) or (reads_first and R0.search(first)):
                found.append((func, ins.strip()))
                break
            if not reads_first and R0.fullmatch(first.strip()):
                break  # r0 is overwritten first
    return found


def main():
    elf = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'Nintendont/kernel/kernel.elf'
    found = uses(elf)
    for func, ins in found:
        print(f'{elf.name}: {func} uses what memcpy returns: {ins}')
    if found:
        sys.exit(f"{len(found)} uses of memcpy's return value in the kernel: the kernel's memcpy returns the end of the "
                 'destination (see tools/check_kernel_memcpy.py)')
    print(f"{elf.name}: nothing uses memcpy's return value")


if __name__ == '__main__':
    main()
