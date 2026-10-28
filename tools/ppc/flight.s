# Slippi Online (Wii): interrupt flight recorder (diagnostics).
#
# Assembled once per hook with -defsym TAG=<n> (and RESTORE=1 for
# OSRestoreInterrupts): at the entry of OSDisableInterrupts (tag 1),
# OSEnableInterrupts (tag 2) and OSRestoreInterrupts (tag 3, low bit of the
# word set when it re-enables), it appends the caller and the time base to
# a 64-entry ring at 0x80197000, in a stretch of Melee's Tournament Mode code
# that nothing outside Tournament Mode reaches (Nintendont zeroes the region
# for its code list). Each write is flushed to RAM at once, so the kernel
# reads it even after the CPU stops: the last "disable" without a matching
# enable or restore is where a console with interrupts off is stuck.
#
#   0x80197000 u32 entries written (the next index)
#   0x80197020 64 x { u32 caller | tag << 0 (caller & ~3 | tag), u32 time base }
#
# r5-r8 are free here (callers treat r0, r3-r12 as clobbered); the replaced
# first instruction comes last.

	mflr	r5
	ori	r5, r5, TAG
.if RESTORE
	cmpwi	r3, 0
	beq	1f
	xori	r5, r5, 3		# tag 3 -> 0: a restore that re-enables
1:
.endif
	lis	r6, 0x8019
	ori	r6, r6, 0x7000
	lwz	r7, 0(r6)
	addi	r8, r7, 1
	stw	r8, 0(r6)
	dcbf	0, r6
	rlwinm	r7, r7, 3, 23, 28	# (index % 64) * 8
	add	r7, r7, r6
	addi	r7, r7, 0x20
	stw	r5, 0(r7)
	mftb	r8
	stw	r8, 4(r7)
	dcbf	0, r7
	sync
.if RESTORE
	cmpwi	r3, 0			# the replaced instruction
.else
	mfmsr	r3			# the replaced instruction
.endif
