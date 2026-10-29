# Changelog

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
