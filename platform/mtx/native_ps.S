/* Espresso's Gekko-compatible paired-single implementations for the hottest
 * GameCube MTX/VEC/J3D routines.  GQR0 is temporarily forced to float so the
 * psq_l/st instructions have the same no-conversion semantics as qr0 in the
 * DOL.  The Cafe caller's GQR0 is restored before every return. */

	.machine "750cl"
	.text
	.align 2

	.macro nativeps_begin
	mfspr 0, 912
	li 12, 0
mtgqr 0, 12
	.endm

	.macro nativeps_end
mtgqr 0, 0
	blr
	.endm

	.globl sms_nativeps_gMTXConcat
	.type sms_nativeps_gMTXConcat, @function
sms_nativeps_gMTXConcat:
	/* Keep the DOL's f14/f15/f31 save set. */
	stwu 1, -64(1)
	stfd 14, 8(1)
	stfd 15, 16(1)
	stfd 31, 40(1)
	nativeps_begin
	psq_l 0, 0(3), 0, 0
	psq_l 6, 0(4), 0, 0
	lis 6, nativeps_unit01@ha
	psq_l 7, 8(4), 0, 0
	addi 6, 6, nativeps_unit01@l
	psq_l 8, 16(4), 0, 0
	ps_muls0 12, 6, 0
	psq_l 2, 16(3), 0, 0
	ps_muls0 13, 7, 0
	psq_l 31, 0(6), 0, 0
	ps_muls0 14, 6, 2
	psq_l 9, 24(4), 0, 0
	ps_muls0 15, 7, 2
	psq_l 1, 8(3), 0, 0
	ps_madds1 12, 8, 0, 12
	psq_l 3, 24(3), 0, 0
	ps_madds1 14, 8, 2, 14
	psq_l 10, 32(4), 0, 0
	ps_madds1 13, 9, 0, 13
	psq_l 11, 40(4), 0, 0
	ps_madds1 15, 9, 2, 15
	psq_l 4, 32(3), 0, 0
	psq_l 5, 40(3), 0, 0
	ps_madds0 12, 10, 1, 12
	ps_madds0 13, 11, 1, 13
	ps_madds0 14, 10, 3, 14
	ps_madds0 15, 11, 3, 15
	psq_st 12, 0(5), 0, 0
	ps_muls0 2, 6, 4
	ps_madds1 13, 31, 1, 13
	ps_muls0 0, 7, 4
	psq_st 14, 16(5), 0, 0
	ps_madds1 15, 31, 3, 15
	psq_st 13, 8(5), 0, 0
	ps_madds1 2, 8, 4, 2
	ps_madds1 0, 9, 4, 0
	ps_madds0 2, 10, 5, 2
	psq_st 15, 24(5), 0, 0
	ps_madds0 0, 11, 5, 0
	psq_st 2, 32(5), 0, 0
	ps_madds1 0, 31, 5, 0
	psq_st 0, 40(5), 0, 0
	mtgqr 0, 0
	lfd 14, 8(1)
	lfd 15, 16(1)
	lfd 31, 40(1)
	addi 1, 1, 64
	blr
	.size sms_nativeps_gMTXConcat, .-sms_nativeps_gMTXConcat

	.globl sms_nativeps_gMTXQuat
	.type sms_nativeps_gMTXQuat, @function
sms_nativeps_gMTXQuat:
	nativeps_begin
	lis 12, nativeps_constants@ha
	lfs 1, nativeps_constants_one@l(12)
	psq_l 4, 0(4), 0, 0
	psq_l 5, 8(4), 0, 0
	fsubs 0, 1, 1
	fadds 2, 1, 1
	ps_mul 6, 4, 4
	ps_merge10 9, 4, 4
	ps_madd 8, 5, 5, 6
	ps_mul 7, 5, 5
	ps_sum0 3, 8, 8, 8
	ps_muls1 10, 9, 5
	fres 11, 3
	ps_sum1 8, 7, 8, 6
	ps_nmsub 3, 3, 11, 2
	ps_muls1 7, 5, 5
	ps_mul 3, 11, 3
	ps_sum0 6, 6, 6, 6
	fmuls 3, 3, 2
	ps_madd 11, 4, 9, 7
	ps_msub 7, 4, 9, 7
	psq_st 0, 12(3), 1, 0
	ps_nmsub 6, 6, 3, 1
	ps_nmsub 8, 8, 3, 1
	psq_st 0, 44(3), 1, 0
	ps_mul 11, 11, 3
	ps_mul 7, 7, 3
	psq_st 6, 40(3), 1, 0
	ps_madds0 9, 4, 5, 10
	ps_merge00 5, 11, 8
	ps_nmsub 10, 10, 2, 9
	ps_merge10 4, 8, 7
	psq_st 5, 16(3), 0, 0
	ps_mul 9, 9, 3
	ps_mul 10, 10, 3
	psq_st 4, 0(3), 0, 0
	psq_st 9, 8(3), 1, 0
	ps_merge10 7, 10, 0
	ps_merge01 11, 10, 9
	psq_st 7, 24(3), 0, 0
	psq_st 11, 32(3), 0, 0
	nativeps_end
	.size sms_nativeps_gMTXQuat, .-sms_nativeps_gMTXQuat

	.globl sms_nativeps_gMTXMultVec
	.type sms_nativeps_gMTXMultVec, @function
