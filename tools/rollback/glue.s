# SlippWii rollback: Slippi's savestates on the Wii's own CPU.
#
# Slippi's online code asks its EXI device to capture (0xB1) and load (0xB2)
# savestates through FN_EXITransferBuffer (0x800055F0, a branch into Slippi's
# code). On a PC, Slippi Dolphin copies memory regions; the Wii's kernel (an
# ARM) is far too slow for that. So this wraps FN_EXITransferBuffer: 0xB1 and
# 0xB2 are handled here, on the PowerPC (rollback.c); every other transfer
# goes on to Slippi's function unchanged. docs/research/ROLLBACK.md.
#
# One Gecko C2 code at HSD_PerfSetStartTime's entry (0x8037E214, mflr r0),
# called at the start of every frame: rb_frame installs the wrapper once
# Slippi's codeset has set up FN_EXITransferBuffer, and turns write tracking
# (the MMU's page tables) on and off with online games.
#
# Position independent (tools/pic_code.py): entry, data, wrapper, helpers,
# the C, then tail.

	.section .pic_start, "ax"
	.globl	_start
_start:
	stwu	r1, -0x90(r1)
	stmw	r3, 0x08(r1)
	mflr	r0
	stw	r0, 0x84(r1)
	bl	1f
1:	mflr	r3
	addi	r3, r3, (engine - 1b)
	addi	r4, r3, (exi_wrapper - engine)
	addi	r5, r3, (data - engine)
	bl	rb_frame			# (Engine *, exi_wrapper, Data *)
	lwz	r0, 0x84(r1)
	mtlr	r0
	lmw	r3, 0x08(r1)
	addi	r1, r1, 0x90
	b	tail

# struct Engine (rollback.c)
	.balign	32
engine:
	.space 0x320
# frame_end.s calls this at engine + 0x320
	b	rb_render_built			# (Engine *)
# heaps_reset.s calls this at engine + 0x324
	b	rb_heaps_reset			# (Engine *)
# trace_hook.s calls this at engine + 0x328
	b	rb_trace_hook			# (Engine *, which, argument)
# struct Data (rollback.c)
data:
	# Slippi Dolphin's savestate regions (SlippiSavestate.cpp; the heap's
	# bounds are read from 0x804D76B8/0x804D76BC), and the parts left out:
	# Dolphin's (sound, the XFB, a few words), and the state of the I/O
	# drivers (the disc, ARAM, HSD DevCom, alarms, controllers). On a PC the
	# disc and ARAM finish at once, so a savestate never falls in one of
	# their transfers; on a Wii it often does (music streams from the disc),
	# and a driver rewound under a transfer crashes or hangs the game. The
	# game is rewound; the I/O is not, as Slippi already does for the sound.
	# Start, end pairs, sorted, not overlapping.
	.long	0x80005520, 0x80005940
	.long	0x803B7240, 0x804DEC00
	.long	0x8065C000, 0x8071B000
	.long	0, 0				# the heap
	.long	0x803BB340, 0x803BB380		# lbaudio_ax bank loader: cur_ssm_file
	.long	0x80400C08, 0x80401048		# dsp, dvd .data
	.long	0x80402358, 0x80402420		# OSSerial .data
	.long	0x80402C90, 0x80402FF0		# pad, vi .data
	.long	0x804031A0, 0x80403240		# AXVPB .data
	.long	0x80407D90, 0x80408480		# synth, axdriver .data; devcom .data
	.long	0x804337C4, 0x80433C24		# lbaudio_ax bank loader: its tables
	.long	0x80433C64, 0x80452AE4
	.long	0x804A7488, 0x804A76C8		# dvdlow, dvd, dvdqueue, fstload .bss
	.long	0x804A7DB8, 0x804A7FB8		# OSSerial .bss
	.long	0x804A89A0, 0x804A8B38		# pad, vi .bss
	.long	0x804A8D78, 0x804C07E0		# AX .bss
	.long	0x804C0980, 0x804C1F78
	.long	0x804C28E0, 0x804CE380		# synth, axdriver .bss; devcom .bss: requests, queues, relay buffers
	.long	0x804D5B80, 0x804D5BA4		# dvd .sdata
	.long	0x804D5C30, 0x804D5C60		# pad .sdata
	.long	0x804D6018, 0x804D6060		# synth, axdriver .sdata; devcom .sdata
	.long	0x804D6438, 0x804D6454		# lbaudio_ax bank loader: its memory counts
	.long	0x804D7210, 0x804D72F0		# dsp, dvd .sbss
	.long	0x804D7358, 0x804D7360		# the OS alarm queue (relinked after loads)
	.long	0x804D73C8, 0x804D73D8		# OSSerial .sbss
	.long	0x804D73F0, 0x804D75A0		# pad, vi, ai, ar, arq .sbss; AX .sbss
	.long	0x804D7720, 0x804D7810		# synth, axdriver .sbss; devcom .sbss
	.long	0x804DE358, 0x804DE3D8
	.long	0x804DE7F0, 0x804DE870
	.long	0, 0				# end of the list

