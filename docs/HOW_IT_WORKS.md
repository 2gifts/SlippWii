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

### Things a console needs that Dolphin doesn't

- **Music:** Dolphin plays the music itself and the codeset mutes the game's. On a Wii those hooks
  are left out, so the game plays its own music. The loader's *No Music* switch puts the two lines
  that mute it back (`MuteMusic.asm`, `PreventMusicAlarm.asm`), with nobody to play it instead:
  silence, as a player who turns the music off wants.
- **"Normal Lag Reduction"** writes frames to a display buffer that only exists in Dolphin; on a
  console the GPU's write hangs the bus, so that code is dropped. The polling fix ("PD+VB") stays.
- **Stage files loaded mid-game** (Pokémon Stadium's transformations) are kept in RAM, as Dolphin
  does. Reading them from the disc during a game stalled Melee long enough to drop inputs.
- **Wired only:** an IOS `sendto` over Wi-Fi takes 13-18 ms, too slow for 60 inputs a second each
  way. Over the USB LAN adapter it takes well under 1 ms.
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
