# Changelog

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
