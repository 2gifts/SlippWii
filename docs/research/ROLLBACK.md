# Rollback on a real Wii: is it possible?

SlippWii plays lockstep: when the opponent's input for a frame is late, the Wii waits for it (Slippi's
own "skip" answer), while the opponent on Slippi Dolphin rolls back as usual. This page asks what it
would take for the Wii to roll back too, as Slippi Dolphin does. Measured November 2026.

The engine built from this, with write tracking by the MMU and copies by the locked cache's DMA,
is described in [../ROLLBACK.md](../ROLLBACK.md).

**Short answer: yes, with Slippi's full 7-frame window.** The Wii can re-run about **5 frames** within
one video frame (with copy-on-write savestates and the locked-cache DMA engine), and a deeper rollback
costs one repeated picture, never a wait or a different game. The limit is Melee's own logic speed,
not the savestate.

## How Slippi's rollback works

All of it is Slippi's own game code (slippi-ssbm-asm), which SlippWii already runs, except two things
Dolphin provides through the EXI device:

- **Savestates** (EXI `0xB1` capture, `0xB2` load), asked for through two fixed functions,
  `FN_CaptureSavestate` (0x80005608) and `FN_LoadSavestate` (0x8000560C), from
  `Online/Core/StartEngineLoop.asm`. A savestate is *only memory*: four regions
  (`SlippiSavestate.cpp`: 0x80005520-0x80005940, 0x803B7240-0x804DEC00, 0x8065C000-0x8071B000 and the
  main heap, read from 0x804D76B8/0x804D76BC), minus sound and video buffers. No CPU or hardware state.
