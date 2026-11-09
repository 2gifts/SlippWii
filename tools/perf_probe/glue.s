# SlippWii diagnostics (test builds only: build_online_boot.py --perf): how
# much of each frame Melee spends on game logic and on building the render,
# during online games on a real Wii, and how fast the Wii's CPU copies
# memory. The numbers say how many frames a console rollback could re-run
# per frame, and what a savestate copy costs (docs/research/ROLLBACK.md).
#
# One Gecko C2 code at HSD_PerfSetTotalTime's entry (0x8037E328, mflr r0):
# Melee calls it once per frame, after the logic (HSD_PerfSetCPUTime) and
# the render's build (HSD_PerfSetDrawTime). perf_probe.c keeps statistics
# and reports them over EXI 0xDA (kind 4), which the kernel logs as PERF.
#
# Position independent (tools/pic_code.py): entry, data, C, tail.

	.section .pic_start, "ax"
	.globl	_start
_start:
	stwu	r1, -0x90(r1)
	stmw	r3, 0x08(r1)
	mflr	r0
	stw	r0, 0x84(r1)
	bl	1f
1:	mflr	r3
	addi	r3, r3, (probe - 1b)
	bl	perf_probe_frame		# (Probe *)
	lwz	r0, 0x84(r1)
	mtlr	r0
	lmw	r3, 0x08(r1)
	addi	r1, r1, 0x90
	b	tail

# struct Probe (perf_probe.c)
	.balign	8
probe:
	.float	10000			# 0x00 fractions of a frame -> 1/10000ths
	.long	0			# 0x04 pad
	.space	0x100			# state

# u64 timebase(void)
	.globl	timebase
timebase:
2:	mftbu	r3
	mftb	r4
	mftbu	r5
	cmpw	r3, r5
	bne	2b
	blr

# u32 bats(int n): DBATnU (n 0-3), DBATnL (n 4-7)
	.globl	bat
bat:
	cmpwi	r3, 0
	bne	3f
	mfspr	r3, 536
	blr
3:	cmpwi	r3, 1
	bne	3f
	mfspr	r3, 538
	blr
3:	cmpwi	r3, 2
	bne	3f
	mfspr	r3, 540
	blr
3:	cmpwi	r3, 3
	bne	3f
	mfspr	r3, 542
	blr
3:	cmpwi	r3, 4
	bne	3f
	mfspr	r3, 537
	blr
3:	cmpwi	r3, 5
	bne	3f
	mfspr	r3, 539
	blr
3:	cmpwi	r3, 6
	bne	3f
	mfspr	r3, 541
	blr
3:	mfspr	r3, 543
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

# set_dbat3(upper, lower): maps a block (MEM2 for the copy tests)
	.globl	set_dbat3
set_dbat3:
	sync
	mtspr	542, r3
	mtspr	543, r4
	isync
	blr

# u32 hid0(void) / set_hid0(u32)
	.globl	hid0
hid0:
	mfspr	r3, 1008
	blr
	.globl	set_hid0
set_hid0:
	sync
	mtspr	1008, r3
	isync
	blr

# u32 hid2(void)
	.globl	hid2
hid2:
	mfspr	r3, 920
	blr

# copy_lines(dst, src, lines, mode): 32-byte lines; mode bit 0: dcbz the
# destination line first (no read of it), bit 1: prefetch the source 4 lines
# ahead (dcbt)
	.globl	copy_lines
copy_lines:
	mtctr	r5
	li	r7, 128
6:	andi.	r0, r6, 2
	beq	7f
	dcbt	r4, r7
7:	andi.	r0, r6, 1
	beq	8f
	dcbz	0, r3
8:	lfd	f0, 0(r4)
	lfd	f1, 8(r4)
	lfd	f2, 16(r4)
	lfd	f3, 24(r4)
	stfd	f0, 0(r3)
	stfd	f1, 8(r3)
	stfd	f2, 16(r3)
	stfd	f3, 24(r3)
	addi	r4, r4, 32
	addi	r3, r3, 32
	bdnz	6b
	blr

# Broadway's locked cache and its DMA engine (libogc's __LCEnable,
# LCAllocTags, LCLoadBlocks/LCStoreBlocks, LCDisable). The locked cache is
# the upper 16 KB of the data cache at 0xE0000000 (DBAT3 maps it during the
# test); the DMA's memory side is a physical address.

# lc_on(): clean the data cache (touch and store 32 KB), enable it, claim its lines
	.globl	lc_on
lc_on:
	lis	r3, 0x8000
	li	r4, 1024
	mtctr	r4
9:	dcbt	0, r3
	dcbst	0, r3
	addi	r3, r3, 32
	bdnz	9b
	sync
	mfspr	r4, 920
	oris	r4, r4, 0x100F		# LCE and the DMA error enables
	mtspr	920, r4
	nop
	nop
	nop
	nop
	isync
	lis	r4, 0xE000
	li	r5, 512
	mtctr	r5
10:	.long	0x100027EC		# dcbz_l r0, r4
	addi	r4, r4, 32
	bdnz	10b
	blr

# lc_off(): give the lines back and disable it
	.globl	lc_off
lc_off:
	lis	r3, 0xE000
	li	r4, 512
	mtctr	r4
11:	dcbi	0, r3
	addi	r3, r3, 32
	bdnz	11b
	mfspr	r4, 920
	rlwinm	r4, r4, 0, 4, 2		# clear LCE
	mtspr	920, r4
	isync
	blr

# lc_queue(): DMA operations still queued (HID2 DMAQL)
	.globl	lc_queue
lc_queue:
	mfspr	r3, 920
	rlwinm	r3, r3, 8, 28, 31
	blr

# lc_load(lc, phys, blocks) / lc_store(lc, phys, blocks): queue one DMA
# (blocks of 32 bytes, 1-128; 128 is 0). phys is a physical address
# (libogc clears the top nibble of a 0x8... one; MEM2 is 0x1...)
	.globl	lc_load
lc_load:
	rlwinm	r6, r5, 30, 27, 31
	or	r6, r6, r4
	mtspr	922, r6
	rlwinm	r6, r5, 2, 28, 29
	or	r6, r6, r3
	ori	r6, r6, 0x12		# load, trigger
	mtspr	923, r6
	blr
	.globl	lc_store
lc_store:
	rlwinm	r6, r5, 30, 27, 31
	or	r6, r6, r4
	mtspr	922, r6
	rlwinm	r6, r5, 2, 28, 29
	or	r6, r6, r3
	ori	r6, r6, 0x02		# store, trigger
	mtspr	923, r6
	blr

	.section .pic_tail, "ax"
tail:
	mflr	r0			# the replaced instruction
