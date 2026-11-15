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
# r29 never being -1 also leaves the visual buffer fix of Slippi's codes ("Polling
# Drift Fix + VB" [tauKhan]: its instruction at 0x80376200 sets a flag for a frame
# that waited, and the game's loop then leaves one frame undrawn) without its
# trigger. That is on purpose. Online the flag is the rollback engine's
# (direct_track sets it itself). Offline it keeps SlippWii's timing that of Slippi
# Nintendont's default, the polling drift fix alone: 33.4 ms from the reading to
# the retrace on the main menu, against 41.7 for Melee's own (with the flag live
# it would be 16.7, faster than the console players know; measured 2026-11-12).
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
