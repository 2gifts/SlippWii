# SlippWii diagnostics (test builds only: build_online_boot.py --latency): how long a
# button press takes from the controller's reading to the screen, measured inside the
# console (latency_probe.c).
#
# Three Gecko C2 codes. This one, with the C and its state, in HSD_VICopyXFBAsync at
# 0x803762A8 (stw r29, 5604(r30): the frame's picture is in frame buffer r29, about to
# be handed to the GPU's draw-done; the volatile registers and lr are free there).
# pad_hook.s (PADRead's end) and retrace_hook.s (HSD_VIPreRetraceCB's entry) find this
# code through the branch the code handler put at 0x803762A8, and call its second and
# third instructions.
#
# Position independent (tools/pic_code.py): entry, data, C, tail.

	.section .pic_start, "ax"
	.globl	_start
_start:
	b	copy_entry		# + 0: from the code handler's branch
	b	pad_entry		# + 4: lat_pad (r3 = PADStatus[4]), as a function
	b	retrace_entry		# + 8: lat_retrace, as a function

copy_entry:
	stwu	r1, -0x10(r1)
	bl	1f
1:	mflr	r3
	addi	r3, r3, (lat - 1b)
	mr	r4, r29
	bl	lat_copy		# (Lat *, idx)
	addi	r1, r1, 0x10
	b	tail

pad_entry:
	mr	r4, r3
	mflr	r0
	bl	2f
2:	mflr	r3
	addi	r3, r3, (lat - 2b)
	mtlr	r0
	b	lat_pad			# (Lat *, status)

retrace_entry:
	mflr	r0
	bl	3f
3:	mflr	r3
	addi	r3, r3, (lat - 3b)
	mtlr	r0
	b	lat_retrace		# (Lat *)

# struct Lat (latency_probe.c)
	.balign	8
lat:
	.space	0xC0

# u32 tb(void): the timebase's low word (60.75 ticks a microsecond: 70 s to wrap)
	.globl	tb
tb:
	mftb	r3
	blr

# u32 irq_off(void) / irq_restore(u32 msr)
	.globl	irq_off
irq_off:
	mfmsr	r3
	rlwinm	r4, r3, 0, 17, 15	# clear EE
	mtmsr	r4
	isync
	blr
	.globl	irq_restore
irq_restore:
	mtmsr	r3
	isync
	blr

	.section .pic_tail, "ax"
tail:
	stw	r29, 5604(r30)		# the replaced instruction
