# SlippWii diagnostics (latency_probe.c): every retrace, before the game picks the next
# frame buffer. A Gecko C2 code at HSD_VIPreRetraceCB's entry (0x803759E4, mflr r0; r3 is
# its argument). Calls lat_retrace() in the probe's code (glue.s).

	.include "find_probe.inc"

	FIND_PROBE r12, r11, 1f
	mflr	r0
	stwu	r1, -0x20(r1)
	stw	r0, 0x24(r1)
	stw	r3, 0x08(r1)
	addi	r12, r12, 8
	mtctr	r12
	bctrl
	lwz	r3, 0x08(r1)
	lwz	r0, 0x24(r1)
	mtlr	r0
	addi	r1, r1, 0x20
1:	mflr	r0			# the replaced instruction
