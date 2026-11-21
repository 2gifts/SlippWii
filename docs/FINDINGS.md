# Findings so far

What has been measured on the Wii, what is still open, and what has been tried. Most of this lived
only in the maintainer's notes until now.

**How to read this page.** It is context, not a list of closed doors. Each entry says what was
tried, what was measured, and why we think it came out that way, so you can start from there
instead of from zero. Many of these results are one afternoon's test on one console. Any of them
could be wrong, or right only for the way it was tried. If you see a way round one, try it: the
entry tells you what to measure to show it works. And if you try something that doesn't work,
please add it here. A negative result with numbers saves the next person the same afternoon.

The only fixed things are what the player gets ([CONTRIBUTING.md](../CONTRIBUTING.md), "What stays
fixed"). How to get there is open.

## Where things stand (1.8.1)

The reference Wii against Slippi Dolphin 3.7.0, bots on both sides, through a simulated connection
([TESTING.md](TESTING.md)). Bots change their inputs far more often than people, so the rollback
numbers are the hard case.

| Goal | Where it is |
|---|---|
| 1 delay frame online is a tournament console's input delay | Met up to about 50 ms of ping, and confirmed by an independent tester. 8 ms more up to about 70 ms, a refresh behind above that ([INPUT_DELAY.md](INPUT_DELAY.md)) |
| No frame shown twice or late on the Wii | At about 70 ms of ping: 0 to 1 slow frame in 16 games. At about 100 ms (2 delay frames): about 3 a game |
| A PC player gets the same game as against a PC | Not yet: 1.4 to 2 times the rollbacks (problem 1). No halts at the rollback limit and no unhealthy intervals by Slippi's own match quality rule at either ping |
| No desyncs | None in test games. Two players' reports of a Teams desync are unexplained |
| Teams and Party | Playable, without rollback: the Wii waits for inputs, at a delay set by the lobby's ping (4 frames at about 72 ms). Smooth, but a lot of delay |

## Open problems

Roughly in order of how much they matter. Each says what is known; none has an owner.

### 1. The PC player rolls back more against a Wii than against a PC

In the same bot games through the same simulated connection, a PC player's Dolphin rolls back 1.4
to 2 times as often against the Wii as against a second Slippi Dolphin (30 and 45 ms one way,
2026-11-19). What is known about why:

- **The Wii's input for a frame leaves 1 to 3 ms later than a PC's.** Most of that is the send
  path: a `sendto` takes about 2 ms inside IOS, the Wii's system software, and waits about 0.8 ms
  for the sender thread first, because the opponent's pad arrives right around the Wii's own poll
  and the ack and the pad queue behind each other every frame. The cost is per datagram, not per
  byte. Nothing tried inside the kernel moved it ("Network" below); our guess is that the floor is
  IOS's network stack or the USB adapter, and that going lower means going round IOS's sockets.
  Nobody has tried that.
- **Slippi's time sync turns reply delay into lateness.** Dolphin places the Wii's pads by when
  its acks come back, so the Wii's inputs sit late by about half of (receive delay + queue wait +
  `sendto`). Pushing the Wii's game earlier doesn't help: Dolphin speeds up and takes it back.
- **Input suggestions** (Slippi 3.7.0) help a PC with a GameCube adapter more than they help a
  Wii. Between two PCs, a simulation took rollbacks from 738 to 248 a minute at 30 ms one way.
  That simulation is idealized (every change suggested, 1 to 15 ms ahead) and should be checked
  against real adapters. The Wii sends suggestions too, but the console polls the controller
  twice a refresh, so they come later and cover fewer input changes than a 1000 Hz adapter's.
  Simulated, a Wii suggesting 95% of changes instead of 60% took the PC's rollbacks from 1317 to
  1139 a minute at 30 ms one way.
- The harness's bots write their inputs at the game's read, so a normal run doesn't exercise the
  Wii's own suggestions. The bot-side sender used for the numbers above isn't in `main` yet; ask
  for it.

### 2. Slow frames on the Wii at a high ping