# FN_EXITransferBuffer(buf, len, mode), via the branch at 0x800055F0: 0xB1
# and 0xB2 are handled here (rb_exi returns 1), the rest goes on to Slippi's
# function with its arguments and return address untouched.
	.globl	exi_wrapper
exi_wrapper:
	stwu	r1, -0x40(r1)
	mflr	r0
	stw	r0, 0x44(r1)
	stw	r3, 0x08(r1)
	stw	r4, 0x0C(r1)
	stw	r5, 0x10(r1)
	mr	r6, r5
	mr	r5, r4
	mr	r4, r3
	bl	2f
2:	mflr	r3
	addi	r3, r3, (engine - 2b)
	bl	rb_exi				# (Engine *, buf, len, mode)
	mr	r11, r3
	bl	3f
3:	mflr	r12
	lwz	r12, (engine + 4 - 3b)(r12)	# Engine.orig_exi
	lwz	r0, 0x44(r1)
	mtlr	r0
	lwz	r3, 0x08(r1)
	lwz	r4, 0x0C(r1)
	lwz	r5, 0x10(r1)
	addi	r1, r1, 0x40
	cmpwi	r11, 0
	bnelr
	mtctr	r12
	bctr

# ---- the inputs on time (rollback.c, rb_early_pad) ------------------------------

# pad_shim: where the branch at 0x80376A28 (right after HSD_PadRenewRawStatus's
# PADRead) to Slippi's TriggerSendInput now goes. A pad already sent to the
# opponent (Engine.early_valid) takes the place of the one just read. Not when
# Slippi skipped the read and branched here itself, leaving this address in r12:
# those are a rollback's inputs. HSD_PadRenewRawStatus's PADStatus[4] is at
# sp + 0x2C; the volatile registers are free here.
pad_shim:
	lis	r11, 0x8037
	ori	r11, r11, 0x6A28
	cmplw	r12, r11
	beq	35f				# (no read: a rollback's inputs)
	# A controller unplugged reads PAD_ERR_NO_CONTROLLER (-1); HSD_PadRenewRawStatus then
	# PADResets its channel, which finds it again once it is plugged back in. But in a
	# game TriggerSendInput writes the netplay pads over these first (the local pad into
	# the local player's port, the opponent's into theirs), so the controller's own
	# channel was never reset (a controller in port 1 for player 2: dead for good once
	# pulled out). So the channels read without a controller are reset here, as read.
	li	r3, 0
	lis	r4, 0x8000
	addi	r5, r1, (0x2C + 0xA)		# PADStatus[0].err
	li	r6, 4
	mtctr	r6
36:	lbz	r6, 0(r5)
	extsb	r6, r6
	cmpwi	r6, -1
	bne	37f
	or	r3, r3, r4
37:	srwi	r4, r4, 1
	addi	r5, r5, 12
	bdnz	36b
	cmpwi	r3, 0
	beq	34f
	lis	r12, 0x8034			# PADReset (mask)
	ori	r12, r12, 0xD5E8
	mtctr	r12
	bctrl
34:	lis	r12, 0				# (a read: anything but 0x80376A28)
35:	mr	r10, r12
	bl	31f
