# Input delay: tournament console, SlippWii online, Slippi on PC

The SlippWii rows describe SlippWii 1.8.0 and later (see "What changed"). A player confirmed them
independently with a light-sensor adapter on a CRT; only the sound is still to be measured (see
"How this was measured").

How long a button press takes to reach the screen, and the speakers, in the three ways Melee is
played competitively. The reference is the console a tournament puts on its tables.

## The short version

From the press to the start of the picture that shows it:

| Setup | Press to picture | |
|---|---|---|
| **Tournament console**: Wii, CRT, Slippi Nintendont, UCF 0.84, polling drift fix on | **50 ms** | steady |
| The same console with the polling drift fix off (Slippi Nintendont's default) | 54 ms on average | moves between 46 and 63 ms, in a 17 s cycle |
| **SlippWii online, 1 delay frame** | **50 ms** | steady; the tournament console's, exactly |
| SlippWii online, 2 delay frames | 67 ms | steady; one frame more |
| Slippi on PC, 2 delay frames, a 120 Hz or faster monitor | "in line with CRT", by Slippi's FAQ | depends on the PC and the monitor |

A CRT draws its picture top to bottom in 16 ms: add 1 ms for the top line, 9 ms for the middle,
17 ms for the bottom, the same in every row.

So on a Wii, **1 delay frame is the setting that plays like a tournament console**, as 2 delay
frames is on a PC. A PC needs the second frame of delay less than a console does, because
Dolphin takes about a frame and a half less than a console to put a frame on the screen; a Wii
is a console.

## Where the time goes on a console

1. **The press waits for the console to ask.** The console asks the controller for its state
   twice per refresh, at fixed moments: about 16.1 ms and 7.8 ms before each new picture starts.
   A press is taken at the next of these.
2. **The game reads the last answer** once per frame. Stock Melee does this on a 60.00 Hz timer
   while the picture runs at 59.94 Hz, so the reading drifts through the refresh every 16.7 s,
   and the delay with it: that is *polling drift*. The polling drift fix reads once per refresh,
   at the start of the picture, which makes it the answer of 7.8 ms before every time.
3. **The frame is worked out and drawn, and shown** two refreshes later.

With the polling drift fix: on average 8.3 ms until the game's next reading comes round, 8.1 ms
that the answer is old by then, and 33.4 ms to the picture: 49.8 ms. Without it: 54.5 ms on
average, between 46.2 and 62.8 ms by where the drift stands.

## SlippWii online

Online, the game reads the controller a few milliseconds *before* the picture starts rather than at
it, but after that same answer of 7.8 ms before, so it plays the same answer the tournament
console plays. The frame that uses it is on the screen one refresh sooner after its work than
offline, which is what makes room for one frame of netcode delay:

| | From the controller's answer to the picture |
|---|---|
| Tournament console (polling drift fix) | 2 refreshes after the next picture's start |
| SlippWii online, 1 delay frame | the same |
| SlippWii online, 2 delay frames | 3 |

**2 delay frames cannot equal a console on a console.** It would take a frame worked out, drawn
and on the screen within 8 ms of its reading, rollbacks included; a Wii needs 6 to 17 ms. What a
second delay frame buys is rollbacks one frame shallower, as on a PC.

**When the connection is too heavy** for a frame's work to fit in its refresh (deep rollbacks: a
high ping for the delay chosen), SlippWii keeps the timing as close to the tournament console's
as the connection allows, in steps, and keeps it the same through a game:

| Step | Press to picture, 1 delay frame | 2 delay frames | Each frame's room for its work |
|---|---|---|---|
| 0: the tournament console's | 50 ms | 67 ms | about 19 ms |
| 1: the console's other answer of the refresh, 8.3 ms older | 58 ms | 75 ms | about 27 ms |
| 2: a refresh behind | 67 ms | 83 ms | a whole refresh more |

- **Each game starts at step 0** while the ping allows it (under about 50 ms at 1 delay frame, 85
  at 2), at step 1 a little above that (to about 70 and 105), and a refresh behind beyond.
- **The opponent's last games count too**: after a game whose frames did not fit, the next starts
  a step lower, and games that fit bring it back up.
- **In a game**, a frame that comes late costs one frame shown twice and the next one not shown;
  the timing stays. Only a second late frame within 10 seconds steps the game down, once, for the
  rest of the game. The timing never moves back up within a game, so the lag a player's hands have
  settled on changes at most once.
- **Input suggestions** (Slippi 3.7.0): a PC player on a GameCube adapter sends their input
  between frames as soon as it changes, and SlippWii predicts with it, as Slippi Dolphin does.
  Fewer and shallower rollbacks leave more of each frame's room free, so step 0 holds at higher
  pings against such players. SlippWii sends its own the same way: the console polls the controller
  twice a refresh, and the answer the game reads later goes out as a suggestion as soon as it comes.
  *[Tested with a person on each side, 2026-11-17: a GameCube adapter at 1000 Hz on the PC, a
  GameCube controller on the Wii; no suggestion refused either way, no desync.]*

Measured with bots against Slippi Dolphin 3.7.0 on a simulated connection, 2026-11-17, 8 games
each (bots change their inputs far more often than people, so these are the hard case):

| Delay, ping | Games at step 0 / 1 / behind | Frames shown twice | As 1.6.1 played it |
|---|---|---|---|
| 1, 38 ms | 8 / 0 / 0 | none | step 0 |
| 1, 50 ms | 4 / 4 / 0 (a hard first game, then three at step 1 while it counted) | 1 in 5 minutes | a refresh behind |
| 1, 70 ms | 0 / 8 / 0 | none | a refresh behind |
| 2, 83 ms | 4 / 4 / 0 | none (1 frame not shown) | a refresh behind |
| 2, 100 ms | 0 / 5 / 3 | 0.4 a minute | a refresh behind |
| 1, 100 ms | 0 / 0 / 4 (over the limits) | 30 a minute | the same |

