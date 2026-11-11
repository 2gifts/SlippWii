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
  2c. No Ranked: the Online Play menu does not show it (tools/ppc/hide_ranked.s).
  2d. The ping in an online game's top left corner, as Slippi Dolphin shows it
     (tools/ping_display/, EXI command 0xEF; the online CSS has its switch).
  2e. A text format's "%s" never reads a wild address, as Slippi's ledge grab limit
     message does at the end of a game on the timer: Dolphin gets away with it, a
     console crashes (tools/text_args/).
  3. With --heartbeat (diagnostics): every 32 frames, where the game is and
     a few of its globals (tools/ppc/heartbeat.s, EXI command 0xDE); and the
     interrupt flight recorder (tools/ppc/flight.s), a ring in RAM the kernel
     reads. (tools/ppc/probe_toy.s, the trophy table probe, is not built.)
  4. With --stall (diagnostics, instead of --heartbeat): where Melee's main
     thread is whenever an online game's loop overruns a frame by 2 retraces
     or more (tools/ppc/stall.s, EXI command 0xDE).
  4b. With --perf (diagnostics): per-frame CPU time in online games and a
     memory copy speed test (tools/perf_probe, EXI 0xDA kind 4, logged as PERF).
  4c. The console savestate engine, for rollback (tools/rollback, EXI 0xD9;
     its numbers over EXI 0xDA kind 4, logged as PERF "rb ..."). In every
     build since 1.4.0; --no-rollback leaves it out (tests, and --perf).
  4e. With --latency (diagnostics): how long a button press takes from the controller's
     reading to the screen (tools/latency_probe, EXI 0xDA kind 5, logged as LAT).
  4d. With --test-exit (automated tests): back to the Homebrew Channel when
     the PC asks (tools/ppc/test_exit.s, kernel test_control).
  5. With --autoplay FILE (automated sweeps only, tools/sweep): the autoplay
     code tools/sweep/build_autoplay.py made, which plays port 1 by itself.

    python tools/build_online_boot.py
