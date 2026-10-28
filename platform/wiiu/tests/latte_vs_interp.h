/*
 * Host-only Latte (R700-family) vertex-program interpreter for the subset the
 * Wii U renderer emits: CALL_FS fetch, ALU clauses, POS/PARAM exports.
 *
 * It executes the real uint64 words produced by gx2_vertex_program.h, so a
 * program is checked as the GPU would run it, not as its C++ model says it
 * should.  Anything outside the modelled subset is an error, never a guess:
 *
 *  - fetch: attribute `location` is written to GPR (n + gprOffset) where
 *    sq_vtx_semantic[n] == location (gprOffset is 1 on GX2: R0 holds the
 *    vertex id).  Float32xN, UNORM8x4 and USCALED8x4 formats with the
 *    GX2_SQ_SEL selects; a component the format lacks reads 0.
 *  - ALU: one instruction group = slots up to the LAST bit, plus literal
 *    slots.  All reads see the state before the group; writes, PV and PS
 *    commit at its end.  A vector op occupies the slot of its destination
 *    channel (PV.chan), a trans-only op (or a second op on a taken channel)
 *    the trans slot (PS).  PV, PS and AR are invalid at every clause start.
 *  - modifiers: src neg/abs, OP2 omod, clamp, write mask; relative constant
 *    reads through AR (index_mode AR_X); inline 0, 1, 1i, -1i, 0.5, literal.
 *  - constant-file read-port limits are NOT modelled: the hardware-proven
 *    generated program reads C194.x and C195.x in one instruction.
 *  - exports: POS (array base 60+) and PARAM; the first export after the
 *    ALU clauses must carry a barrier, the last export of each type must be
 *    EXPORT_DONE, END_OF_PROGRAM must end the CF stream, the PARAM count must
 *    equal spi_vs_out_config, and PARAM n reaches the pixel shader as the
 *    semantic in byte n of spi_vs_out_id.
 *  - every GPR index must be below sq_pgm_resources_vs's GPR count, and a
 *    read of a GPR channel nothing has written is an error.
 */
#ifndef SMS_LATTE_VS_INTERP_H
#define SMS_LATTE_VS_INTERP_H

#include "../gx2_vertex_program.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace latte {

struct VsResult {
    float pos[4];
    bool hasPos;
    uint32_t paramCount;
    float param[32][4];
    bool paramWritten[32];
    float semantic[256][4];
    bool hasSemantic[256];
    uint32_t groups;
    std::string error;
};

class VsInterpreter {
public:
    uint32_t gprOffset = 1;
    float cfile[256][4];

    VsInterpreter() {
        for (auto& v : cfile)
            for (float& f : v) f = std::nanf("");
    }

    void setConst(uint32_t reg, float x, float y, float z, float w) {
        cfile[reg][0] = x; cfile[reg][1] = y; cfile[reg][2] = z; cfile[reg][3] = w;
    }

