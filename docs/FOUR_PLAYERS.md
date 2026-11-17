# Teams and Party: the record

What was built, measured, tried and dropped for Slippi's four-player modes on 2026-11-14 (released
in 1.6.0), so that
the next look at them starts from here and repeats none of it. The design in short is in
[HOW_IT_WORKS.md](HOW_IT_WORKS.md) ("Four players"); the code's own notes are in the kernel's
[README](../Nintendont/kernel/slippi_online/README.md) ("More than one opponent").

## Where it stands

- **Teams and Party play on a Wii**, in any of the four seats, against three copies of Slippi
  Dolphin 3.6.4, from a LAN to about 130 ms of ping. In every run of the final code every replay
  that was recorded is the same as Dolphin's, frame for frame, a player leaving in the middle of a
  game included.
- **The Wii does not roll back in these modes.** Four characters are more than it can run again
  within a refresh (measured, below). Its game waits for every input, takes a higher input delay the
  higher the lobby's ping (2 to 6 frames, chosen by itself), and keeps itself level with the
  others. The three PCs roll back as against anyone.
- **What a Wii in the lobby costs the three PC players:** about half a frame of rollback depth on
  average, a whole frame at pings that sit just under a frame's edge. At about 95 ms of ping that
  is the difference between a game Slippi's match quality rule calls healthy and one it calls
  unhealthy half the time (the rule acts in Ranked only, which is one against one).
- **One against one is unchanged**: the same comparison and soak as the night before, on this code.
- **Not done:** any game on Slippi's real servers or with people, a long soak, loss on the line,
  two Wiis in one lobby. See "Not tested".

All of it is behind "more than one opponent" in the code, except the three things listed under
"One against one is unchanged".

## Before (1.5.1)

A player reported that both modes "instantly disconnect". For Teams the cause is known:

- The Wii set the game up as a free-for-all (no teams, no Teams flag, colours by port) while its
  three opponents set up teams: a different game from the first frame.
- Its sender could not keep up with three opponents (the datagram budget, below).

Party was never reproduced on 1.5.1. Its game is a different one from the other modes' (a timed
game, below), which 1.5.1 did not set up either.

## How it is tested

Everything is local: no Slippi server is reached, the accounts are dummies.

- `tools/sweep/lobby.py --mode teams|party [--wii-ip IP]`: three copies of Slippi Dolphin played by
  bots, and a fourth player that is the Wii, or without `--wii-ip` a fourth Dolphin copy: what four
  PCs do, the reference for every number here.
- The lobby server is `so_pc_lobby_mm.exe` (`Nintendont/kernel/slippi_online/tests/pc/pc_lobby_mm.c`):
  Teams by four tickets with the same lobby code, Party by a queue of `--party-size`; port 1 is the
  host.
- `--net D,J[,L]` plays through `tools/sweep/lobby_relay.py`: a one-way delay and jitter (ms) and
  loss (%) between every two players. The ping is about twice the delay plus 3 ms: `15,3` is about
  33 ms, `30,3` 63 ms, `45,5` 95 ms, `60,8` 128 ms.
- The report compares every player's replay of every game with Dolphin 2's, scores each Dolphin
  against each other player by Slippi's match quality rule (`match_quality.py`: the average
  rollback depth, and the 150-frame intervals the rule calls unhealthy), and says by the relay how
  much later the first account's inputs left than each PC's own for the same frame.
- `tools/sweep/lobby_sum.py RUN...` prints a run as one line: the rows of the table below.

Things to know about the rig:

- **The bots press at random**, far more often than a person: every number is a worst case for
  rollbacks, the same for both sides of a comparison.
- **`--no-mirror` for timing runs.** The Wii's replay comes out of it as a network stream, which a
  player's Wii does not send, and which is a heavy load with four players (below). Without it the
  Wii's own count of frames whose checksums matched its opponents' stands for its replay.
- **A replay may differ in a game's last frames** between two PCs: frames that were still
  predictions when the game ended. The report counts a difference in the last 7 frames as not final.
