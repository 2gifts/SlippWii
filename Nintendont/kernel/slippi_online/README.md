# Slippi Online in the Nintendont kernel

This module lets a Wii running Slippi Nintendont play Slippi online against
Slippi Dolphin. It speaks the same protocols as Slippi Dolphin (Ishiiruka
v3.6.4) on the wire: matchmaking at `mm.slippi.gg`, and the peer-to-peer pad
exchange. The Wii never rolls back. It plays in lockstep: a frame runs once
the opponent's pad for that frame has arrived. The PC side keeps its usual
rollback.

Status: milestones M2 (the network client, tested on hardware) and M3 (the
online EXI device, tested on the PC with a fake Melee). The game needs
Slippi's online codeset and menu files to use it; loading those is M4.

## Layout

| File | Role |
| --- | --- |
| `so_platform.[ch]` | IOS services: time, mutexes and events (message queues), threads, log ring, status, file reads |
| `so_util.[ch]` | Bounded `snprintf`, number and IPv4 parsing, big-endian field helpers |
| `so_alloc.[ch]` | First-fit heap in a fixed 512 KB MEM2 arena (0x12E00000, taken from SlipMem) |
| `so_socket.[ch]` | Non-blocking UDP: async IOS sends from a buffer pool, a receive thread feeding a ring |
| `enet/` | ENet 1.3.13 (Slippi's version, MIT) plus `ios.c`, its platform layer; see `enet/README.md` |
| `so_json.[ch]` | Allocation-free JSON reader and a small writer |
| `so_config.[ch]` | `config.ini` and the Slippi account (`user.json`) |
| `so_mm.[ch]`, `so_mm_msg.c` | Matchmaking: the ENet state machine, and the create-ticket / get-ticket-resp messages |
| `so_mm_ws.c`, `so_ws*.[ch]`, `so_tls.[ch]`, `so_tcp.[ch]`, `so_stun*.[ch]` | Matchmaking as Slippi Dolphin does it after 3.6.4, used only when `mm_url` is set: a WebSocket over TLS (BearSSL) on a worker thread, after a STUN lookup. See `docs/NEXT_SLIPPI.md` |
| `so_pads.[ch]` | Pad queues, acks, ping and time sync, and a lost player's inputs (the drop report after 3.6.4): no networking, unit-tested. A pad is 8 bytes on the wire for 3.6.4 and 9 after it |
| `so_netplay.[ch]` | Peer-to-peer netplay (`SlippiNetplayClient`): connections, messages 0x80-0x86, and 0x87 and 0x88 of the version after 3.6.4 |
| `so_client.[ch]` | Public entry points, the network thread, retries, the PC log sink |
| `so_exi.[ch]` | The online EXI device: Slippi Dolphin's online commands (0xB0-0xC4, 0xD0-0xD8, 0xE1-0xE5), lockstep 0xB0 |
| `so_text.[ch]` | Game strings, connect codes and premade text (chat, logout dialog), as Dolphin encodes them |
| `so_files.[ch]` | Replacement game files and the codeset served over 0xD1-0xD4 |
| `so_selftest.[ch]` | Hardware self-test: a Direct search, then a synthetic lockstep match |
| `tests/` | Host unit tests and a Windows build of the client (`tests/pc`) |

## Threads and timing

- **Network thread** (priority 0x78). It waits on one wake event, which fires when a datagram arrives, when a request comes in, or after 5 ms. It then services ENet with a zero timeout, under the client lock.
- **Receive thread** (0x78). It blocks in IOS `recvfrom`, stores each datagram with its arrival time, and signals the wake event.
- **Send completion thread** (0x78). It recycles send buffers as IOS completes the asynchronous sends.
- **Callers**: the game's EXI device (`so_exi`, on the kernel main thread, from `EXI.c`) or the self-test. They take the client lock around `so_np_*` calls. A pad is sent and flushed from the caller's thread, so it leaves at once.

IOS socket calls take several milliseconds: a blocking `sendto` took 8-11 ms on the USB LAN adapter in M1. Nothing on the netcode's path waits for one. Wi-Fi is too slow for 60 Hz play (M1b); the LAN adapter is required.

There is one UDP socket per session. Matchmaking and netplay use it in turn, on the same port, the way Dolphin does with a forced netplay port. Closing a socket while the receive thread is blocked on it is avoided on purpose.

No packet is sent until the game asks for a match, or until `boot_quiet_s` (60 s) after start-up. Network traffic during Melee's boot crashed it (M1).

## Replays

With the loader's **Slippi Replays** on, `SlippiFileWriter.c` saves the replay stream to `/Slippi/` on
the drive the game isn't on, as on Slippi Nintendont: game on SD, a USB drive (hot-pluggable); game on
USB, the SD card (none: the kernel stops at start-up and the loader says so). It runs below the main
thread's priority, and its writes block in an IOS call, so the game isn't waiting for them. FatFS is
reentrant across drives, so only opening a file takes `so_fs_lock`, for the path buffer `ff_utf8.c`
shares. A file is synced every 10 s, so a game cut short still leaves a readable replay.

## Files on the SD card (or USB drive)

| Path | What |
| --- | --- |
| `/slippwii/user.json` | Slippi Launcher's `user.json`, copied as is. Its play key is only ever sent to the matchmaking server, inside a create-ticket, and it is never logged. |
| `/slippwii/config.ini` | Optional `key=value` settings. Comment lines start with `#` or `;`. |
| `/slippwii/slippi-version.txt` | Written by SlippWii: the newest version Slippi's matchmaking server has named (see *Slippi's version*). |

