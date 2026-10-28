# SlippWii

**Unofficial Slippi Online for the Wii.** Play Slippi's online modes from a real Wii (with a
GameCube controller, on a CRT if you like), against players on Slippi Dolphin.

> [!IMPORTANT]
> **SlippWii is an unofficial fan project.** It is not made by, affiliated with or endorsed by
> [Project Slippi](https://slippi.gg) or Nintendo. Please don't ask the Slippi team for help with
> it: report problems [here](https://github.com/2gifts/SlippWii/issues) instead.
>
> It is **free and open source** (GPLv2), built on [Slippi Nintendont](https://github.com/project-slippi/Nintendont),
> and in **beta**: expect rough edges.

## What works

| | |
|---|---|
| **Direct** (connect codes) | Works. Tested for many hours against Slippi Dolphin on PC, including automated runs of 75 games whose replays matched frame for frame on both sides. |
| **Unranked** | Should work (same code path as Direct) but has had little testing. |
| **Teams** | Untested. |
| **Ranked** | **Not supported.** Ranked's set rules and result reporting are not implemented, so please don't play Ranked with SlippWii. |
| Input delay | Set on the Wii: the **Delay Frames** − / + buttons on the online character select screen. |
| Quick chat, connect code history | Work as on Slippi Dolphin. |

How it feels: your opponent on PC plays with rollback, exactly as against another PC player.
The Wii doesn't roll back. Instead, it waits whenever the opponent's inputs are late (*lockstep*),
so on a bad connection the Wii side stutters briefly rather than rolling back. On a good
connection (same region, wired) both sides feel normal.

## What you need

- A **Wii** with the [Homebrew Channel](https://wii.hacks.guide/). (Wii U's vWii is untested.)
- A **USB LAN adapter**: a wired connection to your router. **Wi-Fi is not supported**: the Wii's
  Wi-Fi is far too slow for online play. Set it up once in *Wii Settings > Internet*.
- The Wii's **SD card** and a computer to set it up.
- Your **Melee disc image**: NTSC 1.02 (`GALE01`, revision 2), the same `.iso` Slippi uses. (`.ciso`
  works too.)
- A **Slippi account**: install [Slippi Launcher](https://slippi.gg) on your computer and log in once.
- A GameCube controller (and adapter ports, on a Wii that has them).

## Install

### Windows (one click)

1. Download **`SlippWii-vX.Y.Z.zip`** from [Releases](https://github.com/2gifts/SlippWii/releases/latest)
   and extract it (right-click > *Extract All*).
2. Put your Wii's SD card in the computer.
3. Double-click **`Install SlippWii.bat`** and answer its questions.

The installer finds the SD card, your Melee `.iso` (from Slippi Launcher's settings, the SD card or
your Downloads folder) and your Slippi account by itself. It asks before using each one. It then
downloads Slippi's online code and menu files from Project Slippi's GitHub, patches the menu files
from *your* disc, and writes everything to the card. Nothing from Melee or from Slippi is
redistributed with SlippWii.

### macOS / Linux

Extract the zip, put the SD card in, and run (Python 3.8 or newer; less tested than Windows so far):

```sh
python3 installer/install.py
```

### Your Slippi account on the Wii

Slippi Launcher keeps your login in a file named `user.json`. The installer copies it to the SD card
(`slippwii/user.json`) so the Wii can log in as you, and shows only your name and connect code
while doing so. **Treat that file like a password:** anyone with it can play as you. Don't share it,
and don't post your SD card's files online.

- No account found? Install Slippi Launcher, log in, click **Play** once so it sets up Dolphin, then
  run the installer again.
- Want a different account on the Wii? Log in to it in Slippi Launcher (or pick its `user.json`)
  and run the installer again. The old one is kept as `user.json.old-…`.

## Play

1. Put the SD card in the Wii and connect the LAN adapter.
2. Open the Homebrew Channel and start **SlippWii**.
3. Pick Melee. In Melee, go to **Online Play** and pick **Direct** or **Unranked**, as on Slippi Dolphin.
4. On the character select screen, point the hand at **−** or **+** under *Delay Frames* and press
   **A** to change your input delay (1-9; 2 is a good start). It's saved for next time.

SlippWii has its own app folder, settings file and data folder, so **Slippi Nintendont stays
installed and works as before**. Use it for offline play, replays to USB and console mirroring as
usual.

What the installer puts on the SD card:

| Path | What |
|---|---|
| `apps/SlippWii/` | The app (Homebrew Channel) |
| `slippwii/user.json` | Your Slippi account (keep it private) |
| `slippwii/netplay.gct`, `slippwii/files/` | Slippi's online code and menu files (made from your disc) |
| `slippwii/config.ini` | Settings (optional) |
| `slippwii_nincfg.bin` | SlippWii's loader settings (Slippi Nintendont uses `slippi_nincfg.bin`) |

To update, run the newer installer. To uninstall, delete `apps/SlippWii`, `slippwii/` and
`slippwii_nincfg.bin`.

## Troubleshooting

- **"Online Play" is locked or says you're not logged in:** `slippwii/user.json` is missing. Run
  the installer again with your account.
- **Can't connect / stuck searching:** check the LAN adapter's connection test in Wii Settings,
  and that the opponent typed your connect code exactly. Direct needs both players searching for each
  other.
- **Black screen after starting SlippWii:** power the Wii off fully (hold the power button), turn it
  on again and start SlippWii from the Homebrew Channel.
- **Stutters during games:** use the wired adapter (not Wi-Fi), raise *Delay Frames* by one or two,
  and play opponents in your region.

## Reporting problems

Open an [issue](https://github.com/2gifts/SlippWii/issues/new/choose) and say what you did, what
happened, your SlippWii version (shown at the top of the SlippWii screen), the mode, and your
opponent's setup. A Slippi replay (`.slp`) from the opponent's side helps with desyncs.

## How it works

Slippi Dolphin runs Slippi's online code inside Melee and acts as Melee's "Slippi device" over EXI.
That device handles matchmaking, the connection to the opponent and the replay stream. SlippWii does
the same on a real Wii: Slippi Nintendont's kernel (on the Wii's ARM chip) stands in for that device,
with a network client (matchmaking, ENet peer connection) and the same online code Slippi Dolphin
3.6.4 runs. See [`docs/HOW_IT_WORKS.md`](docs/HOW_IT_WORKS.md) and the byte-level
[`docs/EXI_DEVICE_SPEC.md`](docs/EXI_DEVICE_SPEC.md).

Building from source: [`docs/BUILDING.md`](docs/BUILDING.md).

## Credits and licence

- [Project Slippi](https://github.com/project-slippi): Slippi, Slippi Dolphin, the online code
  (slippi-ssbm-asm) and Slippi Nintendont, which this is built on. Thank you, Fizzi and the Slippi team.
- [Nintendont](https://github.com/FIX94/Nintendont) by FIX94, crediar and contributors.
- [ENet](http://enet.bespin.org/) by Lee Salzman (MIT).

SlippWii is licensed under the GNU General Public License v2 (see [LICENSE](LICENSE)), as
Nintendont is. Slippi's online code that SlippWii downloads and runs (slippi-ssbm-asm) is GPLv3.
Super Smash Bros. Melee is © Nintendo / HAL Laboratory; you need your own copy.