- **Party games cannot be quit**: the mode has no pause, so each game runs its 5 minutes.
- **Account codes must differ in their letters** (the Dolphin copies are named by them): with the
  same letters the lobby server took three tickets for one player.
- **Windows:** a socket bound to a network address cannot send to a loopback one, so the relay
  gives each Dolphin copy one address for the other copies (loopback) and one for the Wii (the
  PC's), and the lobby server's `--ext CODE=ADDRESS` hands them out.

The baseline's commands (each about 5 minutes; the Wii at the Homebrew Channel):

```
python tools/sweep/lobby.py --mode teams --rounds 3 --lras-s 40 --net 30,3 --no-mirror --wii-ip IP
python tools/sweep/lobby.py --mode teams --rounds 3 --lras-s 40 --net 30,3
python tools/sweep/lobby_sum.py build/sweep/runs/RUN build/sweep/runs/RUN
```

## What the game's setup needed

From Slippi Dolphin 3.6.4's source (the tag `v3.6.4` of Ishiiruka) and its replays:

- **Teams:** the teams as Dolphin draws them (when all four chose one team: one of six splits, by
  the game's random number), the Teams flag, colour shades by team, the teammate left out of the
  opponents' names, the chat's sender among four, a code history of its own beside Direct's.
- **Party:** a timed game of 5 minutes with the score shown (the game block's first byte 0x12, its
  fourth 0xCC, the timer 300), no pause, the results screen and a new search after each game.
- **Rules:** a character past the 26, or in the fixed-rules modes a stage the match does not allow,
  ends the lobby, with Dolphin's message when the character is this console's.
- **A player who leaves** in the middle of a game: the others play on, as Dolphin
  (`prepareOpponentInputs`, `shouldSkipOnlineFrame`): the one gone is given its last inputs, is
  taken out of the game on the first multiple of 30 frames that is 16 past its last input (the
  same frame on every console), and one whose inputs hold the game for 7 seconds is given up alone.

## Findings, in the order they came

### 1. The Wii's system software sends one datagram at a time

IOS takes 2 to 3.5 ms for each `sendto`, one after the other. A pad and an acknowledgement apart,
to each of three opponents, are six a frame: more than a frame's worth. The sender saturated (285
datagrams a second), every pad left 46 ms late, and the game went out of step or froze.

**Fix:** with more than one opponent an acknowledgement waits for the next pad to the same opponent
and rides in its datagram (at most 20 ms): 3.3 datagrams a frame.

### 2. Melee's disc library gives the drive up after 10 seconds

With the sender saturated the disc's reads were starved (0.5 to 1.6 s each). When one took over
10 s, Melee's `AlarmHandlerForTimeout` masked the disc's interrupt for good, and the next loading
screen never ended: the picture froze at the end of the first game.

- **How it shows:** the game spinning in `discIsDone`, the interrupt mask without the disc's bit
  (PI_INTMR lacking 0x4, the OS's mask word at 0x800000C4 holding 0x400).
- **Fix:** the fix of finding 1 took the cause away. As a second line the handler is a plain return
  in every SlippWii boot (`04336E98 4E800020`, `tools/build_online_boot.py`): a starved read ends
  late instead of ending the drive.
- **Tools made for it:** the stall profiler in every scene with the interrupt masks
  (`--boot-arg=--stall-all`), the kernel's disc state on a stuck game, a log line for a disc command
  over half a second.
- **A trap:** the kernel's own reads of the game's memory (scene, context, OS words) can be seconds
  old, because the PowerPC's cache holds its newest writes. The first diagnosis went wrong by them.
  Ask the game's side (the stall profiler) instead.

### 3. Rollback does not fit four characters

Measured with the engine forced on against three opponents and every input shown one frame late
(`lobby.py --lag 1`, the kernel's `test_multi_rollback=1`):

| | |
|---|---|
| A frame with nothing to run again | 11.3 ms of the 16.7 |
| A frame with one frame to run again | 19.9 ms (loading the savestate 5.2 ms for 360 pages, saving one 1.8 ms for 314) |
| Refreshes without a new picture | about 190 of every 300 |
| The game's speed | 86% (1852 frames in the time of 2160), the three PCs waiting for it |
| Savestates the engine can hold | 5 or 6 of four characters' (310 to 390 pages each), where 8 are needed |
| The replays | the same as Dolphin's: it is correct, only too slow |

With three opponents some input changes on nearly every frame, so nearly every frame is the 20 ms
kind. Before the lag test, with the engine simply on: "rb MISS" (a savestate needed that was no
longer held) and a desync within seconds.

**Decision:** with more than one opponent the engine is told "off" for the game, in the normal
build. `test_multi_rollback=1` still forces it on, to measure again.

### 4. Waiting for inputs is not enough by itself

The Wii waiting at Slippi's 2 frames of delay, 63 ms of ping: the replays match, but

- the Wii waited 116, 39 and 15 frames in three games (40 s each),
- its inputs left 43 ms after the PCs' own for the same frame,
- the PCs' depth against it was 2.8 frames, every interval unhealthy (0.4 among themselves).

### 5. A higher delay on the Wii does not help the PCs

Believed first, and wrong. Slippi's time sync lines the players up by their frames' numbers, so a
Wii at delay 5 only has its opponents' inputs earlier for itself: it stops waiting. The PCs' depth
against it stayed at 2.1 to 2.8. What sets the PCs' view is how far behind them the Wii runs.

### 6. Nothing kept the Wii level with the others

Against one opponent the rollback engine keeps the Wii level to a millisecond or two. A waiting
game has no engine, and Dolphin corrects one way only (faster the moment it reads itself behind,
slower only from about 8 ms ahead) and takes half of the ping it measures for the line's time,
which includes how long the Wii holds its acknowledgements. Left alone the PCs sat 25 to 30 ms
ahead of the Wii.

**Fix (`multi_level`):** the kernel runs a frame early (two in one refresh) when the time sync's
offset reads under -11 ms on average over four readings (2 s) and the next frame's inputs are all
here. At 63 ms of ping the PCs' depth against the Wii went from 2.0-2.8 to 1.0-1.1.

### 7. The delay, by the lobby's ping

With the Wii level, its own waiting is what is left, and the delay ends that: an input that leaves
its player `delay` frames early has that long to arrive.

- **The rule (`lobby_delay`):** the slowest opponent's ping / 2 + 20 ms (`multi_delay_room_ms`, 0
  to 60 in `config.ini`), in frames, plus one; at least the player's setting, at most 6. That is 2
  frames under 27 ms of ping, 3 under 60, 4 under 93, 5 under 127, 6 beyond.
- **Which ping:** each opponent's pads' round trip, smoothed (1/16 a pad). Before a lobby's first
  game there are no pads: the shortest of ENet's acknowledgement round trips (a field added to our
  ENet, `sampleRoundTripTime`).
- **What went wrong on the way:** ENet's own mean starts at 500 ms and gave the first game delay 6;
  the last single pad's round trip gave 3, 6 and 4 in three games of one lobby.
- **The room, tuned at 63 ms of ping** (3 games each, the Wii's delay, the frames it waited):

| Room | Delay | Waited | The PCs' depth against the Wii |
|---|---|---|---|
| 10 ms | 3 / 3 / 3 | 1 / 6 / 2 | 1.23 |
| 16 ms | 3 / 6 / 4 (before the ping was smoothed) | 0 / 0 / 0 | 1.09 |
| 25 ms | 4 / 4 / 4 | 0 / 0 / 0 | 1.11 |
| **20 ms** (kept; 4 games) | 4 / 4 / 4 / 4 | 0 / 0 / 0 / 0 | 1.08 |

  A frame less of delay for the Wii's player costs the others waits; the PCs' depth does not depend
  on it. 63 ms sits just past a step of the rule (60): a ping of 55 gets delay 3.

### 8. The test's replay stream is a load of its own

At 63 ms with the Wii's replay stream on: all 9 replays match, but the PCs' depth against the Wii
is 1.74 instead of 1.1, the Wii waited 43, 3 and 1 frames, and 21 disc commands took over half a
second. A player's Wii sends no such stream unless console mirroring is in use. Not reduced then.
Since 2026-11-16 the stream goes out as one send in 100 ms in such a game, not one a frame
(`multi_stream`), which leaves about a twentieth of the waiting: the counts are in
[NEXT_SLIPPI.md](NEXT_SLIPPI.md), "Not proven, and open".

### 9. A player who leaves, and the Wii as host

- **`--drop 4,15`** (a Dolphin copy closed 15 s into a game, LAN, replay stream on): the Wii waited
  about 6 s, as the PCs did, the game went on with three, and all remaining replays match to the
  end.
- **`--wii-seat 1`** (the Wii as port 1, the host): 3 games, 9 of 9 replays.

## The baseline

Teams, 3 games of 40 s a row, no replay stream, the final design (the delay's room was 25 ms in
these runs, which gives the same delays at these pings as the 20 kept). Depth: the three PCs'
average rollback depth against that player, by Slippi's rule. Unhealthy: the 150-frame intervals
the rule would count against the game, of all.

| Ping | Against a fourth PC | Against the Wii | The Wii's delay | Frames the Wii waited, a game | The Wii's inputs later than a PC's own |
|---|---|---|---|---|---|
| LAN | 0.00, 0 of 135 | 0.21, 3 of 135 | 2 | 0 / 0 / 0 | 10 to 12 ms |
| 33 ms | 0.01, 0 of 135 | 0.23, 3 of 135 | 3 | 0 / 0 / 0 | 8 to 12 ms |
| 63 ms | 0.11, 0 of 147 | 1.11, 6 of 135 | 4 | 0 / 0 / 0 | 7 to 12 ms |
| 95 ms | 1.09, 2 of 303 | 1.97, 62 of 135 | 5 | 1 / 1 / 1 | 7 to 11 ms |
| 128 ms | 2.28, 247 of 330 | 2.35, 102 of 135 | 6 | 12 / 48 / 2 | 1 to 3 ms |

- **Sync:** the Wii's checksums differed from its opponents' on 0 frames in every game, and the
  PCs' replays match each other. (One all-PC replay at 128 ms differed in a game's last two frames:
  the end-of-game case above.)
- **Among themselves** the three PCs' depth in a lobby with the Wii is what it is in a lobby of four
  PCs (0.00, 0.00, 0.11, 1.11, 2.27): the Wii does not change how they see each other.
- **Party at 63 ms:** two 5-minute games, depth against the Wii 1.02 (4 of 720 unhealthy), delay 4,
  no frame waited.
- **The Wii's own count** by Slippi's rule: no debt up to 95 ms; at 128 ms 6, 45 and 0 in the three
  games (30 would end a Ranked match).
- **Where the 7 to 12 ms come from:** the three datagrams leave one after the other (about 2.3 ms
  apart: the three opponents read +7, +9.5 and +11.7), and the level-keeping's step is a whole
  frame, taken only from 11 ms behind. Half a frame of depth on average; a whole one where the ping
  already sits just under a frame's edge, as 63 and 95 ms do.
- **95 ms is the weak row.** Four PCs are healthy there (2 of 303); with a Wii the PCs' depth
  against it averages 1.97 against the rule's 2.0, so close to half the intervals count as
  unhealthy. The rule does nothing outside Ranked; it is the fair yardstick all the same.
- **The 3 of 135 on a LAN** were not looked into (one a game: likely each game's first interval,
  before the level-keeping's first step).

## One against one is unchanged

The condition this work was done under: it must not cost the one-against-one game anything. The
same staircase comparison (`ab.py`, 10 games a row, no replay stream) and soak as the night before,
on the four-player code. The PC player's average depth by Slippi's rule, before -> after:

| One-way delay, jitter (ms) | Against a PC | Against the Wii |
|---|---|---|
| 30, 5 | 0.52 -> 0.51 | 0.72 -> 0.72 |
| 33, 5 | 0.88 -> 0.88 | 0.95 -> 0.90 |
| 36, 5 | 0.99 -> 0.99 | 0.95 -> 0.94 |
| 39, 5 | 1.00 -> 1.00 | 0.98 -> 0.98 |
| 42, 5 | 1.01 -> 1.01 | 1.05 -> 1.03 |
| 45, 5 | 1.11 -> 1.07 | 1.47 -> 1.57 |

- 0 unhealthy intervals in all 24 rows; the Wii's inputs 0.6 to 2.2 ms later than a PC's own
  (before: 1.0 to 2.0).
- The 45, 5 row against the Wii is run-to-run spread at a frame's edge: an earlier run of the old
  code gave 1.54, and in the soaks' runs at that connection the PC's rollbacks are 1.71 to 1.72
  frames deep on the new code against 1.64 to 1.82 over eleven runs of the old.
- Soak: 130 games over two hours (15,3 / 30,5 / 45,5 / 60,8,1), every replay the same, no failure.

What the four-player work changes in a game against one opponent:

- the disc library's timeout is a plain return (acts only after a read of over 10 s);
- the rules check also runs in Unranked (a character or stage the menus do not offer there);
- bookkeeping: a compare of the opponents' count on each poll, one store per ENet acknowledgement,
  a timer around each disc command, two stores at the file system's lock.

## Tried and dropped

So that nobody tries them again without a new reason:

| Idea | What happened |
|---|---|
| Rollback against three opponents | Correct, and 86% of the game's speed with one-frame rollbacks; the engine's savestate space holds 5 or 6 of the 8 needed (finding 3). It would take a savestate load well under half of today's 5.2 ms *and* a cheaper frame, or fewer rollbacks than three opponents cause. |
| A higher delay on the Wii, to spare the PCs | Changes nothing for the PCs (finding 5). It is the cure for the Wii's own waiting only. |
| Waiting at delay 2 at every ping | Fine on a LAN (0 frames waited, 15 of 15 replays). From about 30 ms of ping the Wii waits and falls behind (finding 4). |
| ENet's mean round trip as the lobby's ping | Starts at 500 ms: delay 6 in a lobby's first game. |
| The last pad's round trip as the ping | Delay 3, 6, 4 in one lobby. Smoothed now. |
| A room of 10 or 16 ms | A frame less of delay near the steps, paid in waits (finding 7). |
| The kernel reading the game's state from memory to see where it hangs | Stale by seconds (finding 2). The same flaw is still in `so_diag`'s "in a match" test: it reported a stuck game in a menu once. |

## Not tested

- **Slippi's real servers and real players.** What the server sends for Party (how many players,
  items, the stage list) is not known; the test server's choices stand in for it. The first real
  session is the test.
- **A long lobby** (the longest: 4 games in a row; the soak of "After the release" is 105 lobbies
  of one or two games).
- **Pings over 130 ms**, and more loss than 1% (see "After the release").
- **Two Wiis in one lobby** (one console here). Each would wait for the other.
- **The release build in automated runs.** Every four-player run here was a test build
  (`test_control=1`).
- **Slippi's settings beside the game:** 480p, widescreen, the optional codes, with four players.
- **Items** in Party (`--items`), and Party with fewer than four (`--players`): the options exist,
  no run used them.

## After the release

**The first report came the day 1.6.0 went out:** a desync in the first game of a doubles lobby
(Teams by a lobby code). Nothing came with it: a desync wrote nothing to the card unless the
Diagnostic Log happened to be on.

What was done about it, 2026-11-14:

- **The code was read against Slippi Dolphin 3.6.4 again**: the teams, colours and shades of the
  game's setup, the selections message and how it is merged, which opponent is which port, the
  connections and who has left, and Slippi's own team button. No difference found.
- **What a lobby of people does and the lab's bots never did, played on the Wii.** Until then every
  Teams lobby here had all four on the red team, where the game draws the teams itself; people pick
  them. `lobby.py --teams 0,0,1,1` has each bot press Slippi's team button. And the Wii had only
  been in ports 1 and 4.

| Lobby (2 games each) | Result |
|---|---|
| The PCs' teams by hand, the Wii in port 4, 3, 2 and 1 (40 s games, its replays compared) | all replays the same |
| Drawn teams, the Wii in port 2 and in port 3 | all replays the same |
| The PCs' teams by hand, the Wii in port 2, 63 ms with 1% loss, 3-minute games | PCs' replays the same, 0 checksums differ |
| The PCs' teams by hand, the Wii in port 3, 63 ms with 1% loss, 3-minute games, its replays compared | all replays the same |

- **The Wii's own bot never changed its team in those lobbies** (so several were 3 against 1, which
  Slippi allows and which stayed in sync). The test's fault, not the Wii's: the button answers a
  person, and the bot counted its presses by controller polls, which on a Wii go on while the
  character select screen loads, so its press was made before the screen was there (seen on the
  capture card). It now presses until the button itself says the team. With that: the Wii picks
  blue, says so in its selections, the versus screen and the game are 2 against 2, and the
  replays match.