31:	mflr	r12
	addi	r12, r12, (engine - 31b)
	lis	r11, 0x8037
	ori	r11, r11, 0x6A28
	cmplw	r10, r11
	beq	33f
	lwz	r11, 0x70(r12)			# Engine.early_valid
	cmpwi	r11, 0
	beq	33f
	li	r11, 0
	stw	r11, 0x70(r12)
	addi	r9, r12, (0x78 - 4)		# Engine.early_pad
	addi	r8, r1, (0x2C - 4)
	li	r11, 12
	mtctr	r11
32:	lwzu	r10, 4(r9)
	stwu	r10, 4(r8)
	bdnz	32b
33:	lwz	r12, 0x74(r12)			# Engine.orig_input: on to Slippi's code
	mtctr	r12
	bctr

# early_shim: at 0x80019618, fn_800195FC's epilogue, where Slippi's hook branches
# (leaving this address in r12) when it puts off a VI retrace's input renewal
# because a rollback is running: the pad goes out now (rb_early_pad).
early_shim:
	lis	r11, 0x8001
	ori	r11, r11, 0x9618
	cmplw	r12, r11
	bne	42f
	stwu	r1, -0x10(r1)
	bl	41f
41:	mflr	r3
	addi	r3, r3, (engine - 41b)
	bl	rb_early_pad			# (Engine *)
	addi	r1, r1, 0x10
42:	lwz	r0, 0xC(r1)			# the replaced instruction
	lis	r12, 0x8001
	ori	r12, r12, 0x961C
	mtctr	r12
	bctr

# vi_shim: the OS's VI interrupt handler (r3 exception, r4 context): rb_vi_interrupt
vi_shim:
	mr	r5, r4
	mr	r4, r3
	stwu	r1, -0x10(r1)
	mflr	r0
	stw	r0, 0x14(r1)
	bl	61f
61:	mflr	r3
	addi	r3, r3, (engine - 61b)
	bl	rb_vi_interrupt			# (Engine *, exception, context)
	lwz	r0, 0x14(r1)
	mtlr	r0
	addi	r1, r1, 0x10
	blr

# load_shim: FN_LoadSavestate's entry in Slippi's table. rb_load_check says where
# to go on to (r3, r4 and r5 as it left them), or 0 for nowhere.
load_shim:
	stwu	r1, -0x20(r1)
	mflr	r0
	stw	r0, 0x24(r1)
	stw	r3, 0x8(r1)
	stw	r4, 0xC(r1)
	stw	r5, 0x10(r1)
	addi	r4, r1, 0x8
	bl	51f
51:	mflr	r3
	addi	r3, r3, (engine - 51b)
	bl	rb_load_check			# (Engine *, args)
	mtctr	r3
	cmpwi	r3, 0
	lwz	r0, 0x24(r1)
	mtlr	r0
	lwz	r3, 0x8(r1)
	lwz	r4, 0xC(r1)
	lwz	r5, 0x10(r1)
	addi	r1, r1, 0x20
	beqlr
	bctr

# pad_shim_at(), early_shim_at(): their addresses
	.globl	pad_shim_at
pad_shim_at:
	mflr	r0
	bl	43f
43:	mflr	r3
	addi	r3, r3, (pad_shim - 43b)
	mtlr	r0
	blr
	.globl	load_shim_at
load_shim_at:
	mflr	r0
	bl	45f
45:	mflr	r3
	addi	r3, r3, (load_shim - 45b)
	mtlr	r0
	blr
	.globl	vi_shim_at
vi_shim_at:
	mflr	r0
	bl	62f
62:	mflr	r3
	addi	r3, r3, (vi_shim - 62b)
	mtlr	r0
	blr

	.globl	early_shim_at
early_shim_at:
	mflr	r0
	bl	44f
44:	mflr	r3
	addi	r3, r3, (early_shim - 44b)
	mtlr	r0
	blr

# ---- background saves (rollback.c, bg_*) -----------------------------------------

# dsi_stub: the DSI vector's first instruction (0x300, mtsprg 0, r4) is a ba here
# while background saves are on. Real mode: only SPRGs (the OS uses SPRG0 alone,
# here as there). A protection fault is ours, as nothing else protects a page:
# into dsi_tramp, translation on. Anything else goes on into the OS's vector
# just past that first instruction, everything as it found it.
	.balign	4
