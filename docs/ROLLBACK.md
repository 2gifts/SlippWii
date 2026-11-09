# Rollback on the Wii

**Status: in development** (branch `rollback`). Releases play lockstep: the Wii waits for late
inputs while its opponent's Slippi Dolphin rolls back. With the savestate engine built in and
`rollback=1` in `config.ini`, the Wii predicts and rolls back too, the way Slippi Dolphin does,
running Slippi's own rollback code unchanged. The research that led here, with the first
measurements, is in [research/ROLLBACK.md](research/ROLLBACK.md).

## What Slippi needs from the console

Slippi's rollback is Melee code (slippi-ssbm-asm) that SlippWii already runs. It asks the EXI
device for two things ([EXI_DEVICE_SPEC.md](EXI_DEVICE_SPEC.md)):

- **The opponent's inputs, or permission to predict them** (`0xB0`): the kernel's answer
  (`so_exi.c`), as Dolphin's `prepareOpponentInputs`.
- **Savestates** (`0xB1` capture, `0xB2` load): only memory, Slippi Dolphin's four regions less the
  parts it leaves out. No CPU or hardware state.

On a PC, Dolphin copies the regions. The Wii's kernel runs on the ARM, far too slow for that, so the
savestates are made on the PowerPC by a Gecko code, the **savestate engine** (`tools/rollback/`).
It takes `0xB1` and `0xB2` before they reach the kernel; every other command goes through unchanged.

## The engine

| File | What it is |
|---|---|
| `glue.s` | The engine's entry (a C2 code at `HSD_PerfSetStartTime`, each frame), its data (the regions and the excluded ranges), the `FN_EXITransferBuffer` wrapper, and the CPU helpers (caches, BATs, the MMU, the locked cache's DMA). |
| `rollback.c` | Write tracking, the savestates, the alarm queue, the report. Position independent: no globals, no constant data (`tools/pic_code.py`). |
| `excludes.py` | The ranges left out of savestates, written into `glue.s`. |
| `frame_end.s` | Times each frame's work for the report (C2 at `HSD_PerfSetTotalTime`). |
| `sync_loads.s` | Makes the game's file loads finish inside the frame that starts them (C2 in `lbFile_800164A4`). |
| `gx_copy_dst.s`, `gx_copy_tex.s` | Verify builds: where the GPU copies the screen into textures (shadows, refraction). It writes them with no C bit, so verify leaves them alone. |
| `find_engine.inc` | How the hooks find the engine: through the branch at `HSD_PerfSetStartTime`. |
| `test/` | The engine on a PC, against a simulated Wii: `python tools/rollback/test/run.py`. |

`tools/build_online_boot.py --rollback` adds the engine to the boot codes; `--rollback-verify` adds
the checks below (test builds only).

### Write tracking

A savestate only needs the pages the game wrote since the last one: about 0.8 MB a frame of the
7.9 MB Slippi saves. The CPU's MMU can tell which. At a game's first frame the engine maps MEM1 above
2 MB through a page table instead of the game's BATs. The hardware then sets a page's C (changed) bit
on the first write to it. The first 2 MB (code and low data) stay on a BAT and are always copied.

The page table and the savestates live in a 16 MB MEM2 scratch the kernel sets aside
(`SO_PPC_SCRATCH_SIZE`). The engine reaches it through the Homebrew Channel's own MEM2 BAT. It never
maps MEM2 again: two BATs for one address are undefined.

**The page table is read from memory, never through the data cache.** A C bit that the MMU sets in
memory need not reach a cached copy of the table's line. Reading the cached line would hide it.
Writing the line back after clearing another PTE's bit would erase it. Either way a page changes
without being saved, and a later load leaves it half old, half new. Each scan flushes the table's
lines first (`changed_pages`).

### Savestates

- **The base.** At a game's first frame, every page Slippi saves is copied to MEM2.
- **A capture** (`0xB1`, frame F) copies the pages written since the last capture, each as a new
  version of its page.
- **A load** (`0xB2`, frame F) writes back every page written since F's capture, from its newest
  version up to F, or the base. It then drops the later captures. Only the bytes Slippi Dolphin saves
  are written back: not the parts it leaves out, and not the blocks the load must preserve (Slippi's
  ODB, RXB and SSCB, given with the request).