- **The decision**: the `0xB0` answer (wait, advance, or the opponent's inputs to predict with),
  Dolphin's `prepareOpponentInputs`. SlippWii's kernel makes this decision already; it always waits.

While an opponent's input is missing, the online code predicts it and captures a state for each
predicted frame (up to `ROLLBACK_MAX_FRAME_COUNT` = 7). When the real input arrives and differs, it
loads the oldest state and re-runs the frames in one displayed frame (`LoopEngineForRollback.asm`),
dropping the sounds the re-run would repeat.

So on a Wii: the two savestate functions can be replaced by the Wii CPU's own copies (a Gecko code),
and the kernel can answer like Dolphin. Nothing in Slippi's code would change, and the opponent's
Slippi Dolphin wouldn't notice.

## Measurements

### What a savestate must hold

In a real match the regions total **14.9 MB** (the heap is bigger than `SlippiSavestate.cpp`'s
default). Copying that each frame is impossible on a Wii, and 7 of them don't fit: only the memory
card emulation area (16 MB of MEM2, unused by Slippi Nintendont) and part of the disc cache are free.

But little of it changes. A sweep replay played in Slippi's playback Dolphin, memory compared frame
to frame (`tools/research/dirty_pages.py`):

| | Changed |
|---|---|
| per frame, 32-byte lines | 100-160 KB |
| per frame, 4 KB pages | about 600 KB |
| **over a whole match (5,100 frames), 4 KB pages** | **1.2 MB of 14.9 MB** |

### What a Wii frame costs

`tools/perf_probe` (a test build, `build_online_boot.py --perf`) on a Wii with the LAN adapter, two
online games against Slippi Dolphin (15,106 frames). Melee's own `HSD_Perf` timers, checked against the
timebase: Nintendont gives Melee the Wii's real 243 MHz bus clock.

| | Average | Worst 10 s (median) | Worst frame |
|---|---|---|---|
| Game logic, Slippi's online code included | **2.3 ms** | 4.4 ms | 9.3 ms (once) |
| Logic and the render's build | 5.1 ms | 7.2 ms | 12-17 ms (once) |
| The frame | 16.7 ms | | |

Copying 1 MB with a plain 8-byte loop: **2.6 ms** (380 MB/s). The game's BATs map MEM1 only; DBAT3 is
free to map MEM2 (for the savestates).

### Rounds 2-4 on the Wii: copies, heap, histogram

Three more test builds (`tools/perf_probe`, the kernel's per-game rollback-depth line), about 35,000
more online frames:

| Per 1 MB | CPU copy | CPU, `dcbz` on the destination | CPU, `dcbz` + `dcbt` prefetch | **Locked-cache DMA** |
|---|---|---|---|---|
| MEM1 → MEM2 (save) | 9.8 ms | 3.6 ms | 3.6 ms (prefetch does nothing; HID0's NOOPTI is off) | **1.4 ms** |
| MEM2 → MEM1 (restore) | | | 8.2 ms | **2.95 ms** |

Every copy checked exact. The DMA engine (Broadway's locked cache, libogc's `LCEnable`/`LCLoadBlocks`/
`LCStoreBlocks` sequence, four 4 KB buffers, the queue kept full) is unused by Melee (HID2's LCE off)
and keeps the second bank's long latency out of the CPU's way.

- **The game's logic per frame**: 2.1 ms average; 97% of frames ≤ 3 ms, 99.8% ≤ 4 ms. A re-run
  frame skips the kernel's `0xB0` answer (0.1-0.4 ms): about **1.9 ms**.
- **Free memory in MEM1 during a game**: 334 KB of Melee's 5.9 MB heap (the playback Dolphin has a
  bigger one). Savestates live in MEM2.
- **What a savestate must hold**: across 8 sweep matches, 1.95 MB of 4 KB pages were ever written
  (0.82 MB of 32-byte lines), growing slowly per match (objects land at different addresses); one
  frame writes about 0.6 MB of pages.

## What it adds up to

A rollback frame: restore a state, re-run N frames plus the new one, build the render (2.5 ms).

| Savestate design | Save, per predicted frame | Restore | Re-run frames that fit in one frame |
|---|---|---|---|
| Slippi's regions (7.9 MB on a Wii) | 11 ms | 23 ms | none |
| A measured list of the regions that change (~2 MB), DMA | 2.8 ms | 5.9 ms | **3** |
| Copy-on-write pages (CPU page protection; ~0.6-1 MB), DMA | ~2 ms (spread over the frame) | 2-3 ms | **5** |
| A savestate that costs nothing (the limit) | 0 | 0 | 6 |

Heavy frames (logic 4+ ms, under 0.2% of frames) fit 1-2.

**The limit is Melee's own logic, not the savestate.** Re-running 7 frames costs about 13 ms of
logic, which with the render is more than a 16.7 ms frame. Dolphin can because the PC runs Melee's
logic many times faster than real time; the Wii runs it at its real speed.

**What deeper rollbacks cost.** A rollback that doesn't fit takes two video frames instead of one:
Melee's loop then runs two logic frames for the next video frame (two pad polls are queued) and is
back on time. That is one repeated picture: no input is lost, no game frame differs, the opponent
sees nothing. So the Wii can keep Slippi's full 7-frame window, at 60 fps up to about 5 frames and
with a one-frame hitch beyond, where the lockstep today waits for every late input.

**How often that is.** Rollback depth is about the one-way latency in frames minus the input delay:
with delay 2, a 5-frame rollback needs a one-way delay of about 7 frames (about 230 ms round trip),
which is cross-continent. Regional matches need 0-2. (On a LAN the kernel's histogram showed at most
1.)

### Ideas left out

- **Skipping visual-only work (effects, HUD) while re-running frames**: the only way left to make
  re-runs cheaper, maybe enough for 7 frames in one, but some effects may draw from the game's
  random numbers, which would desync, and Dolphin doesn't do it. It would need proof that each
  skipped process leaves the game's state alone.
- **Saving during the idle part of the frame with the DMA engine** (it runs while the CPU waits for
  the next frame): would make saves nearly free; needs care with what the pad interrupt writes then.
- **MEM1 for savestates**: no room (334 KB free).
- **The GPU, the ARM, or ARAM as copy engines**: lossy formats, too slow, or emulated in MEM2.

## The work, and the risks

1. **Savestates on the Wii** (the main work): replace `FN_CaptureSavestate`/`FN_LoadSavestate` with
   copies of the regions that change, kept in MEM2 (map it with DBAT3). Two ways to know the regions:
   - a measured list (simplest; a region it misses would desync, which Slippi's checksums detect and
     the automated sweeps can hunt for);
   - write tracking with the CPU's page protection (robust, more work; hardware DMA writes, e.g. audio
     and EXI, bypass it and need care).
2. **Faster copies**: `dcbz` on the destination, prefetching, or Broadway's locked-cache DMA.
3. **The kernel**: Dolphin's prediction and rollback answers instead of always waiting, with the
   depth limit above.
4. **Validation**: the automated sweep with injected lag and jitter, replays compared frame for frame
   with Dolphin's, Slippi's desync detection.

Risks: a savestate that misses state (desyncs), heavy moments (a long rollback then would drop a
frame), music and sound during re-runs (Slippi's code handles sound; console music is the game's own),
memory (MEM2 space next to Nintendont's cache).

Ranked needs more than rollback: Ranked's own protocol (its set rules and result reporting) and the
Slippi team's agreement to trust an unofficial client.
