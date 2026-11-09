# SlippWii rollback, test builds: a Gecko C2 code at a function's entry (mflr r0) that
# tells RB_TRACE it was called (rollback.c, rb_trace_hook, at engine + 0x328), with which
# (WHICH, from the build) and the function's first argument. Leaves every register as it was.

	.include "find_engine.inc"

	stwu	r1, -0x90(r1)
	stmw	r3, 0x08(r1)
	mflr	r0
	stw	r0, 0x84(r1)
	mr	r5, r3
	FIND_ENGINE r12, r11, 1f
	addi	r3, r12, 0x40
	li	r4, WHICH
	addi	r12, r12, 0x40 + 0x328
	mtctr	r12
	bctrl
1:	lwz	r0, 0x84(r1)
	mtlr	r0
	lmw	r3, 0x08(r1)
	addi	r1, r1, 0x90
	mflr	r0			# the replaced instruction
