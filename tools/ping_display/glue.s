# Slippi Online (Wii): the ping in an online game's top left corner, as Slippi
# Dolphin shows it with "Show NetPlay Ping" on (ping_display.c).
#
# One Gecko C2 code at 0x802F6698, the instruction after Slippi's own in-game
# set-up (Online/Menus/InGame/InitInGame.asm, at 0x802F6694), which makes the
# HUD canvas this draws on. ping_load makes the text and a GObj whose process,
# `think`, runs ping_think every frame. Both go with the scene.
#
# Position independent (built by tools/pic_code.py): the entry code, the data
# (struct Ui), `think`, the C, then `tail`: the replaced instruction, which the
# C2 code's branch back follows.

	.section .pic_start, "ax"
	.globl	_start
_start:
	stwu	r1, -0x90(r1)
	stmw	r3, 0x08(r1)
	stw	r0, 0x80(r1)		# the function's return address, for its mtlr
	mflr	r0
	stw	r0, 0x84(r1)
	bl	1f
1:	mflr	r3
	addi	r3, r3, (ui - 1b)	# Ui *
	addi	r4, r3, (think - ui)
	bl	ping_load		# (Ui *, think)
	lwz	r0, 0x84(r1)
	mtlr	r0
	lwz	r0, 0x80(r1)
	lmw	r3, 0x08(r1)
	addi	r1, r1, 0x90
	b	tail

# struct Ui (ping_display.c). On Slippi's HUD canvas a text unit is about 1.08
# screen pixels (measured in Dolphin, 640x480): this puts the text 52 pixels from
# the left, inside a CRT's overscan, its top level with the game timer's. Its digits
# are 18 lines tall: about 5.5 mm on a 10 inch tube, smaller than the timer's 24.
	.balign	4
ui:
	.long	0			# 0x00 text (0: nothing to show)
	.short	0			# 0x04 frames since the last read
	.byte	0, 0			# 0x06 subtexts: shadow, front
	.float	-248			# 0x08 x
	.float	-202			# 0x0C y
	.float	2			# 0x10 the shadow's offset
	.float	0.66			# 0x14 size (twice Slippi's "Delay: Nf")
	.float	0.1			# 0x18 text scale (Slippi's)
	.long	0x00FFFFFF		# 0x1C cyan: Dolphin's OSD::Color::CYAN
	.long	0x000000FF		# 0x20 black: its shadow
	.float	0.5			# 0x24 size with several opponents
	.balign	4
fmt:
	.string	"Ping: %d"		# 0x28, by the number of opponents
	.org	fmt + 24
	.string	"Ping: %d \x81\x62 %d"	# Shift-JIS bar: Melee's font has no ASCII |
	.org	fmt + 48
	.string	"Ping: %d \x81\x62 %d \x81\x62 %d"
	.org	fmt + 72
	.balign	4

# GObj process: think(gobj)
think:
	stwu	r1, -0x10(r1)
	mflr	r0
	stw	r0, 0x14(r1)
	bl	2f
2:	mflr	r4
	addi	r4, r4, (ui - 2b)
	bl	ping_think		# (gobj, Ui *)
	lwz	r0, 0x14(r1)
	mtlr	r0
	addi	r1, r1, 0x10
	blr

	.section .pic_tail, "ax"
tail:
	addi	r1, r1, 0x18		# the replaced instruction