    bool run(const std::vector<uint64_t>& words, const gx::LatteVertexRegs& regs,
             const gx::LatteAttrib* attribs, uint32_t attribCount,
             const uint8_t* vertex, VsResult& out) {
        memset(out.pos, 0, sizeof out.pos);
        out.hasPos = false;
        out.paramCount = 0;
        memset(out.param, 0, sizeof out.param);
        memset(out.paramWritten, 0, sizeof out.paramWritten);
        memset(out.semantic, 0, sizeof out.semantic);
        memset(out.hasSemantic, 0, sizeof out.hasSemantic);
        out.groups = 0;
        out.error.clear();
        m_out = &out;
        m_numGprs = regs.sq_pgm_resources_vs & 0xff;
        if (m_numGprs == 0 || m_numGprs > 128) return fail("sq_pgm_resources_vs GPR count %u", m_numGprs);
        memset(m_gprBits, 0, sizeof m_gprBits);
        memset(m_gprDef, 0, sizeof m_gprDef);
        bool fetched = false, sawExport = false, sawAluBeforeExport = false, ended = false;
        int lastExportKind[4] = {-1, -1, -1, -1};
        size_t cf = 0;
        for (uint32_t steps = 0; !ended; ++steps, ++cf) {
            if (cf >= words.size() || steps > 64) return fail("control flow ran off the program (CF %zu)", cf);
            const uint32_t w0 = __builtin_bswap32(uint32_t(words[cf] >> 32));
            const uint32_t w1 = __builtin_bswap32(uint32_t(words[cf]));
            const uint32_t inst7 = (w1 >> 23) & 0x7f;
            if (inst7 >= 0x40) {
                const uint32_t inst = (w1 >> 26) & 0xf;
                if (inst != CF_INST_ALU) return fail("CF %zu: unsupported ALU CF inst %u", cf, inst);
                if ((w0 >> 22) || (w1 & 0x3ffff) || ((w1 >> 25) & 1))
                    return fail("CF %zu: kcache/alt-const ALU clause is not modelled", cf);
                const uint32_t addr = w0 & 0x3fffff;
                const uint32_t count = ((w1 >> 18) & 0x7f) + 1;
                if (!fetched) return fail("CF %zu: ALU clause before CALL_FS", cf);
                if (sawExport) return fail("CF %zu: ALU clause after an export", cf);
                if (!runClause(words, addr, count)) return false;
                sawAluBeforeExport = true;
                continue;
            }
            const bool eop = (w1 >> 21) & 1;
            const bool barrier = (w1 >> 31) & 1;
            if (inst7 == CF_INST_CALL_FS) {
                if (fetched || cf != 0) return fail("CF %zu: CALL_FS must be the first CF and run once", cf);
                if (!fetch(regs, attribs, attribCount, vertex)) return false;
                fetched = true;
            } else if (inst7 == 0x27 || inst7 == CF_INST_EXP_DONE) {
                if (!sawAluBeforeExport) return fail("CF %zu: export before any ALU clause", cf);
                if (!sawExport && !barrier)
                    return fail("CF %zu: the first export lacks its barrier (it would race the ALU writes)", cf);
                sawExport = true;
                if (!doExport(w0, w1, cf)) return false;
                const uint32_t type = (w0 >> 13) & 3;
                lastExportKind[type] = int(inst7);
            } else {
                return fail("CF %zu: unsupported CF inst 0x%x", cf, inst7);
            }
            if (eop) {
                ended = true;
                if (inst7 != CF_INST_EXP_DONE) return fail("CF %zu: END_OF_PROGRAM on a non EXPORT_DONE", cf);
            }
        }
        for (int type = 0; type < 4; ++type)
            if (lastExportKind[type] >= 0 && lastExportKind[type] != CF_INST_EXP_DONE)
                return fail("the last export of type %d is EXPORT, not EXPORT_DONE", type);
        if (!out.hasPos) return fail("no POS0 export");
        const uint32_t params = ((regs.spi_vs_out_config >> 1) & 0x1f) + 1;
        for (uint32_t i = 0; i < 32; ++i) {
            if (i < params && !out.paramWritten[i]) return fail("PARAM%u declared by spi_vs_out_config but not exported", i);
            if (i >= params && out.paramWritten[i]) return fail("PARAM%u exported beyond spi_vs_out_config (%u)", i, params);
        }
        out.paramCount = params;
        for (uint32_t i = 0; i < params; ++i) {
            const uint32_t id = (regs.spi_vs_out_id[i / 4] >> ((i % 4) * 8)) & 0xff;
            if (id == 0xff) continue;
            if (out.hasSemantic[id]) return fail("semantic %u exported twice", id);
            out.hasSemantic[id] = true;
            memcpy(out.semantic[id], out.param[i], sizeof out.semantic[id]);
        }
        return true;
    }

private:
    VsResult* m_out = nullptr;
    uint32_t m_numGprs = 0;
    uint32_t m_gprBits[128][4];
    bool m_gprDef[128][4];
    // Per clause.
    uint32_t m_pv[4];
    bool m_pvDef[4];
    uint32_t m_ps;
    bool m_psDef;
    int32_t m_ar;
    bool m_arDef;

