# SlippWii: back to the Homebrew Channel when the kernel says so.
#
# The kernel's word (so_diag.h, 0xD30034E0) reads "EXIT" once the player has
# chosen Exit in the Online Play menu and said yes, or chosen Update there
# (the kernel takes Slippi's log-out and update commands for it: so_exi.c),
# and, in test builds, once the PC asks (kernel test_control), so automated
# tests can start the next build by themselves. The kernel sets the word to 0
# at every start (main.c).
#
# Hooks __VIRetraceHandler at 0x8034E968 (lis r3, 0xCC00, after the entry
# that tools/ppc/stall.s and heartbeat.s use): at every retrace, so even with
# Melee's main thread stuck. r3 is free here (the replaced instruction sets
# it), and r11 (volatile); r0, r1 and r4 are not touched unless leaving.
# A retrace costs one more read.
#
# TEST (build_online_boot.py --test-exit): the retraces are counted for the
# kernel's log (so_diag.c), and the word "HANG" (test_hang_frame) stops here
# for good, as a hung game does.

	.include "exit_to_loader.inc"	# (stops, and the kernel starts the Homebrew Channel)

	lis	r3, 0xD300
.if TEST
	lwz	r11, 0x34E4(r3)		# retraces seen
	addi	r11, r11, 1
	stw	r11, 0x34E4(r3)
.endif
	lwz	r3, 0x34E0(r3)		# the kernel's word
.if TEST
	xoris	r11, r3, 0x4841		# "HANG"?
	cmplwi	r11, 0x4E47
	bne	2f
3:	b	3b			# interrupts off: as a hung game is
2:
.endif
	xoris	r3, r3, 0x4558		# "EXIT"?
	cmplwi	r3, 0x4954
	bne	1f
	EXIT_TO_LOADER
1:	lis	r3, 0xCC00		# the replaced instruction
