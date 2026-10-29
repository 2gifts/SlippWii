# SlippWii

**Unofficial Slippi Online for the Wii.** Play Slippi's online modes from a real Wii (with a
GameCube controller, on a CRT if you like), against players on Slippi Dolphin.

> [!IMPORTANT]
> **SlippWii is an unofficial fan project.** It is not made by, affiliated with or endorsed by
> [Project Slippi](https://slippi.gg) or Nintendo. Please don't ask the Slippi team for help with
> it: report problems [here](https://github.com/2gifts/SlippWii/issues) instead.
>
> It is **free and open source** (GPLv2), built on [Slippi Nintendont](https://github.com/project-slippi/Nintendont),
> and young: please report anything that goes wrong.

## What works

| | |
|---|---|
| **Direct** (connect codes) | Works. Tested for many hours against Slippi Dolphin on PC, including automated runs of 75 games whose replays matched frame for frame on both sides. |
| **Unranked** | Should work (same code path as Direct) but has had little testing. |
| **Teams** | Untested. |
| **Ranked** | **Not available.** Ranked is Slippi's protected competitive mode, so SlippWii leaves it out: it isn't in the Online Play menu. |
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
- The Wii's **SD card** and a computer to copy files to it.
- Your **Melee disc image**: NTSC 1.02 (`GALE01`, revision 2), the same `.iso` Slippi uses (`.ciso`,
  an extracted game or a real disc work too).
- A **Slippi account**: install [Slippi Launcher](https://slippi.gg) on your computer and log in once.
- A GameCube controller (and adapter ports, on a Wii that has them).

## Install

1. Download **`SlippWii-vX.Y.Z.zip`** from [Releases](https://github.com/2gifts/SlippWii/releases).
2. Put your Wii's SD card in your computer and **extract the zip onto the SD card** (it adds the
   `apps` and `slippwii` folders; nothing else on the card changes). Your Melee `.iso` goes where
   Nintendont finds games, e.g. `SD:/games/` (or a USB drive's `games/`), if it isn't there already.
3. On the SD card, double-click **`Copy my Slippi account to SD.bat`**. It copies your Slippi login
   from Slippi Launcher (see below).
4. Put the card in the Wii, start **SlippWii** from the Homebrew Channel and pick Melee.

**The first start takes a little longer:** SlippWii makes Slippi's online menus from *your* disc,
on the Wii, and keeps them on the card. Later starts skip this. Nothing from Melee is shipped with
SlippWii; the zip has Project Slippi's own files (its online code, menu files and patches) and
SlippWii.

To update, extract the newer zip onto the card the same way (your account and settings stay).
To uninstall, delete `apps/SlippWii`, `slippwii/` and `slippwii_nincfg.bin`.

### Your Slippi account

Slippi Launcher keeps your login in a file named `user.json`; SlippWii needs a copy in
`slippwii/` on the SD card.

- **Windows:** double-click `Copy my Slippi account to SD.bat` on the SD card. It finds the
  account Slippi Launcher on that PC is logged in to and copies it, showing only your name and
  connect code. No account found? Install [Slippi Launcher](https://slippi.gg), log in, click
  **Play** once so it sets up Dolphin, then run it again.
- **By hand (any computer):** copy `user.json` from Slippi Launcher's files into `slippwii/` on the
  card. On Windows it is `%APPDATA%\Slippi Launcher\netplay\User\Slippi\user.json`; on macOS and
  Linux, look for `user.json` in Slippi Launcher's application folder.

If the account is missing or damaged, SlippWii says so on the TV before Melee starts, and explains
how to fix it. When it is fine, the start-up screen shows *Slippi account: your name (your code)*.

**Treat `user.json` like a password:** anyone with it can play as you. Don't share it, and don't
post your SD card's files online.

## Play

1. Put the SD card in the Wii and connect the LAN adapter.
2. Open the Homebrew Channel and start **SlippWii**.
3. Pick Melee. In Melee, go to **Online Play** and pick **Direct** or **Unranked**, as on Slippi Dolphin.
4. On the character select screen, point the hand at **−** or **+** under *Delay Frames* and press
   **A** to change your input delay (1-9; 2 is a good start). It's saved for next time.

SlippWii has its own app folder, settings file and data folder, so **Slippi Nintendont stays
installed and works as before**. Use it for offline play, replays to USB and console mirroring as
usual.

On the SD card:

| Path | What |
|---|---|
| `apps/SlippWii/` | The app (Homebrew Channel) |
| `slippwii/user.json` | Your Slippi account (keep it private) |
| `slippwii/netplay.gct`, `slippwii/files/`, `slippwii/patches/` | Slippi's online code, menu files and patches |
| `slippwii/config.ini` | Settings (optional) |
| `slippwii_nincfg.bin` | SlippWii's loader settings (Slippi Nintendont uses `slippi_nincfg.bin`) |

If your games are on a USB drive, SlippWii copies what it needs to `slippwii/` on that drive by itself.

## Troubleshooting

SlippWii checks everything before Melee starts and explains any problem on the TV. The common ones:

- **"No Slippi account on the SD card":** run `Copy my Slippi account to SD.bat` (see above).
- **"Could not make Slippi's online menus":** your Melee disc image isn't an unmodified NTSC 1.02
  copy (GALE01 revision 2), or SlippWii's files are incomplete: extract the zip onto the card again.
  Slippi needs the same vanilla 1.02 image.
- **Can't connect / stuck searching:** check the LAN adapter's connection test in Wii Settings,
  and that the opponent typed your connect code exactly. Direct needs both players searching for each
  other.
- **Black screen after starting SlippWii:** power the Wii off fully (hold the power button), turn it
  on again and start SlippWii from the Homebrew Channel.
- **Stutters during games:** use the wired adapter (not Wi-Fi), raise *Delay Frames* by one or two,
  and play opponents in your region.

## Reporting problems

Open an [issue](https://github.com/2gifts/SlippWii/issues/new/choose) and say what you did, what
happened (a photo of any SlippWii message on the TV helps), your SlippWii version (shown at the top of the SlippWii screen), the mode, and your
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
