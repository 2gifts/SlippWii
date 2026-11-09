# SlippWii rollback, verify builds: where the GPU's copy to a texture goes,
# [dest, dest + Engine.copy_size), into Engine.copies (10 ranges at 0x20; the
# next at Engine.copies_next, 0x1C). A destination seen before keeps its
# entry: the game copies to the same textures each frame (shadows, the screen
# for refraction). See gx_copy_dst.s.
#
# Gecko C2 at GXCopyTex(dest, clear)'s entry (0x8033DCBC, clrlwi. r0, r4, 24,
# which sets cr0: last). r0, r5-r12 and ctr are free.

	.include "find_engine.inc"

	FIND_ENGINE r12, r11, 9f
	lwz	r11, (0x40 + 0x18)(r12)	# the size
	add	r10, r3, r11		# the end
	addi	r8, r12, 0x40 + 0x20
	li	r7, 10
	mtctr	r7
1:	lwz	r7, 0(r8)		# this destination's entry?
	cmpw	r7, r3
	beq	3f
	addi	r8, r8, 8
	bdnz	1b
	lwz	r7, (0x40 + 0x1C)(r12)	# no: the next one
	addi	r9, r7, 1
	cmplwi	r9, 10
	blt	2f
	li	r9, 0
2:	stw	r9, (0x40 + 0x1C)(r12)
	slwi	r7, r7, 3
	add	r8, r12, r7
	addi	r8, r8, 0x40 + 0x20
	stw	r3, 0(r8)
3:	stw	r10, 4(r8)
9:	clrlwi.	r0, r4, 24		# the replaced instruction
