# AGENTS.md

Instructions for AI coding agents (and people) working on SlippWii. Read this whole page before
proposing or making a change. [CONTRIBUTING.md](CONTRIBUTING.md) explains the same scope in prose.

## What the project is

SlippWii is Slippi Online on a real Wii: Slippi Nintendont plus a network client in the Nintendont
kernel that acts as Melee's "Slippi device", the way Slippi Dolphin does on a PC. A Wii plays
Direct, Unranked, Teams and Party against Slippi Dolphin. The aim is "what an official Slippi port
to the Wii would be": no features beyond Slippi's own.

Two goals, in order:
1. **Online feels exactly like offline on a tournament console** (Slippi Nintendont, polling drift
   fix, CRT): the same input delay at 1 delay frame, no frames shown twice or skipped.
2. **A PC player gets the same game against a Wii as against a PC**: the same rollbacks, halts and
   smoothness.

## Hard rules

A change that breaks one of these is rejected, however good its numbers are.

- **The input delay at 1 delay frame stays equal to a tournament console's** (measured, and
  confirmed independently: [docs/INPUT_DELAY.md](docs/INPUT_DELAY.md)). Any change to frame
  timing must show the latency probe unchanged.
- **The game's controller reads don't move.** Don't add SI polls, move the pad read, or change
  Slippi's lag reduction codes online.
- **Offline play stays as Slippi Nintendont has it**: vanilla Melee plus the player's settings. No
  offline speedups or changes.
- **One netcode path.** No second mode, no "low latency" option, no per-player experiments. A
  better method replaces the old one for everyone.
- **Nothing that changes the feel without the player knowing.** No hidden adaptive behaviour that
  makes one game play differently from the next for reasons the player can't see. (Teams' extra
  delay frame after a game that waited is stated in the release notes.)
- **No new features or modes** beyond what Slippi on PC has. No Ranked. Wired only.
- **Stay compatible with Slippi**: the same wire protocol, game files and behaviour as the current
  Slippi Dolphin release. When Slippi changes, follow it.
- **Privacy**: never log, print or send a player's play key, and keep personal data out of logs.
  Problem reports scrub addresses and keys (`so_logfile.c`).

## Before proposing anything

1. Check [docs/ALREADY_TRIED.md](docs/ALREADY_TRIED.md). Many obvious ideas, such as Nintendont's
   "PDF + 1/2F" code online, offline speedups, rollback in Teams, or tuning acks, were measured
   and dropped. Don't propose them again without new evidence.
2. Read the doc for the area you touch:
   - [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md): the overview; start here.
   - [docs/ROLLBACK.md](docs/ROLLBACK.md): the savestate engine, timing steps, what the PC feels.
   - [docs/INPUT_DELAY.md](docs/INPUT_DELAY.md): where the input delay comes from and how it's measured.
   - [docs/TESTING.md](docs/TESTING.md): how a change is measured against `main`.
   - [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md): reading a player's logs and reports.
   - [docs/EXI_DEVICE_SPEC.md](docs/EXI_DEVICE_SPEC.md): the Slippi device's commands.
   - [docs/NEXT_SLIPPI.md](docs/NEXT_SLIPPI.md): following a new Slippi version.
   - History, read only if relevant: ROLLBACK_PLAN, RANKED_READINESS, SLIPPI_GUEST_REPORT,
     FOUR_PLAYERS (the Teams and Party measurements).
3. Say how you'll measure the change before writing it.

## Layout

| Path | What |
|---|---|
| `Nintendont/kernel/slippi_online/` | The online client in the kernel (IOS side, ARM): matchmaking, ENet, the EXI device (`so_exi.c`), pads and timing (`so_pads.c`), problem reports (`so_logfile.c`). Unit tests in `tests/`. |
| `tools/rollback/` | The rollback savestate engine (PowerPC, runs in Melee). Tests in `tools/rollback/test/`. |
| `Nintendont/kernel/gecko/g_online_boot.bin` | The boot codes installed in Melee (built by `tools/build_online_boot.py`, committed). |
| `Nintendont/loader/source/` | The loader: menu, settings, first-start file making, *Check for Updates*. |
| `tools/sweep/` | The Wii test harness: `run_sweep.py` (Wii vs Slippi Dolphin, 1v1), `lobby.py` (Teams/Party), `ab.py` (PC player's side), relays that simulate a network. |
| `tools/latency/`, `tools/latency_probe/` | The input latency probe and its analysis. |
| `docs/` | Design, measurements, troubleshooting. |
| `slippi-ssbm-asm*/` | Slippi's own online code, pinned (not ours to change). |

## Build and test

Windows, Git Bash, Python 3.14. Details in [docs/BUILDING.md](docs/BUILDING.md).

```sh
python tools/fetch_devkitpro.py                                  # toolchain, once
sh tools/build.sh                                                # dist/sd/apps/SlippWii/boot.dol
cd Nintendont/kernel/slippi_online && sh tests/run_tests.sh      # kernel unit tests
python tools/rollback/test/run.py 200 3                          # the engine against a simulated Wii
python tools/online_selftest.py pc --game --rollback 2           # the client vs a fake Melee and peer
```

Without a host gcc: `pip install ziglang` and `CC="python -m ziglang cc"`.

On a Wii, measure as [docs/TESTING.md](docs/TESTING.md) says. The numbers that decide a change are
listed there, along with where each one is read.

## Things that bite

- **The boot codes have a fixed region of 40556 bytes.** Test builds (bots, probes) are close to
  it; a build that doesn't fit fails with "the boot codes are N bytes". Release builds use `-O2`.
- **Never run two harness runs at once.** Each test build swaps `g_online_boot.bin` and
  `so_test_config.inc` in the tree and restores them afterwards. Don't edit build inputs while a
  run is going. If one is killed, restore both files with `git checkout`.
- **Measure stutter with `--no-profiler`.** The stall profiler's own reports delay frames.
- **One seed proves nothing at 45 ms one way.** Use at least two; results vary run to run.
- **Bots change inputs far more than people**, so they're the hard case for rollback.
- **Tests never touch Slippi's real servers.** The harness uses a local fake matchmaking server
  and dummy accounts.
- **Kernel memory is tight** and some structures have fixed sizes (the engine's struct, the log
  ring). Check sizes when adding fields.
- **Files use CRLF line endings.** Keep them.
- **Write comments and docs in plain English**, saying what the code does and why, with the
  measurement that justified a number where there is one.

## Commits and pull requests

- One change per pull request, with the table from `.github/pull_request_template.md`.
- Commit messages: a short summary line, then what changed and why, with measurements.
- Pull requests are squash-merged into `main`; releases are tags on `main`, made and signed by the
  maintainer. Never commit keys; the release key lives outside the repo.
