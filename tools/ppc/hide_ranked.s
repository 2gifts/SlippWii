# SlippWii: no Ranked. Slippi's Online Play menu shows an option only when
# Melee's MainMenu_CheckIfOptionIsUnlocked (0x80229938, NTSC 1.02) says it
# is unlocked; Slippi's own HandleOnlineLockedOptions (0x802299F0) locks
# options the same way when logged out, and has the Ranked line for the
# logged-in state commented out. This locks Ranked always: the option is not
# shown, and the menu starts on the first unlocked one (Unranked).
#
# Ranked is Slippi's protected mode (its set rules and result reporting are
# not implemented in SlippWii either); the kernel refuses a Ranked search
# too (so_client_find_match).
#
# Gecko C2 at the function's entry: r3 = menu (8: Slippi's online submenu),
# r4 = option (0: Ranked, OPTION_RANKED_IDX).

	cmpwi	r3, 8
	bne	1f
	cmpwi	r4, 0
	bne	1f
	li	r3, 0			# locked
	blr				# (lr is still the caller's)
1:	mflr	r0			# the replaced instruction