    bool fail(const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        if (m_out && m_out->error.empty()) m_out->error = buf;
        return false;
    }

    static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
    static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

    bool fetch(const gx::LatteVertexRegs& regs, const gx::LatteAttrib* attribs, uint32_t count,
               const uint8_t* vertex) {
        for (uint32_t a = 0; a < count; ++a) {
            const gx::LatteAttrib& attr = attribs[a];
            int slot = -1;
            for (uint32_t n = 0; n < regs.num_sq_vtx_semantic && n < 32; ++n)
                if (regs.sq_vtx_semantic[n] == attr.location) { slot = int(n); break; }
            if (slot < 0) continue;  // no semantic: the fetch shader's value goes nowhere
            const uint32_t gpr = uint32_t(slot) + gprOffset;
            if (gpr >= m_numGprs) return fail("fetch location %u lands in R%u beyond the GPR count", attr.location, gpr);
            float comp[4] = {0, 0, 0, 0};
            uint32_t have = 0;
            const uint8_t* p = vertex + attr.offset;
            switch (attr.kind) {
            case gx::kLatteAttribFloat32x2: have = 2; memcpy(comp, p, 8); break;
            case gx::kLatteAttribFloat32x3: have = 3; memcpy(comp, p, 12); break;
            case gx::kLatteAttribFloat32x4: have = 4; memcpy(comp, p, 16); break;
            case gx::kLatteAttribUnorm8x4:
                have = 4;
                for (int i = 0; i < 4; ++i) comp[i] = float(p[i]) / 255.0f;
                break;
            case gx::kLatteAttribUscaled8x4:
                have = 4;
                for (int i = 0; i < 4; ++i) comp[i] = float(p[i]);
                break;
            default: return fail("attribute %u: unmodelled format kind %u", attr.location, attr.kind);
            }
            if (attr.buffer != 0) return fail("attribute %u: only buffer 0 is modelled", attr.location);
            for (int c = 0; c < 4; ++c) {
                const uint8_t sel = attr.sel[c];
                float v;
                if (sel <= gx::kLatteSelW) v = sel < have ? comp[sel] : 0.0f;
                else if (sel == gx::kLatteSel0) v = 0.0f;
                else if (sel == gx::kLatteSel1) v = 1.0f;
                else return fail("attribute %u: select %u is not modelled", attr.location, sel);
                m_gprBits[gpr][c] = fbits(v);
                m_gprDef[gpr][c] = true;
            }
        }
        return true;
    }

    bool doExport(uint32_t w0, uint32_t w1, size_t cf) {
        const uint32_t base = w0 & 0x1fff;
        const uint32_t type = (w0 >> 13) & 3;
        const uint32_t gpr = (w0 >> 15) & 0x7f;
        if ((w0 >> 22) & 1) return fail("CF %zu: relative export source is not modelled", cf);
        if ((w0 >> 30) & 3) return fail("CF %zu: export elem_size is not modelled", cf);
        if ((w1 >> 17) & 0xf) return fail("CF %zu: export burst count is not modelled", cf);
        if (gpr >= m_numGprs) return fail("CF %zu: export reads R%u beyond the GPR count", cf, gpr);
        float v[4];
        for (int c = 0; c < 4; ++c) {
            const uint32_t sel = (w1 >> (3 * c)) & 7;
            if (sel <= 3) {
                if (!m_gprDef[gpr][sel]) return fail("CF %zu: export reads undefined R%u.%c", cf, gpr, "xyzw"[sel]);
                v[c] = bitsf(m_gprBits[gpr][sel]);
            } else if (sel == 4) v[c] = 0.0f;
            else if (sel == 5) v[c] = 1.0f;
            else return fail("CF %zu: export select %u is not modelled", cf, sel);
        }
        if (type == EXPORT_TYPE_POS) {
            if (base != 60) return fail("CF %zu: only POS0 is modelled (base %u)", cf, base);
            if (m_out->hasPos) return fail("CF %zu: POS0 exported twice", cf);
            memcpy(m_out->pos, v, sizeof v);
            m_out->hasPos = true;
        } else if (type == EXPORT_TYPE_PARAM) {
            if (base >= 32) return fail("CF %zu: PARAM%u out of range", cf, base);
            if (m_out->paramWritten[base]) return fail("CF %zu: PARAM%u exported twice", cf, base);
            memcpy(m_out->param[base], v, sizeof v);
            m_out->paramWritten[base] = true;
        } else {
            return fail("CF %zu: export type %u in a vertex program", cf, type);
        }
        return true;
    }

