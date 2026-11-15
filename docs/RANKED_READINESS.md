# Towards Ranked

SlippWii plays Direct and Unranked. It does not play Ranked: the menu hides it and the kernel
refuses a Ranked search. This is what has to be true before that changes, what is built and tested
towards it, and what is still open. Whether SlippWii may play Ranked at all is the Slippi team's
decision; nothing here contacts their servers in any new way, and no test contacts them at all.

## What a guest owes the host

SlippWii is not Slippi's client, and its players meet Slippi's players. From what the Slippi team
has said in public about third-party clients, four things matter to them:

1. **A client that Slippi has moved on from must stop queueing.** A Slippi update may break a
   third-party client, and players would blame Slippi for the result.
2. **A client should say what it is**, so that Slippi can tell its games from Dolphin's.
3. **It must effectively never desync.**
4. **Its opponents must not get a worse game** than against Slippi Dolphin.

## Built (2026-11-13)

| Rule | What SlippWii does | Tested |
|---|---|---|
| Slippi's version gate | The Slippi version a build speaks (3.6.4) is compiled in: `app_version` in `config.ini` no longer changes it against Slippi's server. When the server turns the version away and names a newer one, that is kept on the card, the game is told "update required" (as Slippi Dolphin answers itself: the menu locks the online modes), and no search is sent again until a SlippWii for that version is installed. The menu's *Update* and *Exit* then go back to the Homebrew Channel, where SlippWii is updated. | `tools/test_online_gates.py` (the client built for Windows, against a server that turns 3.6.4 away): the lock, across a restart, and its lifting. On the Wii: `run_sweep.py --mm-turns-away 3.6.5`, then `--kernel test_latest_keep=1` |
| A client's name | Slippi's matchmaking has no field for one, in 3.6.4's `create-ticket` or in the `join` of Slippi's development source. Two forms are ready and off (`SO_CLIENT_NAME_MODE`): a `client` key (`"SlippWii/1.5.1"`), or build metadata after the version (`"3.6.4+slippwii.1.5.1"`). | Unit tests; `test_online_gates.py` |
| Wired only | A search is refused when the connection chosen in the Wii's settings is a wireless one and the link's state does not say wired (both read from IOS). | `test_online_gates.py`; on the Wii with `--kernel test_wifi=1` (the search refused, nothing sent). A console really on Wi-Fi: one check by hand, open |
| Slippi's match quality rule | Counted by the kernel for every game and written to its log (`so_mq.c`); scored for the PC opponent by `tools/sweep/match_quality.py`. | Unit tests against the rule in doubles (14000 intervals); the tool against the kernel (`--selftest`, 7000 intervals); on the Wii |

Which form of the name Slippi wants, if any, is theirs to say. A guess cannot be tried without
their server, and a wrong one could stop every SlippWii from being paired.

## Slippi's own yardstick

Slippi's development source (`slippi-rust-extensions`, `match-quality`; read 2026-11-13) ends a
Ranked match in play by a debt each client keeps of what it sees itself:

- Every 150 frames (2.5 s) from frame 120, three signals are judged: the interval's real time (10%,
  50%, 75% over), the average rollback depth (how far the game ran ahead of the opponent's newest
  input: 2, 3, 4.75 frames; Slippi's note: about 105, 140 and 200 ms of ping at 2 delay frames) and
  halts at the 7-frame rollback limit (an event of 5 passes or more, or shorter ones).
- The worst of the three counts: mild +4, bad +8, severe +15, a halt event +9, shorter halts +3; a
  healthy interval -1. At 30 the match is ended as "poor performance", for both players, and
  reported to Slippi's server. Other modes only log that a Ranked match would have been ended.

Which Slippi release carries it is not known here. It makes two questions answerable with numbers:

- **Does a PC player's Dolphin judge a game against a Wii as it would the same game against a PC?**
  `ab.py` plays the same games through the same simulated connection against a second Dolphin and
  against the Wii; its table has the PC player's depth, unhealthy intervals and debt.
- **Would a Wii end matches a PC would not?** The Wii's own count, in its log.

A player's rollback depth is set by the ping and by that player's *own* delay (Slippi's time sync
lines the players up by their pads' frames, which carry the delay). So a Wii that raises its own
delay makes its own rollbacks shallower, which is what its CPU needs at a high ping. It does not
make the PC player's shallower.

## Measured

2026-11-13 and 14, one Wii (wired) against Slippi Dolphin 3.6.4 on one PC, through a relay that
plays a connection (one-way delay, jitter, loss; ping is about twice the delay plus the jitter).
Both players are bots with random inputs, which change far more often than a person's: about 20
to 35 rollbacks a second at these pings. Delay 2 on both sides. One page of this for people outside
the project: [SLIPPI_GUEST_REPORT.md](SLIPPI_GUEST_REPORT.md).

### It stays in sync

| Run | Build | Games | Replays identical, frame for frame | Desyncs, crashes, hangs |
|---|---|---|---|---|
| Overnight, 7 hours (`soak-20261114-002227.md`): 110 games each at about 33, 65, 95 and 128 ms (the last with 1% loss) | 0fb3fc2 | 440 | 440 | 0 |
| The evening before, 2 hours (`soak-20261113-180354.md`): 30 games each at the same four | d92a1af | 120 | 120 | 0 |
| The comparisons below (the Wii as a player's: no replay stream to compare) | both | 110 | not compared | 0 by Slippi's own checksums |

672 games in all (2 more in a check of the exit), none with a line of trouble in the Wii's log.

### What the PC player gets: against a PC, against the Wii

The PC player's own Dolphin, by Slippi's rule (`match_quality.py`), 10 games a row
(`ab-20261113-162100.md` on d92a1af, `ab-20261113-221847.md` on 0fb3fc2; the two builds' netcode
is the same). Depth: how many frames the game ran ahead of the opponent's newest input, averaged.
An interval is unhealthy from 2.0.

| Ping, about | Depth against a PC | Against the Wii | Unhealthy intervals against a PC | Against the Wii |
|---|---|---|---|---|
| 33 ms | 0.00 | 0.00 | 0 of 147 | 0 of 150 |
| 65 ms | 0.33, 0.52 | 0.67, 0.72 | 0 of 342 | 0 of 297 |
| 71 ms | 0.88 | 0.95 | 0 of 213 | 0 of 150 |
| 77 ms | 0.99 | 0.95 | 0 of 222 | 0 of 146 |
| 83 ms | 1.00 | 0.98 | 0 of 223 | 0 of 144 |
| 89 ms | 1.01 | 1.05 | 0 of 227 | 0 of 149 |
| 95 ms | 1.14, 1.11 | 1.54, 1.47 | 0 of 434 | 0 of 298 |
| 128 ms, 1% loss | 2.26 | 2.58 | 204 of 227 | 146 of 147 |
| 158 ms, 1% loss | 3.05 | 3.41 | 279 of 279 | 148 of 148 |

- **A Wii opponent costs the PC player about a tenth of a frame of rollback depth.** Over the six
  connections from 65 to 95 ms, which step across one whole frame of delay, the depth averages
  0.92 against a PC and 1.02 against the Wii: what 3 ms more ping would do. The cause is known
  and measured by the relay (`tools/sweep/input_lateness.py`, and the "later by" column of
  `ab.py`): the Wii's input for a frame reaches its opponent 1 to 3 ms later than
  a PC's would (the Wii's network time in and out; by our reading Slippi's time sync shares it
  between the two players, as the Wii's own depth is up by about as much).
- **A single row can show more or less than that.** Depth counts whole frames, so it moves in
  steps as the ping grows: a connection that sits a millisecond or two under a step shows nearly
  all of the Wii's 1 to 3 ms as depth (65 and 95 ms above, 0.2 to 0.4 frames), one that sits past
  it shows none (77 and 83 ms). The same row also moves by about 0.2 from one run to another
  (65 ms against a PC: 0.33 and 0.52). The average over a frame is the number.
- **Slippi's rule draws its own line near 105 ms of ping at 2 delay frames**, whoever the
  opponent is: at 128 ms two PCs fail it too (a depth of 2 or more adds 4 to the debt each 2.5
  seconds; 30 ends the match). Against a Wii the line is a few milliseconds of ping lower.
- **Before "GO"** the PC waits 3 to 6 frames in the frozen countdown while the Wii finishes
  loading (18 at the lowest ping), against 0 to 2 with a PC. Slippi's rule leaves that stretch
  out, and nobody can move in it.

The overnight games give the same picture with more games (110 a connection, about 67 minutes
of play each; the PC player's score against the Wii): depth 0.00, 0.79, 1.59 and 2.59 at 33, 65,
95 and 128 ms; unhealthy intervals 0 of 1617, 0 of 1636, 4 of 1620 (highest debt 4 of 30) and
1596 of 1606.

### What the Wii's own count says

The kernel keeps the same debt for the Wii's side of every game and writes it to its log. In the
440 overnight games:

| Ping, about | The Wii's depth | Unhealthy intervals | By speed | By halts | By depth | Games past the limit |
|---|---|---|---|---|---|---|
| 33 ms | 0.00 | 0 of 1609 | 0 | 0 | 0 | 0 of 110 |
| 65 ms | 0.53 | 0 of 1630 | 0 | 0 | 0 | 0 of 110 |
| 95 ms | 1.43 | 25 of 1611 | 0 | 0 | 25 (mild) | 0 of 110 (highest debt 7 of 30) |
| 128 ms, 1% loss | 2.34 | 1470 of 1596 | 0 | 0 | 1437 mild, 33 bad | 107 of 110 |

By the two signals that are about the machine and not the connection, the console was never at
fault: no interval ran 10% over its time, and the game never halted at the 7-frame rollback
limit. Everything unhealthy came from depth, at the pings where a PC's game is unhealthy too.

## Open

- **The Wii at a high ping.** Its rollbacks cost it more than a frame from about 120 ms of ping:
  its own picture is then less smooth, and its inputs reach the opponent unevenly (at 158 ms one
  in ten a whole frame late). Slippi's rule does not see that as slowness (the Wii keeps its
  speed), and at those pings the rule ends a 2-delay match between two PCs as well; it is the
  Wii player's cost first.
- **The 1 to 3 ms.** The Wii's network time in and out is the whole measured difference for its
  opponent. Shortening it would close the gap; nothing has been tried yet.
- **A console really on Wi-Fi**: the refusal has been tested with a test switch and on the values a
  wired console reports. One start with the Wii set to a wireless connection is still to be seen.
- **Field numbers.** The diagnostic log now carries Slippi's judgement of every game; no player's
  log has been collected yet. Everything above is one console, one PC, a simulated connection
  and bots: not the internet, and not Slippi's servers.
- **Consoles not seen.** On the console tested both readings say wired (the chosen connection's
  flags `a7`, the link's state 3). A Wii U's Wii mode has not been looked at; a console whose
  settings cannot be read is not refused.

## What Ranked itself needs (not built)

- **Game set-up between the games of a set** (stage strikes and picks): EXI `0xBF`, `0xC0`, `0xC1`
  and netplay packet `0x85` (docs/EXI_DEVICE_SPEC.md).
- **Reporting results** to Slippi's server for every game and at a set's end, with the exchange
  that settles a disconnect or a desync (`0xC2`, `0xC4`, packet `0x86`). This is HTTPS, from the
  kernel, during play. The loader has TLS (for updates); the kernel has none, and its code region
  has about 7 KB free.
- **The rank**, fetched and shown.
- **Ending a match by the rule above**, with the disconnect reason the opponent's client shows.
- Slippi's development source also has a **new matchmaking service**, a WebSocket over TLS
  (`wss://matchmaking.slippi.gg/v1/queue`, with STUN). When Slippi moves to it, a SlippWii without
  TLS in play cannot queue in any mode. So TLS in the kernel is on the path whatever happens to
  Ranked.

All of it would be built and tested against a local server only.
