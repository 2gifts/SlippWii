/* SlippWii: getting Slippi Online ready before Melee starts (slippwii.c). */
#ifndef SLIPPWII_H
#define SLIPPWII_H

#include <gctypes.h>

/* For NTSC 1.02 Melee, after the game is chosen (main.c): checks the Slippi
 * account (SD:/slippwii/user.json), makes Slippi's online menu files from
 * the disc on the first start (SD:/slippwii/patches/ -> files/), and puts
 * the online files on the game's drive, all on screen. Returns when the game
 * may start; on a problem it explains it and goes back to the Homebrew
 * Channel, or lets the player start Melee without online play. */
void SlippWii_Prepare(unsigned int CurDICMD, u32 ISOShift);

/* The account line for the boot screen ("" if none). */
extern char SlippWii_AccountLine[64];

#endif