    struct Ins {
        bool op3;
        uint32_t inst;
        uint32_t sel[3], rel[3], chan[3], neg[3], abs[3];
        uint32_t nsrc;
        uint32_t dst, dstChan, dstRel, clamp, writeMask, omod, indexMode, predSel, uem, up, bank;
        int slot;  // 0..3 vector, 4 trans
    };

    static bool transOnly(const Ins& i) {
        if (i.op3) return false;
        switch (i.inst) {
        case ALU_OP2_INST_RECIP_IEEE: case ALU_OP2_INST_RECIPSQRT_IEEE: case ALU_OP2_INST_SQRT_IEEE:
        case ALU_OP2_INST_FLT_TO_INT: case ALU_OP2_INST_INT_TO_FLT: case ALU_OP2_INST_MULLO_INT:
            return true;
        default: return false;
        }
    }

    static uint32_t srcCount(const Ins& i) {
        if (i.op3) return 3;
        switch (i.inst) {
        case 0x19: case ALU_OP2_INST_MOVA_INT: case ALU_OP2_INST_FLT_TO_INT: case ALU_OP2_INST_INT_TO_FLT:
        case ALU_OP2_INST_RECIP_IEEE: case ALU_OP2_INST_RECIPSQRT_IEEE: case ALU_OP2_INST_SQRT_IEEE:
        case ALU_OP2_INST_FRACT: case 0x13: case 0x14:
            return 1;
        default: return 2;
        }
    }

    static Ins decode(uint64_t op) {
        Ins i;
        memset(&i, 0, sizeof i);
        const uint32_t w0 = __builtin_bswap32(uint32_t(op >> 32));
        const uint32_t w1 = __builtin_bswap32(uint32_t(op));
        i.sel[0] = w0 & 0x1ff; i.rel[0] = (w0 >> 9) & 1; i.chan[0] = (w0 >> 10) & 3; i.neg[0] = (w0 >> 12) & 1;
        i.sel[1] = (w0 >> 13) & 0x1ff; i.rel[1] = (w0 >> 22) & 1; i.chan[1] = (w0 >> 23) & 3; i.neg[1] = (w0 >> 25) & 1;
        i.indexMode = (w0 >> 26) & 7;
        i.predSel = (w0 >> 29) & 3;
        i.op3 = ((w1 >> 15) & 7) != 0;
        if (i.op3) {
            i.sel[2] = w1 & 0x1ff; i.rel[2] = (w1 >> 9) & 1; i.chan[2] = (w1 >> 10) & 3; i.neg[2] = (w1 >> 12) & 1;
            i.inst = (w1 >> 13) & 0x1f;
            i.writeMask = 1;
        } else {
            i.abs[0] = w1 & 1; i.abs[1] = (w1 >> 1) & 1;
            i.uem = (w1 >> 2) & 1; i.up = (w1 >> 3) & 1;
            i.writeMask = (w1 >> 4) & 1;
            i.omod = (w1 >> 5) & 3;
            i.inst = (w1 >> 7) & 0x7ff;
        }
        i.bank = (w1 >> 18) & 7;
        i.dst = (w1 >> 21) & 0x7f;
        i.dstRel = (w1 >> 28) & 1;
        i.dstChan = (w1 >> 29) & 3;
        i.clamp = (w1 >> 31) & 1;
        i.nsrc = srcCount(i);
        return i;
    }

