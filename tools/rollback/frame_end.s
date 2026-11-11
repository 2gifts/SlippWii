# SlippWii rollback: when each frame's work ends, for the engine's report.
# A Gecko C2 code at HSD_PerfSetTotalTime's entry (0x8037E328, mflr r0), once
# the frame's render is built. Stores the timebase and counts the call in the
# engine (Engine.end_tb, .ends), found through the branch the code handler put
# at HSD_PerfSetStartTime (0x8037E214) to the engine's code.

	.include "find_engine.inc"

	FIND_ENGINE r12, r11, 1f
	mftb	r0
	stw	r0, (0x40 + 0x08)(r12)	# engine + end_tb
	lwz	r11, (0x40 + 0x0C)(r12)
	addi	r11, r11, 1
	stw	r11, (0x40 + 0x0C)(r12)	# engine + ends
	addi	r3, r12, 0x40		# rb_render_built(engine)
	addi	r12, r12, 0x40 + 0x320
	stwu	r1, -0x10(r1)
	mflr	r0
	stw	r0, 0x14(r1)
	mtctr	r12
	bctrl
	lwz	r0, 0x14(r1)
	mtlr	r0
	addi	r1, r1, 0x10
1:	mflr	r0			# the replaced instruction
