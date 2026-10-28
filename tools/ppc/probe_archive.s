# Slippi Online (Wii): archive probe (diagnostics).
#
# Hooks the entry of lbArchive_InitializeDAT(archive r3, data r4, length r5)
# (0x80016A54, Melee NTSC 1.02), which parses every .dat/.usd the game loads
# and asserts when HSD_ArchiveParse rejects it. Sends EXI command 0xDD:
#
#   0x00 u8 0xDD   0x01 u8 2 (archive probe)
#   0x04 u32 archive   0x08 u32 data   0x0C u32 length   0x10 u32 caller (lr)
#   0x14 u32 reserved  0x18 8 x u32 the first words of the data (when in MEM1)

	.set FRAME, 0x100

	mflr	r0
	stw	r0, 4(r1)
	stwu	r1, -FRAME(r1)
	stmw	r3, 0x08(r1)

	addi	r31, r1, 0x9F
	rlwinm	r31, r31, 0, 0, 26
	li	r6, 0xDD
	rlwinm	r6, r6, 24, 0, 7
	oris	r6, r6, 2
	stw	r6, 0x00(r31)
	stw	r3, 0x04(r31)
	stw	r4, 0x08(r31)
	stw	r5, 0x0C(r31)
	stw	r0, 0x10(r31)
	li	r6, 0
	stw	r6, 0x14(r31)

	li	r7, 0			# 8 header words, or zeros
	lis	r8, 0x8000
	cmplw	r4, r8
	blt	header_zero
	lis	r8, 0x817F
	cmplw	r4, r8
	bge	header_zero
	li	r6, 0
header_copy:
	lwzx	r7, r4, r6
	addi	r9, r6, 0x18
	stwx	r7, r31, r9
	addi	r6, r6, 4
	cmpwi	r6, 0x20
	blt	header_copy
	b	send
header_zero:
	li	r6, 0x18
header_zero_loop:
	stwx	r7, r31, r6
	addi	r6, r6, 4
	cmpwi	r6, 0x38
	blt	header_zero_loop

send:
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
	mflr	r0			# the replaced instruction