- **One lobby did fail, and not by a desync** (run 20261114-220434, 1 of about 30 lobbies that day,
  and not again in four more of the same). All four had picked; the Wii went to the versus screen
  and stayed on it; the three PCs started the game and stood at READY for good, giving the Wii's
  player up every 7 seconds and never moving on (their log: `Force-disconnecting player 0 after 7s
  stall`, over and over). The Wii loaded the versus screen's files and then nothing more: not one
  file of the game. That is a first game that never starts for anyone, which a player may well call
  a desync. Its cause is not known. For the next time it shows: `--boot-arg=--stall-all` (where the
  game is), and a test build now says the kernel's disc state by itself when the Wii sits on the
  versus screen (`so_diag.c`).
- **What cannot be played here:** two Wiis in one lobby; real routers (a connection that arrives
  from another port than matchmaking named is filed under the first opponent, in Dolphin's code as
  in ours); Slippi's own server.
- **The next report brings its evidence.** `slippwii/logs/desync-N.txt` is written by itself when a
  frame's checksum is not an opponent's, when the game gives an opponent up, or when all three leave
  a game of four in its middle: the game's setup, everyone's choices, the connections, who was
  taken for gone and at what frame, and the inputs the Wii had from each opponent up to the first
  bad frame ([ROLLBACK.md](ROLLBACK.md), "The log on the SD card"). With a PC player's replay of the
  same game: the same inputs and another result is the game's state; other inputs is the netcode.
  Checked on the Wii with a test's desync, against one opponent and in a Teams lobby, each file read
  back from the card. A game that never starts, as above, leaves the same file 20 seconds after
  its setup, with what the kernel's disc reads were doing.

