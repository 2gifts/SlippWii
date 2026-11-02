# Slippi Online (Wii): the online CSS's settings. Slippi Dolphin has them in
# its settings windows; a console has none, so this adds lines under Slippi's
# User and Connect Code: "Delay Frames", "－ N ＋", and "Rumble", "On"/"Off",
# whose buttons the hand cursor presses with A (css_delay.c).
#
# One Gecko C2 code at 0x802669DC, in mnCharSel_Scene_OnEnter just after the
# CSS is built (mnCharSel_802640A0, where Slippi's LoadCSSText has made its
# own text): css_delay_load makes the text and a GObj whose process,
# `think`, runs css_delay_think every frame. Both go with the scene.
#
# Rumble off: css_delay_load also points PADControlMotor's first instruction
# (0x8034DEBC, mflr r0) at `motor`, which turns PAD_MOTOR_RUMBLE into
# PAD_MOTOR_STOP in Slippi's online scenes while the switch is off. (Melee's
# own Options > Rumble can't do it online: it is per in-game port, and Slippi
# redirects the local player's rumble to the controller's port.)
#
# Position independent (built by tools/pic_code.py): the entry code, the
# data (struct Ui), `think`, `motor`, `flush_code`, the C, then `tail`: the
# replaced instruction, which the C2 code's branch back follows.

	.section .pic_start, "ax"
	.globl	_start
_start:
	stwu	r1, -0x90(r1)
	stmw	r3, 0x08(r1)
	mflr	r0
	stw	r0, 0x84(r1)
	bl	1f
1:	mflr	r3
	addi	r3, r3, (ui - 1b)		# Ui *
	addi	r4, r3, (think - ui)
	addi	r5, r3, (motor - ui)
	bl	css_delay_load			# (Ui *, think, motor)
	lwz	r0, 0x84(r1)
	mtlr	r0
	lmw	r3, 0x08(r1)
	addi	r1, r1, 0x90
	b	tail

# struct Ui (css_delay.c)
	.balign	4
ui:
	.long	0			# 0x00 text (0: not on the online CSS)
	.long	0			# 0x04 delay shown
	.byte	0, 0, 0, 0		# 0x08 subtexts: minus, value, plus; look
	.float	-112			# 0x0C x: Slippi's user display
	.float	110			# 0x10 label y, Direct (under Connect Code)
	.float	65			# 0x14 label y, other modes (no connect code)
	.float	20			# 0x18 the value line below the label
	.float	28			# 0x1C between the buttons and the value
	.float	0.4			# 0x20 label size (Slippi's)
	.float	0.5			# 0x24 value size (Slippi's)
	.float	0.1			# 0x28 text scale (Slippi's)
	.float	0.1			# 0x2C text to cursor x (world units)
	.float	-0.1			# 0x30 text to cursor y
	.float	8			# 0x34 a button's centre from its origin, x
	.float	21			# 0x38 a button's centre from its origin, y
	.float	18			# 0x3C a button's half width (text units)
	.float	14			# 0x40 a button's half height
	.float	4.9			# 0x44 the hand's fingertip from the cursor, x
	.float	0			# 0x48 (world units, measured in Dolphin), y
	.long	0x8E9196FF		# 0x4C gray (Slippi's labels; locked)
	.long	0xFFFFFFFF		# 0x50 white
	.long	0xFFD800FF		# 0x54 yellow (hovered)
	.string	"Delay Frames"		# 0x58
	.balign	4
	.byte	0x81, 0x7C, 0, 0	# 0x68 －
	.byte	0x81, 0x7B, 0, 0	# 0x6C ＋
	.string	"%d"			# 0x70
	.balign	4
	.float	45			# 0x74 the rumble label below the delay label
	.float	14			# 0x78 "On"/"Off"'s centre from its origin, x
	.byte	1			# 0x7C rumble: 1 on, 0 off (read by `motor`)
	.byte	0			# 0x7D the rumble value's subtext
	.byte	0			# 0x7E its colour set: 1 + hovered
	.byte	0
	.string	"Rumble"		# 0x80
	.balign	4
	.string	"On"			# 0x88
	.balign	4
	.string	"Off"			# 0x8C
	.balign	4

# GObj process: think(gobj)
think:
	stwu	r1, -0x10(r1)
	mflr	r0
	stw	r0, 0x14(r1)
	bl	2f
2:	mflr	r4
	addi	r4, r4, (ui - 2b)
	bl	css_delay_think		# (gobj, Ui *)
	lwz	r0, 0x14(r1)
	mtlr	r0
	addi	r1, r1, 0x10
	blr

# PADControlMotor(chan, cmd), from its first instruction's branch: with the
# rumble switch off, in Slippi's online scenes (major 8), PAD_MOTOR_RUMBLE (1)
# becomes PAD_MOTOR_STOP (0). r0 must hold the caller's lr when it goes on
# (the function saves it next); r5 and r6 are free at the entry.
	.globl	motor
motor:
	mflr	r0			# the instruction the branch replaced
	bl	3f
3:	mflr	r5
	lbz	r6, (ui + 0x7C - 3b)(r5)	# Ui.rumble
	cmpwi	r6, 0
	bne	4f
	lis	r6, 0x8048
	lbz	r6, -0x62D0(r6)		# the major scene (0x80479D30)
	cmpwi	r6, 8
	bne	4f
	cmpwi	r4, 1
	bne	4f
	li	r4, 0
4:	lis	r5, 0x8034
	ori	r5, r5, 0xDEC0		# PADControlMotor's second instruction
	mtctr	r5
	bctr

# flush_code(address, bytes): makes written instructions visible
	.globl	flush_code
flush_code:
	add	r4, r3, r4
	rlwinm	r3, r3, 0, 0, 26
5:	dcbst	0, r3
	sync
	icbi	0, r3
	addi	r3, r3, 32
	cmplw	r3, r4
	blt	5b
	sync
	isync
	blr

	.section .pic_tail, "ax"
tail:
	lwz	r0, 0x24(r1)		# the replaced instruction
