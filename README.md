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
| **Rollback netcode** | Yes, the way Slippi Dolphin does it. |
| **Direct**, **Unranked** | Work. |
| **Teams**, **Party** | Work, with four players as on Slippi Dolphin. The Wii can't use rollback with four players, so in these modes your own inputs have extra delay, more on slower connections. Your opponents aren't affected. |
| **Ranked** | **Not available.** Ranked is Slippi's protected competitive mode, so SlippWii leaves it out. |
| Input delay | Set on the Wii, with the **Delay Frames** − / + buttons on the online character select screen. |
| Rumble, ping display | Switches on the online character select screen. The ping shows in the top left corner during a game, as on Slippi Dolphin. |
| Quick chat, connect code history | Work as on Slippi Dolphin. |
| Replays | As on Slippi Nintendont: turn on **Slippi Replays** (the *Slippi Settings* page) and each game is saved as a `.slp` file in a `Slippi` folder on your *other* drive: a USB drive if your game is on the SD card, the SD card if your game is on USB. Off by default. |
| Updates | SlippWii updates itself on the Wii with **Check for Updates**. |

### How it plays

- **Like a console.** With *Delay Frames* at 1 and a good connection (under about 50 ms of ping),
  a button press reaches the screen exactly when it does on a tournament console (Slippi Nintendont
  with the polling drift fix). A player has confirmed this with their own test equipment on a CRT.
  The details are in [`docs/INPUT_DELAY.md`](docs/INPUT_DELAY.md).
- **Which delay?** Use **1** under about 50 ms of ping and **2** above that. It starts at 2.
- **High ping.** From about 90 ms of ping the Wii is near its limit, and you may see brief hitches.
  Opponents in your region play best.
- **Your opponent** needs nothing special: they play you like anyone else on Slippi.

## What you need

- A **Wii** with the [Homebrew Channel](https://wii.hacks.guide/). (Wii U's vWii is untested.)
- A **USB LAN adapter**: a wired connection to your router. **Wi-Fi is not supported**: the Wii's
  Wi-Fi is far too slow for online play, and SlippWii will not search for an opponent on it. Set
  the wired connection up once in *Wii Settings > Internet*.
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
SlippWii.

To uninstall, delete `apps/SlippWii`, `slippwii/` and `slippwii_nincfg.bin`.

### Updating

After the first install, the Wii updates itself:

1. Start SlippWii and press **B** on the game list. *Check for Updates* is the first row of the
   *Slippi Settings* page: press **A**.
2. If a newer version is out, SlippWii shows what is new. Press **A** to install it. Your account
   and your settings are kept.
3. Start SlippWii again from the Homebrew Channel. The first start after an update takes a little
   longer.

It needs the same wired connection online play does. Don't turn the console off while it installs.
If anything goes wrong, SlippWii says what, and the old version stays on the card.

You can also update with a computer: extract the newer zip onto the card, as for the first
install. (A very old SlippWii with no *Check for Updates* row has to be updated this way once.)

SlippWii only installs its own signed releases, and it contacts GitHub only when you press the
button: nothing about you or your console is sent.

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
3. Pick Melee. In Melee, go to **Online Play** and pick **Direct**, **Unranked**, **Teams** or
   **Party**, as on Slippi Dolphin.
4. On the character select screen, point the hand at **−** or **+** under *Delay Frames* and press
   **A** to change your input delay (see *How it plays* above). You can change it while connected
   to your opponent: it applies from the next game. The **On**/**Off** switches under *Rumble* and
   *Show Ping* work the same way. All of these are saved for next time.
5. When you are done, pick **Exit** at the bottom of the **Online Play** menu and answer **Yes**:
   the Wii goes back to the Homebrew Channel. You don't have to turn the console off to leave.

To change settings, press **B** on the game list (**L**/**R** change pages). They are saved when
you leave the settings or start a game.

On the *Slippi Settings* page:

| Setting | What it does |
|---|---|
| **Check for Updates** | Looks for a newer SlippWii and installs it (see *Updating*). |
| **No Music** | Turns the game's music off, in the menus and in matches. Sound effects stay. |
| **Quick Chat** | Slippi's quick chat, with Slippi Dolphin's three choices: **Yes**, **Direct** (in Direct only) or **No**. |
| **Team Indicators** | In online games your own character's tag says YOU, and in Teams your teammates keep their marker. |
| **L-Cancel Red Flash** | A character flashes red when it misses an L-cancel. |
| **No Screen Shake** | The screen does not shake. |
| **Center 2P HUD** | With two players, the percents sit in the middle of the screen. |
| **Freeze FD** | Final Destination's background stands still. |
| **Screen** (under *MELEE CODES*) | Widescreen, online too. |
| **Diagnostic Log** | Saves a detailed log for a problem report (see *Reporting problems*). It doesn't affect your games; the note on the Wii's screen saying it can is out of date. |