**The soak, the night after (2026-11-14 22:57 to 05:58):** 105 lobbies one after the other, 193
games, with the stall profiler in every scene. By turns the Wii in each port (26 or 27 lobbies
each), teams of two picked by hand or drawn, the replay stream on and off, a LAN and 63 ms of ping
with 1% loss; one lobby in six was Party.

| Lobbies | Games | In sync | Frames the Wii waited, a game (median, most) | The PCs' depth against the Wii |
|---|---|---|---|---|
| Teams, LAN, no replay stream: 35 | 70 | 140 of 140 PC replays, 0 checksums differ | 0, 0 | |
| Teams, LAN, replay stream: 35 | 70 | 210 of 210 replays | 7, 35 | |
| Teams, 63 ms and 1% loss, replay stream: 18 | 36 | 108 of 108 replays | 17, 38 | 1.73, 513 of 1647 unhealthy |
| Party, 63 ms and 1% loss, no replay stream: 17 | 17 | 34 of 34 PC replays, 0 checksums differ | 1, 5 | 1.04, 40 of 6120 unhealthy |

- **Every lobby started and every game stayed in sync**: 492 of 492 replays the same, no frame's
  checksum different in any game. The lobby that never left the versus screen did not come again:
  once in about 135 lobbies in all, with the replay stream on that time.