"""
import struct
import subprocess
import tempfile
from pathlib import Path

import pic_code
from rollback import slippi_offsets

ROOT = Path(__file__).resolve().parent.parent
GECKO_TOOLS = ROOT / '.toolchain/gecko'
BOOTLOADER = ROOT / 'slippi-ssbm-asm/Output/Bootloader/bootloader.gct'
OUT = ROOT / 'Nintendont/kernel/gecko/g_online_boot.bin'
CODES_REGION = 0x19AF4C - 0x1910E0  # Patch.c: CODELIST_TOURNAMENT_END, _BASE

GCT_HEADER = bytes.fromhex('00D0C0DE00D0C0DE')
GCT_FOOTER = bytes.fromhex('F000000000000000')
HEARTBEAT_HOOK_ADDRESS = 0x8034E964
CRASH_HOOK = 0x80345870  # __OSUnhandledException entry (NTSC 1.02)
PERF_HOOK = 0x8037E328  # HSD_PerfSetTotalTime entry (mflr r0)
TEST_EXIT_HOOK = 0x8034E968  # __VIRetraceHandler + 4 (lis r3, 0xCC00)
ROLLBACK_HOOK = 0x8037E214  # HSD_PerfSetStartTime entry (mflr r0)
HEAPS_RESET_HOOK = 0x80015900  # lbHeap_80015900 entry (mflr r0): a scene's heaps are about to go
SYNC_LOADS_HOOK = 0x8001656C  # lbFile_800164A4 after its bl HSD_DevComRequest (lmw r24, 96(r1))
TRACE_HOOKS = ((HEARTBEAT_HOOK_ADDRESS, 0), (0x803761C0, 1), (0x803762C4, 2), (0x803598AC, 3))  # __VIRetraceHandler,
#   HSD_VICopyXFBAsync, HSD_VIDrawDoneXFB, __AXOutAiCallback: entries (mflr r0), for --rollback-trace
GX_COPY_DST_HOOK = 0x8033D5CC  # GXSetTexCopyDst entry (mflr r0)
GX_COPY_TEX_HOOK = 0x8033DCBC  # GXCopyTex entry (clrlwi. r0, r4, 24)
HIDE_RANKED_HOOK = 0x80229938  # MainMenu_CheckIfOptionIsUnlocked entry (mflr r0)
LATENCY_COPY_HOOK = 0x803762A8  # HSD_VICopyXFBAsync, the frame copied (stw r29, 5604(r30))
LATENCY_PAD_HOOK = 0x8034DD94  # PADRead's end (mr r3, r20)
LATENCY_RETRACE_HOOK = 0x803759E4  # HSD_VIPreRetraceCB entry (mflr r0)
XFB_WAIT_HOOK = 0x803761F8  # HSD_VICopyXFBAsync's bl HSD_VIGetXFBDrawEnable, in its inlined buffer wait (tools/ppc/xfb_wait.s)
PING_DISPLAY_HOOK = 0x802F6698  # right after Slippi's in-game set-up (addi r1, r1, 0x18), which makes its HUD
TEXT_ARGS_HOOK = 0x803A6B98  # Text_InitializeSubtext entry (mflr r0)
CSS_DELAY_HOOK = 0x802669DC  # mnCharSel_Scene_OnEnter, after the CSS is built (lwz r0, 0x24(r1))
HEARTBEAT_HOOK = HEARTBEAT_HOOK_ADDRESS  # __VIRetraceHandler entry
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
        subprocess.run([str(GECKO_TOOLS / 'powerpc-eabi-as.exe'), '-mregnames', '-mgekko', '--fatal-warnings', *defs,
                        '-I', str(Path(source).parent), '-o', str(obj), str(source)], check=True)
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
    ap.add_argument('--perf', action='store_true', help='add the frame time and copy speed probe (tools/perf_probe)')
    ap.add_argument('--rollback', action='store_true',
                    help='(the savestate engine, tools/rollback: in every build since 1.4.0; the option is kept for old command lines)')
    ap.add_argument('--no-rollback', action='store_true',
                    help='tests: without the savestate engine; a game waits for late inputs, as 1.3 did (needed with --perf)')
    ap.add_argument('--rollback-verify', action='store_true',
                    help='tests: the engine checks its captures and loads, sampled (docs/ROLLBACK.md)')
    ap.add_argument('--rollback-verify-all', action='store_true',
                    help='tests: with --rollback-verify, every load and all its pages (a slow game)')
    ap.add_argument('--rollback-verify-deep', action='store_true',
                    help="tests: the engine as a player has it, with a sum of each page saved, looked for again in "
                         "what each deep load puts back (rollback.c, RB_VERIFY_DEEP)")
    ap.add_argument('--rollback-walk', action='store_true',
                    help="tests: the engine walks the game's allocators' free lists after each load, before each capture "
                         "and at each frame's end, and says when one is first wrong (rollback.c, RB_WALK)")
    ap.add_argument('--rollback-hwtest', action='store_true',
                    help="tests: at each game's start the engine writes memory and reads it back at once the ways its "
                         "copies and the MMU do, with and without the SDK's bus sync (rollback.c, RB_HWTEST)")
    ap.add_argument('--rollback-trace', action='store_true',
                    help='tests: record what the player sees and hears, with their marks (tools/rollback/trace_view.py); '
                         'not with --stall or --heartbeat')
    ap.add_argument('--small-engine', action='store_true',
                    help='tests: the engine built for size (-Os), so the autoplay fits beside the delay buttons')
    ap.add_argument('--no-delay-buttons', action='store_true',
                    help="tests: no delay buttons on the CSS (a blind test: they show and change the version)")
    ap.add_argument('--latency', action='store_true', help='add the input latency probe (tools/latency_probe)')
    ap.add_argument('--no-text-args', action='store_true',
                    help="tests: without the text format fix (tools/text_args: a game that ends on the timer with a "
                         "player over the ledge grab limit crashes; the bots' games are quit before that)")
    ap.add_argument('--no-ping-display', action='store_true',
                    help="tests: no ping in the game's corner (room for the latency probe beside the autoplay and "
                         "the player's engine)")
    ap.add_argument('--test-exit', action='store_true',
                    help='tests: back to the Homebrew Channel when the PC asks (tools/ppc/test_exit.s)')
    args = ap.parse_args()
    args.rollback = not args.no_rollback
    if args.perf and args.rollback:
        raise SystemExit('--perf hooks the place the savestate engine does: add --no-rollback')
    if args.stall and args.heartbeat:
        raise SystemExit('--stall and --heartbeat hook the same place: pick one')
    reporter = c2(CRASH_HOOK, assemble(ROOT / 'tools/ppc/crash_report.s'))
    # Melee's panics and OS reports (tools/ppc/text_report.s, EXI 0xDA)
    for hook, kind in TEXT_HOOKS:
        reporter += c2(hook, assemble(ROOT / 'tools/ppc/text_report.s', KIND=kind))
    if not args.no_delay_buttons:
        css_delay, _ = pic_code.build(ROOT / 'tools/css_delay/glue.s', [ROOT / 'tools/css_delay/css_delay.c'])
        reporter += c2(CSS_DELAY_HOOK, bytes(css_delay))
    reporter += c2(HIDE_RANKED_HOOK, assemble(ROOT / 'tools/ppc/hide_ranked.s'))
    reporter += c2(XFB_WAIT_HOOK, assemble(ROOT / 'tools/ppc/xfb_wait.s'))
    if not args.no_ping_display:
        ping_display, _ = pic_code.build(ROOT / 'tools/ping_display/glue.s', [ROOT / 'tools/ping_display/ping_display.c'])
        reporter += c2(PING_DISPLAY_HOOK, bytes(ping_display))
    if not args.no_text_args:
        text_args, _ = pic_code.build(ROOT / 'tools/text_args/glue.s', [ROOT / 'tools/text_args/text_args.c'])
        reporter += c2(TEXT_ARGS_HOOK, bytes(text_args))
    extra = b''
    if args.heartbeat:
        extra += c2(HEARTBEAT_HOOK, assemble(ROOT / 'tools/ppc/heartbeat.s'))
        extra += c2(ARCHIVE_PROBE_HOOK, assemble(ROOT / 'tools/ppc/probe_archive.s'))
    if args.stall:
        extra += c2(HEARTBEAT_HOOK, assemble(ROOT / 'tools/ppc/stall.s'))
    if args.perf:
        probe, _ = pic_code.build(ROOT / 'tools/perf_probe/glue.s', [ROOT / 'tools/perf_probe/perf_probe.c'])
        extra += c2(PERF_HOOK, bytes(probe))
    if args.rollback:
        engine, _ = pic_code.build(ROOT / 'tools/rollback/glue.s', [ROOT / 'tools/rollback/rollback.c'],
                                   defines=slippi_offsets.defines() +
                                   (['RB_TRACE'] if args.rollback_trace else []) +
                                   (['RB_VERIFY'] if args.rollback_verify else []) +
                                   (['RB_VERIFY_DEEP'] if args.rollback_verify_deep else []) +
                                   (['RB_WALK'] if args.rollback_walk else []) +
                                   (['RB_HWTEST'] if args.rollback_hwtest else []) +
                                   (['RB_DIRECT_LOG'] if args.latency else []) +
                                   (['RB_VERIFY_ALL'] if args.rollback_verify_all else []),
                                   # (a test build's trace and autoplay fill the codes' region: its engine small)
                                   size=args.rollback_trace or args.small_engine)
        extra += c2(ROLLBACK_HOOK, bytes(engine))
        extra += c2(PERF_HOOK, assemble(ROOT / 'tools/rollback/frame_end.s'))
        extra += c2(HEAPS_RESET_HOOK, assemble(ROOT / 'tools/rollback/heaps_reset.s'))
        extra += c2(SYNC_LOADS_HOOK, assemble(ROOT / 'tools/rollback/sync_loads.s'))
        if args.rollback_trace:
            for hook, which in TRACE_HOOKS:
                extra += c2(hook, assemble(ROOT / 'tools/rollback/trace_hook.s', WHICH=which))
        if args.rollback_verify or args.rollback_verify_deep:  # where the GPU writes textures: verify leaves them alone
            extra += c2(GX_COPY_DST_HOOK, assemble(ROOT / 'tools/rollback/gx_copy_dst.s'))
            extra += c2(GX_COPY_TEX_HOOK, assemble(ROOT / 'tools/rollback/gx_copy_tex.s'))
    if args.latency:
        offsets = slippi_offsets.constants()
        probe, _ = pic_code.build(ROOT / 'tools/latency_probe/glue.s', [ROOT / 'tools/latency_probe/latency_probe.c'],
                                  defines=[f'{n}={offsets[n]}' for n in
                                           ('OFST_R13_ODB_ADDR', 'ODB_LOCAL_PLAYER_INDEX', 'ODB_DELAY_FRAMES')])
        extra += c2(LATENCY_COPY_HOOK, bytes(probe))
        extra += c2(LATENCY_PAD_HOOK, assemble(ROOT / 'tools/latency_probe/pad_hook.s'))
        extra += c2(LATENCY_RETRACE_HOOK, assemble(ROOT / 'tools/latency_probe/retrace_hook.s'))
    if args.test_exit:
        extra += c2(TEST_EXIT_HOOK, assemble(ROOT / 'tools/ppc/test_exit.s'))
    if args.autoplay:
        extra += Path(args.autoplay).read_bytes()
    if args.flight:
        for hook, tag, restore in FLIGHT_HOOKS:
            extra += c2(hook, assemble(ROOT / 'tools/ppc/flight.s', TAG=tag, RESTORE=restore))
    codes = boot[8:-8] + reporter + extra
    # Patch.c: the tournament mode's code, which these codes replace, with the crash report and the
    # list's header and footer. Past it they run into code the disc read or the kernel's patches may
    # touch: seen, a Melee that hangs before its first retrace, on most boots.
    total = 8 + len(codes) + (OUT.parent / 'g_crash_output.bin').stat().st_size + 8
    if total > CODES_REGION:
        raise SystemExit(f'the boot codes are {total} bytes, and their region holds {CODES_REGION}')
    OUT.write_bytes(codes)
    print(f'{OUT}: bootloader {len(boot) - 16} bytes + crash reporter {len(reporter)} bytes'
          + (f' + the savestate engine and diagnostics {len(extra)} bytes' if extra else ''))


if __name__ == '__main__':
    main()
