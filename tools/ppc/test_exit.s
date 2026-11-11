# Slippi Online (Wii), test builds: back to the Homebrew Channel when the
# PC asks (kernel test_control, so_diag.h), so automated tests can boot the
# next build by themselves.
#
# Hooks __VIRetraceHandler at 0x8034E968 (lis r3, 0xCC00, after the entry
# that tools/ppc/stall.s and heartbeat.s use): at every retrace, so even with
# Melee's main thread stuck. r3 is free here (the replaced instruction sets
# it), and r11 (volatile); r0, r1 and r4 are not touched unless leaving.

	.include "exit_to_loader.inc"

	lis	r3, 0xD300
	lwz	r11, 0x34E4(r3)		# retraces seen, for the kernel's log (so_diag.c)
	addi	r11, r11, 1
	stw	r11, 0x34E4(r3)
	lwz	r3, 0x34E0(r3)		# the kernel's word (so_diag.h): "HANG" (test_hang_frame)?
	xoris	r11, r3, 0x4841
	cmplwi	r11, 0x4E47
	bne	2f
3:	b	3b			# interrupts off: as a hung game is
2:					# "EXIT"?
	xoris	r3, r3, 0x4558
	cmplwi	r3, 0x4954
	bne	1f
	EXIT_TO_LOADER
1:	lis	r3, 0xCC00		# the replaced instruction
