# Changelog

## 1.5.1

- Fixed: with the Wii in **480p** (a component cable, or *Force Progressive*), online games ran in
  slow motion and the screen could go black a few seconds in. Rollback read the controller only
  every other screen refresh in 480p, so the game ran at about half speed, and then tried to catch
  up in a way a 480p display may answer with a black screen. Every rollback version had this; 480i
  (the standard cables) was never affected. Found from a player's diagnostic log.
- The diagnostic log's first line now says whether the game runs in 480p.
- This is the first update you can install from the Wii: **Check for Updates** on the *Slippi
  Settings* page.

## 1.5.0

- **Check for Updates**, the first row of the *Slippi Settings* page: SlippWii looks on GitHub for a
  newer release and installs it on the card, on the Wii, over the wired connection. Your account
  and your settings are kept; if anything fails, the old version stays. It installs only a release
  signed with SlippWii's key, and contacts GitHub only when you press the button. This first
  version with it still comes by SD card; the ones after it need no computer.
- **No Music**, a switch on the same page: the game's music off, in the menus and in matches,
  online and offline, remembered with the other settings. Sound effects stay.

## 1.4.0

**Rollback netcode is now the regular SlippWii.** What the "SlippWii Rollback" betas were is this
release: one app, one download.

- Rollback: when the opponent's input is late, the Wii carries on with a guess and corrects it when
  the input arrives, as Slippi Dolphin does, where 1.3 stopped and waited. A late packet no longer
  freezes the game, and to the opponent a Wii plays like another PC.
- With it, everything of the betas (below): the fix for the crash or desync a few seconds into a
  match, less input lag, four of Slippi Dolphin's optional codes as switches, widescreen online,
  crash reports and the **Diagnostic Log** on the SD card.
- If you had a beta: delete `apps/SlippWii-Rollback` from the SD card. The regular app replaces it.
- Next to a virtual memory card of 1019 blocks or more there is no room for rollback, and a game
  waits for late inputs as in 1.3. The line `rollback=0` in `slippwii/config.ini` does the same on
  purpose, for troubleshooting.
- Since 1.4.0-beta.3 nothing else changed: the version, and rollback on without a setting.

## 1.4.0-beta.3 (rollback beta)

- Fixed a crash or desync a few seconds into a match that some consoles had again and again: "GO" left
  on the screen, characters that could not move, or a crash. The rollback code used a table it had not
  cleared, and what the table held depended on what the console had done before.
- Less input lag: under about 75 ms of ping at 2 delay frames (42 ms at 1), each frame reaches the
  screen one refresh sooner than in beta 2. About 92 ms from a button to the middle of a CRT's picture
  becomes about 75 at 2 delay frames, and about 75 becomes about 59 at 1, which is what a console has
  offline. Over those pings a game is as it was.
- Four of Slippi Dolphin's optional codes, as switches on the *Slippi Settings* page: **L-Cancel Red
  Flash**, **No Screen Shake**, **Center 2P HUD** and **Freeze FD**. They work online and offline and
  change only what your console draws.
- Widescreen (the *Screen* setting) now works online too.
- A crash report is saved on the SD card (`slippwii/logs`) when the game crashes or hangs, and the new
  **Diagnostic Log** switch saves the whole log there, to send with a problem report. IP addresses and
  connect codes are masked; names and the Slippi play key are never written.
- The loader's screen and the logs now say the beta's version (betas 1 and 2 said 1.3.1).

## 1.3.1

- The online character select's settings (Delay Frames, Rumble, Show Ping) now work from any controller port, not just port 1 (#4).

## 1.3.0

- Ping display: your ping to your opponent in the top left corner during a game, in the same cyan as
  Slippi Dolphin, updated once a second and sized to read on a small CRT. Turn it on or off with the
  new **Show Ping** switch beside *Rumble* on the online character select screen (remembered).
- Change your delay while connected: the **Delay Frames** buttons no longer lock while searching or
  connected to an opponent, so there's no need to disconnect. A change applies from the next game.
- Fixed the Wii sometimes stopping sending its inputs in the middle of a game, which ended the game in a
  disconnect 7 seconds later (about 1 game in 18 at 25-30 ms ping).
- Fixed a crash at the end of a game that ran out the timer with a player over the ledge grab limit
  (45). Slippi's "Exceeded Ledge Grab Limit" message reads a bad address that Dolphin tolerates and a
  console doesn't. The game now ends with that message, as on Dolphin.
- Fixed Nintendont's settings not being kept: the loader threw away its own settings file on every
  start. They're now saved when you leave the settings screen, exit or start a game.
- Replays: **Slippi Replays** works in online games, as on Slippi Nintendont: each game is saved as a
  `.slp` in a `Slippi` folder on your other drive (game on SD: a USB drive; game on USB: the SD card).
  The folder is now made when it's missing; before, nothing could be saved without it. And a USB drive
  that was in the Wii from the start no longer stops replays from saving.
- The polling drift fix (Lag Reduction: PDF) is on by default in the settings, for games without online
  play. Slippi's online code always has it.
- Connect code autocomplete works as on Dolphin: scrolling past the last code no longer clears it.

## 1.2.0

- Rumble switch: turn your controller's rumble on or off for online play with the new **Rumble**
  switch under *Delay Frames* on the online character select screen. It's remembered between
  sessions (saved on the SD card). Melee's own Options > Rumble couldn't do it online, because
  Slippi sends your character's rumble to your controller's port. ([#1](https://github.com/2gifts/SlippWii/issues/1))

## 1.1.0

No more installer: SlippWii is now files to put on the SD card, like Slippi Nintendont.

- Extract the zip onto the SD card, then double-click `Copy my Slippi account to SD.bat` on the card
  (it copies your login from Slippi Launcher). Nothing else runs on your computer.
- On the first start, SlippWii makes Slippi's online menus on the Wii, from the disc you start
  (Slippi's patches applied to your own NTSC 1.02 disc and checked), and keeps them on the card.
- Before Melee starts, SlippWii checks your Slippi account and shows it ("Slippi account: name
  (code)"). A missing or damaged account, a disc that isn't NTSC 1.02, or incomplete files are
  explained on the TV, with the choice to start Melee without online play.
- Games on a USB drive: SlippWii puts what it needs on that drive by itself.

## 1.0.0

The first release.

- Slippi Online on a real Wii, from Slippi Nintendont's kernel: Slippi's online code (as in Slippi
  Dolphin 3.6.4), matchmaking, Direct and Unranked, quick chat, connect code history.
- Lockstep against Slippi Dolphin's rollback; desync-free in hours of testing, including Pokémon
  Stadium's transformations.
- Input delay buttons on the online character select screen.
- Its own app, settings file and SD folder, so it installs beside Slippi Nintendont.
- An installer for the SD card (replaced in 1.1.0).
- No Ranked: it is Slippi's protected mode, so SlippWii hides it from the Online Play menu (and
  refuses a Ranked search).