dsi_stub:
	mtsprg	0, r4
	mtsprg	1, r3
	mfcr	r3
	mtsprg	2, r3
	mfdsisr	r3
	andis.	r3, r3, 0x0800			# DSISR[4]: protection
	beq	81f
	mfsrr0	r3
	mtsprg	3, r3				# the faulting instruction
	mfsrr1	r4				# and its MSR, carried in r4
	lis	r3, 0				# DSI_STUB_TRAMP (40): dsi_tramp's address, set
	ori	r3, r3, 0			# at run time (bg_hook)
	mtsrr0	r3
	mfmsr	r3
	ori	r3, r3, 0x30			# IR DR
	mtsrr1	r3
	rfi
81:	mfsprg	r3, 2
	mtcr	r3
	mfsprg	r3, 1
	ba	0x304

# dsi_tramp: translation on, interrupts off. Everything the C may touch saved in
# its area (T_DSI_SAVE, its address set at run time), its own stack; rb_dsi;
# everything back, and the faulting instruction again.
	.balign	4
dsi_tramp:
	lis	r3, 0
	ori	r3, r3, 0
	stw	r4, 0x00(r3)			# SRR1
	mfsprg	r4, 3
	stw	r4, 0x04(r3)			# SRR0
	mfsprg	r4, 2
	stw	r4, 0x08(r3)			# CR
	mfsprg	r4, 1
	stw	r4, 0x0C(r3)			# r3
	mfsprg	r4, 0
	stw	r4, 0x10(r3)			# r4
	mflr	r4
	stw	r4, 0x14(r3)
	mfctr	r4
	stw	r4, 0x18(r3)
	mfxer	r4
	stw	r4, 0x1C(r3)
	stw	r0, 0x20(r3)
	stw	r1, 0x24(r3)
	stw	r2, 0x28(r3)
	stmw	r5, 0x2C(r3)			# r5-r31, to 0x98
	mr	r31, r3
	addi	r1, r3, 0x7F0			# the stack: the area's top
	li	r0, 0
	stw	r0, 0(r1)
	mfdar	r4
	bl	82f
82:	mflr	r3
	addi	r3, r3, (engine - 82b)
	bl	rb_dsi				# (Engine *, dar)
	mr	r3, r31
	lwz	r4, 0x00(r3)
	mtsrr1	r4
	lwz	r4, 0x04(r3)
	mtsrr0	r4
	lwz	r4, 0x14(r3)
	mtlr	r4
	lwz	r4, 0x18(r3)
	mtctr	r4
	lwz	r4, 0x1C(r3)
	mtxer	r4
	lwz	r4, 0x08(r3)
	mtcr	r4
	lwz	r0, 0x20(r3)
	lwz	r1, 0x24(r3)
	lwz	r2, 0x28(r3)
	lmw	r5, 0x2C(r3)
	lwz	r4, 0x10(r3)
	lwz	r3, 0x0C(r3)
	rfi

# dec_shim: DecrementerExceptionHandler's b to its callback (0x80343E40), while
# background saves are on: every register is in the context (r4) by then.
# rb_dec feeds the copies and, while they're going, sets the next tick and goes
# back to the game itself (OSLoadContext); otherwise on into the callback.
# (The exception number and context ride in r30 and r31, already saved in the
# context; seen with them on the stack: the callback given a null context.)
dec_shim:
	mr	r30, r3
	mr	r31, r4
	stwu	r1, -0x10(r1)
	bl	83f
83:	mflr	r3
	addi	r3, r3, (engine - 83b)
	mr	r4, r31
	bl	rb_dec				# (Engine *, context)
	addi	r1, r1, 0x10
	mr	r3, r30
	mr	r4, r31
	lis	r12, 0x8034			# DecrementerExceptionCallback
	ori	r12, r12, 0x3BC8
	mtctr	r12
	bctr

	.globl	dsi_stub_at
dsi_stub_at:
	mflr	r0
	bl	84f
84:	mflr	r3
	addi	r3, r3, (dsi_stub - 84b)
	mtlr	r0
	blr

	.globl	dsi_tramp_at
dsi_tramp_at:
	mflr	r0
	bl	85f
85:	mflr	r3
	addi	r3, r3, (dsi_tramp - 85b)
	mtlr	r0
	blr

	.globl	dec_shim_at
dec_shim_at:
	mflr	r0
	bl	86f
