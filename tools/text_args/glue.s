# Slippi Online (Wii): a text format's "%s" never reads a wild address (text_args.c).
#
# One Gecko C2 code at the entry of Text_InitializeSubtext (0x803A6B98, Melee
# NTSC 1.02). This function is variadic: the check changes its saved r5..r10, and
# cr1 (its float arguments' flag) and the float registers stay as they came.
#
# Position independent (built by tools/pic_code.py): the entry code, the data, the
# C, then `tail`: the replaced instruction, which the C2 code's branch back follows.

	.section .pic_start, "ax"
	.globl	_start
_start:
	stwu	r1, -0x90(r1)
	stmw	r3, 0x08(r1)
	mfcr	r0
	stw	r0, 0x80(r1)
	mflr	r0
	stw	r0, 0x84(r1)
	bl	1f
1:	mflr	r5
	addi	r6, r5, (report - 1b)	# the report's format
	addi	r5, r5, (empty - 1b)	# ""
	mr	r3, r4			# the format
	addi	r4, r1, 0x10		# the saved r5..r10
	bl	text_args_check
	lwz	r0, 0x80(r1)
	mtcr	r0
	lwz	r0, 0x84(r1)
	mtlr	r0
	lmw	r3, 0x08(r1)
	addi	r1, r1, 0x90
	b	tail

empty:
	.byte	0
report:
	.string	"text: a bad string argument, in: %s\n"
	.balign	4

	.section .pic_tail, "ax"
tail:
	mflr	r0			# the replaced instruction
