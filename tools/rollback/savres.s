# The register save and restore routines GCC calls at -Os (libgcc's crtsavres,
# 32-bit SVR4/EABI): pic_code.build(size=True) links them. Position independent.

	.text
	.globl	_savegpr_14
_savegpr_14:
	stw	r14, -72(r11)
	.globl	_savegpr_15
_savegpr_15:
	stw	r15, -68(r11)
	.globl	_savegpr_16
_savegpr_16:
	stw	r16, -64(r11)
	.globl	_savegpr_17
_savegpr_17:
	stw	r17, -60(r11)
	.globl	_savegpr_18
_savegpr_18:
	stw	r18, -56(r11)
	.globl	_savegpr_19
_savegpr_19:
	stw	r19, -52(r11)
	.globl	_savegpr_20
_savegpr_20:
	stw	r20, -48(r11)
	.globl	_savegpr_21
_savegpr_21:
	stw	r21, -44(r11)
	.globl	_savegpr_22
_savegpr_22:
	stw	r22, -40(r11)
	.globl	_savegpr_23
_savegpr_23:
	stw	r23, -36(r11)
	.globl	_savegpr_24
_savegpr_24:
	stw	r24, -32(r11)
	.globl	_savegpr_25
_savegpr_25:
	stw	r25, -28(r11)
	.globl	_savegpr_26
_savegpr_26:
	stw	r26, -24(r11)
	.globl	_savegpr_27
_savegpr_27:
	stw	r27, -20(r11)
	.globl	_savegpr_28
_savegpr_28:
	stw	r28, -16(r11)
	.globl	_savegpr_29
_savegpr_29:
	stw	r29, -12(r11)
	.globl	_savegpr_30
_savegpr_30:
	stw	r30, -8(r11)
	.globl	_savegpr_31
_savegpr_31:
	stw	r31, -4(r11)
	blr

	.globl	_restgpr_14
_restgpr_14:
	lwz	r14, -72(r11)
	.globl	_restgpr_15
_restgpr_15:
	lwz	r15, -68(r11)
	.globl	_restgpr_16
_restgpr_16:
	lwz	r16, -64(r11)
	.globl	_restgpr_17
_restgpr_17:
	lwz	r17, -60(r11)
	.globl	_restgpr_18
_restgpr_18:
	lwz	r18, -56(r11)
	.globl	_restgpr_19
_restgpr_19:
	lwz	r19, -52(r11)
	.globl	_restgpr_20
_restgpr_20:
	lwz	r20, -48(r11)
	.globl	_restgpr_21
_restgpr_21:
	lwz	r21, -44(r11)
	.globl	_restgpr_22
_restgpr_22:
	lwz	r22, -40(r11)
	.globl	_restgpr_23
_restgpr_23:
	lwz	r23, -36(r11)
	.globl	_restgpr_24
_restgpr_24:
	lwz	r24, -32(r11)
	.globl	_restgpr_25
_restgpr_25:
	lwz	r25, -28(r11)
	.globl	_restgpr_26
_restgpr_26:
	lwz	r26, -24(r11)
	.globl	_restgpr_27
_restgpr_27:
	lwz	r27, -20(r11)
	.globl	_restgpr_28
_restgpr_28:
	lwz	r28, -16(r11)
	.globl	_restgpr_29
_restgpr_29:
	lwz	r29, -12(r11)
	.globl	_restgpr_30
_restgpr_30:
	lwz	r30, -8(r11)
	.globl	_restgpr_31
_restgpr_31:
	lwz	r31, -4(r11)
	blr

	.globl	_restgpr_14_x
_restgpr_14_x:
	lwz	r14, -72(r11)
	.globl	_restgpr_15_x
_restgpr_15_x:
	lwz	r15, -68(r11)
	.globl	_restgpr_16_x
_restgpr_16_x:
	lwz	r16, -64(r11)
	.globl	_restgpr_17_x
_restgpr_17_x:
	lwz	r17, -60(r11)
	.globl	_restgpr_18_x
_restgpr_18_x:
	lwz	r18, -56(r11)
	.globl	_restgpr_19_x
_restgpr_19_x:
	lwz	r19, -52(r11)
	.globl	_restgpr_20_x
_restgpr_20_x:
	lwz	r20, -48(r11)
	.globl	_restgpr_21_x
_restgpr_21_x:
	lwz	r21, -44(r11)
	.globl	_restgpr_22_x
_restgpr_22_x:
	lwz	r22, -40(r11)
	.globl	_restgpr_23_x
_restgpr_23_x:
	lwz	r23, -36(r11)
	.globl	_restgpr_24_x
_restgpr_24_x:
	lwz	r24, -32(r11)
	.globl	_restgpr_25_x
_restgpr_25_x:
	lwz	r25, -28(r11)
	.globl	_restgpr_26_x
_restgpr_26_x:
	lwz	r26, -24(r11)
	.globl	_restgpr_27_x
_restgpr_27_x:
	lwz	r27, -20(r11)
	.globl	_restgpr_28_x
_restgpr_28_x:
	lwz	r28, -16(r11)
	.globl	_restgpr_29_x
_restgpr_29_x:
	lwz	r29, -12(r11)
	.globl	_restgpr_30_x
_restgpr_30_x:
	lwz	r30, -8(r11)
	.globl	_restgpr_31_x
_restgpr_31_x:
	lwz	r0, 4(r11)
	lwz	r31, -4(r11)
	mtlr	r0
	mr	r1, r11
	blr
