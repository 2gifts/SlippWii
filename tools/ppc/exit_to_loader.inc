# Back to the Homebrew Channel: the game stops and tells the kernel, and the
# kernel does the rest (main.c, so_diag_launch_loader: it closes the devices
# and the memory card, then asks IOS to start the Homebrew Channel, which
# restarts the console into it).
#
# Interrupts and audio DMA off, the kernel told (RESET_STATUS 0x1DEA) and
# waited for, the step reported to its log (SO_EXIT_STEP 1: "stopped, over to
# you"), then nothing until IOS takes the PowerPC away. Does not return.
# Uses r3-r6.
#
# (Until 2026-11-13 this went on as Nintendont's own exit combo does
# (loader/source/ppc/PADReadGC.c, DoExit): caches and memory protection off,
# Nintendont's return stub copied to 0x80004000 and run. On a console that
# route never got back; test builds were brought back by the kernel ten seconds
# later, which is now the way for every build, with no wait.)

.macro EXIT_TO_LOADER
	mfmsr	r3
	rlwinm	r3, r3, 0, 17, 15	# interrupts off
	mtmsr	r3
	lis	r4, 0xCC00
	lhz	r3, 0x5036(r4)		# DSP: audio DMA off
	rlwinm	r3, r3, 0, 17, 15
	sth	r3, 0x5036(r4)
	lis	r5, 0xD300
	li	r3, 0x1DEA
	stw	r3, 0x3420(r5)		# RESET_STATUS: the kernel closes down
9001:	lwz	r6, 0x3420(r5)
	cmpw	r6, r3
	beq	9001b
	li	r3, 1
	stw	r3, 0x34E8(r5)		# exit step 1 (so_diag.h SO_EXIT_STEP): stopped, over to the kernel
9002:	b	9002b
.endm
