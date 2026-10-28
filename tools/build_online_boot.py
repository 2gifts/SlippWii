"""Builds Nintendont/kernel/gecko/g_online_boot.bin: the Gecko codes Slippi
Nintendont installs in Melee for Slippi Online (kernel/Patch.c).

  1. Slippi's bootloader (slippi-ssbm-asm Output/Bootloader/bootloader.gct,
     built for slot B): at start-up it fetches the online codeset over EXI.
  2. The crash reporter (tools/ppc/crash_report.s): on a fatal exception it
     sends the faulting instruction, address and call stack to the kernel
     (EXI command 0xDF), which logs it.
  2b. The online delay setting on the online CSS: "Delay Frames" and
     "- N +" under Slippi's User and Connect Code, buttons for the hand
     cursor (tools/css_delay/, EXI command 0xDC; Slippi Dolphin has it in its
     settings window, a console has none).
  3. With --heartbeat (diagnostics): every 32 frames, where the game is and
     a few of its globals (tools/ppc/heartbeat.s, EXI command 0xDE); and the
     interrupt flight recorder (tools/ppc/flight.s), a ring in RAM the kernel
     reads. (tools/ppc/probe_toy.s, the trophy table probe, is not built.)
  4. With --stall (diagnostics, instead of --heartbeat): where Melee's main
     thread is whenever an online game's loop overruns a frame by 2 retraces
     or more (tools/ppc/stall.s, EXI command 0xDE).
  5. With --autoplay FILE (automated sweeps only, tools/sweep): the autoplay
     code tools/sweep/build_autoplay.py made, which plays port 1 by itself.

    python tools/build_online_boot.py
"""
import struct
import subprocess
import tempfile
from pathlib import Path

import pic_code

ROOT = Path(__file__).resolve().parent.parent
GECKO_TOOLS = ROOT / '.toolchain/gecko'
BOOTLOADER = ROOT / 'slippi-ssbm-asm/Output/Bootloader/bootloader.gct'
OUT = ROOT / 'Nintendont/kernel/gecko/g_online_boot.bin'

GCT_HEADER = bytes.fromhex('00D0C0DE00D0C0DE')
GCT_FOOTER = bytes.fromhex('F000000000000000')
CRASH_HOOK = 0x80345870  # __OSUnhandledException entry (NTSC 1.02)
CSS_DELAY_HOOK = 0x802669DC  # mnCharSel_Scene_OnEnter, after the CSS is built (lwz r0, 0x24(r1))
HEARTBEAT_HOOK = 0x8034E964  # __VIRetraceHandler entry
TOY_PROBE_HOOK = 0x803060BC  # Toy_803060BC entry (trophy table lookup)
ARCHIVE_PROBE_HOOK = 0x80016A54  # lbArchive_InitializeDAT entry
TEXT_HOOKS = ((0x80388278, 1),  # HSD_Panic
              (0x80345728, 2),  # OSPanic
              (0x803456A8, 3))  # OSReport
FLIGHT_HOOKS = ((0x80347364, 1, 0),  # OSDisableInterrupts
                (0x80347378, 2, 0),  # OSEnableInterrupts
                (0x8034738C, 3, 1))  # OSRestoreInterrupts


def assemble(source, **symbols):
    with tempfile.TemporaryDirectory() as tmp:
        obj, raw = Path(tmp) / 'code.o', Path(tmp) / 'code.bin'
        defs = [f'--defsym={k}={v}' for k, v in symbols.items()]
        subprocess.run([str(GECKO_TOOLS / 'powerpc-eabi-as.exe'), '-mregnames', '-mgekko', *defs, '-o', str(obj),
                        str(source)], check=True)
        subprocess.run([str(GECKO_TOOLS / 'powerpc-eabi-objcopy.exe'), '-O', 'binary', '-j', '.text', str(obj),
                        str(raw)], check=True)
        return raw.read_bytes()


def c2(address, code):
    """A Gecko C2 (insert ASM) code: the handler branches back after it."""
    if len(code) % 4:
        raise SystemExit('code is not whole instructions')
    words = len(code) // 4
    body = code + (b'\x00\x00\x00\x00' if words % 2 else bytes.fromhex('6000000000000000'))
    return struct.pack('>II', 0xC2000000 | (address & 0x01FFFFFF), len(body) // 8) + body


def main():
    boot = BOOTLOADER.read_bytes()
    if boot[:8] != GCT_HEADER or boot[-8:] != GCT_FOOTER:
        raise SystemExit(f'{BOOTLOADER} is not a GCT')
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--heartbeat', action='store_true', help='add the diagnostics heartbeat')
    ap.add_argument('--flight', action='store_true', help='add the interrupt flight recorder (slows the game)')
    ap.add_argument('--stall', action='store_true', help='add the game-loop stall profiler (not with --heartbeat)')
    ap.add_argument('--autoplay', help='append this autoplay C2 code (tools/sweep; test builds only)')
    args = ap.parse_args()
    if args.stall and args.heartbeat:
        raise SystemExit('--stall and --heartbeat hook the same place: pick one')
    reporter = c2(CRASH_HOOK, assemble(ROOT / 'tools/ppc/crash_report.s'))
    # Melee's panics and OS reports (tools/ppc/text_report.s, EXI 0xDA)
    for hook, kind in TEXT_HOOKS:
        reporter += c2(hook, assemble(ROOT / 'tools/ppc/text_report.s', KIND=kind))
    css_delay, _ = pic_code.build(ROOT / 'tools/css_delay/glue.s', [ROOT / 'tools/css_delay/css_delay.c'])
    reporter += c2(CSS_DELAY_HOOK, bytes(css_delay))
    extra = b''
    if args.heartbeat:
        extra += c2(HEARTBEAT_HOOK, assemble(ROOT / 'tools/ppc/heartbeat.s'))
        extra += c2(ARCHIVE_PROBE_HOOK, assemble(ROOT / 'tools/ppc/probe_archive.s'))
    if args.stall:
        extra += c2(HEARTBEAT_HOOK, assemble(ROOT / 'tools/ppc/stall.s'))
    if args.autoplay:
        extra += Path(args.autoplay).read_bytes()
    if args.flight:
        for hook, tag, restore in FLIGHT_HOOKS:
            extra += c2(hook, assemble(ROOT / 'tools/ppc/flight.s', TAG=tag, RESTORE=restore))
    OUT.write_bytes(boot[8:-8] + reporter + extra)
    print(f'{OUT}: bootloader {len(boot) - 16} bytes + crash reporter {len(reporter)} bytes'
          + (f' + heartbeat {len(extra)} bytes' if extra else ''))


if __name__ == '__main__':
    main()