86:	mflr	r3
	addi	r3, r3, (dec_shim - 86b)
	mtlr	r0
	blr

# ---- helpers ------------------------------------------------------------------

# u64 timebase(void)
	.globl	timebase
timebase:
4:	mftbu	r3
	mftb	r4
	mftbu	r5
	cmpw	r3, r5
	bne	4b
	blr

# u32 irq_off(void) / irq_restore(msr)
	.globl	irq_off
irq_off:
	mfmsr	r3
	rlwinm	r4, r3, 0, 17, 15
	mtmsr	r4
	blr
	.globl	irq_restore
irq_restore:
	mtmsr	r3
	blr

# flush_code(address, bytes)
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

# store_lines / kill_lines / flush_lines(address, lines): dcbst / dcbi / dcbf
	.globl	store_lines
store_lines:
	mtctr	r4
6:	dcbst	0, r3
	addi	r3, r3, 32
	bdnz	6b
	sync
	blr
	.globl	kill_lines
kill_lines:
	mtctr	r4
7:	dcbi	0, r3
	addi	r3, r3, 32
	bdnz	7b
	sync
	blr
	.globl	flush_lines
flush_lines:
	mtctr	r4
8:	dcbf	0, r3
	addi	r3, r3, 32
	bdnz	8b
	sync
	blr

# zero_lines(address, lines): dcbz
	.globl	zero_lines
zero_lines:
	mtctr	r4
9:	dcbz	0, r3
	addi	r3, r3, 32
	bdnz	9b
	blr

# ---- the MMU ------------------------------------------------------------------

# u32 rd_bat(i): DBAT0U, DBAT0L, ... DBAT3L (i = 0-7)
	.globl	rd_bat
rd_bat:
	cmpwi	r3, 0
	mfspr	r4, 536
	beq	20f
	cmpwi	r3, 1
	mfspr	r4, 537
	beq	20f
	cmpwi	r3, 2
	mfspr	r4, 538
	beq	20f
	cmpwi	r3, 3
	mfspr	r4, 539
	beq	20f
	cmpwi	r3, 4
	mfspr	r4, 540
	beq	20f
	cmpwi	r3, 5
	mfspr	r4, 541
	beq	20f
	cmpwi	r3, 6
	mfspr	r4, 542
	beq	20f
	mfspr	r4, 543
20:	mr	r3, r4
	blr

# u32 rd_hid4(void) / rd_xbat(i): Broadway's HID4 and its extra DBATs, DBAT4U,
# DBAT4L, ... DBAT7L (i = 0-7; with HID4's SBE). The Homebrew Channel's
# environment maps MEM2 there (0x90000000, 0xD0000000), and a GameCube game
# leaves them as they are.
	.globl	rd_hid4
rd_hid4:
	mfspr	r3, 1011
	blr
	.globl	rd_xbat
rd_xbat:
	cmpwi	r3, 0
	mfspr	r4, 568
	beq	22f
	cmpwi	r3, 1
	mfspr	r4, 569
	beq	22f
	cmpwi	r3, 2
	mfspr	r4, 570
	beq	22f
	cmpwi	r3, 3
	mfspr	r4, 571
	beq	22f
	cmpwi	r3, 4
	mfspr	r4, 572
	beq	22f
	cmpwi	r3, 5
	mfspr	r4, 573
	beq	22f
	cmpwi	r3, 6
	mfspr	r4, 574
	beq	22f
	mfspr	r4, 575
22:	mr	r3, r4
	blr

# set_dbat0 / set_dbat2 / set_dbat3(upper, lower)
	.globl	set_dbat0
set_dbat0:
	sync
	mtspr	536, r3
	mtspr	537, r4
	isync
	blr
	.globl	set_dbat2
set_dbat2:
	sync
	mtspr	540, r3
	mtspr	541, r4
	isync
	blr
	.globl	set_dbat3
set_dbat3:
	sync
	mtspr	542, r3
	mtspr	543, r4
	isync
	blr

# u32 rd_sdr1(void) / wr_sdr1(v)
	.globl	rd_sdr1
rd_sdr1:
	mfspr	r3, 25
	blr
	.globl	wr_sdr1
wr_sdr1:
	sync
	mtspr	25, r3
	isync
	blr