- **The replay stream is the load.** Without it the Wii waited 0 frames in 70 LAN games and no disc
  command took half a second; with it, 703 disc commands of over half a second in 35 lobbies (up
  to 5 s for one), and the Wii's own count by Slippi's rule went as high as 45. A player's Wii
  sends no such stream unless console mirroring is in use: with four players, better not.
  (These runs sent it a frame at a time; see finding 8 for what it is now.)
- **Party at 63 ms with loss, as a player's Wii is:** the PCs' depth against the Wii 1.04, 40 of
  6120 intervals unhealthy.

**Teams beyond two against two (2026-11-15).** Slippi's FAQ lists 2v2, 3v1, 2v1, 2v1v1 and 1v1v1.
One lobby of each of the three never played here, two games each on a LAN, the Wii's replays
compared, with *Team Indicators* on: 2v1v1 (four players, the Wii alone on blue), 2v1 (three
players, the Wii in port 1 on the side of two) and 1v1v1 (three players, three teams). All 14
replays the same, no checksum different; the versus screen and the game show the teams as picked.
(`lobby.py --players 3`: the test's lobby server makes a Teams lobby of the size asked. How Slippi's
own server decides a lobby of three is not known here.)

Two more things the first lobbies showed. With the test's replay stream on, a lobby's first game waits
more than its second (19 and 5 frames, 7 and 2, 41 and 5, 23 and 6), with 11 to 37 disc commands
of over half a second a lobby; one lobby whose stream did not connect waited 0 frames and had no
slow command. And the loader's start-up hang has a second address (`tools/build.sh`): a test
build whose BSS began at 801d5860 gave a black screen, and the same sources with 32 bytes of
padding booted.

