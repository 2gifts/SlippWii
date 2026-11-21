# Troubleshooting a player's problem

How to get a player's data, what is in it, and how to read it. Written for whoever looks into a
report, a person or Claude.

## What to ask the player for

1. **The `slippwii/logs` folder from the SD card, zipped.** Every report is in it. Nothing in it
   needs a setting turned on.
2. What they saw, in their words, and **what the screen said at the end** ("DISCONNECTED",
   "DESYNC DETECTED", a frozen picture, the Homebrew Channel).
3. Their SlippWii version (the loader's menu shows it) and how the Wii is connected (adapter, any
   splitter or switch).
4. For a problem that happens often: turn on **Diagnostic Log** in the loader's settings and play
   until it happens again. Since 1.8.1 it costs a game nothing: nothing is written to the card in a
   match.

Then: `python tools/triage.py logs.zip` (or the unzipped folder).

For a desync, also ask for **the opponent's replay of that game** (Slippi saves one on the PC), and
run `python tools/desync_compare.py desync-N.txt Game_....slp`. It compares the seed, the
characters and teams, and every input the Wii had for the 12 frames up to the desync with what the
PC played. Different inputs: the netcode. The same inputs: the Wii's game went another way by
itself (its setup, its seed, or its game state).

On our own test runs, `python tools/sweep/setup_check.py build/sweep/runs/RUN` checks every game (not
only one that desynced): the seed, the characters and teams, and the whole setup block the Wii built,
against the PCs' replays.

## What is on the card

