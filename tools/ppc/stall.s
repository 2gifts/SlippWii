# Slippi Online (Wii): game-loop stall profiler over EXI (diagnostics).
#
# Hooks the entry of __VIRetraceHandler (0x8034E964, Melee NTSC 1.02), as
# heartbeat.s does (build one or the other). During an online game, when
# Melee's game loop has not finished a logic frame for 2 retraces or more
# (its frame counter at 0x80479D58 did not move), it reports where the CPU
# was at each retrace, up to 12 per stall, and once more when the stall ends.
# Melee's pad queue holds 5 polls: a stall past that drops inputs Slippi has
# already sent (the remote side then plays other inputs: a desync).
#
# Report, EXI command 0xDE (big-endian), as heartbeat.s with:
#   0x00 u8  0xDE
#   0x04 u32 retraceCount
#   0x08 u32 interrupted srr0     0x0C u32 interrupted lr    0x10 u32 its sp
#   0x14 u32 scene controller word
#   0x18 u32 OSCurrentThread
#   0x1C u32 main thread state << 16
#   0x20 u32 main thread srr0     0x24 u32 its lr            0x28 u32 logic frame counter
#   0x2C u32 'S' << 24 | ended << 16 | retraces stalled
#   0x30 4 x u32 saved link registers up the main thread's stack
#   0x40 8 x u32 saved link registers up the interrupted stack

	.set FRAME, 0x120
	.set MAIN_THREAD, 0x804A83C8

	mflr	r0
	stw	r0, 4(r1)
	stwu	r1, -FRAME(r1)
	stmw	r3, 0x08(r1)

	bl	state_end
state:
	.long	0			# 0: logic counter last seen
	.long	0			# 4: retraces it has not moved
	.long	0			# 8: reports sent in this stall
state_end:
	mflr	r30

	# Online games only (scene minor 0x02, major 0x08)
	lis	r8, 0x8048
	lwz	r0, -0x62D0(r8)		# 0x80479D30
	rlwinm	r0, r0, 8, 16, 31
	cmplwi	r0, 0x0208
	bne	reset

	lis	r8, 0x8048
	lwz	r29, -0x62A8(r8)	# 0x80479D58: logic frames run
	lwz	r6, 0(r30)
	cmplw	r29, r6
	bne	moved
	lwz	r6, 4(r30)		# no logic frame since the last retrace
	addi	r6, r6, 1
	stw	r6, 4(r30)
	cmpwi	r6, 2
	blt	done
	lwz	r7, 8(r30)
	cmpwi	r7, 12
	bge	done
	addi	r7, r7, 1
	stw	r7, 8(r30)
	li	r28, 0			# not ended
	b	report

moved:
	stw	r29, 0(r30)
	lwz	r6, 4(r30)
	li	r0, 0
	stw	r0, 4(r30)
	stw	r0, 8(r30)
	cmpwi	r6, 2
	blt	done
	li	r28, 1			# the stall ended after r6 retraces
	b	report

reset:
	li	r0, 0
	stw	r0, 4(r30)
	stw	r0, 8(r30)
	b	done

# r6 = retraces stalled, r28 = ended, r29 = logic counter, r4 = interrupted context
report:
	addi	r31, r1, 0x9F
	rlwinm	r31, r31, 0, 0, 26	# 32-byte aligned buffer at 0x80..0x11F
	li	r0, 0xDE
	rlwinm	r0, r0, 24, 0, 7
	stw	r0, 0x00(r31)
	lis	r5, 0x804D
	lwz	r0, 0x7420(r5)		# retraceCount
	stw	r0, 0x04(r31)
	lwz	r0, 0x198(r4)		# OSContext.srr0
	stw	r0, 0x08(r31)
	lwz	r0, 0x84(r4)		# OSContext.lr
	stw	r0, 0x0C(r31)
	lwz	r5, 0x04(r4)		# OSContext.gpr[1]
	stw	r5, 0x10(r31)
	lis	r8, 0x8048
	lwz	r0, -0x62D0(r8)
	stw	r0, 0x14(r31)
	lis	r8, 0x8000
	lwz	r0, 0xE4(r8)
	stw	r0, 0x18(r31)
	lis	r9, MAIN_THREAD@ha
	addi	r9, r9, MAIN_THREAD@l
	lwz	r0, 0x2C8(r9)		# OSThread.state (u16) << 16
	stw	r0, 0x1C(r31)
	lwz	r0, 0x198(r9)
	stw	r0, 0x20(r31)
	lwz	r0, 0x84(r9)
	stw	r0, 0x24(r31)
	stw	r29, 0x28(r31)
	lis	r0, 0x5300		# 'S'
	rlwinm	r7, r28, 16, 8, 15
	or	r0, r0, r7
	clrlwi	r7, r6, 16
	or	r0, r0, r7
	stw	r0, 0x2C(r31)

	li	r10, 0x40		# the interrupted stack: 8 callers
	li	r11, 8
	bl	walk
	lwz	r5, 0x04(r9)		# the main thread's saved stack: 4 callers
	li	r10, 0x30
	li	r11, 4
	bl	walk

	mr	r3, r31
	li	r4, 0x60
	li	r5, 1			# write
	lis	r12, 0x8000
	ori	r12, r12, 0x55F0	# FN_EXITransferBuffer
	mtctr	r12
	bctrl
	b	done

# Stores up to r11 saved link registers from the stack at r5 into the
# buffer from offset r10 (0 past the stack's end). Uses r5-r8, r10, r11.
walk:
	lis	r6, 0x8000
	lis	r7, 0x8180
walk_loop:
	cmpwi	r11, 0
	beqlr
	li	r8, 0
	cmplw	r5, r6
	blt	walk_store
	cmplw	r5, r7
	bge	walk_store
	lwz	r5, 0(r5)		# back chain
	cmplw	r5, r6
	blt	walk_zero
	cmplw	r5, r7
	bge	walk_zero
	lwz	r8, 4(r5)		# saved lr
	b	walk_store
walk_zero:
	li	r5, 0
walk_store:
	stwx	r8, r31, r10
	addi	r10, r10, 4
	subi	r11, r11, 1
	b	walk_loop

done:
	lmw	r3, 0x08(r1)
	addi	r1, r1, FRAME
	lwz	r0, 4(r1)
	mtlr	r0
	mflr	r0			# the replaced instruction
