# Slippi Online (Wii): the online input delay, set on the online CSS. Slippi
# Dolphin has it in its settings window; a console has none, so this adds a
# line under Slippi's User and Connect Code: "Delay Frames", then "－ N ＋",
# whose buttons the hand cursor presses with A (css_delay.c).
#
# One Gecko C2 code at 0x802669DC, in mnCharSel_Scene_OnEnter just after the
# CSS is built (mnCharSel_802640A0, where Slippi's LoadCSSText has made its
# own text): css_delay_load makes the text and a GObj whose process,
# `think`, runs css_delay_think every frame. Both go with the scene.
#
# Position independent (built by tools/pic_code.py): the entry code, the
# data (struct Ui), `think`, the C, then `tail`: the replaced instruction,
# which the C2 code's branch back follows.

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
	bl	css_delay_load			# (Ui *, think)
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

	.section .pic_tail, "ax"
tail:
	lwz	r0, 0x24(r1)		# the replaced instruction
