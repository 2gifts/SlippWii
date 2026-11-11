# SlippWii

**Unofficial Slippi Online for the Wii, with rollback netcode.** Play Slippi's online modes from a
real Wii (with a GameCube controller, on a CRT if you like), against players on Slippi Dolphin.

> [!IMPORTANT]
> **SlippWii is an unofficial fan project.** It is not made by, affiliated with or endorsed by
> [Project Slippi](https://slippi.gg) or Nintendo. Please don't ask the Slippi team for help with
> it: report problems [here](https://github.com/2gifts/SlippWii/issues) instead.
>
> It is **free and open source** (GPLv2), built on [Slippi Nintendont](https://github.com/project-slippi/Nintendont),
> and young: please report anything that goes wrong.

**Join the [SlippWii Discord](https://discord.gg/ytSjxmknHx)** to get help, find opponents to test
with and help improve the project.

## What works

| | |
|---|---|
| **Rollback netcode** | Since 1.4.0. The Wii predicts and rolls back the way Slippi Dolphin does, running Slippi's own rollback code. In automated games against Slippi Dolphin through simulated connections, every frame matched the PC's replay. |
| **Direct** (connect codes) | Works. Tested for many hours against Slippi Dolphin on PC, including automated runs of 75 games whose replays matched frame for frame on both sides. |
| **Unranked** | Should work (same code path as Direct) but has had little testing. |
| **Teams** | Untested. |
| **Ranked** | **Not available.** Ranked is Slippi's protected competitive mode, so SlippWii leaves it out: it isn't in the Online Play menu. |
| Input delay | Set on the Wii: the **Delay Frames** − / + buttons on the online character select screen. You can change it while connected to your opponent: it applies from the next game. |
| Rumble | On or off for online play: the **Rumble** switch on the online character select screen (remembered). |
| Ping display | The ping to your opponent in the top left corner during a game, as on Slippi Dolphin. On or off with the **Show Ping** switch on the online character select screen (remembered). |
| Replays | As on Slippi Nintendont: with **Slippi Replays** on in the settings (the *Slippi* page), each game is saved as a Slippi replay (`.slp`) in a `Slippi` folder on your *other* drive: a USB drive if your game is on the SD card, the SD card if your game is on a USB drive. Both must be plugged in; the LAN adapter takes one of the Wii's USB ports, so the drive goes in the other. Off by default. |
| Quick chat, connect code history | Work as on Slippi Dolphin. |
| Updates | Since 1.5.0, SlippWii updates itself on the Wii: **Check for Updates** on the *Slippi Settings* page downloads a newer version and installs it on the card. |

How it feels: both sides play with rollback. When your opponent's input is late, the Wii carries
on with a guess and corrects it when the input arrives, as Slippi Dolphin does. A late packet no
longer freezes the game, and to your opponent the Wii plays like another PC.

- **Input lag.** At 2 delay frames (Slippi's standard), a button press reaches the middle of a
  CRT's picture in about 75 ms while your ping is under about 75 ms, and in about 92 ms above that.
  At 1 delay frame it is about 59 ms, the same as a Wii playing offline, under about 42 ms of ping.
- **High ping.** From about 90 ms of ping the Wii is near its limit: rollback still works, but you
  may see brief hitches.
- **Memory card.** Rollback needs memory that a very large virtual memory card also uses. With a
  card of 1019 blocks or more (Nintendont's *Memcard Blocks* setting), the Wii waits for late inputs
  instead of rolling back, as SlippWii 1.3 did. The default card (251 blocks) is fine.

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

To uninstall, delete `apps/SlippWii`, `slippwii/` and `slippwii_nincfg.bin`.

### Updating

The first install is done with a computer, as above. After that the Wii updates itself (SlippWii
1.5.0 and later):

1. Start SlippWii and press **B** on the game list. *Check for Updates* is the first row of the
   *Slippi Settings* page: press **A**.
2. If a newer version is out, SlippWii shows what is new. Press **A** to install it. It downloads
   the release from GitHub, checks it, and replaces SlippWii's files on the card. Your account and
   your settings are kept.
3. Start SlippWii again from the Homebrew Channel. The first start after an update takes a little
   longer.

It needs the same wired connection online play does. Don't turn the console off while it installs.
If anything goes wrong, SlippWii says what, and the old version stays on the card.

You can still update with a computer: extract the newer zip onto the card the same way as the first
time. From 1.4.0 or older, that is the way to get 1.5.0.

SlippWii only installs a release that carries SlippWii's own signature, and it contacts GitHub only
when you press the button: nothing about you or your console is sent.

**Had the rollback beta?** Rollback is in the regular SlippWii since 1.4.0. Extract the newest zip
onto the card, then delete `apps/SlippWii-Rollback` from it: the beta's app is no longer needed.

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
   **A** to change your input delay (1-9; 2 is a good start). You don't have to disconnect: change it
   while connected to your opponent and it applies from the next game. Point at **On**/**Off** under
   *Rumble* and press **A** to turn your controller's rumble on or off for online play, or under
   *Show Ping* to show or hide your ping during games. All of these are saved for next time.

   (Melee's own *Options > Rumble* can't do this online: online, your character can be in another
   port than your controller, and Slippi sends its rumble to your controller.)

To change Nintendont's own settings, press **B** on the game list (**L**/**R** change pages). They
are saved when you leave the settings or start a game.

On the *Slippi Settings* page:

| Setting | What it does |
|---|---|
| **Check for Updates** | Looks for a newer SlippWii and installs it (see *Updating*). |
| **No Music** | Turns the game's music off, in the menus and in matches. Sound effects stay. |
| **L-Cancel Red Flash** | A character flashes red when it misses an L-cancel. |
| **No Screen Shake** | The screen does not shake. |
| **Center 2P HUD** | With two players, the percents sit in the middle of the screen. |
| **Freeze FD** | Final Destination's background stands still. |
| **Screen** (under *MELEE CODES*) | Widescreen, online too. |
| **Diagnostic Log** | Saves SlippWii's log on the SD card for a problem report (see *Reporting problems*). Leave it off otherwise. |

*L-Cancel Red Flash*, *No Screen Shake*, *Center 2P HUD* and *Freeze FD* are Slippi's own optional
codes, the ones you can turn on in Slippi Dolphin. They and *No Music* work online and offline,
change only what your console shows and plays, and your opponent does not need them.

SlippWii has its own app folder, settings file and data folder, so **Slippi Nintendont stays
installed and works as before**. Use it for offline play and console mirroring as usual.

On the SD card:

| Path | What |
|---|---|
| `apps/SlippWii/` | The app (Homebrew Channel) |
| `slippwii/user.json` | Your Slippi account (keep it private) |
| `slippwii/netplay.gct`, `slippwii/files/`, `slippwii/patches/` | Slippi's online code, menu files and patches |
| `slippwii/config.ini` | Settings (optional) |
| `slippwii/logs/` | Crash reports, and the diagnostic log when it is on |
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
- **Stutters during games:** use the wired adapter (not Wi-Fi) and play opponents in your region:
  *Show Ping* shows your ping in a game, and from about 90 ms the Wii has little room left. If it
  stutters at a low ping, send us a log (see *Reporting problems*).
- **Worse than SlippWii 1.3 was for you?** Tell us, with a log. Until it is fixed you can add the
  line `rollback=0` to `slippwii/config.ini` on the card (make the file if it isn't there): the Wii
  then waits for late inputs, as 1.3 did.

Still stuck? Ask in #help on the [SlippWii Discord](https://discord.gg/ytSjxmknHx).

## Reporting problems

Open an [issue](https://github.com/2gifts/SlippWii/issues/new/choose) and say what you did, what
happened (a photo of any SlippWii message on the TV helps), your SlippWii version (shown at the top of the SlippWii screen), the mode, and your
opponent's setup. A Slippi replay (`.slp`) from the opponent's side helps with desyncs.

**Send the log.** It tells us far more than a description can.

1. If the game **crashed**, there is nothing to turn on: SlippWii saved a crash report by itself.
2. For anything else (stutter, a freeze, a black screen, a disconnect), press **B** on the game list,
   go to the *Slippi Settings* page and set **Diagnostic Log** to **Yes**. Then play until the problem
   happens again. If the console hangs, wait ten seconds before you switch it off.
3. Put the SD card (or the USB drive your game is on) in a computer and attach the whole
   `slippwii/logs` folder to your report, zipped.
4. Set **Diagnostic Log** back to **No**: while it is on, a match can hitch now and then when the
   log is written.

The log is plain text, so you can read it first. It has your SlippWii version, console type, settings
and what the game and the network did. IP addresses and connect codes are masked, and names and your
Slippi play key are never written.

## How it works

Slippi Dolphin runs Slippi's online code inside Melee and acts as Melee's "Slippi device" over EXI.
That device handles matchmaking, the connection to the opponent and the replay stream. SlippWii does
the same on a real Wii: Slippi Nintendont's kernel (on the Wii's ARM chip) stands in for that device,
with a network client (matchmaking, ENet peer connection) and the same online code Slippi Dolphin
3.6.4 runs. See [`docs/HOW_IT_WORKS.md`](docs/HOW_IT_WORKS.md) and the byte-level
[`docs/EXI_DEVICE_SPEC.md`](docs/EXI_DEVICE_SPEC.md).

Rollback needs savestates, which Dolphin makes from outside the game. A Wii has no outside, so a
small engine inside Melee makes them on the Wii's own CPU: it tracks what the game writes each frame
and copies only that. See [`docs/ROLLBACK.md`](docs/ROLLBACK.md).

Building from source: [`docs/BUILDING.md`](docs/BUILDING.md).

## Credits and licence

- [Project Slippi](https://github.com/project-slippi): Slippi, Slippi Dolphin, the online code
  (slippi-ssbm-asm) and Slippi Nintendont, which this is built on. Thank you, Fizzi and the Slippi team.
- [Nintendont](https://github.com/FIX94/Nintendont) by FIX94, crediar and contributors.
- [ENet](http://enet.bespin.org/) by Lee Salzman (MIT).
- [BearSSL](https://bearssl.org/) by Thomas Pornin (MIT) and puff by Mark Adler (zlib licence), for
  *Check for Updates*.

SlippWii is licensed under the GNU General Public License v2 (see [LICENSE](LICENSE)), as
Nintendont is. Slippi's online code that SlippWii downloads and runs (slippi-ssbm-asm) is GPLv3.
Super Smash Bros. Melee is © Nintendo / HAL Laboratory; you need your own copy.
