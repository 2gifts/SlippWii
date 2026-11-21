# Testing a change

Every change is compared with `main`: the same Wii, the same simulated connection, the same seeds.
Absolute numbers differ between setups; the difference to `main` on one setup is what counts. The
maintainer's own changes go through the same runs.

The Wii runs need hardware and an afternoon of setup. If that is out of reach, send your change
with the PC tests and say what you couldn't run. The maintainer can run the rest on the reference
Wii.

## What a change needs

| The change touches | Needed |
|---|---|
| Anything | It builds; the PC tests pass; no desync |
| Netcode, timing, the kernel's game loop, the rollback engine | The **A/B on a Wii** |
| Frame timing, the pad read, video, the delay | The **input delay check** as well |
| Boot, exit, hangs, the drive, the SD card | One run **as a release has it** |
| Teams or Party | Two or more lobbies (below) |
| The loader's menus, docs, tools | Whatever covers it; a picture of the menu if it changed (`tools/menu_shot.py`) |

## What counts as better

**Must not get worse:**
- The input delay at 1 delay frame: the same as `main`.
- Desyncs: none.
- Slow frames on the Wii (a frame shown twice, or late): no more than `main`.

**Better** is any of: fewer or shallower rollbacks for the PC player, fewer halts, fewer slow
frames on the Wii, fewer frames waited. When a change helps one and costs another, show both: that
is a judgment to make together.

**Mind the noise.** On the reference setup, four runs of one build gave 1525 to 1749 rollbacks a
minute at 30 ms one way. At 45 ms they gave 1808 to 1851, and runs had 8 to 22 slow frames each. A
difference inside that spread needs more runs before it means anything.

## On a PC

```sh
cd Nintendont/kernel/slippi_online && sh tests/run_tests.sh      # the kernel's logic (a host C compiler)
python tools/rollback/test/run.py 200 3                          # the rollback engine, simulated (pip install ziglang)
```

Without gcc: `pip install ziglang` and `CC="python -m ziglang cc"`. More tests, and what each
needs, are in [BUILDING.md](BUILDING.md).

## Setting up the Wii harness

A real Wii plays Slippi Dolphin on your PC, bots on both sides, through a relay that adds a
connection's delay and jitter. Nothing reaches Slippi's servers: the harness runs its own
matchmaking server with dummy accounts, on a copy of Slippi Dolphin that can't reach the real
ones.

**So far the harness has only run on the maintainer's PC.** This is what it is known to need.
Expect to find something that isn't on the list, and please fix the list or the tool when you do.

1. Windows with Git Bash and Python 3.14, the toolchain and a full build ([BUILDING.md](BUILDING.md)),
   including the two checkouts of Slippi's code that the boot codes are built from.
2. A Wii on the same LAN by cable, with the Homebrew Channel, and an SD card with SlippWii
   installed and a game started from its menu once.
3. Where things are:
   ```sh
   export SLIPPWII_WII_IP=192.168.1.50          # your Wii
   export SLIPPWII_ISO="D:/games/Melee.iso"     # your NTSC 1.02 disc image, for Slippi Dolphin
   ```
4. The loader's settings, saved from the card (with the card in the PC, here as `E:`):
   `python tools/wii_boot.py --save-cfg E:`
5. A dummy account on the card, `slippwii/test-user.json`:
   `python tools/online_selftest.py setup --sd E:` writes it. It also writes a `slippwii/config.ini`
   for another test and keeps yours as a `.bak`: delete the new one or put yours back, or a normal
   SlippWii on that card will look for your PC instead of Slippi.