sms_nativeps_gMTXMultVec:
	nativeps_begin
	psq_l 0, 0(4), 0, 0
	psq_l 2, 0(3), 0, 0
	psq_l 1, 8(4), 1, 0
	ps_mul 4, 2, 0
	psq_l 3, 8(3), 0, 0
	ps_madd 5, 3, 1, 4
	psq_l 8, 16(3), 0, 0
	ps_sum0 6, 5, 6, 5
	psq_l 9, 24(3), 0, 0
	ps_mul 10, 8, 0
	psq_st 6, 0(5), 1, 0
	ps_madd 11, 9, 1, 10
	psq_l 2, 32(3), 0, 0
	ps_sum0 12, 11, 12, 11
	psq_l 3, 40(3), 0, 0
	ps_mul 4, 2, 0
	psq_st 12, 4(5), 1, 0
	ps_madd 5, 3, 1, 4
	ps_sum0 6, 5, 6, 5
	psq_st 6, 8(5), 1, 0
	nativeps_end
	.size sms_nativeps_gMTXMultVec, .-sms_nativeps_gMTXMultVec

	.globl sms_nativeps_gVECNormalize
	.type sms_nativeps_gVECNormalize, @function
sms_nativeps_gVECNormalize:
	nativeps_begin
	lis 12, nativeps_constants@ha
	psq_l 2, 0(3), 0, 0
	ps_mul 5, 2, 2
	psq_l 3, 8(3), 1, 0
	ps_madd 4, 3, 3, 5
	ps_sum0 4, 4, 3, 5
	frsqrte 6, 4
	fmuls 7, 6, 6
	lfs 8, nativeps_constants_half@l(12)
	fmuls 8, 6, 8
	lfs 9, nativeps_constants_three@l(12)
	fnmsubs 7, 7, 4, 9
	fmuls 6, 7, 8
	ps_muls0 2, 2, 6
	psq_st 2, 0(4), 0, 0
	ps_muls0 3, 3, 6
	psq_st 3, 8(4), 1, 0
	nativeps_end
	.size sms_nativeps_gVECNormalize, .-sms_nativeps_gVECNormalize

	.globl sms_nativeps_gJ3DPSMtxArrayConcat
	.type sms_nativeps_gJ3DPSMtxArrayConcat, @function
sms_nativeps_gJ3DPSMtxArrayConcat:
	stwu 1, -64(1)
	stfd 14, 8(1)
	stfd 15, 16(1)
	stfd 31, 40(1)
	nativeps_begin
	cmpwi 6, 0
	bne 1f
	li 6, 1
1:
	lis 7, nativeps_unit01@ha
	addi 7, 7, nativeps_unit01@l
	subi 4, 4, 8
	subi 5, 5, 8
	mtctr 6
2:
	psq_l 0, 0(3), 0, 0
	psq_l 6, 8(4), 0, 0
	psq_l 7, 16(4), 0, 0
	psq_l 8, 24(4), 0, 0
	ps_muls0 12, 6, 0
	psq_l 2, 16(3), 0, 0
	ps_muls0 13, 7, 0
	psq_l 31, 0(7), 0, 0
	ps_muls0 14, 6, 2
	psq_l 9, 32(4), 0, 0
	ps_muls0 15, 7, 2
	psq_l 1, 8(3), 0, 0
	ps_madds1 12, 8, 0, 12
	psq_l 3, 24(3), 0, 0
	ps_madds1 14, 8, 2, 14
	psq_l 10, 40(4), 0, 0
	ps_madds1 13, 9, 0, 13
	psq_lu 11, 48(4), 0, 0
	ps_madds1 15, 9, 2, 15
	psq_l 4, 32(3), 0, 0
	psq_l 5, 40(3), 0, 0
	ps_madds0 12, 10, 1, 12
	ps_madds0 13, 11, 1, 13
	ps_madds0 14, 10, 3, 14
	ps_madds0 15, 11, 3, 15
	psq_st 12, 8(5), 0, 0
	ps_muls0 2, 6, 4
	ps_madds1 13, 31, 1, 13
	ps_muls0 0, 7, 4
	psq_st 14, 24(5), 0, 0
	ps_madds1 15, 31, 3, 15
	psq_st 13, 16(5), 0, 0
	ps_madds1 2, 8, 4, 2
	ps_madds1 0, 9, 4, 0
	ps_madds0 2, 10, 5, 2
	psq_st 15, 32(5), 0, 0
	ps_madds0 0, 11, 5, 0
	psq_st 2, 40(5), 0, 0
	ps_madds1 0, 31, 5, 0
	psq_stu 0, 48(5), 0, 0
	bdnz 2b
	mtgqr 0, 0
	lfd 14, 8(1)
	lfd 15, 16(1)
	lfd 31, 40(1)
	addi 1, 1, 64
	blr
	.size sms_nativeps_gJ3DPSMtxArrayConcat, .-sms_nativeps_gJ3DPSMtxArrayConcat

	.section .rodata
	.align 2
nativeps_unit01:
	.float 0.0
	.float 1.0
nativeps_constants:
nativeps_constants_zero:
	.float 0.0
nativeps_constants_one:
	.float 1.0
nativeps_constants_two:
	.float 2.0
nativeps_constants_half:
	.float 0.5
nativeps_constants_three:
	.float 3.0
	.size nativeps_constants, .-nativeps_constants