At about 100 ms of ping and 2 delay frames, a 40 second bot game has about 3 slow frames (a frame
shown twice, or one that comes late). At about 70 ms there are next to none.

At that ping nearly every frame is a rollback of one or two frames. A pass with a one-frame
rollback costs 14 to 18 ms against a refresh of 16.7 ms: about 4 ms to load the savestate, 2.2 ms
to capture the new one, the rest Melee running the frames again. Matchups differ: a deeper pass
runs about 13.5 ms with Fox and DK or Sheik and Samus, and 15 to 17 ms with Peach and Luigi, the
Ice Climbers and Link, or Bowser and Game & Watch. Between 85 and 105 ms of ping a game runs with
8 ms more room (step 1 in INPUT_DELAY.md). A heavy matchup that follows a light one can still lose
its timing in the first seconds of play, which costs 2 to 10 slow frames before it settles a
refresh behind. The harness changes characters every game, which makes this look worse than a real
set.

### 3. Rollback in Teams and Party

With four characters, a pass with a one-frame rollback measured 19.9 ms (a normal frame is 11.3
ms), and the savestate store holds only 5 or 6 captures of 310 to 390 pages. So Teams and Party
wait for inputs instead, at 2 to 8 delay frames by the lobby's ping. A lobby that had to wait adds a
frame for its next game. A player's verdict at 4 frames: smooth, but definitely a lot of input
delay. Rollback there needs a pass about half as expensive, or a different idea.

### 4. Lag spikes

How a game on the Wii looks when the connection hitches for 100 ms or so (Wi-Fi on the other
side, a busy line) hasn't been measured cleanly: the stall profiler that those runs used turned
out to cause slow frames itself. The relay can make spikes (`--spikes MS,EVERY_S`).

### 5. Reports from players that are unexplained

Instant disconnects; two Teams desyncs; a USB stick that sometimes fails to mount
after a boot from it. Since 1.8.1 every such problem leaves a report on the card
([TROUBLESHOOTING.md](TROUBLESHOOTING.md)), so there should soon be data.

### 6. Not measured at all

- The sound's timing against the picture (INPUT_DELAY.md, "Sound").
- What a 480p display shows; the reference setup can only capture composite.
- Two Wiis in one game or lobby, and real routers and NAT. Every test runs on one LAN.
- The first game after a boot: the PC still waits up to about 20 frames at its start, against a
  few frames at most for later games. Not the drive; cause unknown.

## What has been tried

### Input delay and timing

