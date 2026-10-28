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
  menu files, from `SD:/slippwii/`.
- `so_client.c`, `so_config.c`: the client's threads and `config.ini`.

### Lockstep instead of rollback

The Wii can't save and restore Melee's state fast enough to roll back, so it doesn't predict: when
the opponent's input for a frame hasn't arrived, the kernel tells the online code to *skip* the
frame (Slippi's own "wait" answer) and the game holds until it does. The opponent's Slippi Dolphin
still predicts and rolls back as usual. Both sides run the exact same inputs, so the games stay in
sync. Desync detection (Slippi's per-frame checksums) works as on PC.

### Things a console needs that Dolphin doesn't

- **Music:** Dolphin plays the music itself and the codeset mutes the game's. On a Wii those hooks
  are left out, so the game plays its own music.
- **"Normal Lag Reduction"** writes frames to a display buffer that only exists in Dolphin; on a
  console the GPU's write hangs the bus, so that code is dropped. The polling fix ("PD+VB") stays.
- **Stage files loaded mid-game** (Pokémon Stadium's transformations) are kept in RAM, as Dolphin
  does. Reading them from the disc during a game stalled Melee long enough to drop inputs.
- **Wired only:** an IOS `sendto` over Wi-Fi takes 13-18 ms, too slow for 60 inputs a second each
  way. Over the USB LAN adapter it takes well under 1 ms.
- **Input delay** is set on the character select screen (`tools/css_delay/`), since there's no
  Dolphin settings window.

The research and measurements that led here are in [research/FEASIBILITY.md](research/FEASIBILITY.md).

## Testing without a second person

- `tools/online_selftest.py pc --game`: the kernel's online client, built for Windows, against a
  fake Melee and a fake opponent.
- `tools/sweep/run_sweep.py`: the Wii against a local copy of Slippi Dolphin on the same PC, with a
  local fake matchmaking server. Both players are driven by an autoplay Gecko code through the menus
  and random games; the Wii's replays are compared frame by frame with Dolphin's
  (`tools/slp_compare.py`). It never contacts Slippi's servers.
