# SlippWii rollback: the game's file loads finish before the frame goes on.
#
# lbFile_800164A4 starts a file load (HSD DevCom) and returns; a callback
# later tells the game's loader (the preload cache, effects, Stadium's
# transformations) it is done. The loaders are rewound with the game, DevCom
# and the disc are not (excludes.py): a load that finishes between a capture
# and a load back to it is done for DevCom and never for the game, whose
# effects then lack their models. On a PC (Slippi Dolphin reads the disc at
# once) a load never spans a savestate. So in online games a load is waited
# for here, as lbFile_800161C4 does, up to a second.
#
# Gecko C2 at 0x8001656C (lmw r24, 96(r1), after the bl HSD_DevComRequest):
# r3 holds the request's number (dcReq). r0 and r3-r12 are free; r24-r31 are
# restored by the replaced instruction. The waits are counted in the engine
# (Engine.load_waits, .load_wait_max; find_engine.inc).

	.include "find_engine.inc"

	lis	r12, 0x8048
	lbz	r11, -0x62D0(r12)	# 0x80479D30: the scene, major
	cmpwi	r11, 8
	bne	9f
	lbz	r11, -0x62CD(r12)	# minor: an online game
	cmpwi	r11, 2
	bne	9f
	mfmsr	r11
	andi.	r11, r11, 0x8000	# interrupts off: it could not finish
	beq	9f
	li	r4, 0			# waited?
	mftb	r10
1:	lis	r9, 0x804C		# devComStatus[0..3]: the queued requests
	ori	r9, r9, 0x6320
	li	r8, 4
	mtctr	r8
2:	lwz	r7, 0(r9)
	li	r8, 64			# (a list's length, at most)
3:	cmpwi	r7, 0
	beq	4f
	lwz	r6, 4(r7)		# ->dcReq
	cmpw	r6, r3
	beq	5f			# still queued
	lwz	r7, 0(r7)		# ->next
	addic.	r8, r8, -1
	bne	3b
4:	addi	r9, r9, 4
	bdnz	2b
	b	6f			# done
5:	li	r4, 1
	mftb	r6
	subf	r6, r10, r6
	lis	r5, 0x039E		# a second: 60.75 million ticks (0x039EF8B0)
	ori	r5, r5, 0xF8B0
	cmplw	r6, r5
	blt	1b
6:	cmpwi	r4, 0
	beq	9f
	mftb	r6			# the wait, into the engine
	subf	r6, r10, r6
	FIND_ENGINE r12, r11, 9f
	lwz	r11, (0x40 + 0x10)(r12)	# engine + load_waits
	addi	r11, r11, 1
	stw	r11, (0x40 + 0x10)(r12)
	lwz	r11, (0x40 + 0x14)(r12)	# engine + load_wait_max
	cmplw	r6, r11
	ble	9f
	stw	r6, (0x40 + 0x14)(r12)
9:	lmw	r24, 96(r1)		# the replaced instruction