**Nintendont's "PDF + 1/2F" lag reduction, online.** It is already in there, in another form.
Slippi's online codeset, which SlippWii runs as Slippi Dolphin does, carries the same code
(tauKhan's polling drift fix with the visual buffer fix). Online, SlippWii's engine drives the
visual buffer part itself: one frame is left undrawn in the countdown, and every frame after it is
shown a refresh sooner. That half frame is what pays for the delay frame the netcode needs, and it
is how 1 delay frame online equals a tournament console offline ([ROLLBACK.md](ROLLBACK.md),
"Frames a refresh sooner"). An independent tester's numbers on 1.6.1, before the timing work,
press to the middle of a CRT: offline through Nintendont about 57 ms; SlippWii at 1 delay frame
about 53 ms; with the loader's *PDF + 1/2F* setting on as well, about 52 ms. On 1.8.0 the same
tester found 1 delay frame in line with their offline results. What the loader's setting does on
top of that online hasn't been measured since. The target is the console's timing, not the lowest
number: a change that makes online faster than a tournament console misses it too.

**Offline, faster.** A fix once brought the visual buffer part back to life offline, which made
offline half a frame faster than Slippi Nintendont's. It was undone on purpose: offline is the
reference that online is measured against, so it stays what Slippi Nintendont gives.

**More controller polls, so the Wii's suggestions go out sooner.** Promising for the PC player
(the 1317 to 1139 above). On hold, because the console's polling is what the input delay rests
on. Two things are known. The game plays the controller's answer from 7.8 ms before the refresh
(Melee polls twice a refresh), and a read earlier than that answer plays one 8.3 ms older, which
was the lag that came and went before 1.8.0. And a faster polling rate would make the delay move
with the read's lead instead of staying constant. A version that shows the game's reads and the
delay untouched ([TESTING.md](TESTING.md), "The input delay check") would be very interesting.

**A first game against an opponent starting a refresh behind** (85 to 105 ms of ping). It removed
most of the slow frames of problem 2: about one left per heavy game. Reverted all the same,
because one game then plays 8 ms later than the next for a reason the player can't see. A rule the
player can know, by ping alone for example, would be a different proposal.

**Waiting out frames when the Wii gets ahead** (after the PC hitches). Dolphin catches up too, the
Wii ends up behind, and that costs more corrections than it saved. Removed.

**Staying level by shortening every video line.** The picture was lost on the capture card.
Ending a frame one line early holds a picture, and is what is used, rarely.

### What a rollback costs

Where a pass's time goes at 45 ms one way: the load restores about 255 pages by DMA at 11.5 to 13
µs a page; a capture takes about 2.2 ms; finding the changed pages takes 0.25 to 0.35 ms each time;
the rest is Melee running the frames again. Tried so far:

| Idea | Result |
|---|---|
| Not restoring pages whose contents didn't change | About half of the captured pages are identical, but a load must still restore every page written since the capture, so nothing was saved |
| A checksum at capture to skip identical pages | The checksum (2.8 µs to fetch a page, 1.4 to 2.8 µs to sum it) costs what the skipped store saves (2.6 µs). Same pass time |
| Restoring by CPU copy instead of DMA | 33 µs a page against 11.5 |
| DMA and CPU copies pipelined | 17.8 µs a page against 13.0: they share the bus |
| Capture's stores interleaved with DMA | Capture 2.14 ms became 2.94 ms, the same contention |
| The newest captures, or the page table, in MEM1 | Only about 680 KB of MEM1 is free in a match; the page table's gain measured 0.1 to 0.3 ms a pass |
| Leaving the GX copy destinations out of the savestate | No measurable gain; put back |
| Two sender threads, thread priorities | See "Network" |

About 320 KB is rewritten every frame and so is in every savestate: Melee's object pool, where the
game writes a counter into every process each frame and animations update. We took that as real
state that has to be saved. Skipping animation work while frames are run again has not been tried;
we expect a desync, since hitboxes and platforms follow animations.

Our read is that the copying itself is near what this hardware can do, so the room left is in
saving or restoring fewer pages, or running less again. That is a read, not a proof.

### Network

| Idea | Result |
|---|---|
| Holding an ack 4 to 8 ms to share a datagram with the next pad | Worse: the Wii's inputs reached the PC 3 to 4 ms late instead of 1.9, and the PC rolled back more (1516 to 1649 a minute against 1317) |
| Acks sent ahead of queued pads | Only 11 to 18 a game change places; an ack mostly waits behind a pad already inside `sendto` |
| Two sender threads | The queue wait fell from 0.7 to 0.5 ms and `sendto` rose from 1.7 to 1.9 ms: IOS sends one at a time |
| Smaller pad packets (113 to 91 bytes) | `sendto` 2025 against 2027 µs |
| The network thread below IOS's own | `sendto` 1.1 ms, but received packets then wait 1.1 ms and polls up to 4.4 ms. No gain overall |
| Wi-Fi | Far too slow and uneven on a Wii. SlippWii checks for a wired adapter before it searches |

In Teams, raising the Wii's delay doesn't help the PC players (Slippi's time sync goes by the
frames' numbers); it stops the Wii's own waiting. A 4 ms hold for late inputs gave 30% fewer waits
at 144 ms of ping.

### Loading

**Disc reads as fast as the card allows.** One load in 27 crashed. Reads are paced instead (a 2 ms
seek, 60 MB/s), and the next game's files are read ahead between games. Together they took the
PC's wait at a game's start from about 23 frames to under one.
