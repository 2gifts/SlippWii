# Testing a change

Every change is compared with `main`: the same Wii, the same simulated network, the same seeds, run
one after the other. Absolute numbers vary between setups; the difference to `main` on the same
setup is what counts.

## What a change needs

| The change touches | Needed |
|---|---|
| Anything | It builds; the unit tests pass; no desync |
| Netcode, timing, the kernel's game loop, the rollback engine | The **Wii A/B** below |
| Frame timing, the pad read, video, the delay | The **input delay check** below, as well |
| Boot, exit, hangs, the drive, the SD card | One Wii run as a release has it (`--kernel test_control=0`) |
| Teams or Party | Two or more lobbies with `lobby.py`: replays match, frames the Wii waited |
| Loader menus, docs, tools only | Tests that cover it; a picture of the menu if it changed (`tools/menu_shot.py`) |

If you don't have the hardware for a Wii run, say so in the pull request. The maintainer can run it
on the reference Wii, but it takes longer.

## What "better" means

**Never worse:**
- **Input delay at 1 delay frame:** the same as `main`. This was independently confirmed to equal
  a tournament console, and it must stay that way.
- **Desyncs:** none.
- **Frames shown twice on the Wii** (slow frames): no more than `main`.

**Better:** fewer rollbacks for the PC player, shallower rollbacks, fewer halts, fewer slow frames,
fewer frames waited. A change that helps one of these and costs another is a judgment call: show
both.

A difference smaller than the spread between two seeds of the same build is noise. At 45 ms one way
the same build varies by about ±5% in rollbacks a minute and by a few slow frames.

## Unit tests

```sh
cd Nintendont/kernel/slippi_online && sh tests/run_tests.sh      # the kernel's logic
python tools/rollback/test/run.py 200 3                          # the rollback engine, simulated
python tools/online_selftest.py pc --game --rollback 2           # client vs a fake Melee and peer
```

## The Wii A/B

A real Wii against Slippi Dolphin on this PC, bots playing both sides, through a relay that adds the
network's delay and jitter. Nothing reaches Slippi's servers: the harness runs a fake matchmaking
server and uses dummy accounts.

**You need:**
- A Wii with the Homebrew Channel, a wired LAN adapter, an SD card set up with SlippWii, and a
  Melee NTSC 1.02 disc image.
- Slippi Dolphin and the fake matchmaking tools (`SLIPPWII_TEST_TOOLS`); see
  [BUILDING.md](BUILDING.md).
- A worktree of `main` beside your branch, so both builds come from the same machine:
  `git worktree add ../slippwii-main main`.

**Run**, for each of the two trees in turn, with the Wii on the Homebrew Channel:

```sh
export SLIPPWII_RELAY_ARGS="--suggest B"     # the PC player's input suggestions, as Slippi 3.7.0 sends them
for seed in 1 2; do for net in 30,5 45,5; do
  python tools/sweep/run_sweep.py --wii-ip WII_IP --rounds 4 --net $net --seed $seed \
      --rollback 0 --no-mirror --no-profiler --timeout-min 15
done; done
```

That's 8 runs of 4 games per tree, about 45 minutes each. `30,5` is 30 ms one way with 5 ms jitter,
about 70 ms of ping as the Wii measures it; `45,5` about 100 ms. Alternate the trees run by run if you can, so a change in
the room (the network, the PC's load) hits both. If the Diagnostic Log could matter to your change,
repeat with `--diag-log`.

**Score:**

```sh
python tools/sweep/score.py --base MAIN_RUNS... --change YOUR_RUNS...
```

It prints each run and then the table for the pull request. The numbers come from:

| Number | From |
|---|---|
| Wii slow frames | The kernel's `game pacing: N polls over 20 ms apart`, each game. A frame shown twice or a late frame |
| Wii frames waited | `game over: ... waited N frames`: the Wii waiting for the opponent's inputs |
| Desyncs | The kernel's `game checksums: ... N differ`: the Wii's game against its opponent's, frame by frame |
| PC rollbacks a minute, depth | Slippi Dolphin's own replays (`slp_rollbacks.py`) |
| PC halts | Slippi Dolphin's log: halts at the rollback limit during play |
| Unhealthy intervals | Slippi's own match quality rule, as the PC scores the game (`match_quality.py`) |

Don't use the stall profiler's `SHOWN` counters for stutter: rollback rewinds the counter they read,
and the profiler's own reports delay frames. That's why the runs use `--no-profiler`.

## The input delay check

The latency probe times each press inside the console, from the game's controller reading to the
retrace that shows it:

```sh
python tools/sweep/run_sweep.py --wii-ip WII_IP --latency --rollback 0 --net 15,3 --kernel delay=1 \
    --no-delay-buttons --no-mirror --rounds 4
python tools/latency/analyze.py build/sweep/runs/RUN
```

At 1 delay frame "reading -> retrace" is about 40-41 ms (three refreshes) on the reference Wii.
Your change must give the same figure as `main` on your Wii, within 0.5 ms, with the same number of
refreshes. Do the same at `--kernel delay=2` if your change touches the delay. Background:
[INPUT_DELAY.md](INPUT_DELAY.md) and [tools/latency/README.md](../tools/latency/README.md).

## Release-style boot

Test builds carry safety nets a player's build doesn't have: for example, they bring the Wii back
to the Homebrew Channel after a hang. For anything touching boot, exit or hangs, run once with the
kernel as a release has it:

```sh
python tools/sweep/run_sweep.py --wii-ip WII_IP --rounds 3 --net 30,5 --rollback 0 --no-mirror \
    --no-profiler --kernel test_control=0
```

If it hangs, the Wii won't recover by itself: it needs a power cycle.

## Teams and Party

```sh
python tools/sweep/lobby.py --mode teams --wii-ip WII_IP --rounds 3 --net 30 --no-mirror
python tools/sweep/lobby.py --mode teams --wii-ip WII_IP --rounds 3 --teams 0,0,1,1 --wii-seat 3
```

Three Slippi Dolphins and the Wii. The report compares every replay with Dolphin 2's. In the Wii's
log, `game over: ... waited N frames` is how often the Wii waited for inputs; `exi: the game: ...
delay N` is the delay it used. `--human` lets a person play the Wii.

## Pitfalls

- **One harness run at a time.** A test build swaps `g_online_boot.bin` and `so_test_config.inc`
  in the tree and puts them back when it ends. If a run is killed, stop everything with
  `python tools/sweep/stop.py --wii-ip WII_IP`, then
  `git checkout Nintendont/kernel/gecko/g_online_boot.bin Nintendont/kernel/slippi_online/so_test_config.inc`.
- **Don't edit the tree while a batch runs:** each run builds from the tree as it is at its start.
- **Bots change their inputs far more often than people,** so rollback numbers are the hard case.
  The harness also changes characters every game, which makes the first games after a change of
  matchup harder than real play.
- **A Wii that stops answering on the network mid-run** is usually a loose cable or adapter before
  it's code.
- **The test tools have the reference setup's habits.** They expect Git Bash on Windows, and a
  capture card for `--capture` and `menu_shot.py`. Making them easier to run elsewhere is a welcome
  contribution.
