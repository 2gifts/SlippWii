# Back to the Homebrew Channel, as Nintendont's own exit combo does
# (loader/source/ppc/PADReadGC.c, DoExit): interrupts and audio DMA off, the
# kernel told (RESET_STATUS 0x1DEA) and waited for, caches off, memory
# protection off, MEM1 on one BAT (not the rollback engine's page table), Nintendont's
# return stub (0x93011810) to 0x80004000 and
# into it; its steps to the kernel's log (SO_EXIT_STEP). Does not return.
# Uses r3-r6.

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
	stw	r3, 0x34E8(r5)		# exit step 1 (so_diag.h SO_EXIT_STEP): let go
	sync
	isync
	mfspr	r3, 1008
	rlwinm	r3, r3, 0, 18, 15	# HID0: caches off
	mtspr	1008, r3
	li	r3, 0xF
	sth	r3, 0x401E(r4)		# memory interface: protection off
	li	r3, 0
	sth	r3, 0x4020(r4)
	li	r3, 0xFF
	sth	r3, 0x4010(r4)
	# MEM1 on one BAT again (256 MB at 0x80000000): the rollback engine may
	# have it on its page table, in MEM2, which the stub's IOS reload wipes
	li	r3, 0
	mtspr	541, r3			# DBAT2 off
	mtspr	540, r3
	li	r3, 2
	mtspr	537, r3			# DBAT0: 0x80000000-0x8FFFFFFF, read/write
	lis	r3, 0x8000
	ori	r3, r3, 0x1FFF
	mtspr	536, r3
	isync
	lis	r5, 0x9301		# the stub, 0x93011810 -> 0x80004000
	ori	r5, r5, 0x1810
	lis	r6, 0x8000
	ori	r6, r6, 0x4000
	li	r3, 0x1800 / 4
	mtctr	r3
9002:	lwz	r3, 0(r5)
	stw	r3, 0(r6)
	addi	r5, r5, 4
	addi	r6, r6, 4
	bdnz	9002b
	lis	r5, 0xD300
	li	r3, 2
	stw	r3, 0x34E8(r5)		# exit step 2: into the stub
	lis	r3, 0xCD80
	li	r4, 0x36
	stw	r4, 4(r3)		# all IOS interrupts again
	lis	r3, 0x8000
	ori	r3, r3, 0x4000
	mtlr	r3
	blr
.endm