(The rows at 50 and 83 ms are from the final code; the others from the first version of the steps,
whose two later fixes change only games that lost their timing in the countdown and games over
the limits.)

At 1 delay frame and 100 ms of ping even a refresh behind is not enough for the Wii's rollbacks:
choose 2 there.

## Slippi on PC

From Slippi's FAQ: "Since we have removed about 1.5 frames of visual delay from Melee, adding 2
frames brings us very close to a CRT experience. Using a 120hz+ monitor removes an additional half
frame giving to bring us in line with CRT." The recommended default there is 2 delay frames, up to
130 ms of ping.

Published measurements with a light sensor (press to the sensor's point on the screen; other
people's, not ours):

- A real console on a CRT, polling drift fix on: 62 ms. Slippi on a 144 Hz monitor with no delay
  frames: 24 ms, so about 57 ms with 2 (Dolphin Progress Report, December 2025; the delay frames
  added here).
- Slippi at 2 delay frames on a 300 Hz monitor: about 52 ms, "5 to 6 ms faster than CRT" (a
  player's report, April 2026).

## Sound

On a console the sound of an action comes before its picture: about 17 ms before, in Kadano's
oscilloscope measurements (sound 38 ms after the press on average, picture 56 ms, polling drift
fix off).

The sound belongs to the game's frame, and a delay frame delays the frame. SlippWii online shows
the frame sooner after its work than offline, which hides the first delay frame from the eyes
but not from the ears: at 1 delay frame the frame's work starts about 10 ms later after a press
than on a tournament console, at 2 about 26 ms, and the sound with it; picture and sound are
that much closer together than offline. *[From the engine's timing; not measured yet.]*

On a PC the sound depends on the audio settings: Kadano measured Dolphin's default about 50 ms
later than a console's, and close to it with WASAPI's exclusive mode.

## Which delay to choose on SlippWii

- **1** for the tournament console's timing: exactly it under about 50 ms of ping, 8 ms more up to
  about 70.
- **2** above that: one frame more than a tournament console, to about 85 ms; 8 ms more again to
  about 105.

Your delay is your own: it changes nothing for your opponent.

## What changed

Up to 1.7.0 the reading could come 0.2 ms *before* the controller's answer it should have played,
in games with deep rollbacks or after a long stretch of time-sync corrections. It then played the
answer before: 8.3 ms more delay, coming and going within a game. Since 1.8.0 every reading
comes after that answer.

Up to 1.7.0, a game's timing had two settings, the tournament console's or a whole refresh behind,
and one late frame in play left the game behind for the rest of it. Now there is a step between,
a late frame is put right at once, and the ping limits are higher (above).

## How this was measured

- **Inside the console**: a probe in the game times each press from the game's reading of the
  controller to the refresh that shows the frame with it, to the microsecond (`tools/latency`).
  On the test Wii, 2026-11-17: stock 41.8 ms (35.1 to 48.6 over the drift), polling drift fix
  33.3 ms, online at 1 delay frame 40.4 ms and at 2 57.0 ms (with the reading 7.0 ms before the
  refresh).
- **Before the reading**: when the console asks the controller is not something the game sees.
  It is taken from the hardware's own setting (131 lines between the two, read back from the
  console) and from a look at when the answers arrive. In stock timing the reading's place in the
  refresh, and so the answer's age, follows from the drift.
- **Checks against others' measurements**: Kadano's oscilloscope figure for stock Melee is 55.7 ms
  on average (42.4 to 66.8); this page's, at the top of the picture, 55.8 (47.5 to 64.1). A
  player with a light-sensor adapter found 1 delay frame on SlippWii 1.6.1 3.7 ms faster than a
  console with the polling drift fix off; this page says 4.7.
- **Independent confirmation**: the same player measured 1.8.0 the same way (real presses, a
  sensor at the middle of a CRT, against the same test on offline Melee) and found 1 delay frame
  "perfectly in line" with their offline results (2026-11-19). This is the figure every change
  must keep ([TESTING.md](TESTING.md)).
- **Still to confirm**: the sound, with a recording of the console's output.

## Sources

- Slippi Launcher FAQ: https://github.com/project-slippi/slippi-launcher/blob/main/FAQ.md
- Slippi Nintendont's settings (the polling drift fix, "PDF"; default off):
  https://github.com/project-slippi/Nintendont/blob/slippi/common/config/MeleeCodes.c
- UCF 0.84 (PracticalTAS: "the polling drift fix isn't part of UCF but is often used alongside
  it"; the UCF 0.84 memory-card files include one):
  https://www.reddit.com/r/SSBM/comments/17t2g9y/ and
  https://github.com/PracticalTAS/melee-gci-compiler/tree/main/ucf0.84
- Rulesets that ask for it, for example Kill Roy Volume 9: "All setups (CRT only) will require
  UCF 0.84 and utilize Lag Reduction if possible using the Polling Drift Fix code built into
  Slippi Nintendont": https://www.start.gg/tournament/kill-roy-volume-9/event/melee-singles/overview/rules
- Kadano, SSBM input lag: https://kadano.net/SSBM/inputlag/ and
  https://kadano.net/SSBM/inputlag/audio.html
- Dolphin Progress Report, Release 2512:
  https://dolphin-emu.org/blog/2025/12/22/dolphin-progress-report-release-2512/
- A player's report at 300 Hz: https://www.reddit.com/r/SSBM/comments/1sg0lp9/
