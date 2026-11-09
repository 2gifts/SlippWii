# SlippWii rollback: a scene's heaps are about to be destroyed, so the game is over. A Gecko
# C2 code at lbHeap_80015900's entry (0x80015900, mflr r0): the pad goes back to being read
# at the retrace (rollback.c, rb_heaps_reset, at engine + 0x324).

	.include "find_engine.inc"

	FIND_ENGINE r12, r11, 1f
	addi	r3, r12, 0x40
	addi	r12, r12, 0x40 + 0x324
	stwu	r1, -0x10(r1)
	mflr	r0
	stw	r0, 0x14(r1)
	mtctr	r12
	bctrl
	lwz	r0, 0x14(r1)
	mtlr	r0
	addi	r1, r1, 0x10
1:	mflr	r0			# the replaced instruction