- **Eight captures are kept**; Slippi needs seven. The oldest is folded into the base by moving
  pointers, without a copy.
- **The copies** go through Broadway's locked cache and its DMA engine (four 4 KB buffers, the queue
  kept full). That is about three times faster than the CPU. Melee never uses the locked cache.
  HID2's queue length need not count the transfer in progress, so the engine never trusts an empty
  queue on its own. A buffer is refilled only with at most 4 operations queued. Waiting for the copies
  means queueing one 32-byte fence store and waiting for the engine to take it: it runs its queue
  in order (`dma_wait`). Without this, copies came out with another page's bytes or zeros, and
  restoring them corrupted the game.

### What is rewound, and what is not

One rule: **what the game's main thread writes is rewound; what interrupts and I/O completions write
is not.** On a PC, the disc and ARAM finish their transfers at once, so a savestate never falls inside
one. On a Wii it often does. A driver rewound under a transfer crashes or hangs the game.

Left out of savestates, besides what Slippi Dolphin leaves out (`excludes.py`):

- **The I/O drivers' state:** the disc, the controllers (OSSerial, PAD), VI, AI, ARAM, the DSP, and
  HSD DevCom's requests and buffers.
- **The sound engine** (AX, HSD's synth and audio driver), and the bank loader of Melee's sound
  manager. Its loads are completed by interrupts.
- **The audio heap and its descriptor**, found at run time by walking its cells; and the GPU FIFO's
  tail.
- **The OS alarm queue.** It links the drivers' alarms with the game's, so each load rebuilds it
  from the alarms armed now and at the frame loaded (`relink_alarms`).

Two consequences are handled explicitly:

- **DMA writes are not tracked.** What the disc, the GPU's texture copies or the DSP write sets no C
  bit. That is right for I/O. The game's own disc loads are made to finish inside the frame that
  starts them (`sync_loads.s`), so their data never spans a savestate.
- **Loads in flight at a capture.** Slippi Dolphin never sees one; `sync_loads.s` makes sure the Wii
  doesn't either.

### The kernel's side

`so_exi.c` answers `0xB0` as Dolphin does:

- **Prediction**, within the finalized frame plus `ROLLBACK_MAX_FRAMES` (7), with Dolphin's exact
  skip rule.
- **The opponent's heads** never go back: the game never predicts a frame again once it has its
  input.
- **Time sync.** In the first 2 s, the kernel waits out up to 5 frames when ahead by over 10 ms.
  After that, when behind by over a frame and 10 ms, it runs up to 3 frames twice (one every 5).
- **Missed refreshes made up.** Melee runs one frame per screen refresh, so a frame over 16.7 ms
  (a deep rollback) leaves the Wii a frame behind for good. Slippi Dolphin's frame limiter paces
  by the clock and makes such a frame up at once. The kernel does the same: for each refresh
  missed between two `0xB0` polls it answers *advance* (two frames in one refresh), at most one
  every 3 frames. Left to the network time sync alone (3 frames every 30), a run of slow frames
  put the Wii several frames behind, and Dolphin halted at its rollback limit: frozen frames
  for the PC player.
- **Desync detection** with Slippi's per-frame checksums.

**Loading.** Slippi Nintendont paces disc reads like a real drive; Slippi Dolphin reads at once,
so the Wii starts a match after its opponent. Online games read at a 2 ms seek and 60 MB/s
(`ReadSpeed_Quicker`; a drive is 50 ms and 3 MB/s). Reading with no pacing is not safe, for a reason
not understood: about one stage load in 27 crashed (a stack frame just under Melee's frame buffers
overwritten). At 10 ms and 15 MB/s hundreds of loads were clean; at this pace 108 so far, and the
PC waits under a frame a game for the Wii's load (was ~7, 21 in the first game).

**The opponent does not wait for that.** It used to stop at its rollback limit near frame 8, for about
35 frames every game. But Slippi's `TriggerSendInput` clears the inputs of the frozen frames, whatever the
controller says (those for frames under `START_SYNC_FRAME`, 78), so those pads are known before the game
runs them. While the game loads they go out on the opponent's time (`so_netplay.c`, `send_frozen_pads`),
each pad of its bringing ours up to where they must be by now. Once running, the game runs the frames it
missed two to a refresh (`catch_up_after_load`), to the last pad sent, then until within half a frame by
the time sync's reading. To the opponent, this is a player on time from the first frame.
`--kernel test_no_frozen_pads=1` turns this off, for comparing.

- **Not faster than the game takes its polls.** HSD's pad queue holds 5, and merges the oldest away when one
  more comes. The frame numbers then run one ahead of the game's for good: every input a frame early, a
  desync. A game's first frames can take several refreshes, so the engine says how many frames the game has
  started (`T_GAME_FRAMES`), and there is no advance with more than one waiting.
- **Too late to be level in time,** a game holds the opponent at frame 41 (`FROZEN_PADS_BEFORE_GAME`, 34, plus
  its 7-frame limit), frozen anyway. Unheld it would stop at frame 85, as GO comes up, where a player sees it.

**After a power cut** MEM2 holds garbage, where a warm restart keeps what was there. The engine runs in
every scene once installed, so any word it reads from MEM2 must be set by the kernel first
(`clear_engine_words`). One that was not hung Melee's first scene, on a black screen, after a real power loss only.

**Threads.** Nintendont's kernel has one ARM core, shared with IOS, whose modules are separate
processes: a switch into or out of IOS's network module costs far more than the code it runs. What
wakes above that module (it sits between 0x30 and 0x38) slows its sends by about 200 µs a time.

| Thread | Priority | Why |
|---|---|---|
| Network (ENet, the client's work) | 0x78 | Runs briefly, a few times a frame. |
| Socket threads (receive, send) | 0x40 | They only wait for IOS. Higher, they cut in as each call ends. |
| Kernel main loop, online | 0x7C, 500 µs between passes | It answers the game's polls, which wait with interrupts off. |

Nothing the socket threads hold is wanted by a higher thread: the rings have one writer and one
reader each, each counter one writer, and `so_time_us` takes no lock.

The game waits for each `0xB0` with interrupts off, so the time the kernel takes is gone from a frame's
16.7 ms. With the main loop below the network and disc threads, a third of polls took over 2 ms and some 5,
and a heavy frame on top of that missed its refresh: shown twice, then one skipped. That was most of the
stutter a player felt with no rollback at all. Above them it costs the network module, which it preempts
at each pass, so the passes are spaced out, and a pass comes at once only when the game is reading an answer
(`so_main_loop_quick`). Fountain of Dreams, no latency added:

| Main loop | Polls over 2 ms a game | Ping | `sendto` |
|---|---|---|---|
| 0x30, 200 µs | 556-756 | 3.2 ms | 1.1 ms |
| 0x7C, 200 µs | 0 | 4.7 ms | 2.1 ms |
| 0x7C, 500 µs | 4-9 | 2.9-3.7 ms | 1.0 ms |
| 0x7C, 1000 µs | 813-955 | 4.3-5.2 ms | 1.0 ms |

**A stuck `sendto`.** A blocking `sendto` that does not return would stop the game's inputs. A sender stuck
for 250 ms is replaced by a spare, and the game goes on after a hitch (`--kernel test_stuck_frame=N`
sticks one on purpose). What looked like this was a lost wake-up: `so_event_wait` cleared `pending` before
dropping the queue's extra messages, so a signal between the two left `pending` set with the queue empty, and
the sender thread, which waits with no timer, slept for good (sent count frozen, `send_dropped` rising, about
1 game in 18 at 25-30 ms). It drains first now. The same code is in release 1.2.0.

**The input wait and the retrace.** `input_wait` spins for a late input with interrupts on, for the sound's sake.
The next retrace must stay out: its poll, nested in the waiting one, is a whole frame's input with that frame's
number, and the outer poll then sends the number again. The game's frames run ahead of its polls for good, and
every later input goes out for the wrong frame: a desync or a lost savestate, about 1 game in 6 at 45 ms. The
tell in the last-polls dump is one frame twice in a row. The hardware's mask is not enough, as the OS's
dispatcher goes by its own: a retrace pending when the AI's interrupt comes still runs. So `__OSMaskInterrupts`.

**A race in Slippi's savestates.** A game behind runs queued inputs on one copy of the rollback flags
(`ForceEngineOnRollback`, once per outer loop). A poll between two of them can find frame F, not yet run,
mispredicted: it sets the finalized frame to F, so StartEngineLoop's check (`frame > finalized`) leaves F
uncaptured. F runs, and the next loop asks to load F: "Load state requested ... but frame was not found", and
`b 0` with interrupts off, a frozen console. It needs a frame to be late, rare on a PC and not on a Wii at a
high ping. The kernel changes that one `ble` to `blt` when it reads `netplay.gct`
(`so_gct_fix_capture_check`). This costs only a capture of a frame that turns out not to need one.
`--kernel test_slow_frame_ms=4 --kernel test_old_capture_check=1 --net 45,5` freezes within seconds of a game;
without the old check the race was counted 206 times in 8 games (` sv=` in `PERF rb`), with no savestate lost
and all replays matching. It is in Slippi's own codes, so a PC could hit it too. An engine safety net
stays: a load of a frame never captured ends the game with `rb LOST` and the flight recorder's last polls.

**The handshake** (EXI `0xD9`). The engine says *starting* (0) at its game's first frame, then
*ready* (1) or *failed* (2). The kernel predicts only in a game whose engine said ready, from the
first poll after it. A slow stage load can put that before or after frame 1's poll; any frame is
fine, since the savestates start at the game's first frame.

### Each game's numbers

The engine reports to the kernel's log every 1200 frames and at a game's end (EXI `0xDA` kind 4,
logged as `PERF rb ...`):

| Line | Values |
|---|---|
| `rb n=` | frames; `bus=` the frame's work and `per=` the frame period, average/max µs |
| `base=` | the base copy, in timebase ticks (60.75 per µs) |
| `cap=` | captures; `pg=` pages and `t=` ticks per capture, average/max |
| `rb ld=` | loads; `pg=`, `t=` as above; `dep=` the deepest; `miss=` loads of a frame never captured; `err=` |
| `rb h=` | frames by work time: under 8, 10, 12, 14, 15, 16, 16.7 ms, and over |
| `rb wrote` | where the captured pages are: the 7 busiest 64 KB blocks, each with its pages per capture × 10 |
| `ld=` | a load's ticks: scan / whole pages / partial pages, and partial pages per load; `cs=` a capture's scan; `dvd=` savestates taken with a disc read out; `lw=` file loads waited for (`sync_loads.s`), the longest in µs |
| `rb p` | passes of the game's loop, by engine loops in the pass (1, 2, then 3, 4, 5 or more): each count / average µs / how many went over a refresh |
| `rb exi` | the CPU's waits on the EXI, by what for (the `0xB0` sent, its answer, the rest): each count / average µs |

## Testing

| Test | Runs on | What it checks |
|---|---|---|
| `python tools/rollback/test/run.py [frames] [seed]` | PC | The engine against a simulated Wii (MEM1, MEM2, the locked cache's DMA, C bits, the heaps, the alarm queue). Random writes, captures and loads. After every load, all memory is compared with a snapshot of the frame. |
| `python tools/online_selftest.py pc --game --rollback LAG [--drop PCT]` | PC | The kernel's side: its online client built for Windows, driven by a fake Melee that plays as Slippi's online code does with the engine. It does the handshake before frame 1's poll, predicts, and rolls back. Every confirmed frame must have run with the opponent's real input, and no rollback may need a frame older than Slippi's 7 savestates. With `SO_PC_SLOW_EVERY=N` in the environment it plays like a console with slow frames (every Nth frame takes 1.3 refreshes, a missed refresh is lost) and must keep time. It also checks the connect-code suggestions against Slippi Dolphin's rules. Build the tools first: `sh Nintendont/kernel/slippi_online/tests/pc/build.sh` (MinGW gcc, or `CC="python -m ziglang cc -target x86_64-windows-gnu"`). The fake matchmaking server and opponent come from `--tools`. |
| `CC="python -m ziglang cc" sh tests/run_tests.sh` (in `Nintendont/kernel/slippi_online`) | PC | The online code's units: pads, time-sync maths, matchmaking messages, the config, files. |
| `python tools/sweep/run_sweep.py --rollback LAG [--spike MS] [--verify] [--rounds N]` | Wii and PC | Real games against a local Slippi Dolphin, both driven by autoplay. Every game's replays are compared frame for frame (`tools/slp_compare.py`). `LAG` frames of injected input lag make the Wii predict, and roll back, every frame; `--spike` holds the opponent back `MS` every 5 s, so the Wii waits at the rollback limit, then rolls back 7 frames. The report opens with what the PC player felt, from Slippi Dolphin's own log: its halts at the rollback limit (before the Wii's first input, and in play), its time sync's halts, its speed. Against another PC on a LAN all of them are about 0. |
| `python tools/sweep/run_sweep.py --wii-ip IP --rollback 0 --human --net 30,5 --rounds 20` | Wii and PC | A person plays the Wii against the PC's bot over a simulated connection; D-pad presses are marked in the record. `--kernel rollback=0` for delay. |
| `python tools/sweep/blind.py prepare --wii-ip IP --net 30,5 --delay 3`, then `next`, `stop`, `reveal` | Wii and PC | A blind comparison of rollback and delay: a person plays both in a random order, and the key comes last. |
| `python tools/sweep/soak.py --wii-ip IP --nets 45,5 60,8,1 --rounds 8 --hours N` | Wii and PC | Games through a list of connections, other inputs and matchups each run, until something fails or the time is up. High latency finds what constant `--rollback LAG` does not: both the savestate race and the HSD crash below came from here. |
| `python tools/sweep/ab.py --wii-ip IP --rounds 4 --nets 30,5 45,5 --pc-nets 32,5` | Wii and PC | The same games through the same connection against a second Slippi Dolphin and against the Wii, from the PC player's side (below). `--pc-nets` runs a PC alone, for comparing at the same ping. |

**Verify builds** (`--verify`, `RB_VERIFY`) check, on the Wii:

- **Captures:** every 8th, each copy against its page.
- **Loads:** every 4th, each restored page against its copy. A difference is `rb VERIFY`, which stops
  a sweep.
- **Every 16th load, all saved pages.** A page that differs without having been restored changed
  without a C bit. Each run of such pages is logged at once (`rb unmarked <first differing byte> |
  frame F now <word>`). The GPU's texture copies (shadows, the screen copy for refraction) show up
  here as expected; anything else is a bug.

**Crash reports** (`tools/ppc/crash_report.s`) send the exception, the faulting thread's 32 registers
and its stack to the log. In test builds the Wii then exits to the Homebrew Channel by itself.

## Measured

On a Wii with a LAN adapter, against Slippi Dolphin on the same LAN, release builds (no verify),
2 frames of injected lag: the opponent is always predicted, and a wrong guess (about one frame in
five) is a 2-frame rollback.

| | |
|---|---|
| A frame's work | about 8.5 ms on average, at most 17-21 ms |
| Capture | about 225 pages, about 2 ms |
| Load | about 250-290 pages, 3.1-4.1 ms |
| Frames within 16.7 ms | over 99% |
| Replays compared with Dolphin's | full games match frame for frame |

Verify builds check memory as they go, and take much longer: judge the speed only from release
builds. About 40% of a capture is the particle pool (`rb wrote`: a few hundred KB of the heap, where
depending on the stage): Melee's particle update (`hsd_8039CEAC`) moves every live particle each
frame, and live particles are scattered over the pool's pages. They are game state like any other
(the update draws from the game's random numbers), so they are saved as Slippi Dolphin saves them.

With deeper rollbacks the frames get longer: at 4 frames of injected lag, 95% of frames still fit in
16.7 ms (the longest 22 ms), and the Wii makes up each refresh a late frame missed.

### What the PC player feels

Slippi Dolphin's own log, release builds, four games each (sweeps of 2026-11-03), on a LAN with
the lag injected on the Wii's side:

| Injected | Replays | Dolphin halted in play | Its time sync in play |
|---|---|---|---|
| none | match | 0 | 0 |
| 2-frame rollbacks | match | 0 | 0 |
| 4-frame rollbacks | match | 0 (58 before missed refreshes were made up) | 0 |
| lag spikes (`--spike 300`) | match | about 5 frames a spike | 0 |

**Against a PC, through the same connection** (`tools/sweep/ab.py`; `--net D,J[,L]`: D ms each way,
up to J ms more and L% lost, a relay between the players; `--baseline` puts a second Slippi Dolphin in the
Wii's place), four games a run with the same inputs, on the final kernel (2026-11-04):

| Connection | Rollbacks a minute: PC | Wii | Frames rolled back a minute: PC | Wii | PC 2 ms further: rollbacks | frames |
|---|---|---|---|---|---|---|
| 28,5 | 34 | 215 | 34 | 215 | 172 | 172 |
| 30,5 | 172 | 337 | 172 | 337 | 443 | 443 |
| 33,5 | 578 | 514 | 578 | 514 | 629 | 629 |
| 45,5 | 661 | 580 | 694 | 754 | 615 | 947 |
| 60,8,1 | 640 | 611 | 1363 | 1314 | 609 | 1498 |

All replays matched. Dolphin did not halt at its rollback limit in play against either opponent, and its speed
stayed within 1%. Each run is short and 30 ms is a knife edge against the frame boundary: run to run, a
number there moves by about a quarter.

**Why the Wii's inputs arrive later.** Dolphin's time sync takes half the round trip as the way
here. The Wii's ack turnaround T is part of its round trip, so at rest the Wii's inputs reach Dolphin
**T/2 later than level**, whatever their send path. Dolphin's T is 0.04 ms, the Wii's 3.3 ms: a
receive through IOS, a `sendto` of 1.25 ms and, at times, the wait for a send already under way.
Raising the Wii's input delay does not move this (it goes by frame labels), and neither does sending
earlier.

The Wii's clock also runs slower than Dolphin's (about 70 parts per million here), and nothing pulls
it back until the Wii is 2 ms behind by its own reckoning (about 4 ms at the relay): it advances one
frame (fine sync), Dolphin speeds up by about 0.3% for some 10 s and they are level again. That is
once or twice a minute. Advancing sooner did not help the PC player (1 ms: 375 rollbacks a minute
over 4 games) and costs the Wii: while it is ahead it rolls back every frame, and misses refreshes.
At most 12 a game, as Dolphin warns its player of poor performance after 25 s spent over 10 ms behind.

**So the honest comparison.** The Wii adds about 3.6 ms to the round trip (its own reading of the ping, over
the relay's delay and jitter; Dolphin does not log its). On the same wire a PC player gets more rollbacks against
it just under a frame boundary (here 28-30 ms), where a PC is only now starting to roll back, and fewer above it;
frames rolled back are within about 10% from 33 ms up, either way. At the same ping, the last two columns, the
Wii comes out ahead in four rows of five and within run-to-run variance in the other (28,5).

- **A lag spike** above holds Dolphin's inputs back from the Wii alone. The Wii waits at its
  rollback limit, and Dolphin, which may not run more than 7 frames past the Wii's last input,
  waits a few frames too, as it would for a PC whose connection hiccupped.
- **At a game's start** Dolphin waits 10-20 frames a game, against 2-4 against a PC: at frame 42 and in the freeze
  before anyone can move (Loading, above). At 45 ms and over, about one game in four it also halts 1-2 frames at
  frame 120, by the time sync, just after the freeze.
- **After a KO** Dolphin's log fills with "Halting ... rollback limit" (about 95 lines) and at times "packet
  too small", as the Wii has stopped sending pads for a game that is over. Neither is a wait: the game
  runs on through GAME!, the frame in those lines rising 60 times a second. A true halt repeats its frame.

**The player's lag.** Melee reads the pad at the retrace (the polling drift fix's post-retrace callback), so a frame
has one refresh to reach the screen, and on a Wii a rollback's frame does not fit: a load of about 4 ms, then each
frame again. A third frame buffer gave every frame two refreshes, 33 ms from poll to screen, which nearly none needed.
Now the pad is read before the retrace, on a line interrupt of its own (`poll_early_on`, DI2 and DI3, with the SI's polls
moved to match), and a frame has one refresh and that much. Replay data and the `0xB0`'s answer no longer wait for the
kernel's main loop, which stays awake when a poll is due, and `input_wait` runs when no rollback is due.

- **`rollback_poll_early_us`, 5500 by default:** how early while there are rollbacks: a load must end before the retrace,
  or the frame the retrace puts up shows a refresh late with no sign of it in the record but `retraces by VI line`.
- **Without rollbacks** it stays at 2 ms, as on a regional connection with a low ping. The first load raises it, and 120
  frames without one bring it down by 1 ms.
- **Not further than that.** A fast frame drawn before the coming retrace would show a refresh sooner than a slow one
  beside it: up to 12 ms early, hundreds of frames a game shown twice, then a jump.

Bot games, `--no-mirror`, 2 games each, frames shown twice in play:

| Connection | Stage | Poll to screen | Shown twice | The PC player's rollbacks a minute |
|---|---|---|---|---|
| 15,3 | any | 18.7 ms | 0, 0 | 0 |
| 30,5 | any | 22.2 ms | 7, 0 | 243 |
| 30,5 | Pokémon Stadium | 22.2 ms | 2, 6 | 307 |
| 45,5 | any | 22.2 ms | 38, 34 | 587 |

What is left is at a high ping, where a rollback's frames still pass 22 ms: their load and captures are what to make
faster. With 2 frames of input delay the player's lag is 52-56 ms from pad to screen, against 50 with delay based netcode.

**The PC must keep up too.** A sweep's Dolphin starved for CPU runs slower than 60 frames a
second, and the Wii then waits for its inputs at the 7-frame limit: that looks like a slow Wii.
The sweep runs Dolphin at high priority and gives the PC's CPU load in its report.

Without injected lag, rollbacks happen only when an input is actually late, a few per second at
most on a regional connection.

**Without the replay stream, as a player's Wii** (`ab.py --no-mirror`, 3 games a connection, 2026-11-05). The PC player against a PC / against the Wii. Replays are not compared here; checksums are, and none differed. No halts in play on either side.

| Connection | Rollbacks a minute | Frames a rollback | Frames waited while frozen | Opponent's input later by |
|---|---|---|---|---|
| 15,3 | 0 / 0 | - | 1 / 31 | -0.2 / 2.8 ms |
| 30,5 | 257 / 204 | 1.00 / 1.00 | 2 / 30 | -0.2 / -0.6 ms |
| 45,5 | 664 / 652 | 1.12 / 1.91 | 2 / 31 | -0.2 / 4.5 ms |
| 60,8,1 | 644 / 653 | 2.17 / 2.94 | 8 / 32 | -0.6 / 7.5 ms |

## Known issues

- **Fixed: crashes creating objects** (`HSD_ObjAlloc`, `hsdNew`) **and desyncs at the first playable
  frame.** They came from savestate copies corrupted by the DMA wait above. Logging each mismatch's
  word in the copy as well as in memory showed it: memory right, copies wrong. Since the fix, sweeps
  of 2-frame rollbacks match Dolphin's replays in every game, verify builds included.
- **Not reproduced: a "GO" left on screen, and a desync at the first playable frame.** Both came in
  one run where a starved Dolphin kept the Wii at the rollback limit for most of each game. With
  Dolphin at full speed they have not come back: not with 6-frame rollbacks every frame, nor with
  lag spikes (`--spike`) that make 7-frame rollbacks across the countdown's end, every replay
  matching.
- **Going back to the Homebrew Channel** through Nintendont's reload stub hangs after IOS is
  reloaded. The test exit works around it: when the PowerPC has not reloaded IOS, the kernel
  launches the Homebrew Channel itself (`so_launch_title`).
- **Open: Dolphin's time sync advances 2 frames near frame 300,** about one game in five against the Wii, never
  against a PC (4 games of 20). It reads about -27 ms there: the Wii has moved two frames ahead, soon after its first
  fine sync, though some of these games logged only one fine sync and no missed refresh. Not yet explained.
- **Open: the PC player's rollbacks are deeper at a high ping** (45,5: 1.91 frames against a PC's 1.12; 60,8,1:
  2.94 against 2.17), at about the same rate. The Wii cannot change its speed as Dolphin does, Dolphin only
  slows for an opponent over 8 ms behind, so a Wii drifts to 4-8 ms behind. A lower fine sync threshold would close
  that at the price of a skipped frame on the Wii about every 30 s.
- **Open: the PC player waits about 31 frames at each game's start,** against 2 with a PC, while the Wii
  loads. `test_frozen_pads` removes the wait at the price of the Wii fast-forwarding its first frames.
- **Open: rare crashes while drawing at a high ping.** `HSD_TExp*` once at 60,8,1, not seen since on this
  branch. `HSD_DObjDisp` on a dangling DObj of an item once, right after a load, on
  `rollback-async-exi` only so far. If one recurs, `--verify --net` is the tool not yet used on it.