The game options here change only what your own console shows and plays. Your opponent does not
need them.

SlippWii has its own app folder, settings file and data folder, so **Slippi Nintendont stays
installed and works as before**. Use it for offline play and console mirroring as usual.

On the SD card:

| Path | What |
|---|---|
| `apps/SlippWii/` | The app (Homebrew Channel) |
| `slippwii/user.json` | Your Slippi account (keep it private) |
| `slippwii/netplay.gct`, `slippwii/files/`, `slippwii/patches/` | Slippi's online code, menu files and patches |
| `slippwii/logs/` | Problem reports and logs (see *Reporting problems*) |
| `slippwii_nincfg.bin` | SlippWii's settings (Slippi Nintendont uses `slippi_nincfg.bin`) |

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
- **"Online needs a wired LAN adapter (no Wi-Fi)":** the connection the Wii is set to use is a
  wireless one. In *Wii Settings > Internet > Connection Settings*, pick the wired connection (or
  set one up) and choose *Use this connection*.
- **The online modes are locked, and only "Update" can be picked:** Slippi has released a new
  version, and SlippWii has to be updated to play with it: use **Check for Updates** (see
  *Updating*). If it says there is nothing newer, the matching SlippWii is not out yet.
- **Black screen after starting SlippWii:** power the Wii off fully (hold the power button), turn it
  on again and start SlippWii from the Homebrew Channel.
- **Stutters during games:** play opponents in your region. *Show Ping* shows your ping in a game,
  and from about 90 ms the Wii has little room left. If it stutters at a low ping, send us your
  logs (see *Reporting problems*).
- **The game stops and starts whenever your opponent lags:** check the *Memcard Blocks* setting.
  A very large memory card (1019 blocks or more) leaves no room for rollback. The default (251)
  is fine.

Still stuck? Ask in #help on the [SlippWii Discord](https://discord.gg/ytSjxmknHx).

## Reporting problems

Open an [issue](https://github.com/2gifts/SlippWii/issues/new/choose) and say what you did and
what happened. A photo of any SlippWii message on the TV helps.

**Send the logs.** They tell us far more than a description can.

1. Put the SD card (or the USB drive your game is on) in a computer, zip the `slippwii/logs`
   folder and attach it. SlippWii saves a report there by itself when something goes wrong: a
   crash, a freeze, a desync, a lost connection.
2. If the problem keeps happening, or it is something else (stutter, a black screen): press **B**
   on the game list, go to the *Slippi Settings* page and set **Diagnostic Log** to **Yes**. Play
   until it happens again, then send the folder. If the console hangs, wait ten seconds before you
   switch it off.
3. For a desync, a Slippi replay (`.slp`) of the same game from a PC player in it helps a lot.

The logs are plain text, so you can read them first. IP addresses and connect codes are masked,
and names and your Slippi play key are never written.

## Contributing

Help is welcome. SlippWii aims to be what an official Slippi port to the Wii would be: online play
that feels exactly like offline, and a Wii a PC player can't tell from a PC, with no features
beyond Slippi's own. [CONTRIBUTING.md](CONTRIBUTING.md) says what stays fixed and what a change
needs, and [docs/FINDINGS.md](docs/FINDINGS.md) says where things stand, which problems are open
and what has been tried so far. AI agents: start at [AGENTS.md](AGENTS.md).

## How it works

Slippi Dolphin runs Slippi's online code inside Melee and acts as Melee's "Slippi device". That
device handles matchmaking, the connection to the opponent and the replay stream. SlippWii does
the same on a real Wii: Slippi Nintendont's kernel stands in for that device, with a network
client and the same online code Slippi Dolphin runs. See
[`docs/HOW_IT_WORKS.md`](docs/HOW_IT_WORKS.md).

Rollback needs savestates, which Dolphin makes from outside the game. A Wii has no outside, so a
small engine inside Melee makes them on the Wii's own CPU. See
[`docs/ROLLBACK.md`](docs/ROLLBACK.md), and [`docs/FOUR_PLAYERS.md`](docs/FOUR_PLAYERS.md) for
Teams and Party.

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
