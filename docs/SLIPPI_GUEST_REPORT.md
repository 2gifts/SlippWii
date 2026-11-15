# SlippWii as a guest on Slippi: what we measured

2026-11-14. For the Slippi team and anyone who plays against a Wii.

SlippWii runs Slippi's netplay on a real Wii (a fork of Slippi Nintendont, open source). Its
players meet Slippi Dolphin players in Direct and Unranked, and from 1.6.0 in Teams and Party (a
section of their own, below). It does not play Ranked: the option is
hidden and the client refuses the search. This page is about what a third-party client owes
Slippi and its players, and what we can show for each point. Every number is from automated runs
of one wired Wii against Slippi Dolphin 3.6.4 through a simulated connection, both sides at 2
delay frames, played by bots. None of it touched Slippi's servers.

The measurements are of 1.6.0 (released 2026-11-14) and of the builds just before it, which have
the same one-against-one netcode. The first and third points below are new in 1.6.0.

## It stops queueing when Slippi moves on

A build speaks one Slippi version (3.6.4) and reports it; players cannot change that number. When
the matchmaking server turns the version away and names a newer one, SlippWii locks its online
modes, as Slippi Dolphin does, and sends no search again until a SlippWii for the new version is
installed. Tested against our own test server only.

## It will say what it is

Matchmaking has no field for a client's name, so SlippWii sends none today. Two forms are built
and switched off (a `client` key, or build metadata after the version). Tell us which, or another,
and it goes out in the next release.

## Wired only

SlippWii refuses to search on Wi-Fi: on a Wii each send over Wi-Fi takes 13 to 18 ms, and the
opponent would get every input that late.

## It stays in sync

| Games | Both replays compared frame for frame | Identical | Desyncs by Slippi's checksums, crashes, hangs |
|---|---|---|---|
| 690 | yes | 690 | 0 |
| 170 more | no | | 0 |

440 of the 690 are one 7-hour run, at about 33, 65, 95 and 128 ms of ping (the last with 1%
loss). 130 of them, and 60 of the others, are on the code as released.

## What a PC player gets against a Wii

We ported the match-quality rule from `slippi-rust-extensions` and scored the PC player's own
game with it, against a second Dolphin and against the Wii, same connection, 10 games each.
"Depth" is the rule's average rollback depth; an interval is unhealthy from 2.0.

| Ping, about | Depth against a PC | Against the Wii | Unhealthy intervals against a PC | Against the Wii |
|---|---|---|---|---|
| 33 ms | 0.00 | 0.00 | 0 of 147 | 0 of 150 |
| 65 to 95 ms, six connections | 0.92 | 1.02 | 0 of 1265 | 0 of 885 |
| 128 ms, 1% loss | 2.26 | 2.58 | 204 of 227 | 146 of 147 |
| 158 ms, 1% loss | 3.05 | 3.41 | 279 of 279 | 148 of 148 |

- Against a Wii the PC player's rollbacks are about a tenth of a frame deeper, as with 3 ms more
  ping: the Wii's input for a frame arrives 1 to 3 ms later than a PC's would. At a ping that sits
  just under a whole frame it can show as 0.2 to 0.4 frames; at others as nothing.
- Up to about 95 ms no interval was unhealthy against either opponent. Above about 105 ms the rule
  fails a 2-delay match between two PCs as well; against a Wii that line is a few milliseconds
  lower.
- In the 7-hour run (110 games a connection against the Wii) the PC player had 0, 0 and 4
  unhealthy intervals of about 1600 each at 33, 65 and 95 ms; the highest debt was 4 of 30.

## What the Wii's own count says

SlippWii keeps the same debt for its own side of every game and logs it. In those 440 games no
interval ran 10% over its time and the game never halted at the rollback limit, at any ping. Its
unhealthy intervals were all by depth: 0, 0, 25 of 1611 (highest debt 7 of 30) and, at 128 ms,
nearly all, as for a PC there.

## Teams and Party

New in 1.6.0; before it, a Wii in one of these lobbies broke the game for all four. Here the Wii
does **not** roll back: four characters are more than it can run again within a frame (measured:
the game at 86% of its speed). Its game waits for every input instead, at an input delay of its
own that rises with the ping (2 to 6 frames), and its three opponents roll back as against anyone.

The three PC players' depth against a fourth PC in the Wii's place, and against the Wii (Teams, 3
games each, with the unhealthy intervals of all):

| Ping, about | Against a fourth PC | Against the Wii |
|---|---|---|
| 33 ms | 0.01, 0 of 135 | 0.23, 3 of 135 |
| 63 ms | 0.11, 0 of 147 | 1.11, 6 of 135 |
| 95 ms | 1.09, 2 of 303 | 1.97, 62 of 135 |
| 128 ms | 2.28, 247 of 330 | 2.35, 102 of 135 |

- No desync in about 40 games with the Wii, a player leaving in the middle of one included (by the
  replays, or by the Wii's checksums where its replay was not recorded). That is far fewer games
  than above, none long, none with loss.
- The Wii's input for a frame reaches the others 7 to 12 ms later than a PC's would: as a player
  with 15 to 25 ms more ping. At about 95 ms that is the difference between healthy and unhealthy
  by the rule, half the time.
- Party is a public queue: there this cost falls on players who did not choose a Wii.

What was tried, and what would close the gap: [FOUR_PLAYERS.md](FOUR_PLAYERS.md).

## Where a Wii differs

- Before "GO" the PC waits 3 to 6 frames in the frozen countdown while the Wii finishes loading
  (18 at the lowest ping), against 0 to 2 with a PC.
- Above about 120 ms of ping the Wii's rollbacks take longer than a frame at times. Its own
  picture is less smooth, and its inputs arrive unevenly (at 158 ms, one in ten a frame late).

## What these tests are not

One console, one PC, a simulated connection, and bots whose inputs change far more often than a
person's (20 to 35 rollbacks a second). Not the internet, not many consoles, not people. About
fifty people play on SlippWii today. From 1.6.0 a console logs the same match-quality
judgement for every game (when its diagnostic log is on), so numbers from real matches can follow.

## Ranked

Not built: the set flow, result reporting (which needs TLS in the console's kernel), the rank, and
ending a match by the rule above. We would build and test all of it against a local server, and
turn nothing on without your say.
