# SlippWii rollback, verify builds: the size of the GPU's next copy to a
# texture, which GXCopyTex starts (gx_copy_tex.s keeps where it goes). The GPU
# writes it with no C bit, at its own time: verify leaves those bytes alone.
#
# Gecko C2 at GXSetTexCopyDst(width, height, format, mipmap)'s entry
# (0x8033D5CC, mflr r0). At most 4 bytes a pixel; a mipmap copy is a quarter
# of the size. Engine.copy_size (0x18). r0, r7-r12 are free.

	.include "find_engine.inc"

	FIND_ENGINE r12, r11, 9f
	clrlwi	r10, r3, 16		# width (u16)
	clrlwi	r11, r4, 16		# height
	mullw	r10, r10, r11
	clrlwi.	r11, r6, 24		# mipmap
	bne	1f
	slwi	r10, r10, 2
1:	stw	r10, (0x40 + 0x18)(r12)
9:	mflr	r0			# the replaced instruction
