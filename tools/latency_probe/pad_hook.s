# SlippWii diagnostics (latency_probe.c): every controller reading. A Gecko C2 code at
# PADRead's end (0x8034DD94, mr r3, r20: interrupts are back as they were, r31 is past
# the caller's PADStatus[4], and the volatile registers and lr are free: the epilogue
# takes lr from the stack). Calls lat_pad(status) in the probe's code (glue.s).

	.include "find_probe.inc"

	FIND_PROBE r12, r11, 1f
	stwu	r1, -0x10(r1)
	subi	r3, r31, 48
	addi	r12, r12, 4
	mtctr	r12
	bctrl
	addi	r1, r1, 0x10
1:	mr	r3, r20			# the replaced instruction
