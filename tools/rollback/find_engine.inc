# FIND_ENGINE reg, tmp, fail: reg = the engine's code (glue.s's _start), found
# through the branch the code handler put at HSD_PerfSetStartTime (0x8037E214)
# to it; to `fail` when the branch is not there. Engine fields are then at
# (0x40 + offset)(reg). Uses r0.

.macro FIND_ENGINE reg, tmp, fail
	lis	\reg, 0x8037
	ori	\reg, \reg, 0xE214
	lwz	\tmp, 0(\reg)
	rlwinm	r0, \tmp, 6, 26, 31
	cmpwi	r0, 18			# a b?
	bne	\fail
	slwi	\tmp, \tmp, 6
	srawi	\tmp, \tmp, 6
	rlwinm	\tmp, \tmp, 0, 0, 29	# its displacement
	add	\reg, \reg, \tmp
.endm