    bool runClause(const std::vector<uint64_t>& words, uint32_t addr, uint32_t count) {
        if (size_t(addr) + count > words.size()) return fail("ALU clause %u+%u beyond the program", addr, count);
        m_pvDef[0] = m_pvDef[1] = m_pvDef[2] = m_pvDef[3] = false;
        m_psDef = false;
        m_arDef = false;
        uint32_t at = addr;
        const uint32_t end = addr + count;
        while (at < end) {
            std::vector<Ins> group;
            bool last = false;
            while (!last) {
                if (at >= end) return fail("ALU group at slot %u runs past its clause", at);
                const uint64_t op = words[at++];
                last = (__builtin_bswap32(uint32_t(op >> 32)) >> 31) & 1;
                group.push_back(decode(op));
                if (group.size() > 5) return fail("ALU group at slot %u has more than five instructions", at);
            }
            uint32_t literal[4] = {0, 0, 0, 0};
            uint32_t literalChans = 0;
            for (const Ins& i : group)
                for (uint32_t s = 0; s < i.nsrc; ++s)
                    if (i.sel[s] == ALU_SRC_LITERAL) literalChans = std::max(literalChans, i.chan[s] + 1);
            if (literalChans) {
                const uint32_t slots = literalChans > 2 ? 2 : 1;
                if (at + slots > end) return fail("literal slots run past the clause");
                for (uint32_t k = 0; k < slots; ++k) {
                    literal[k * 2] = __builtin_bswap32(uint32_t(words[at + k] >> 32));
                    literal[k * 2 + 1] = __builtin_bswap32(uint32_t(words[at + k]));
                }
                at += slots;
            }
            if (!runGroup(group, literal)) return false;
            ++m_out->groups;
        }
        return true;
    }

    bool readSrc(const Ins& i, uint32_t s, const uint32_t* literal, uint32_t& bits, bool isFloat) {
        const uint32_t sel = i.sel[s];
        const uint32_t chan = i.chan[s];
        if (i.rel[s] && (sel < 256)) return fail("relative GPR source is not modelled");
        if (sel < 128) {
            if (sel >= m_numGprs) return fail("source R%u beyond the GPR count %u", sel, m_numGprs);
            if (!m_gprDef[sel][chan]) return fail("read of undefined R%u.%c", sel, "xyzw"[chan]);
            bits = m_gprBits[sel][chan];
        } else if (sel >= 256) {
            int32_t reg = int32_t(sel - 256);
            if (i.rel[s]) {
                if (i.indexMode != ALU_INDEX_AR_X) return fail("relative constant index mode %u is not modelled", i.indexMode);
                if (!m_arDef) return fail("relative constant read of C%d with no AR in this clause", reg);
                reg += m_ar;
            }
            if (reg < 0 || reg > 255) return fail("relative constant read C%d out of range", reg);
            bits = fbits(cfile[reg][chan]);
        } else if (sel == ALU_SRC_0) bits = fbits(0.0f);
        else if (sel == ALU_SRC_1) bits = fbits(1.0f);
        else if (sel == 0xFA) bits = 1;
        else if (sel == 0xFB) bits = uint32_t(-1);
        else if (sel == ALU_SRC_0_5) bits = fbits(0.5f);
        else if (sel == ALU_SRC_LITERAL) bits = literal[chan];
        else if (sel == ALU_SRC_PV) {
            if (!m_pvDef[chan]) return fail("read of PV.%c with no producer in the previous group", "xyzw"[chan]);
            bits = m_pv[chan];
        } else if (sel == ALU_SRC_PS) {
            if (!m_psDef) return fail("read of PS with no trans producer in the previous group");
            bits = m_ps;
        } else {
            return fail("source select %u is not modelled", sel);
        }
        if (i.abs[s] || i.neg[s]) {
            if (!isFloat) return fail("neg/abs on an integer operand");
            float f = bitsf(bits);
            if (i.abs[s]) f = std::fabs(f);
            if (i.neg[s]) f = -f;
            bits = fbits(f);
        }
        return true;
    }

