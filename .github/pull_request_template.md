## What changes for the player

<!-- One or two sentences: what someone on the Wii, or a PC player against a Wii, would notice. -->

## Why

<!-- The problem, and how you found it (a log, a measurement, an issue). If it's in
docs/ALREADY_TRIED.md, what's different this time. -->

## Measurements against `main`

<!-- As docs/TESTING.md describes. Same Wii, same nets, same seeds, both builds with --no-profiler.
Delete the rows your change can't affect, and say why. -->

| | `main` | this change |
|---|---|---|
| Wii: slow frames (`game pacing`, polls over 20 ms), 30,5 / 45,5 | | |
| PC player: rollbacks per minute, 30,5 / 45,5 | | |
| PC player: average rollback depth, 45,5 | | |
| PC player: halts at the rollback limit, unhealthy intervals | | |
| Replays matching / games | | |
| Input delay at 1 delay frame (latency probe), ms | | |

Runs (folder names under `build/sweep/runs`, or attached logs):

## Checklist

- [ ] Unit tests pass (`tests/run_tests.sh`, and `tools/rollback/test/run.py` if the engine changed)
- [ ] Nothing changes offline play, when the controller is read, or the delay at 1 delay frame
- [ ] One change, as simple as it can be
- [ ] Docs updated if behaviour changed (`CHANGELOG.md` is updated by the maintainer at release)
