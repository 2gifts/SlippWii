## What changes for the player

<!-- One or two sentences: what someone on the Wii, or a PC player against a Wii, would notice. -->

## Why, and how it works

<!-- The problem and the idea. If docs/FINDINGS.md has an entry on it, say what is different this
time. -->

## Measurements against `main`

<!-- docs/TESTING.md has the commands; tools/sweep/score.py prints this table. Same Wii, same nets,
same seeds for both. Delete rows your change can't affect. No Wii? Leave the table out and say so
below: the maintainer can run it. -->

| Net (one way, jitter) | | `main` | this change |
|---|---|---|---|
| 30,5 | Wii: slow frames (polls over 20 ms) / games | | |
| 30,5 | PC: rollbacks per minute | | |
| 45,5 | Wii: slow frames (polls over 20 ms) / games | | |
| 45,5 | PC: rollbacks per minute | | |
| 45,5 | PC: average rollback depth | | |
| both | PC: halts per game, unhealthy intervals | | |
| both | Games desynced | | |
| 15,3 | Input delay check, 1 delay frame: reading -> retrace, median and 90% (ms) | | |

## What was tested, and what wasn't

<!-- PC tests run; Wii runs done (folder names under build/sweep/runs, or attached logs); anything
you couldn't run. If an AI agent did the work, say which parts you checked yourself. -->

## Checklist

- [ ] The PC tests pass (`tests/run_tests.sh`; `tools/rollback/test/run.py` if the engine changed)
- [ ] Offline play and the input delay at 1 delay frame are unchanged
- [ ] One change, as simple as it can be
- [ ] Docs updated where behaviour changed; docs/FINDINGS.md if this tried something listed there