6. Slippi Dolphin 3.7.0 (Slippi's `FM-Slippi-3.7.0-Win.zip`) unpacked into
   `build/next/dolphin-3.7.0`.
7. The test matchmaking server:
   ```sh
   python tools/build_bearssl.py --host --cc "$CC"
   sh Nintendont/kernel/slippi_online/tests/pc/build.sh
   ```
   (`CC="python -m ziglang cc -target x86_64-windows-gnu"` if you have no MinGW gcc.)
8. Slippi's game files in a test folder on the card, over the network, once:
   `python tools/sweep/push_next_files.py --wii-ip $SLIPPWII_WII_IP --iso "$SLIPPWII_ISO"`
   (a leftover from following Slippi 3.7.0; it could be made unnecessary).
9. Windows Firewall will ask about Python and the test server: allow them on private networks.

## The A/B on a Wii

With the Wii on the Homebrew Channel, on `main` and then on your branch:

```sh
export SLIPPWII_RELAY_ARGS="--suggest B"
for seed in 1 2; do for net in 30,5 45,5; do
  python tools/sweep/run_sweep.py --rounds 4 --net $net --seed $seed \
      --rollback 0 --no-mirror --no-profiler --timeout-min 15
done; done
```

That is 4 runs of 4 games, about 25 minutes, for each of the two. `30,5` is 30 ms one way with up
to 5 ms of jitter, about 70 ms of ping as the Wii measures it; `45,5` is about 100 ms. The relay
option plays the PC player's input suggestions to the Wii, as a PC with a GameCube adapter sends
them since Slippi 3.7.0; the bots don't send any by themselves. If the two results are close, run
`main` once more at the end: the room changes too (the network, the PC's load). If the Diagnostic
Log could matter to your change, repeat with `--diag-log`.

Then:

```sh
python tools/sweep/score.py --base MAIN_RUN... --change YOUR_RUN...
```

It prints each run (the folders under `build/sweep/runs`) and then the table for the pull request.
Where the numbers come from:

| Number | From |
|---|---|
| Wii: slow frames | The kernel's `game pacing: N polls over 20 ms apart`, each game: a frame shown twice, or one that came late |
| Wii: frames waited | `game over: ... waited N frames`: the Wii waiting for its opponent's inputs |
| Desyncs | The kernel's `game checksums: ... N differ`: the Wii's game against its opponent's, frame by frame |
| PC: rollbacks a minute, depth | Slippi Dolphin's own replays (`slp_rollbacks.py`) |
| PC: halts | Slippi Dolphin's log: halts at the rollback limit in play |
| PC: unhealthy intervals | Slippi's own match quality rule, as the PC scores the game (`match_quality.py`) |

## The input delay check

The latency probe times each press inside the console, from the game's read of the controller to
the refresh that shows it:

```sh
python tools/sweep/run_sweep.py --latency --rollback 0 --net 15,3 --kernel delay=1 \
    --no-delay-buttons --no-mirror --rounds 4
python tools/latency/analyze.py build/sweep/runs/RUN
```

In its row "online game, delay 1", against the same run on `main`:

- **Reading -> retrace, the median: the same.** On the reference Wii it is 40.3 to 40.4 ms: two
  refreshes of 16.68 ms and the read's lead of 7 ms.
- **Its 90% figure: no higher.** A read more than 7.8 ms before the refresh plays the controller's
  answer before the one a console plays, which is 8.3 ms more delay while this number moves by a
  fraction of a millisecond. That was the lag that came and went before 1.8.0.
- **"refreshes from the reading to the picture": the same split**, and **`controller polls sr=`
  unchanged**: that is the console's own polling, which the part before the read depends on.
- **0 frames shown twice, 0 jumps.**

Run it with `--kernel delay=2` as well if your change touches the delay. The background is in
[INPUT_DELAY.md](INPUT_DELAY.md) and [tools/latency/README.md](../tools/latency/README.md). The
probe starts at the game's read; what comes before it follows from the console's polling. The
last word on input delay is a measurement from outside the console, as the independent tester's
was.

## As a release has it

Test builds carry safety nets that a player's build doesn't: they bring the Wii back to the
Homebrew Channel after a hang, for one. For anything about boot, exit or hangs, run once with the
kernel as a release has it:

```sh
python tools/sweep/run_sweep.py --rounds 3 --net 30,5 --rollback 0 --no-mirror --no-profiler \
    --kernel test_control=0
```

If it hangs, the Wii won't come back by itself: it needs its power button. `--usb`, `--cheats` and
`--diag-log` set those switches for the run.

## Teams and Party

```sh
python tools/sweep/lobby.py --mode teams --wii-ip $SLIPPWII_WII_IP --rounds 3 --net 30 --no-mirror
python tools/sweep/lobby.py --mode teams --wii-ip $SLIPPWII_WII_IP --rounds 3 --teams 0,0,1,1 --wii-seat 3
```

Three Slippi Dolphins and the Wii. The report compares every replay with Dolphin 2's. In the
Wii's log, `game over: ... waited N frames` is how often the Wii waited for inputs, and `exi: the
game: ... delay N` is the delay it played at. `--human` lets a person play the Wii.

## Pitfalls

- **One harness run at a time**, and don't edit the tree while one runs. A test build swaps
  `g_online_boot.bin` and `so_test_config.inc` in the tree and puts them back at its end. If a run
  is killed: `python tools/sweep/stop.py --wii-ip $SLIPPWII_WII_IP`, then
  `git checkout Nintendont/kernel/gecko/g_online_boot.bin Nintendont/kernel/slippi_online/so_test_config.inc`.
- **`--no-profiler` for anything about smoothness.** The stall profiler's own reports delay frames,
  and its `SHOWN` counters are fooled by rollback, which rewinds the counter they read.
- **Bots change their inputs far more often than people.** Rollback numbers are the hard case.
  The harness also changes characters every game, which is harder on the first seconds of each
  game than a real set.
- **A Wii that stops answering on the network mid-run** has usually lost a cable or its adapter.
  Check that before the code.
- **A capture card** is only needed for `--capture` and `tools/menu_shot.py`.