| Key | Default | Meaning |
| --- | --- | --- |
| `delay` | 2 | Online input delay, 1-9 frames (the CSS buttons save `delay.txt`, which wins) |
| `mm_host`, `mm_port` | `mm.slippi.gg`, 43113 | Matchmaking server (tests use a PC on the LAN) |
| `mm_url` | (none) | `wss://host[:port]/path`: the WebSocket matchmaking service of Slippi Dolphin after 3.6.4, in the place of `mm_host` (an `mm_host` line after it chooses the old service again). For tests: against Slippi's own server the service is the build's, not a setting, and no released Slippi uses the new one yet (`docs/NEXT_SLIPPI.md`) |
| `stun_hosts` | Slippi's four | With `mm_url`: the STUN servers asked where the netplay socket is seen from, `name:port,name:port,...` |
| `app_version` | `3.6.4` | The Slippi version reported to a test server; one over 3.6.4 also makes the kernel speak that version's netplay and EXI layouts. Against Slippi's own server it is not a setting: see *Slippi's version* below |
| `local_port` | random 41000-50999 | Fixed UDP port, for a router port-forward |
| `lan_ip` | this console | Forced `ipAddressLan` |
| `user_path` | `/slippwii/user.json` | Account file; tests point it at a dummy |
| `log_host` | off | `ip[:port]` (port 51450 by default): send log lines to a PC |
| `status_beacon` | 0 | 1: show the client state as the console nickname in Slippi's LAN broadcast |
| `log_packets` | 0 | 1: log every pad packet |
| `boot_quiet_s` | 60 | Seconds without traffic after start-up |
| `ack_delay_us` | 0 | Hold a pad ack up to this long to share a datagram with our next pad |
| `selftest`, `selftest_code`, `selftest_frames`, `selftest_allow_real_mm` | 0, -, 600, 0 | Self-test (below) |
| `chat` | 1 | Quick chat: 1 as the loader's *Quick Chat* setting says, 2 in Direct only, 0 off (received messages answer "chat disabled") |
| `rumble` | 1 | 0: controller rumble off in online play (the CSS switch saves `rumble.txt`, which wins) |
| `ping_display` | 1 | 0: no ping in a game's top left corner (the CSS switch saves `ping.txt`, which wins) |
| `test_drop_pct` | 0 | Tests: drop this share of our pad packets |

## Slippi's version

SlippWii speaks one Slippi version's netplay and matchmaking (`SO_APP_VERSION_DEFAULT` in
`so_config.h`, 3.6.4) and reports that version to the matchmaking server. Slippi's server turns
away versions it no longer supports, with an error and the latest version's number; Slippi Dolphin
then locks its online modes until it is updated. SlippWii does the same:

- **The version is the build's.** `app_version` in `config.ini` is ignored against Slippi's server
  (`so_config_finish`): a player cannot keep an old SlippWii queueing by typing a newer number.
  The number changes only with a SlippWii release checked against that Slippi version.
- **The server's word is kept.** A version the server names with its error goes into
  `/slippwii/slippi-version.txt`. While it is over the build's, the game is told "update required"
  (EXI `0xB9`, `app_state` 2, as Dolphin answers): the main menu locks the online modes, and no
  search is sent, at this start or any later one, until a newer SlippWii is installed (*Check for
  Updates* in the loader). The menu's *Update* (EXI `0xB8`) goes back to the Homebrew Channel, as
  its *Exit* does (see *Leaving*). `user.json`'s own `latestVersion` is not used: it is as old as the day
  the file was copied to the card.
- **A client's name.** Slippi's protocol has no field for one (3.6.4's `create-ticket`, and the
  `join` of Slippi's development source). `SO_CLIENT_NAME_MODE` in `so_config.h` has two forms
  ready, both off: a `client` key (`"SlippWii/1.5.1"`), or the version with build metadata
  (`"3.6.4+slippwii.1.5.1"`). Which one, if any, is for the Slippi team to say: a guess could stop
  every SlippWii from queueing, and cannot be tried without their server.

The version after 3.6.4 is built too, and chosen by one switch for the whole checkout:
`SO_SLIPPI_NEXT` in `so_config.h` (`tools/slippi_version.py`). It sets the version reported and the
matchmaking service, and the tools build the boot codes and a release's files for the same
version. It is 0 until Slippi releases that version: `docs/NEXT_SLIPPI.md` has what changes, what
was proven and the steps for that day.

## Leaving

The Online Play menu's **Exit** is Slippi's *Log-out* option, which Slippi's own code locks in
every state and SlippWii's shows in every state (`tools/ppc/online_menu.s`). Its place in the
list, its "Are you sure?" box and its command (EXI `0xB7`, sent on *Yes*) are Slippi's. The kernel
takes `0xB7`, and `0xB8` (*Update*), as "leave" (`so_diag_request_exit`): it sets the word at
`0x130034E0` to "EXIT", which `tools/ppc/exit.s` reads at every retrace. The game then stops
(`tools/ppc/exit_to_loader.inc`: interrupts and audio off, `RESET_STATUS` 0x1DEA), the kernel's
main loop closes the devices and the memory card as for any exit, and `so_diag_launch_loader` has
IOS's ES start the Homebrew Channel (LULZ, OHBC, JODI or HAXX; the Wii Menu if none is installed),
which restarts the console into it. Nintendont's own route back, a reload stub run by the game,
never got back on the console tested; until 2026-11-13 only test builds had the kernel's, as a
fallback ten seconds later, and so every test run came back and a release build would not have.
`run_sweep.py --menu-exit --kernel test_control=0` is the test that showed it. The word is the one
automated tests leave by (`test_control`); the kernel sets it to 0 at every start, since that
memory outlives a restart.

The option's name (a picture) and the two lines under the menu are SlippWii's own, put into
Slippi's menu files by the loader after it has made them (`tools/menu_edits.py`,
`loader/source/slippwii_edits.h`).

## Wired only

Over Wi-Fi an IOS `sendto` takes 13-18 ms (`so_socket.h`), so an opponent would get every input
late. A search is refused when the console's network is its Wi-Fi (`on_wifi` in `so_client.c`):
the connection chosen in the Wii's settings is a wireless one and the link's state does not say
wired, both read from IOS (`so_net_link`: `/dev/net/ncd/manage`, read only). The game shows
"Online needs a wired LAN adapter (no Wi-Fi)". The log has a `client: network:` line at start-up
and at each search, with the raw values: a wired console says `wired (link 3, connections 26 26 a7)`
(the link's state 3 is wired, 5 wireless and up; each connection's flags, `80` the chosen one,
`01` wired). A console that gives neither reading is not refused.