    static float dx9Mul(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }

    bool execute(const Ins& i, const uint32_t* src, uint32_t& result, bool& isFloat, bool& setsAr, int32_t& ar) {
        const float a = bitsf(src[0]), b = bitsf(src[1]), c = bitsf(src[2]);
        const int32_t ia = int32_t(src[0]), ib = int32_t(src[1]);
        isFloat = true;
        setsAr = false;
        float r = 0.0f;
        if (i.op3) {
            switch (i.inst) {
            case ALU_OP3_INST_MULADD: { volatile float p = dx9Mul(a, b); r = p + c; break; }
            case ALU_OP3_INST_MULADD_D2: { volatile float p = dx9Mul(a, b); volatile float s = p + c; r = s * 0.5f; break; }
            case 0x18: r = a == 0.0f ? b : c; break;   // CNDE
            case ALU_OP3_INST_CNDGT: r = a > 0.0f ? b : c; break;
            case 0x1A: r = a >= 0.0f ? b : c; break;   // CNDGE
            default: return fail("OP3 instruction 0x%x is not modelled", i.inst);
            }
            result = fbits(r);
            return true;
        }
        switch (i.inst) {
        case 0x00: r = a + b; break;
        case 0x01: r = dx9Mul(a, b); break;
        case ALU_OP2_INST_MUL_IEEE: r = a * b; break;
        case ALU_OP2_INST_MAX: r = a >= b ? a : b; break;
        case ALU_OP2_INST_MIN: r = a < b ? a : b; break;
        case ALU_OP2_INST_SETE: r = a == b ? 1.0f : 0.0f; break;
        case ALU_OP2_INST_SETGT: r = a > b ? 1.0f : 0.0f; break;
        case ALU_OP2_INST_SETGE: r = a >= b ? 1.0f : 0.0f; break;
        case ALU_OP2_INST_SETNE: r = a != b ? 1.0f : 0.0f; break;
        case ALU_OP2_INST_FRACT: r = a - std::floor(a); break;
        case 0x13: r = std::nearbyint(a); break;
        case 0x14: r = std::floor(a); break;
        case 0x19: result = src[0]; return true;  // MOV keeps the bits
        case ALU_OP2_INST_MOVA_INT: isFloat = false; setsAr = true; ar = ia; result = src[0]; return true;
        case ALU_OP2_INST_FLT_TO_INT: isFloat = false; result = uint32_t(int32_t(std::trunc(a))); return true;
        case ALU_OP2_INST_INT_TO_FLT: r = float(ia); break;
        case ALU_OP2_INST_RECIP_IEEE: r = 1.0f / a; break;
        case ALU_OP2_INST_RECIPSQRT_IEEE: r = 1.0f / std::sqrt(a); break;
        case ALU_OP2_INST_SQRT_IEEE: r = std::sqrt(a); break;
        case ALU_OP2_INST_ADD_INT: isFloat = false; result = uint32_t(ia) + uint32_t(ib); return true;
        case ALU_OP2_INST_AND_INT: isFloat = false; result = uint32_t(ia) & uint32_t(ib); return true;
        case ALU_OP2_INST_SETGE_INT: isFloat = false; result = ia >= ib ? ~0u : 0u; return true;
        case ALU_OP2_INST_LSHL_INT: isFloat = false; result = uint32_t(ia) << (uint32_t(ib) & 31); return true;
        case ALU_OP2_INST_LSHR_INT: isFloat = false; result = uint32_t(ia) >> (uint32_t(ib) & 31); return true;
        case ALU_OP2_INST_MULLO_INT: isFloat = false; result = uint32_t(ia) * uint32_t(ib); return true;
        default: return fail("OP2 instruction 0x%x is not modelled", i.inst);
        }
        result = fbits(r);
        return true;
    }

