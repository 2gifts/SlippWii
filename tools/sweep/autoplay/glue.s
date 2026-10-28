# Autoplay for the automated Slippi Online sweep: one Gecko C2 code (insert
# at HSD_PadRenewRawStatus, 0x803769FC) that is fully position independent,
# so it runs wherever the code list sits (the Wii's Tournament Mode region,
# or Dolphin's code area).
#
# Each run makes sure Melee's PADRead (0x8034DA00) branches to `wrapper`:
# the wrapper runs the real PADRead through `tramp` (PADRead's first
# instruction, copied, then a branch to the rest; Nintendont replaces
# PADRead with its own stub on the Wii, Dolphin runs Melee's, and either
# works) and then lets autoplay_poll (autoplay.c) write one controller's
# inputs. Inputs enter Melee where a controller's would, before Slippi's
# own input code (TriggerSendInput) reads them, so both sides of an
# online game see exactly the inputs the autoplay chose.
#
# Layout: the entry code, the data (struct Bot, filled in by
# build_autoplay.py), the functions, then `tail`: the replaced
# instruction, which the C2 code's branch back follows.

	.section .ap_start, "ax"
	.globl	_start
_start:
	stwu	r1, -0x90(r1)
	stmw	r3, 0x08(r1)
	mflr	r0
	stw	r0, 0x84(r1)
	bl	1f
1:	mflr	r3
	addi	r3, r3, (bot_data - 1b)		# Bot *
	addi	r4, r3, (wrapper - bot_data)
	addi	r5, r3, (tramp - bot_data)
	bl	autoplay_install		# (Bot *, wrapper, tramp)
	lwz	r0, 0x84(r1)
	mtlr	r0
	lmw	r3, 0x08(r1)
	addi	r1, r1, 0x90
	b	tail

	.balign	8
	.globl	bot_data
bot_data:
	.space	0x200

# PADRead(status) replacement
wrapper:
	stwu	r1, -0x30(r1)
	mflr	r0
	stw	r0, 0x34(r1)
	stw	r3, 0x08(r1)		# the status array
	bl	tramp			# the real PADRead; its blr returns here
	stw	r3, 0x0C(r1)		# its result
	bl	2f
2:	mflr	r4
	addi	r4, r4, (bot_data - 2b)
	lwz	r3, 0x08(r1)
	bl	autoplay_poll		# (PADStatus *status, Bot *bot)
	lwz	r3, 0x0C(r1)
	lwz	r0, 0x34(r1)
	mtlr	r0
	addi	r1, r1, 0x30
	blr

tramp:
	.long	0			# PADRead's first instruction (install)
	.long	0			# b 0x8034DA04 (install)

# flush_code(address, bytes): makes written instructions visible
	.globl	flush_code
flush_code:
	add	r4, r3, r4
	rlwinm	r3, r3, 0, 0, 26
3:	dcbst	0, r3
	sync
	icbi	0, r3
	addi	r3, r3, 32
	cmplw	r3, r4
	blt	3b
	sync
	isync
	blr

	.section .ap_tail, "ax"
tail:
	mflr	r0			# the replaced instruction
