# Input latency, measured

How long a button press takes to reach the screen on a Wii running SlippWii, by what is being
played: an online game at each delay setting, or an offline one with each of Slippi's lag
reduction codes. On a CRT the console's own numbers are the whole answer, as the display adds
only where the line is on the screen.

## The pieces

| Piece | Where | What it does |
|---|---|---|
| The probe | `tools/latency_probe/` (boot codes, `build_online_boot.py --latency`) | In the game: for a press, the time from the first controller reading with the button down to the retrace that shows the first frame with it. Reported over EXI as `LAT` lines in the Wii's log |
| The bot | `tools/sweep/autoplay/` | Its presses are followed as a controller's, so sweeps measure by themselves. `--menu-latency` keeps it on the main menu pressing the d-pad (offline timing) |
| The analysis | `tools/latency/analyze.py RUN_DIR` | The table: reading to retrace, the press before the reading, the top and the middle of a CRT's picture |
| The test controller | `tools/latency/tadpole/` | A Rana Tadpole's firmware that presses d-pad left by itself and tells the console the press's age: the press itself, not an estimate. The bot leaves that button to it |

## Runs

    # offline timing on the main menu: Slippi's codes as shipped (the polling drift fix + VB),
    # none (Melee's own), and the polling drift fix alone
    python tools/sweep/run_sweep.py --latency --menu-latency 70 --no-delay-buttons --rounds 1
    python tools/sweep/run_sweep.py --latency --menu-latency 70 --no-delay-buttons --rounds 1 --kernel test_lag_reduction=1
    python tools/sweep/run_sweep.py --latency --menu-latency 70 --no-delay-buttons --rounds 1 --kernel test_lag_reduction=2

    # online games against the PC's bot, rollback, at a delay and a connection
    python tools/sweep/run_sweep.py --latency --rollback 0 --net 30,5 --kernel delay=2 --no-delay-buttons --no-mirror --rounds 2

    # a person's presses (any controller): the probe follows every button
    python tools/sweep/run_sweep.py --human --latency --rollback 0 --net 30,5

    python tools/latency/analyze.py build/sweep/runs/RUN [--all]

## What the numbers mean

- **Reading -> retrace** is measured: timebase to timebase, to the microsecond. The retrace is the
  one after which the video hardware's own register points at the frame's buffer.
- **The press before the reading** is the part the console can't see. A press waits for the next
  controller poll and then for the next reading the game uses, so a finger pressing at any moment
  waits half the time between those readings on average. The table adds that half; the test
  controller replaces it with the press's real age.
- **Top / middle of the screen**: a CRT draws the picture's first line about 1.3 ms after the
  retrace and its middle line about 9 ms after it.

## What a run also says

- **The screen.** `the screen, over N min in play: X frames shown twice, Y jumps`: the probe watches
  which frame buffer the video hardware shows at each retrace, from the game's frame 125 on (past
  the loading, the countdown and the first time sync). A frame shown twice is a refresh with no new
  frame; a jump is a frame never shown.
- **Frames a refresh sooner** (`rollback_direct`, docs/ROLLBACK.md). A game a line: skips asked /
  runs level ended / frames drawn a refresh behind / passes of over 19 ms (too long for a frame
  level) / the opponent's number the game began with (what the engine decides a skip by: over 2,
  none), then each event with its tick: `skip@`, `ENDED@` with the late frame's work and the time
  from its poll, and `long@` for each pass of over 19 ms with its work and the retraces from its
  poll to its render built (1: on time for a level frame; 2: late for one, on time for a frame a
  refresh behind). Ticks count a rollback's loops too, so a heavy game has more of them than
  frames. (`PERF rb dx` lines, 15 a game; test builds only.) `compare.py` lists the fourth and
  fifth numbers game by game ("too long a game", "began with"). Refreshes from the reading to the
  picture, at 2 delay frames: 4 is level, 5 a refresh behind.
- **The kernel**, in test builds: `poll gap:` at once when a game stops for 100 ms or more in play
  (with the network's counters), and `game slow polls (frame:ms)` at each game's end.

Two more views of runs: `python tools/latency/compare.py NAME=RUN_DIR ...` puts runs side by side, one
line each (lag, the screen, the engine, and what the PC player saw); `python tools/latency/phases.py
RUN_DIR` shows each game's stretches level and behind.

Runs for the pacing itself:

    # level under 75 ms of ping (the engine decides by the frames' work above it)
    python tools/sweep/run_sweep.py --latency --rollback 0 --net 30,5 --kernel delay=2 --kernel rollback_direct=1 --no-delay-buttons --no-mirror --rounds 12
    # 1 delay frame at a low ping: a stock console's lag
    python tools/sweep/run_sweep.py --latency --rollback 0 --net 15,3 --kernel delay=1 --kernel rollback_direct=1 --no-delay-buttons --no-mirror --rounds 12

## The test controller

`python tools/latency/tadpole/build.py` builds `build/latency/tadpole_latency.uf2` (arduino-cli and
earlephilhower's arduino-pico 3.6.3 in `.toolchain/arduino`; joybus-pio 1.2.3 is in the sketch's
`src/joybus`, LGPL-3.0). `python tools/latency/tadpole/flash.py [FILE.uf2]` puts a firmware on a
controller in its flashing mode and shows what it prints. With the test firmware in controller port
1, any `--latency` sweep measures its presses too: the analysis says "(measured)" for them.

It is not a controller to play with (its buttons do nothing). To put the usual firmware back, plug
it into the PC and run `flash.py` with that firmware's UF2: no button is needed, the test firmware
goes to the flashing mode when asked over USB.
