# AGENTS.md

For AI coding agents working on SlippWii, and for people who want the short version.
[CONTRIBUTING.md](CONTRIBUTING.md) says the same in prose.

## The project in a paragraph

SlippWii is Slippi Online on a real Wii: Slippi Nintendont, plus a network client in the Nintendont
kernel that acts as Melee's "Slippi device" the way Slippi Dolphin does on a PC, plus a savestate
engine in the game for rollback. A Wii plays Direct, Unranked, Teams and Party against Slippi
Dolphin. The aim is what an official Slippi port to the Wii would be, and it has two goals:

1. **Online feels exactly like offline on a tournament console** (Slippi Nintendont, polling drift
   fix, CRT): the same input delay, no frame shown twice or skipped.
2. **A PC player gets the same game against a Wii as against a PC**: the same rollbacks and
   smoothness.

Goal 1 is met on good connections. Goal 2 is not: a PC player rolls back 1.4 to 2 times as often
against a Wii. [docs/FINDINGS.md](docs/FINDINGS.md) has the current numbers and the open problems.
That page is the best place to look for something worth doing.

## What must stay true

These are outcomes for the player. A change that breaks one is not taken, whatever it improves.

- **1 delay frame online equals a tournament console's input delay**: not slower, not faster. It
  rests on two things that the input delay check in [docs/TESTING.md](docs/TESTING.md) measures:
  the game plays the same controller answer a console would (the console's own polling, with the
  game's read no more than 7 ms before the refresh), and a frame reaches the screen the same number
  of refreshes later.
- **Offline play is untouched.** It is the reference.
- **One online behaviour for everyone**: no second mode, no option that trades smoothness for
  delay.
- **Nothing changes the feel without the player being able to know why.** A rule the player can
  be told ("above 85 ms of ping, 8 ms more") is fine; a hidden one that makes one game differ from
  the next is not.
- **Slippi's features only.** No new modes, menus or settings. No Ranked. Wired only.
- **Compatible with the current Slippi release**: the same protocol, game files and behaviour as
  Slippi Dolphin.
- **Players' privacy**: a play key is never logged, printed or sent anywhere but Slippi's
  matchmaking; problem reports mask addresses and connect codes (`so_logfile.c`).

## Everything else is open

How the engine saves and restores state, how packets leave the console, how timing is kept, the
tools, the structure of the code: all of it can be changed, replaced or rethought. Be ambitious.

[docs/FINDINGS.md](docs/FINDINGS.md) records what was tried and what was measured. Use it as a
head start. It is not a list of forbidden ideas, and "the floor" or "the limit" there means the
limit of what was tried. If you see a way round a finding, say what you expect to be different and
how you'd measure it, then try it. Skepticism about those findings is welcome; so is checking them
again.

## How to work here

- **Measure.** A claim about timing, smoothness or rollbacks needs numbers against `main` on the
  same setup ([docs/TESTING.md](docs/TESTING.md)). Reasoning alone has been wrong here many times.
  This hardware surprises.
- **Say what you ran and what you didn't.** You probably can't reach a Wii. Do what a PC allows
  (below), then tell your user exactly which Wii runs are still needed, with the commands. Never
  report a hardware result you didn't get.
- **Negative results count.** If an idea didn't work, add it to docs/FINDINGS.md with its numbers.
- **Docs can be stale.** Trust the code and the newest measurement, and fix the doc.
  [docs/ROLLBACK.md](docs/ROLLBACK.md) grew over weeks; where it disagrees with INPUT_DELAY.md or
  FINDINGS.md, those are newer.
- **Keep a change small and single-purpose.** Simple and polished beats clever.
- **Comments and docs** say what the code does and why, in plain English, with the measurement
  behind a number where there is one.

## Where things are

| Path | What |
|---|---|
| `Nintendont/kernel/slippi_online/` | The online client in the kernel (ARM, under IOS): matchmaking, ENet, the EXI device the game talks to (`so_exi.c`), pads and timing rules (`so_pads.c`), sockets (`so_socket.c`), problem reports (`so_logfile.c`). Unit tests in `tests/` |
| `tools/rollback/` | The rollback savestate engine (PowerPC, runs inside Melee): `rollback.c`. Its PC test in `test/` |
| `Nintendont/kernel/gecko/g_online_boot.bin` | The codes installed in Melee, built by `tools/build_online_boot.py` and committed |
| `Nintendont/loader/source/` | The loader: menu, settings, making Slippi's files from the disc, *Check for Updates* |
| `tools/sweep/` | The Wii test harness: `run_sweep.py` (Wii against Slippi Dolphin), `lobby.py` (Teams and Party), `net_relay.py` (a simulated connection), `score.py` (the comparison table) |
| `tools/latency/`, `tools/latency_probe/` | The input latency probe and its analysis |
| `slippi-ssbm-asm*/` | Slippi's own online code, pinned. Not ours to change |

Docs, in the order worth reading:

1. [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md): the overview.
2. [docs/FINDINGS.md](docs/FINDINGS.md): where things stand, open problems, what was tried.
3. [docs/TESTING.md](docs/TESTING.md): how a change is measured.
4. [docs/INPUT_DELAY.md](docs/INPUT_DELAY.md): where the input delay comes from.
5. [docs/ROLLBACK.md](docs/ROLLBACK.md): the engine and the timing in depth.
6. As needed: TROUBLESHOOTING (reading a player's logs), EXI_DEVICE_SPEC (the Slippi device's
   commands), NEXT_SLIPPI (following a new Slippi version), FOUR_PLAYERS (Teams and Party),
   BUILDING. ROLLBACK_PLAN, RANKED_READINESS and SLIPPI_GUEST_REPORT are history.

## Build and test on a PC

Developed on Windows 10/11 with Git Bash and Python 3.14; nothing else has been tried. Details in
[docs/BUILDING.md](docs/BUILDING.md).

```sh
python tools/fetch_devkitpro.py                                # the toolchain, once
sh tools/build.sh                                              # dist/sd/apps/SlippWii/boot.dol
pip install ziglang                                            # a C compiler for the PC tests, if you have no gcc
cd Nintendont/kernel/slippi_online && CC="python -m ziglang cc" sh tests/run_tests.sh   # the kernel's logic
python tools/rollback/test/run.py 200 3                        # the engine against a simulated Wii
```

`tools/online_selftest.py pc --game --rollback 2` runs the kernel client against a fake Melee and
opponent. It needs two helper programs that aren't in this repository yet (BUILDING.md).

What only a Wii can tell you: frame timing, slow frames, the input delay, anything about IOS or
the disc, and what the PC player gets. See [docs/TESTING.md](docs/TESTING.md).

## Things that bite

- **The boot codes have a fixed region of 40556 bytes.** Test builds (engine, bot, probe) sit
  within a few hundred bytes of it; one that doesn't fit fails with "the boot codes are N bytes".
- **One harness run at a time, and no edits while one runs.** A test build swaps
  `g_online_boot.bin` and `so_test_config.inc` in the tree and puts them back at its end. If a run
  is killed, restore both with `git checkout`.
- **Measure stutter with `--no-profiler`**, from the kernel's `game pacing` lines
  (`tools/sweep/score.py`). The stall profiler's own reports delay frames, and its counters are
  fooled by rollback.
- **One run proves nothing.** The same build varies run to run; use at least two seeds.
- **Bots change inputs far more often than people.** They're the hard case for rollback.
- **Game memory is in the savestates.** A flag an engine sets in Melee's memory can be undone by
  the next rollback's load.
- **Test builds rescue hangs that a release doesn't.** Anything about boot, exit or hangs needs a
  run with `--kernel test_control=0`.
- **Tests never reach Slippi's servers.** The harness runs its own matchmaking server with dummy
  accounts. Keep it that way.
- **Memory is tight.** The kernel's online heap and the engine's state struct have fixed sizes.

## Commits and pull requests

- One change per pull request, with the table from `.github/pull_request_template.md`.
- A commit message says what changed and why, with the measurements.
- Pull requests are squash-merged into `main`. The maintainer tags and signs releases; never
  commit a key.
