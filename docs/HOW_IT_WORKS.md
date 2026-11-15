# How SlippWii works

## On a PC

Slippi Online on a PC is two halves:

- **Slippi's online code** ([slippi-ssbm-asm](https://github.com/project-slippi/slippi-ssbm-asm)):
  Gecko codes Slippi Dolphin loads into Melee. They add the online menus, the character select
  screen's online panel and the per-frame logic: send my inputs, get the opponent's, predict and
  roll back.
- **Slippi Dolphin's EXI device** (`EXI_DeviceSlippi`): the "Slippi" device the code talks to over
  Melee's EXI bus. It does matchmaking with Slippi's server, connects to the opponent (ENet over UDP),
  exchanges inputs, serves Slippi's menu files and records the replay.

## On a Wii

Slippi Nintendont already runs Melee on a Wii and emulates EXI devices in its kernel, which runs on
the Wii's ARM coprocessor (Starlet) beside the game. SlippWii adds the online half there
(`Nintendont/kernel/slippi_online/`):

- `so_exi.c`: Slippi Dolphin's online EXI commands (layouts in [EXI_DEVICE_SPEC.md](EXI_DEVICE_SPEC.md)).
- `so_mm.c`: matchmaking with `mm.slippi.gg` (the same protocol Slippi Dolphin 3.6.4 speaks).
- `so_netplay.c`: the ENet peer connection and the input exchange, Slippi's netplay protocol.
- `so_socket.c`, `enet/ios.c`: UDP through the Wii's IOS network stack.
- `so_files.c`: Slippi's online code (`netplay.gct`, served to Slippi's bootloader at start-up) and
  menu files, from `SD:/slippwii/`. Melee's own menu files with Slippi's changes can't be shipped,
  so the loader makes them before Melee starts, on the first start (`loader/source/slippwii.c`):
  Slippi's patches (`slippwii/patches/*.diff`) applied to the disc being started, each result
  checked against the release's manifest (the patches carry no checksums of their own).
- `so_client.c`, `so_config.c`: the client's threads and `config.ini`.

### Rollback

Since 1.4.0 the Wii predicts and rolls back as Slippi Dolphin does. Slippi's own rollback code asks
the EXI device for the opponent's inputs, or leave to predict them, and for savestates. A PC makes
savestates from outside the game; a Wii has no outside, so they are made by a small engine inside
Melee, on the Wii's CPU, which tracks the game's writes with the MMU and copies only what changed.
See [ROLLBACK.md](ROLLBACK.md).

