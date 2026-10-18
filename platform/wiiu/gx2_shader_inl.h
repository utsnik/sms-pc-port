/*
 * Latte instruction packing used by the Wii U TEV shader generator.
 *
 * This is the small instruction subset mirrored from
 * lus-rebase/include/fast/backends/gx2_shader_inl.h.  Keep the encodings in
 * sync with lus-rebase when extending the generator; the renderer deliberately
 * uses the same macros as the libultraship GX2 backend rather than a second
 * hand-written bytecode format.
 */
#ifndef SMS_GX2_SHADER_INL_H
#define SMS_GX2_SHADER_INL_H

#include <stdint.h>

#define to_QWORD(w0, w1) (((uint64_t)(w0) << 32ull) | (w1))
#define to_LE(x) (__builtin_bswap32(x))

#define CF_DWORD0(addr) to_LE(addr)
#define CF_DWORD1(popCount, cfConst, cond, count, callCount, inst) \
    to_LE(popCount | (cfConst << 3) | (cond << 8) | (count << 10) | \
          (callCount << 13) | (inst << 23) | (1 << 31))
#define CF_ALU_WORD0(addr, kcacheBank0, kcacheBank1, kcacheMode0) \
    to_LE(addr | (kcacheBank0 << 22) | (kcacheBank1 << 26) | (kcacheMode0 << 30))
#define CF_ALU_WORD1(kcacheMode1, kcacheAddr0, kcacheAddr1, count, altConst, inst) \
    to_LE(kcacheMode1 | (kcacheAddr0 << 2) | (kcacheAddr1 << 10) | \
          (count << 18) | (altConst << 25) | (inst << 26) | (1 << 31))
#define CF_EXP_WORD0(dstReg_and_type, srcReg, srcRel, indexGpr, elemSize) \
    to_LE(dstReg_and_type | (srcReg << 15) | (srcRel << 22) | \
          (indexGpr << 23) | (elemSize << 30))
#define CF_EXP_WORD1(srcSelX, srcSelY, srcSelZ, srcSelW, validPixelMode, inst) \
    to_LE(srcSelX | (srcSelY << 3) | (srcSelZ << 6) | (srcSelW << 9) | \
          (validPixelMode << 22) | (inst << 23) | (1 << 31))
#define NO_BARRIER &(~to_QWORD(0, to_LE(1ull << 31)))

#define ALU_WORD0(src0Sel, src0Rel, src0Chan, src0Neg, src1Sel, src1Rel, src1Chan, src1Neg, indexMode, predSel) \
    to_LE(src0Sel | ((src0Rel) << 9) | ((src0Chan) << 10) | ((src0Neg) << 12) | \
          ((src1Sel) << 13) | ((src1Rel) << 22) | ((src1Chan) << 23) | \
          ((src1Neg) << 25) | ((indexMode) << 26) | ((predSel) << 29))
#define ALU_WORD1_OP2(src0Abs, src1Abs, updateExecuteMask, updatePred, writeMask, omod, inst, encoding, bankSwizzle, dstGpr, dstRel, dstChan, clamp) \
    to_LE(src0Abs | (src1Abs << 1) | (updateExecuteMask << 2) | (updatePred << 3) | \
          (writeMask << 4) | (omod << 5) | (inst << 7) | (encoding << 15) | \
          (bankSwizzle << 18) | ((dstGpr & 0x7f) << 21) | (dstRel << 28) | \
          ((dstChan & 0x3) << 29) | (clamp << 31))

#define TEX_WORD0(inst, bcFracMode, fetchWholeQuad, resourceID, srcReg, srcRel, altConst) \
    to_LE(inst | (bcFracMode << 5) | (fetchWholeQuad << 7) | (resourceID << 8) | \
          (srcReg << 16) | (srcRel << 23) | (altConst << 24))
#define TEX_WORD1(dstReg, dstRel, dstSelX, dstSelY, dstSelZ, dstSelW, lodBias, coordTypeX, coordTypeY, coordTypeZ, coordTypeW) \
    to_LE(dstReg | (dstRel << 7) | (dstSelX << 9) | (dstSelY << 12) | \
          (dstSelZ << 15) | (dstSelW << 18) | (lodBias << 21) | \
          (coordTypeX << 28) | (coordTypeY << 29) | (coordTypeZ << 30) | \
          (coordTypeW << 31))
#define TEX_WORD2(offsetX, offsetY, offsetZ, samplerID, srcSelX, srcSelY, srcSelZ, srcSelW) \
    to_LE(offsetX | (offsetY << 5) | (offsetZ << 10) | (samplerID << 15) | \
          (srcSelX << 20) | (srcSelY << 23) | (srcSelZ << 26) | (srcSelW << 29))

#define _x 0
#define _y 1
#define _z 2
#define _w 3
#define _0 4
#define _1 5
#define __ (0x80)
#define _R(x) (x)

#define ALU_SRC_0 0xF8
#define ALU_SRC_1 0xF9
#define ALU_SRC_0_5 0xFC
#define ALU_SRC_LITERAL 0xFD
#define ALU_SRC_PV 0xFE
#define ALU_SRC_PS 0xFF
#define ALU_SRC_CONST_FILE_BASE 0x100
#define _GX2_C(x) (ALU_SRC_CONST_FILE_BASE + (x))
#define _NEG | (1 << 12)

#define ALU_OMOD_OFF 0x0
#define ALU_OMOD_M2 0x1
#define ALU_OMOD_M4 0x2
#define ALU_OMOD_D2 0x3
#define ALU_LAST | to_QWORD(to_LE(1ull << 31), 0)
#define CLAMP | to_QWORD(0, to_LE(1ull << 31))