## More than one opponent

Teams and Party (docs/HOW_IT_WORKS.md, "Four players"). Everything here is behind `remote_count > 1`
or `remotes > 1`: a game against one opponent takes none of these paths.

- `so_exi.c`, `rollback_wanted`: the engine is told "off" for the game (its 0xD9), and the game waits
  for inputs. `test_multi_rollback=1` rolls back all the same, to measure what that costs
  (`tools/sweep/lobby.py --lag 1`).
- `lobby_delay`: the delay the game is given (the match state's byte 9): the setting, or more by
  the lobby's slowest ping and `multi_delay_room_ms` (20). The ping is each opponent's pads' round
  trip, smoothed; before a lobby's first game, the shortest of ENet's acknowledgements' round trips
  (`sampleRoundTripTime`, a field added to our copy of ENet: its own mean starts at 500 ms).
- `multi_level`: an ADVANCE when the time sync's offset reads under -11 ms on average over four
  readings (2 s) and the next frame's inputs are all here. The log's `SHOWN` lines count them
  ("fine").
- `so_netplay.c`, `held_ack`: an ack rides in the next pad's datagram to its opponent, at most
  20 ms late.
- `SlippiNetwork.c`, `multi_stream` (100): the replay stream to a PC that mirrors the console goes
  out as one send in 100 ms during such a game, not one a frame. A send a frame cost the game 25
  to 108 waited frames in 40 s and, on Slippi's next version, one game in three on the heaviest
  stage (docs/NEXT_SLIPPI.md). `multi_stream=1` in `config.ini` is the old way, `0` holds the
  stream until the game is over (`streamHolds`). The replay file on the card is not concerned.
- A player who has left: `so_np_player_active` is 0 for it once its connections are closed, the
  game is not held for it, and `fill_gone` hands the game what Slippi Dolphin 3.6.4 does (its last
  pads, the newest opponent's frame and checksum, and the "take it out" byte from the first multiple
  of 30 frames that is 16 past its last input). One whose inputs hold the game for 7 s is given up
  alone (`so_np_drop_player`); with the last one gone the game ends as against one opponent.
- The game's setup: teams as Dolphin draws them, Party's time mode, the rules check
  (`build_game`, `rules_broken`).

## Match quality

Slippi's development source ends a Ranked match when its "match quality" debt reaches 30
(`so_mq.h` has the rule). SlippWii does not play Ranked, but counts the same debt for every game
and says in its log how the game would have been judged: a `quality:` line for an unhealthy
interval and for the one that reaches the limit, and two `game quality` lines at a game's end.
`tools/sweep/match_quality.py` scores the same games for the PC opponent.

## Testing

- Unit tests (any host C compiler): `sh tests/run_tests.sh`.
- The whole client on Windows, against the 3DS fork's fake matchmaking
  server and fake Dolphin peer:
  `sh tests/pc/build.sh`, then `python ../../../tools/online_selftest.py pc`
  (from the slippi-wii checkout).
- The EXI device on Windows: the same, with `--game`. `so_pc_game.exe` is a
  fake Melee: it sends the online codeset's commands (log in, lock in and
  search Direct, the CSS's 0xB3 every frame, a lockstep game through 0xB0,
  leave) at 59.94 Hz and checks every answer and every opponent pad.
- On a Wii: `python tools/online_selftest.py setup --sd E:`, then
  `python tools/online_selftest.py run`, then boot the Wii into Melee. The Wii
  plays a synthetic match against the fake peer on the PC; its log arrives on
  UDP 51450.

The self-test never uses Slippi's real server unless `selftest_allow_real_mm=1`.
Its pads follow a deterministic pattern (`so_test_pad`), so each side checks
every pad it receives.

## Provenance

The code is written for this project from Slippi's open-source references:
- Slippi Dolphin (GPLv2+): `SlippiMatchmaking.cpp` and `SlippiNetplay.cpp` in Ishiiruka v3.6.4;
- ENet (MIT);
- libogc (zlib), for the IOS socket ioctl layouts.

Nintendont is GPLv2.
