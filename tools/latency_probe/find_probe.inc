# FIND_PROBE reg, tmp, fail: reg = the probe's code (glue.s's _start), found through the
# branch the code handler put at 0x803762A8 to it; to `fail` when the branch is not
# there. Its entries are then at 4(reg) (lat_pad) and 8(reg) (lat_retrace). Uses r0.

.macro FIND_PROBE reg, tmp, fail
	lis	\reg, 0x8037
	ori	\reg, \reg, 0x62A8
	lwz	\tmp, 0(\reg)
	rlwinm	r0, \tmp, 6, 26, 31
	cmpwi	r0, 18			# a b?
	bne	\fail
	slwi	\tmp, \tmp, 6
	srawi	\tmp, \tmp, 6
	rlwinm	\tmp, \tmp, 0, 0, 29	# its displacement
	add	\reg, \reg, \tmp
.endm