#define ALU_OP2(inst, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, omod) \
    to_QWORD(ALU_WORD0(((src0Sel) & ((1 << 13) - 1)), 0, src0Chan, 0, \
                       ((src1Sel) & ((1 << 13) - 1)), 0, src1Chan, 0, 0, 0), \
             ALU_WORD1_OP2(((src0Sel) >> 13), ((src1Sel) >> 13), 0, 0, \
                           (((dstGpr & __) >> 7) ^ 1), omod, inst, 0, 0, \
                           dstGpr, 0, dstChan, 0))
#define ALU_ADD(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(0x0, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_OFF)
#define ALU_ADD_D2(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(0x0, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_D2)
#define ALU_MUL(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(0x1, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_OFF)
#define ALU_MUL_x2(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(0x1, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_M2)
#define ALU_MUL_x4(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(0x1, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_M4)
#define ALU_MOV(dstGpr, dstChan, src0Sel, src0Chan) \
    ALU_OP2(0x19, dstGpr, dstChan, src0Sel, src0Chan, ALU_SRC_0, 0, ALU_OMOD_OFF)

/* Predicate and comparison instructions from the lus-rebase Latte assembler. */
#define ALU_OP2_INST_MIN 0x04
#define ALU_OP2_INST_MAX 0x03
#define ALU_OP2_INST_SETE 0x08
#define ALU_OP2_INST_SETGT 0x09
#define ALU_OP2_INST_SETGE 0x0A
#define ALU_OP2_INST_SETNE 0x0B
#define ALU_OP2_INST_PRED_SETGT 0x21
#define ALU_OP2_INST_KILLGT 0x2D

#define ALU_MIN(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(ALU_OP2_INST_MIN, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_OFF)
#define ALU_MAX(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(ALU_OP2_INST_MAX, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_OFF)
#define ALU_SETE(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(ALU_OP2_INST_SETE, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_OFF)
#define ALU_SETGT(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(ALU_OP2_INST_SETGT, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_OFF)
#define ALU_SETGE(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(ALU_OP2_INST_SETGE, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_OFF)
#define ALU_SETNE(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(ALU_OP2_INST_SETNE, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_OFF)
#define ALU_PRED_SETGT(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(ALU_OP2_INST_PRED_SETGT, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_OFF)
#define ALU_KILLGT(dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan) \
    ALU_OP2(ALU_OP2_INST_KILLGT, dstGpr, dstChan, src0Sel, src0Chan, src1Sel, src1Chan, ALU_OMOD_OFF)

#define CF_INST_TEX 0x01
#define CF_INST_ALU 0x08
#define CF_INST_CALL_FS 0x13
#define CF_INST_EXP_DONE 0x28
#define CF_COND_ACTIVE 0x0
#define TEX_INST_SAMPLE 0x10
#define TEX_NORMALIZED 0x1
#define EXPORT_TYPE_PIXEL 0x0
#define EXPORT_TYPE_POS 0x1
#define EXPORT_TYPE_PARAM 0x2
#define POS(id) ((0x3C + (id)) | (EXPORT_TYPE_POS << 13))
#define PARAM(id) ((id) | (EXPORT_TYPE_PARAM << 13))
#define PIX(id) ((id) | (EXPORT_TYPE_PIXEL << 13))
#define POS0 POS(0)
#define PIX0 PIX(0)
#define CALL_FS to_QWORD(CF_DWORD0(0), CF_DWORD1(0, 0, 0, 0, 0, CF_INST_CALL_FS))
#define EXP_DONE(dstReg_and_type, srcReg, srcSelX, srcSelY, srcSelZ, srcSelW) \
    to_QWORD(CF_EXP_WORD0(dstReg_and_type, srcReg, 0, 0, 0), \
             CF_EXP_WORD1(srcSelX, srcSelY, srcSelZ, srcSelW, 0, CF_INST_EXP_DONE))
#define EXP(dstReg_and_type, srcReg, srcSelX, srcSelY, srcSelZ, srcSelW) \
    to_QWORD(CF_EXP_WORD0(dstReg_and_type, srcReg, 0, 0, 0), \
             CF_EXP_WORD1(srcSelX, srcSelY, srcSelZ, srcSelW, 0, 0x27))
#define TEX(addr, cnt) \
    to_QWORD(CF_DWORD0(addr), CF_DWORD1(0, 0, CF_COND_ACTIVE, ((cnt) - 1), 0, CF_INST_TEX))
#define ALU(addr, cnt) \
    to_QWORD(CF_ALU_WORD0(addr, 0, 0, 0), \
             CF_ALU_WORD1(0, 0, 0, ((cnt) - 1), 0, CF_INST_ALU))
#define TEX_SAMPLE(dstReg, dstSelX, dstSelY, dstSelZ, dstSelW, srcReg, srcSelX, srcSelY, srcSelZ, srcSelW, resourceID, samplerID) \
    to_QWORD(TEX_WORD0(TEX_INST_SAMPLE, 0, 0, resourceID, srcReg, 0, 0), \
             TEX_WORD1(dstReg, 0, dstSelX, dstSelY, dstSelZ, dstSelW, 0, \
                       TEX_NORMALIZED, TEX_NORMALIZED, TEX_NORMALIZED, TEX_NORMALIZED)), \
        to_QWORD(TEX_WORD2(0, 0, 0, samplerID, _x, _y, _0, _x), 0)
#define VALID_PIX | to_QWORD(0, to_LE(1 << 22))
#define END_OF_PROGRAM | to_QWORD(0, to_LE(1 << 21))

#endif
