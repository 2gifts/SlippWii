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
| `so_pads.[ch]` | Pad queues, acks, ping and time sync: no networking, unit-tested |
| `so_netplay.[ch]` | Peer-to-peer netplay (`SlippiNetplayClient`): connections, messages 0x80-0x86 |
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

## Files on the SD card (or USB drive)

| Path | What |
| --- | --- |
| `/slippwii/user.json` | Slippi Launcher's `user.json`, copied as is. Its play key is only ever sent to the matchmaking server, inside a create-ticket, and it is never logged. |
| `/slippwii/config.ini` | Optional `key=value` settings. Comment lines start with `#` or `;`. |

| Key | Default | Meaning |
| --- | --- | --- |
| `delay` | 2 | Online input delay, 1-9 frames |
| `mm_host`, `mm_port` | `mm.slippi.gg`, 43113 | Matchmaking server (tests use a PC on the LAN) |
| `app_version` | `3.6.4` | Netplay version sent to the server |
| `local_port` | random 41000-50999 | Fixed UDP port, for a router port-forward |
| `lan_ip` | this console | Forced `ipAddressLan` |
| `user_path` | `/slippwii/user.json` | Account file; tests point it at a dummy |
| `log_host` | off | `ip[:port]` (port 51450 by default): send log lines to a PC |
| `status_beacon` | 0 | 1: show the client state as the console nickname in Slippi's LAN broadcast |
| `log_packets` | 0 | 1: log every pad packet |
| `boot_quiet_s` | 60 | Seconds without traffic after start-up |
| `ack_delay_us` | 0 | Hold a pad ack up to this long to share a datagram with our next pad |
| `selftest`, `selftest_code`, `selftest_frames`, `selftest_allow_real_mm` | 0, -, 600, 0 | Self-test (below) |
| `chat` | 1 | 0: quick chat off (received messages answer "chat disabled") |
| `test_drop_pct` | 0 | Tests: drop this share of our pad packets |

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
