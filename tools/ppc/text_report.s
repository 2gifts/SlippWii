# Slippi Online (Wii): Melee's panics and OS reports over EXI.
#
# Assembled once per hook with -defsym KIND=<n>:
#   1 HSD_Panic(file, line, message)      (0x80388278)
#   2 OSPanic(file, line, format, ...)    (0x80345728)
#   3 OSReport(format, ...)               (0x803456A8)
# At the function's entry it sends EXI command 0xDA (128 bytes) with the
# text the CPU sees, then continues into the function. Melee's asserts end
# in HSD_Panic, whose handler starts the debug console (debugconsole_main.c)
# without any exception, so the crash reporter alone never sees them.
#
#   0x00 u8 0xDA   0x01 u8 kind
#   0x04 u32 r4 (line, or OSReport's first argument)
#   0x08 u32 caller (lr)              0x0C u32 r5
#   0x10 48 bytes: the string at r3 (file, or OSReport's format)
#   0x40 64 bytes: the string at r5 (message or format), or for OSReport
#        the one at r4 (its first argument, when it points into MEM1)
#
# cr1 and the floating-point registers stay untouched (OSReport is
# variadic); only cr0 is used.

	.set FRAME, 0x140

	mflr	r0
	stw	r0, 4(r1)
	stwu	r1, -FRAME(r1)
	stmw	r3, 0x08(r1)

	addi	r31, r1, 0x9F
	rlwinm	r31, r31, 0, 0, 26	# 32-byte aligned buffer at 0x80..0x11F

	li	r6, 0			# zero the 128-byte buffer
	li	r7, 0
zero:
	stwx	r7, r31, r6
	addi	r6, r6, 4
	cmpwi	r6, 0x80
	blt	zero

	li	r6, 0xDA
	rlwinm	r6, r6, 24, 0, 7
	oris	r6, r6, KIND		# kind in the second byte
	stw	r6, 0x00(r31)
	stw	r4, 0x04(r31)
	stw	r0, 0x08(r31)
	stw	r5, 0x0C(r31)

	mr	r6, r3			# string A
	addi	r7, r31, 0x10
	li	r8, 47
	bl	copy
.if KIND == 3
	mr	r6, r4			# OSReport: its first argument
.else
	mr	r6, r5
.endif
	addi	r7, r31, 0x40
	li	r8, 63
	bl	copy

	mr	r3, r31
	li	r4, 0x80
	li	r5, 1			# write
	lis	r12, 0x8000
	ori	r12, r12, 0x55F0	# FN_EXITransferBuffer
	mtctr	r12
	bctrl
	b	done

# Copies at most r8 bytes of the string at r6 to r7 when r6 lies in MEM1.
# Uses r6-r10 and cr0.
copy:
	lis	r9, 0x8000
	cmplw	r6, r9
	bltlr
	lis	r9, 0x8180
	cmplw	r6, r9
	bgelr
copy_loop:
	cmpwi	r8, 0
	beqlr
	lbz	r10, 0(r6)
	cmpwi	r10, 0
	beqlr
	stb	r10, 0(r7)
	addi	r6, r6, 1
	addi	r7, r7, 1
	subi	r8, r8, 1
	b	copy_loop

done:
	lmw	r3, 0x08(r1)
	addi	r1, r1, FRAME
	lwz	r0, 4(r1)
	mtlr	r0
	mflr	r0			# the replaced instruction