All in `/slippwii/logs/`. Each file starts with what it is, its `VERDICT` line (the problem in one
line), the console's setup, and this boot's counts (the card's writes and the longest, failed card
reads, disc commands over half a second, the kernel's heap peak). Then the log lines of this boot
up to the problem, and what followed.

| File | Written when | Always? |
|---|---|---|
| `reports.txt` | One line per problem: the file's number, the file, the verdict. The card's whole history. | yes |
| `crash-N.txt` | The game crashed or panicked, or a match had no pad poll for 5 s (a hang). Written at once: the game has stopped. | yes |
| `freeze-N.txt` | A match had no pad poll for 1 s or more, then went on. | yes |
| `desync-N.txt` | A frame's checksum differed from an opponent's. | yes |
| `lost-N.txt` | The connection to every opponent ended in the middle of a game (not a player's own quit). | yes |
| `dropped-N.txt` | In a game of four, an opponent was given up after 7 s and the others played on. | yes |
| `never-started-N.txt` | A game was set up and did not start within 20 s. | yes |
| `connect-N.txt` | A search or connection ended in an error (the on-screen text is the verdict). Three a boot at most. | yes |
| `disc-N.txt` | The card failed a read, or a disc command took over half a second. | yes |
| `log-N.txt` | Diagnostic Log on: every line of the boot. Problems then go in it, not in files of their own. | switch |
| `newest.txt`, `number.txt` | The newest file's name; the count the numbers come from. | yes |

N goes round 1 to 8 per kind (the count in `number.txt` never repeats; `reports.txt` keeps all).

**Never in a file:** names, the play key, the last two numbers of an IP address, a connect code
but for its first character.

**When files are written:** never in a match; never from the moment every player has picked to the
game's first frame (the versus screen and the load); never while the disc is busy or was in the last
0.3 s, while the prefetch is reading the next game's files, or while anything waits for the card.
That is checked again before each 2 KB written. The one exception is a crash's or a hang's file,
written at once: that game has stopped. The writes are made by the kernel's lowest-priority thread,
not the network thread. So a report can never cost a frame or an input; the price is that a report
from a match reaches the card after it. Measured on the Wii (2026-11-18, 8 games at 60 ms of ping):
no frame halted in play with the Diagnostic Log on (1.8.0: 10, one write of 648 ms in a match), and
the PC's wait for each game's start the same as without it (5.1 frames a game).

## Reading a freeze or a hang

The line `the game has stopped: no pad poll for N ms in a match; where things stand:` is followed by:

- `di: irq 1, ...`: a disc read was in progress (the kernel's side). `irq 0`: none.
- `fs lock: held from ADDR; waiting ADDR`: who held the SD card's lock (look the address up in
  `kernel.elf`). The disc's reads wait for it.
- `prefetch: state ...`: the read-ahead of the next files.
- `where: threads ...`: each kernel thread's last checkpoint (net, receive, send completion, test:
  file letters and line), how long ago the network thread passed one, the game's main thread
  (scene, pc, lr: may be seconds old, the PowerPC's cache), and the network's counts.

Patterns:

- **Music looping during a frozen picture:** the disc's reads stopped finishing (the music streams
  from the disc by interrupts, not by the game's frames). Look at `di:` and `fs lock:`.
- **The network thread's checkpoint long ago, the fs lock held:** a card write held everything
  (before 1.8.1 a desync's report did this in a game).
- **Everything fresh, the game's thread elsewhere:** the game itself is stuck in its own code. A
  player build cannot say where; a test build with `--boot-arg=--stall-all` can.

## Signatures

`tools/triage.py` knows these (the same list is in the script):

| Signature | Means | Next |
|---|---|---|
| `disc: a read failed` | The card failed a read (1.8.1+ tries again) | A card wearing out or a bad copy |
| `PANIC (HSD_Panic) lbarchive.c` | A garbage file load: before 1.8.1, a failed read | Version; read failures before it |
| `di: a command took N ms` | A disc read over 0.5 s; past 10 s Melee gives the drive up | Slow card, or the card busy writing |
| `the card's writes ... the longest N ms` (header) | The slowest write this boot | A slow card |
| `CRASH: exception E at ADDR` | A PowerPC exception | Look ADDR up |
| `checksum differs at frame F` | A desync | Both sides' replays; `hard` > 0 ends the game |
| `no input from the opponent for 7 s` | The opponent's inputs stopped | Their side: their Dolphin log |
| `NETWORK SILENT` | The Wii's own network was lost in a match | Cable, adapter, splitter |
| `np: no connection left with player N` | A connection ended | Any freeze before it caused it |
| `connect:` verdicts | The text the player saw | See below |

Connection errors (the `connect` verdict): `Could not reach the Slippi server` (internet, DNS,
firewall, or Slippi down), `Timed out waiting for other players to connect` (NAT or firewall on
either side), `Online needs a wired LAN adapter` (Wi-Fi), `Update SlippWii to play online again`
(Slippi has moved on: update).

## Each game's numbers

Every game leaves these in the log, whatever else happens: `game over: F frames in T ms; waited W
frames ... ping P us` (W: frames the Wii waited for an opponent's input), `game pacing` (polls over
20 ms apart), `game checksums` (frames compared, any differing), `game rollback` (frames predicted,
stalls at the limit). With more than one opponent, `game late inputs` says how late the inputs the
Wii waited for came.

In Teams and Party the Wii waits for every input (no rollback), and its delay is set by the lobby's
slowest ping. A game that waited for more than 4 frames in 1000 gives the lobby's next game one more
frame (up to 8): `game waited N frames in F at delay D: the lobby's next game one more`. A player
who says the delay "keeps changing" in Teams: look at `waited` in each `game over` line and at these.

## What is not caught

- **A hard lock of the whole console**, or one switched off before the write after the game: no
  file (the next boot's files still count from `number.txt`).
- **Where the game's own code was stuck** in a hang: a player build sees the kernel's side only.
- **A card that has died** cannot hold its own report.
- **The other side** of a desync or a lost connection: the opponent's replay or Dolphin log is
  needed for that.
- **How a game felt** without an error (delay that seems to change, stutter): the per-game numbers
  above, and the player's description.

## Testing the reports

Each kind can be forced on a test build (`run_sweep.py --kernel KEY=VALUE`), and
`test_logfile_dump=2` prints the previous boot's `reports.txt` and newest file to the PC's log:
`test_freeze_frame` (the kernel stops 2 s), `test_desync_frame`, `test_stuck_frame` (the
socket stops: a lost connection), `test_crash_frame`, `test_hang_frame`, `test_disc_fail`
(failed reads), `--mm-turns-away 9.9.9` (a connect error).
