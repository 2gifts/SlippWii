# Slippi Online (Wii): misaligned float emulation and Melee crash report.
#
# Hooks the entry of __OSUnhandledException (0x80345870, Melee NTSC 1.02),
# which the OS calls for an exception with r3 = exception number, r4 = the
# faulting OSContext, r5 = DSISR, r6 = DAR.
#
# 1. Alignment exceptions (5) from lfs/lfsu/lfd/lfdu/stfs/stfsu/stfd/stfdu:
#    Slippi's online code keeps floats at unaligned offsets (e.g. the CSS
#    user display's stfs f1,10(r30)). A real console's CPU traps a float
#    load or store that is not word aligned; Dolphin performs it. This
#    performs it too: the bytes move with byte loads and stores, the float
#    register with one aligned instruction built for its number, then the
#    thread resumes after the instruction (OSLoadContext).
# 2. Anything else: one 64-byte EXI command 0xDF to the kernel
#    (slippi_online/so_exi.c logs it), then on into the original function,
#    so Melee's own crash handling runs as before. FN_EXITransferBuffer
#    (0x800055f0) flushes the buffer before the DMA, so the report reaches
#    the kernel even though the CPU stops afterwards.
#
# Report (big-endian):
#   0x00 u8  0xDF
#   0x04 u32 exception number
#   0x08 u32 srr0 (faulting instruction)    0x0C u32 srr1
#   0x10 u32 dar  (faulting data address)   0x14 u32 dsisr
#   0x18 u32 lr                             0x1C u32 r1 (stack pointer)
#   0x20 8 x u32 saved link registers up the stack (0 past its end)

	.set FRAME, 0x100
	.set OSLoadContext, 0x80345174

	mflr	r0
	stw	r0, 4(r1)
	stwu	r1, -FRAME(r1)
	stmw	r3, 0x08(r1)

	addi	r31, r1, 0x9F		# a 32-byte aligned buffer at 0x80..0xDF
	rlwinm	r31, r31, 0, 0, 26

# ---- 1. misaligned float loads and stores ------------------------------------
	cmpwi	r3, 5
	bne	report
	# The faulting thread had the float unit (it was running a float
	# instruction), so its float registers are the live ones, whatever
	# OSContext.state says about a saved copy (OSLoadContext does not
	# restore floats; the OS reloads them lazily). Check MSR[FP] anyway.
	lwz	r10, 0x19C(r4)		# srr1
	andi.	r10, r10, 0x2000
	beq	report
	lwz	r7, 0x198(r4)		# srr0
	lwz	r8, 0(r7)		# the instruction
	rlwinm	r9, r8, 6, 26, 31	# primary opcode
	cmplwi	r9, 48
	blt	report
	cmplwi	r9, 55
	bgt	report

	rlwinm	r10, r8, 16, 27, 31	# rA
	extsh	r11, r8			# d
	li	r12, 0
	cmpwi	r10, 0
	beq	1f
	slwi	r12, r10, 2
	lwzx	r12, r4, r12		# gpr[rA]
1:
	add	r11, r11, r12		# effective address
	andi.	r12, r9, 1		# update form: rA = EA
	beq	2f
	slwi	r12, r10, 2
	stwx	r11, r4, r12
2:
	# The aligned instruction: <op & ~1> fN, 0(r31)
	rlwinm	r12, r9, 26, 0, 4	# (op & ~1) << 26
	rlwinm	r10, r8, 0, 6, 10	# frD/frS field, in place
	or	r12, r12, r10
	oris	r12, r12, 31		# rA = r31
	bl	3f
3:
	mflr	r10
	addi	r10, r10, slot - 3b
	stw	r12, 0(r10)
	dcbst	0, r10
	sync
	icbi	0, r10
	isync

	mfmsr	r12			# the float unit may be off here
	ori	r12, r12, 0x2000
	mtmsr	r12
	isync

	li	r12, 4			# bytes: 4 (single) or 8 (double)
	andi.	r0, r9, 2
	beq	4f
	li	r12, 8
4:
	cmplwi	r9, 52
	bge	store

	# load: the bytes into the buffer, then the register
	li	r6, 0
5:
	lbzx	r0, r11, r6
	stbx	r0, r31, r6
	addi	r6, r6, 1
	cmpw	r6, r12
	blt	5b
	bl	slot
	b	resume

store:	# store: the register into the buffer, then the bytes
	bl	slot
	li	r6, 0
6:
	lbzx	r0, r31, r6
	stbx	r0, r11, r6
	addi	r6, r6, 1
	cmpw	r6, r12
	blt	6b

resume:
	lwz	r7, 0x198(r4)		# on after the instruction
	addi	r7, r7, 4
	stw	r7, 0x198(r4)
	mr	r3, r4
	lis	r12, OSLoadContext@h
	ori	r12, r12, OSLoadContext@l
	mtctr	r12
	bctr				# does not return

slot:
	.long	0
	blr

# ---- 2. crash report -----------------------------------------------------------
report:
	lwz	r3, 0x08(r1)		# the arguments, as they came
	lwz	r4, 0x0C(r1)
	lwz	r5, 0x10(r1)
	lwz	r6, 0x14(r1)
	li	r0, 0xDF
	rlwinm	r0, r0, 24, 0, 7	# 0xDF000000: the command byte first
	stw	r0, 0x00(r31)
	stw	r3, 0x04(r31)
	lwz	r0, 0x198(r4)		# OSContext.srr0
	stw	r0, 0x08(r31)
	lwz	r0, 0x19C(r4)		# OSContext.srr1
	stw	r0, 0x0C(r31)
	stw	r6, 0x10(r31)
	stw	r5, 0x14(r31)
	lwz	r0, 0x84(r4)		# OSContext.lr
	stw	r0, 0x18(r31)
	lwz	r5, 0x04(r4)		# OSContext.gpr[1]
	stw	r5, 0x1C(r31)

	# Walk the faulting thread's stack: back chain, then the saved LR.
	li	r6, 0
	lis	r8, 0x8000
	lis	r9, 0x8180
walk:
	cmplwi	r6, 8
	bge	pad
	cmplw	r5, r8
	blt	pad
	cmplw	r5, r9
	bge	pad
	lwz	r5, 0(r5)
	cmplw	r5, r8
	blt	pad
	cmplw	r5, r9
	bge	pad
	lwz	r7, 4(r5)
	slwi	r10, r6, 2
	addi	r10, r10, 0x20
	stwx	r7, r31, r10
	addi	r6, r6, 1
	b	walk
pad:
	li	r7, 0
pad_loop:
	cmplwi	r6, 8
	bge	send
	slwi	r10, r6, 2
	addi	r10, r10, 0x20
	stwx	r7, r31, r10
	addi	r6, r6, 1
	b	pad_loop

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