    static bool intSources(const Ins& i) {
        if (i.op3) return false;
        switch (i.inst) {
        case ALU_OP2_INST_MOVA_INT: case ALU_OP2_INST_INT_TO_FLT: case ALU_OP2_INST_ADD_INT:
        case ALU_OP2_INST_AND_INT: case ALU_OP2_INST_SETGE_INT: case ALU_OP2_INST_LSHL_INT:
        case ALU_OP2_INST_LSHR_INT: case ALU_OP2_INST_MULLO_INT:
            return true;
        default: return false;
        }
    }

    bool runGroup(std::vector<Ins>& group, const uint32_t* literal) {
        bool slotUsed[5] = {false, false, false, false, false};
        for (Ins& i : group) {
            if (i.predSel || i.uem || i.up) return fail("predication/exec-mask updates are not modelled");
            if (i.dstRel) return fail("relative destination is not modelled");
            if (i.bank) return fail("non-default bank swizzle is not modelled");
            if (transOnly(i) || slotUsed[i.dstChan]) i.slot = 4;
            else i.slot = int(i.dstChan);
            if (slotUsed[i.slot]) return fail("two instructions need the trans slot in one group");
            slotUsed[i.slot] = true;
        }
        struct Pending { int slot; uint32_t bits; bool write; uint32_t dst, chan; };
        std::vector<Pending> pending;
        bool setAr = false;
        int32_t newAr = 0;
        for (const Ins& i : group) {
            uint32_t src[3] = {0, 0, 0};
            const bool intOp = intSources(i);
            for (uint32_t s = 0; s < i.nsrc; ++s) {
                if (!readSrc(i, s, literal, src[s], !intOp)) return false;
            }
            uint32_t result = 0;
            bool isFloat = true, setsAr = false;
            int32_t ar = 0;
            if (!execute(i, src, result, isFloat, setsAr, ar)) return false;
            if (setsAr) { setAr = true; newAr = ar; }
            if (isFloat && !i.op3) {
                float f = bitsf(result);
                if (i.omod == ALU_OMOD_M2) f *= 2.0f;
                else if (i.omod == ALU_OMOD_M4) f *= 4.0f;
                else if (i.omod == ALU_OMOD_D2) f *= 0.5f;
                result = fbits(f);
            } else if (!isFloat && !i.op3 && i.omod) {
                return fail("omod on an integer result");
            }
            if (i.clamp) {
                if (!isFloat) return fail("clamp on an integer result");
                float f = bitsf(result);
                f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
                result = fbits(f);
            }
            if (i.writeMask && i.dst >= m_numGprs) return fail("write to R%u beyond the GPR count %u", i.dst, m_numGprs);
            pending.push_back({i.slot, result, i.writeMask != 0, i.dst, i.dstChan});
        }
        m_pvDef[0] = m_pvDef[1] = m_pvDef[2] = m_pvDef[3] = false;
        m_psDef = false;
        for (const Pending& p : pending) {
            if (p.write) {
                m_gprBits[p.dst][p.chan] = p.bits;
                m_gprDef[p.dst][p.chan] = true;
            }
            if (p.slot == 4) { m_ps = p.bits; m_psDef = true; }
            else { m_pv[p.slot] = p.bits; m_pvDef[p.slot] = true; }
        }
        if (setAr) { m_ar = newAr; m_arDef = true; }
        return true;
    }
};

} // namespace latte

#endif
