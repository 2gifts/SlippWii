# SlippWii: which options Slippi's Online Play menu shows.
#
# The menu shows an option only when Melee's MainMenu_CheckIfOptionIsUnlocked
# (0x80229938, NTSC 1.02) says it is unlocked; Slippi's own
# HandleOnlineLockedOptions (0x802299F0, inside it) locks options by the
# account's state: logged out, logged in, update required. Two of SlippWii's
# own rules come first, here, at the function's entry:
#
#   Ranked (option 0) is never shown. It is Slippi's protected mode (its set
#   rules and result reporting are not implemented in SlippWii); the kernel
#   refuses a Ranked search too (so_client_find_match). The menu then starts
#   on the first option that is shown.
#
#   Exit (option 6) is always shown. It is Slippi's "Log-out", which Slippi
#   itself locks in every state: its place in the list, its "Are you sure?"
#   box and its command to the EXI device are Slippi's, and on a Wii they
#   mean "back to the Homebrew Channel" (the kernel: so_exi.c; the name and
#   the line under the menu: tools/menu_edits.py; leaving: exit.s).
#
# The two rules do not know of each other: with Ranked shown again (SHOW_RANKED,
# for tests of the list with six options) Exit stays where it is.
#
# Gecko C2 at the function's entry: r3 = menu (8: Slippi's online submenu),
# r4 = option. Returns 0 (locked) or 1.

	cmpwi	r3, 8
	bne	1f
.if SHOW_RANKED == 0
	cmpwi	r4, 0			# OPTION_RANKED_IDX
	bne	2f
	li	r3, 0			# locked
	blr				# (lr is still the caller's)
2:
.endif
	cmpwi	r4, 6			# OPTION_LOGOUT_IDX: SlippWii's Exit
	bne	1f
	li	r3, 1			# shown
	blr
1:	mflr	r0			# the replaced instruction