## If this is picked up again

In the order of what it would give the three PC players:

1. **A finer step for the level-keeping.** Its step is a whole frame, so the Wii sits up to 11 ms
   behind before it acts. The engine's own nudge of the video timing (used against one opponent)
   would hold it within a millisecond or two; in a waiting game the engine is off, so that piece
   would have to run without the rest. Expected: most of the 7 to 12 ms.
2. **Acknowledgements sooner.** They wait for the next pad (up to a frame), and the PCs take that
   for line time. Sending them at once costs datagrams the Wii does not have (finding 1); a middle
   way was not tried. Expected: about 3 ms.
3. **The replay stream with four players** (finding 8), for those who mirror their console.
   Done 2026-11-16 (`multi_stream`): what is left with a mirroring PC is about 6 waited frames in
   a 40 s game on the heaviest stage, and 10 ms on the ping.
4. **Telling the Wii's player the delay a game got.** Today it is only in the log.
5. **Rollback for four** only with a different engine: see "Tried and dropped".
6. **Slippi's newer answer format.** Slippi's development source has a newer answer to the game's
   poll (despawn frames, input predictions) than 3.6.4's 297 bytes: a future codeset will need it.
7. (Done in 1.6.1: the "Show Friendly Player Indicators" optional code is the *Team Indicators*
   switch.)

## Where things are

| What | Where |
|---|---|
| The game's setup, the delay, the level-keeping, a player gone | `Nintendont/kernel/slippi_online/so_exi.c`: `build_game`, `rules_broken`, `rollback_wanted`, `lobby_delay`, `multi_level`, `fill_gone` |
| Acknowledgements riding with pads, the lobby's ping, giving one player up | `so_netplay.c`: `held_ack`, `so_np_slowest_ping_ms`, `so_np_drop_player` |
| Settings | `so_config.c`: `multi_delay_room_ms`, `multi_stream`; tests only: `test_multi_rollback` |
| The disc timeout, the stall profiler in every scene | `tools/build_online_boot.py` (`DVD_TIMEOUT_HANDLER`, `--stall-all`), `tools/ppc/stall.s` |
| The tests | `tools/sweep/lobby.py`, `lobby_relay.py`, `lobby_sum.py`; `tests/pc/pc_lobby_mm.c` |
| In the Wii's log, each game | `game quality (Slippi's rule): ... delay N, debt at most N`; `game over: ... waited N frames`; `game checksums: ... N differ`; `SHOWN ... N fine` (the level-keeping's steps) |
