# Slippi Online (Wii): trophy table probe (diagnostics).
#
# Hooks the entry of Toy_803060BC (0x803060BC, Melee NTSC 1.02), which walks
# the trophy tables (r13-18400 Japanese, r13-18396 US; 36-byte entries ended
# by id -1) for trophy r3 and returns field r4. Before it runs, this sends
# EXI command 0xDD: the arguments, the caller, both table pointers, and how
# many entries each table has before its -1 (walked here within MEM1, so the
# probe itself never runs off the end of memory; -1 when no end is found).
#
#   0x00 u8 0xDD      0x04 trophy id      0x08 field        0x0C caller (lr)
#   0x10 jp table     0x14 us table       0x18 jp entries   0x1C us entries
#   0x20 us[0].id     0x24 us[1].id       0x28 jp[0].id     0x2C r13

	.set FRAME, 0x100

	mflr	r0
	stw	r0, 4(r1)
	stwu	r1, -FRAME(r1)
	stmw	r3, 0x08(r1)

	addi	r31, r1, 0x9F
	rlwinm	r31, r31, 0, 0, 26
	li	r5, 0xDD
	rlwinm	r5, r5, 24, 0, 7
	stw	r5, 0x00(r31)
	stw	r3, 0x04(r31)
	stw	r4, 0x08(r31)
	stw	r0, 0x0C(r31)
	stw	r13, 0x2C(r31)

	lwz	r5, -18400(r13)		# jp table
	stw	r5, 0x10(r31)
	bl	count
	stw	r3, 0x18(r31)
	lwz	r5, -18400(r13)
	bl	first
	stw	r3, 0x28(r31)

	lwz	r5, -18396(r13)		# us table
	stw	r5, 0x14(r31)
	bl	count
	stw	r3, 0x1C(r31)
	lwz	r5, -18396(r13)
	bl	first
	stw	r3, 0x20(r31)
	lwz	r5, -18396(r13)
	addi	r5, r5, 36
	bl	first
	stw	r3, 0x24(r31)

	mr	r3, r31
	li	r4, 0x40
	li	r5, 1			# write
	lis	r12, 0x8000
	ori	r12, r12, 0x55F0	# FN_EXITransferBuffer
	mtctr	r12
	bctrl

	lmw	r3, 0x08(r1)
	addi	r1, r1, FRAME
	lwz	r0, 4(r1)
	mtlr	r0
	b	done

# r3 = entries in the table at r5 before an id of -1, or -1. Uses r6-r8.
count:
	li	r3, 0
	lis	r7, 0x8000
	lis	r8, 0x817F		# stay below the end of MEM1
	ori	r8, r8, 0xFF00
count_loop:
	cmplw	r5, r7
	blt	count_fail
	cmplw	r5, r8
	bge	count_fail
	cmplwi	r3, 4000
	bge	count_fail
	lwz	r6, 0(r5)
	cmpwi	r6, -1
	beqlr
	addi	r3, r3, 1
	addi	r5, r5, 36
	b	count_loop
count_fail:
	li	r3, -1
	blr

# r3 = the word at r5 when r5 lies in MEM1, else 0xBAD0BAD0.
first:
	lis	r7, 0x8000
	lis	r8, 0x8180
	cmplw	r5, r7
	blt	first_bad
	cmplw	r5, r8
	bge	first_bad
	lwz	r3, 0(r5)
	blr
first_bad:
	lis	r3, 0xBAD0
	ori	r3, r3, 0xBAD0
	blr

done:
	mflr	r0			# the replaced instruction