# u32 rd_sr(ea) / wr_sr(ea, v): the segment register for an address
	.globl	rd_sr
rd_sr:
	mfsrin	r3, r3
	blr
	.globl	wr_sr
wr_sr:
	sync
	mtsrin	r4, r3
	isync
	blr

# set_dec(ticks): the decrementer (the OS's alarm timer)
	.globl	set_dec
set_dec:
	mtdec	r3
	blr

# tlb_flush(): every TLB entry for MEM1 (24 MB at 0x80000000), each page's
# by its own address. A store sets a page's C bit only when its TLB entry's C
# is still 0, so an entry left from before a scan cleared the bit would hide
# every later write to that page (the hottest pages, which stay in the TLB).
# By address, whatever bits Broadway's TLB is indexed by: 64 tlbie a page
# apart left part of it in place.
	.globl	tlb_flush
tlb_flush:
	sync
	lis	r3, 0x8000
	li	r4, 6144
	mtctr	r4
21:	tlbie	r3
	addi	r3, r3, 0x1000
	bdnz	21b
	sync
	isync
	blr

# tlb_kill(pages, n): the TLB entries of n MEM1 pages (u16 page numbers): the
# pages a scan just cleared the C bits of. Only an entry of a written page can
# hold C = 1 (the store that set it set the PTE's too), so after each scan
# these are the only entries that could hide a write: tlb_flush's guarantee
# for a few hundred tlbie instead of 6144.
	.globl	tlb_kill
tlb_kill:
	cmpwi	r4, 0
	beqlr
	sync
	mtctr	r4
	subi	r3, r3, 2
22:	lhzu	r5, 2(r3)
	slwi	r5, r5, 12
	oris	r5, r5, 0x8000
	tlbie	r5
	bdnz	22b
	sync
	isync
	blr

# ---- the locked cache ---------------------------------------------------------

# Broadway's locked cache and its DMA engine (libogc's __LCEnable,
# LCAllocTags, LCLoadBlocks/LCStoreBlocks, LCDisable): the upper 16 KB of the
# data cache at 0xE0000000 (DBAT3 maps it); the DMA's memory side is a
# physical address.
	.globl	lc_on
lc_on:
	lis	r3, 0x8000
	li	r4, 1024
	mtctr	r4
30:	dcbt	0, r3
	dcbst	0, r3
	addi	r3, r3, 32
	bdnz	30b
	sync
	mfspr	r4, 920
	oris	r4, r4, 0x100F
	mtspr	920, r4
	nop
	nop
	nop
	nop
	isync
	lis	r4, 0xE000
	li	r5, 512
	mtctr	r5
31:	.long	0x100027EC		# dcbz_l r0, r4
	addi	r4, r4, 32
	bdnz	31b
	blr

	.globl	lc_off
lc_off:
	lis	r3, 0xE000
	li	r4, 512
	mtctr	r4
32:	dcbi	0, r3
	addi	r3, r3, 32
	bdnz	32b
	mfspr	r4, 920
	rlwinm	r4, r4, 0, 4, 2		# clear LCE
	rlwinm	r4, r4, 0, 16, 11	# and the DMA error enables (0x000F0000)
	mtspr	920, r4
	isync
	blr

	.globl	lc_queue
lc_queue:
	mfspr	r3, 920
	rlwinm	r3, r3, 8, 28, 31
	blr

# lc_load(lc, phys, blocks) / lc_store(lc, phys, blocks): queue one DMA
# (32-byte blocks, 1-128; 128 is 0)
	.globl	lc_load
lc_load:
	rlwinm	r6, r5, 30, 27, 31
	or	r6, r6, r4
	mtspr	922, r6
	rlwinm	r6, r5, 2, 28, 29
	or	r6, r6, r3
	ori	r6, r6, 0x12
	mtspr	923, r6
	blr
	.globl	lc_store
lc_store:
	rlwinm	r6, r5, 30, 27, 31
	or	r6, r6, r4
	mtspr	922, r6
	rlwinm	r6, r5, 2, 28, 29
	or	r6, r6, r3
	ori	r6, r6, 0x02
	mtspr	923, r6
	blr

	.section .pic_tail, "ax"
tail:
	mflr	r0			# the replaced instruction
