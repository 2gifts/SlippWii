# SlippWii: a frame shown twice because HSD's wait for a frame buffer missed the
# retrace that freed one. HSD_VICopyXFBAsync (0x803761C0, NTSC 1.02) waits for
# a buffer to draw into with, inlined:
#
#	803761f4: bl VIWaitForRetrace
#	803761f8: bl HSD_VIGetXFBDrawEnable	<- here
#	803761fc: addi r29, r3, 0
#	80376200: cmpwi r29, -1
#	80376204: beq 803761f4
#
# A retrace that comes between "no buffer free" and VIWaitForRetrace's own
# OSDisableInterrupts is not seen: VIWaitForRetrace counts from after it, and
# sleeps a whole refresh more although that retrace freed a buffer. The frame is
# copied a refresh late and the one before is shown twice. With the pad read
# before the retrace (rollback), a frame's copy often comes right at a retrace
# (seen: 0.2-0.4 repeats a minute at 30 ms ping, each with the GPU idle and the
# copy's buffer wait the whole refresh).
#
# So the check and the wait go with interrupts off between them: VIWaitForRetrace
# with them off still sleeps (the thread switch lets them through) and comes
# back with them off. Returns the buffer in r3, never -1, so the loop above is
# never taken. (Slippi's "Normal Lag Reduction" skips this wait; SlippWii drops
# it, so_files.c, as on a console it tears.)
#
# Gecko C2 in place of the bl at 0x803761F8: r3-r12, r0 and lr are free, as
# they are across that call.

	stwu	r1, -0x10(r1)
	lis	r12, 0x8034			# OSDisableInterrupts
	ori	r12, r12, 0x7364
	mtctr	r12
	bctrl
	stw	r3, 0x8(r1)
1:	lis	r12, 0x8037			# HSD_VIGetXFBDrawEnable
	ori	r12, r12, 0x5E70
	mtctr	r12
	bctrl
	cmpwi	r3, -1
	bne	2f
	lis	r12, 0x8034			# VIWaitForRetrace
	ori	r12, r12, 0xF314
	mtctr	r12
	bctrl
	b	1b
2:	stw	r3, 0xC(r1)
	lwz	r3, 0x8(r1)
	lis	r12, 0x8034			# OSRestoreInterrupts
	ori	r12, r12, 0x738C
	mtctr	r12
	bctrl
	lwz	r3, 0xC(r1)
	addi	r1, r1, 0x10