Without the engine's memory (next to a virtual memory card of 1019 blocks or more) or with
`rollback=0` in `config.ini`, the Wii doesn't predict: when the opponent's input for a frame hasn't
arrived, the kernel tells the online code to *skip* the frame (Slippi's own "wait" answer) and the
game holds until it does (*lockstep*; releases up to 1.3.1 played this way). The opponent's Slippi
Dolphin still predicts and rolls back as usual. Either way both sides run the exact same inputs, so
the games stay in sync. Desync detection (Slippi's per-frame checksums) works as on PC.

### Four players

Teams and Party are games of four: three opponents, each on a connection of its own. Four things
are different from one opponent.

- **No rollback.** A rollback loads a savestate and runs the frames since again, all within one
  refresh. With two characters the Wii fits that; with four it does not: a frame with nothing to
  run again takes 11 ms of the 16.7, one with a single frame to run again 20, and with three
  opponents some input changes on nearly every frame. Measured with every input shown one frame
  late: the game correct (its replay the same as Dolphin's), but 6 refreshes in 10 without a new
  picture and the game at 86% of its speed, its opponents waiting for it. So with more than one
  opponent the game waits for every input, the *lockstep* of the paragraph above, and its opponents
  roll back as against anyone.
- **Delay by the ping.** A game that waits needs each input before its frame, and what gives an
  input the time to arrive is the delay: it leaves its player that many frames early. Slippi's 2
  frames cover about 27 ms of ping here, so the Wii takes a frame more for each 33 ms past that, up
  to 6 (the lobby's slowest connection counts; a setting that is higher stays). Slippi's time sync
  goes by the frames' numbers, so the opponents simply run that much ahead of the Wii: they see its
  inputs no later, and play at their own delay.
- **Level with the others.** With one opponent the rollback engine keeps the Wii level with it to
  a millisecond or two. A game that waits has no engine, and Dolphin corrects in one direction only
  (it runs faster the moment it is behind, slower only from about 8 ms ahead), so left alone the
  three PCs sat 25 to 30 ms ahead of the Wii and saw its inputs that late. The kernel now runs a
  frame early (two in one refresh) when it reads itself 11 ms behind on average.
- **One datagram an opponent a frame.** The Wii's system software (IOS) sends one datagram at a
  time and takes 2 to 3.5 ms for each. A pad and an acknowledgement apart, to each of three
  opponents, are six a frame: more than a frame's worth. So an acknowledgement waits for the next
  pad to the same opponent and travels with it.

What three PC players get from a Wii in their lobby, against a fourth PC in its place (Slippi's
match quality rule's measure, the frames each ran ahead of that player's inputs on average; three
games of Teams a row through `tools/sweep/lobby.py`, 2026-11-14):

| Ping | A fourth PC | The Wii | The Wii's delay |
|---|---|---|---|
| LAN | 0.00 | 0.21 | 2 |
| 33 ms | 0.01 | 0.23 | 3 |
| 63 ms | 0.11 | 1.11 | 4 |
| 95 ms | 1.09 | 1.97 | 5 |
| 128 ms | 2.28 | 2.35 | 6 |

The Wii's inputs leave it 7 to 12 ms after a PC's own for the same frame (its three datagrams go
one after the other, and the PCs take its acknowledgements, up to a frame late, for the line's
time): half a frame of rollback on average, a whole one at a ping that sits just under a frame's
edge, as 63 and 95 ms do. A player who leaves in the middle of a game is taken out of it on the
same frame on every console, by Slippi's own rule.

What was tried on the way, what was dropped and why, what is not tested, and what to do next:
[FOUR_PLAYERS.md](FOUR_PLAYERS.md).

### Things a console needs that Dolphin doesn't

- **Music:** Dolphin plays the music itself and the codeset mutes the game's. On a Wii those hooks
  are left out, so the game plays its own music. The loader's *No Music* switch puts the two lines
  that mute it back (`MuteMusic.asm`, `PreventMusicAlarm.asm`), with nobody to play it instead:
  silence, as a player who turns the music off wants.
- **"Normal Lag Reduction"** writes frames to a display buffer that only exists in Dolphin; on a
  console the GPU's write hangs the bus, so that code is dropped. "Polling Drift Fix + VB" [tauKhan]
  stays. Its first half reads the controller once a refresh, at the retrace. Its second half (a
  frame left undrawn after one that waited for a buffer, so the next goes up a refresh sooner) is
  the rollback engine's to use online, and is left off offline, where SlippWii keeps the timing of
  Slippi Nintendont's default, the polling drift fix (docs/ROLLBACK.md, "Offline").
- **Stage files loaded mid-game** (Pokémon Stadium's transformations) are kept in RAM, as Dolphin
  does. Reading them from the disc during a game stalled Melee long enough to drop inputs.
- **A way out.** A PC player closes a window. A game under Slippi Nintendont could only be left
  by turning the console off (Nintendont's own exit buttons are part of its controller code, which
  Slippi Nintendont does not use: the game reads the controller ports itself). The Online Play
  menu has an option Slippi no longer shows, *Log-out*, with a place in the list, an "Are you
  sure?" box and a command to the EXI device. SlippWii shows it as **Exit**
  (`tools/ppc/online_menu.s`): the kernel takes the command as "leave" and sets a word the game
  reads at every retrace (`tools/ppc/exit.s`). The game stops there; the kernel closes the devices
  and the memory card and has IOS start the Homebrew Channel, which restarts the console into it
  (about 7 seconds). Nintendont's own way back, through a reload stub on the game's side, never
  got back on a console here. The option's name and the line under the menu are the only things
  changed in Slippi's menu files; the loader puts them in after making the files
  (`tools/menu_edits.py`). Slippi's *Update* option leaves the same way.
- **Wired only:** an IOS `sendto` over Wi-Fi takes 13-18 ms, too slow for 60 inputs a second each
  way. Over the USB LAN adapter it takes well under 1 ms. A search is refused on Wi-Fi: the kernel
  asks IOS which of the Wii's connections is in use (the one chosen in the Wii's settings, and the
  link's state) and the game shows "Online needs a wired LAN adapter (no Wi-Fi)".
- **Input delay** is set on the character select screen (`tools/css_delay/`), since there's no
  Dolphin settings window. As on Dolphin it can change while connected: Slippi's code reads it at the
  start of each game.
- **What only Dolphin tolerates.** Slippi's online code only ever ran in Dolphin, which forgives what a
  console doesn't. Two cases are handled in Melee's own codes: floats at unaligned addresses
  (`tools/ppc/crash_report.s` performs them) and strings at wild addresses (`tools/text_args/`:
  Slippi's ledge grab limit message passes `%s` the value 1, which crashed a console at the end of a
  game on the timer).
- **The ping display** that Dolphin draws over the game is drawn by the game here
  (`tools/ping_display/`), on the HUD Slippi's code makes for its own "Delay" text. The value is the
  same measurement: the round trip of the last pad acknowledged.

The research and measurements that led here are in [research/FEASIBILITY.md](research/FEASIBILITY.md).

### Being a guest on Slippi's servers

SlippWii is not Slippi's own client, and plays against people who are on it. Three rules follow.

- **One Slippi version, and Slippi's say on it.** A build speaks one Slippi Dolphin version
  (3.6.4) and reports it to matchmaking. That number is compiled in; `config.ini` cannot change it
  against Slippi's server. When Slippi's server turns the version away (it names its latest when
  it does), SlippWii keeps that on the card and tells the game "update required", the answer Slippi
  Dolphin gives itself: the main menu locks the online modes, and no search goes out again until a
  SlippWii for the new version is installed. What the menu then offers, *Update* and *Exit*, both
  go back to the Homebrew Channel: a console is updated from SlippWii's start menu. So after a Slippi update an old SlippWii stops
  queueing by itself, where Slippi's own check would stop an old Dolphin.
- **A name of its own, when Slippi says where.** Slippi's matchmaking messages have no field for a
  client's name. Two forms are built and off (`SO_CLIENT_NAME_MODE` in `so_config.h`); which one is
  the Slippi team's call.
- **The same judgement of a game.** Slippi's development source ends a Ranked match by a "match
  quality" debt: every 2.5 seconds the worst of the game's speed, its average rollback depth and
  its freezes adds to it or takes from it, and 30 ends the match. SlippWii counts the same debt in
  every game (`so_mq.c`) and writes into its log how Slippi would have judged it. Nothing is ended
  by it: SlippWii does not play Ranked. `tools/sweep/match_quality.py` gives the PC opponent's
  score of the same games.

### Updates

*Check for Updates* runs in the loader, before any game (`loader/source/update.c`), on the network
the Wii already has for online play.

- **What it asks.** One small text file, `update.txt`, from the latest release on GitHub
  (`releases/latest/download/update.txt`): the version, where the release's zip is, its size and
  SHA-256, and a few lines of what is new. If the version is newer than the one running and the
  player says yes, it fetches the zip: the same file a person extracts onto the card.
- **Why a file is trusted.** `update.txt` ends with a signature made with SlippWii's release key
  (ECDSA P-256), whose public half is compiled into the loader. The loader reads nothing of a file
  that is not signed with it, and installs no zip but the one whose SHA-256 the signed text names.
  That holds whatever server answers and whoever sits on the connection, so the connection's
  certificates are not checked: a Wii's clock is often wrong, which alone fails such a check, and no
  certificate could say that a file is SlippWii's anyway.
- **TLS.** GitHub takes TLS 1.2 and later; the Wii's own SSL (IOS, 2006) is older. The loader brings
  BearSSL, which needs no operating system: it is given a socket's read and write.
- **Onto the card.** The zip is unpacked in memory first, every file against its CRC-32 and against
  a list of what a release may contain (the app, Slippi's files, the notes beside them; never the
  account, the settings or the logs). Then each file is written under a temporary name and read
  back; only when all are there do the new ones replace the old, the app itself last; and they are
  read back once more. A card that fails before the replacing leaves the old version untouched.
- **What leaves the console:** the two requests, to GitHub, when the button is pressed. No account
  data, no identifier.

The part that is the same on a Wii and a PC (`loader/source/update/`) is tested on a PC against a
web server that plays GitHub and a folder that plays the card (`tools/test_update.py`), and on a
Wii by a test build that updates into a folder of its own (`tools/update_wii_test.py`).

## Testing without a second person

- `tools/online_selftest.py pc --game`: the kernel's online client, built for Windows, against a
  fake Melee and a fake opponent.
- `tools/sweep/run_sweep.py`: the Wii against a local copy of Slippi Dolphin on the same PC, with a
  local fake matchmaking server. Both players are driven by an autoplay Gecko code through the menus
  and random games; the Wii's replays are compared frame by frame with Dolphin's
  (`tools/slp_compare.py`). It never contacts Slippi's servers.
