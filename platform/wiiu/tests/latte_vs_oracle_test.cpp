/*
 * Validates the host Latte interpreter on the generated GPU-XF vertex program,
 * which is proven on hardware: the interpreter, fed the renderer's own words,
 * register description and fetch layout, must reproduce the CPU XF path
 * (xfTransformPositionMath, the code transformPosition runs) for the clip
 * position and the vertex colour.  From that it settles two contract
 * questions with working code rather than a reviewer's reading:
 *
 *  N1  which GPR fetch location n lands in (only one offset reproduces the
 *      CPU path);
 *  N5  which semantic the TEV pixel shader imports for the rasterised colour
 *      and which semantic the generated TEV vertex shader exports it on.
 */
#include "latte_vs_interp.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace gx;

namespace {

uint32_t s_seed = 0x5eed1234u;
uint32_t rnd() { s_seed = s_seed * 1664525u + 1013904223u; return s_seed >> 8; }
float rndf(float lo, float hi) { return lo + (hi - lo) * float(rnd() & 0xffff) / 65535.0f; }

struct Scene {
    float xfPos[64][4];  // C0..C63: XF position/texgen matrix words, 4 per vector
    float proj[6];
    bool ortho;
    float viewport[6];
    float xfViewport[6];
    XfViewInputs view;
};

void makeScene(Scene& s, bool ortho, bool widescreen) {
    for (auto& v : s.xfPos)
        for (float& f : v) f = rndf(-2.0f, 2.0f);
    // Rigid-ish model-view matrices at the indices the vertices use.
    const uint32_t mats[] = {0, 3, 9, 30, 57};
    for (uint32_t k : mats) {
        const float a = rndf(-3.0f, 3.0f), b = rndf(-1.0f, 1.0f);
        const float m[12] = {std::cos(a), 0.0f, std::sin(a), rndf(-200.0f, 200.0f),
                             std::sin(b) * std::sin(a), std::cos(b), -std::sin(b) * std::cos(a), rndf(-100.0f, 100.0f),
                             -std::cos(b) * std::sin(a), std::sin(b), std::cos(b) * std::cos(a), rndf(-1500.0f, -600.0f)};
        memcpy(&s.xfPos[k][0], m, sizeof m);
    }
    s.ortho = ortho;
    if (ortho) {
        const float p[6] = {2.0f / 640.0f, -1.0f, -2.0f / 480.0f, 1.0f, -1.0f / 2000.0f, -0.5f};
        memcpy(s.proj, p, sizeof p);
    } else {
        const float p[6] = {1.2071f, 0.0125f, 1.6094f, -0.0083f, -1.0010005f, -10.005f};
        memcpy(s.proj, p, sizeof p);
    }
    const float vp[6] = {0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f};
    memcpy(s.viewport, vp, sizeof vp);
    const float xvp[6] = {320.0f, -240.0f, 16777215.0f, 662.0f, 582.0f, 16777215.0f};
    memcpy(s.xfViewport, xvp, sizeof xvp);
    s.view.proj = s.proj;
    s.view.ortho = ortho;
    s.view.viewport = s.viewport;
    s.view.xfViewport = s.xfViewport;
    s.view.xmapA = widescreen ? 0.75f : 1.0f;
    s.view.xmapB = widescreen ? 80.0f : 0.0f;
    s.view.xmapClip = widescreen ? 1.3333334f : 1.0f;
    s.view.gameScale = 1.5f;
    s.view.gameX = 160.0f;
    s.view.tvW = 1280.0f;
    s.view.tvH = 720.0f;
}

void loadConstants(latte::VsInterpreter& vm, const Scene& s) {
    for (uint32_t r = 0; r < 64; ++r) vm.setConst(r, s.xfPos[r][0], s.xfPos[r][1], s.xfPos[r][2], s.xfPos[r][3]);
    for (uint32_t r = 64; r < kXfUniformRegs; ++r) vm.setConst(r, rndf(-1, 1), rndf(-1, 1), rndf(-1, 1), rndf(-1, 1));
    float live[kVtxLiveRegs * 4] = {};
    xfPackLiveView(s.view, live);
    for (uint32_t i = 20; i < 36; ++i) live[i] = float(rnd() & 255) / 255.0f;  // material/ambient
    for (uint32_t i = 36; i < 52; ++i) live[i] = 1.0f;                        // manual texcoord scale
    for (uint32_t c = 0; c < 8; ++c) {                                         // copy rectangle
        live[52 + c * 4] = 1.0f; live[53 + c * 4] = 1.0f; live[54 + c * 4] = 0.0f; live[55 + c * 4] = 0.0f;
    }
    for (uint32_t r = 0; r < kVtxLiveRegs; ++r)
        vm.setConst(kVtxLiveBase + r, live[r * 4], live[r * 4 + 1], live[r * 4 + 2], live[r * 4 + 3]);
}

bool close(float gpu, float cpu, float scale) {
    return std::fabs(gpu - cpu) <= 2e-5f * scale;
}

VertexKey grassStateKey(bool ortho) {
    // The XF state TMapObjGrassManager::initDrawNear sets: one colour channel,
    // material from the vertex, lighting off; no texgens.
    VertexKey k;
    memset(&k, 0, sizeof k);
    k.layoutVersion = kVertexKeyLayoutVersion;
    k.outputMode = 0;
    k.numChans = 1;
    k.colorCtrl[0] = k.alphaCtrl[0] = 1;
    k.numTexGens = 0;
    k.projectionOrtho = ortho ? 1 : 0;
    return k;
}

float s_maxError;

/* Runs `count` vertices through the generated program; returns how many
 * matched the CPU XF path (position) and the expected colour. */
int runGenerated(const VertexKey& key, const LatteVertexFlags& flags, uint32_t gprOffset,
                 bool ortho, bool widescreen, int count, std::string* firstError) {
    LatteVertexImage image;
    if (!buildGeneratedVertexImage(key, flags, image)) {
        if (firstError) *firstError = std::string("build rejected: ") + image.reject;
        return -1;
    }
    Scene scene;
    makeScene(scene, ortho, widescreen);
    latte::VsInterpreter vm;
    vm.gprOffset = gprOffset;
    loadConstants(vm, scene);
    const uint32_t mats[] = {0, 3, 9, 30, 57};
    int good = 0;
    for (int n = 0; n < count; ++n) {
        HostVertex v;
        memset(static_cast<void*>(&v), 0, sizeof v);
        for (int i = 0; i < 3; ++i) v.pos[i] = rndf(-300.0f, 300.0f);
        for (int i = 0; i < 3; ++i) { v.nrm[i] = rndf(-1, 1); v.bin[i] = rndf(-1, 1); v.tan[i] = rndf(-1, 1); }
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 4; ++i) v.clr[c][i] = uint8_t(rnd());
        for (int t = 0; t < 8; ++t) { v.tex[t][0] = rndf(-4, 4); v.tex[t][1] = rndf(-4, 4); }
        const uint32_t mat = mats[rnd() % 5];
        v.mtx[0] = uint8_t(mat);
        for (int t = 1; t < 9; ++t) v.mtx[t] = uint8_t(mats[rnd() % 5]);
        latte::VsResult r;
        if (!vm.run(image.words, image.regs, image.attribs, image.attribCount,
                    reinterpret_cast<const uint8_t*>(&v), r)) {
            if (firstError && firstError->empty()) *firstError = r.error;
            continue;
        }
        float cpu[4];
        xfTransformPositionMath(&scene.xfPos[mat][0], v.pos, scene.view, cpu);
        const float scale = std::fabs(cpu[3]) + 1.0f;
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            if (!close(r.pos[i], cpu[i], scale)) ok = false;
            s_maxError = std::max(s_maxError, std::fabs(r.pos[i] - cpu[i]) / scale);
        }
        if (!ok && firstError && firstError->empty()) {
            char buf[256];
            snprintf(buf, sizeof buf, "clip mismatch gpu=(%g %g %g %g) cpu=(%g %g %g %g)",
                     r.pos[0], r.pos[1], r.pos[2], r.pos[3], cpu[0], cpu[1], cpu[2], cpu[3]);
            *firstError = buf;
        }
        // Colour: what chanColor gives for material-from-vertex, lighting
        // off (clr / 255), on the semantic the TEV pixel shader imports.
        if (!key.outputMode) {
            if (!r.hasSemantic[8] || !r.hasSemantic[9]) ok = false;
            for (int i = 0; ok && i < 4; ++i)
                if (r.semantic[8][i] != float(v.clr[0][i]) / 255.0f || r.semantic[9][i] != 0.0f) ok = false;
            if (!ok && firstError && firstError->empty()) *firstError = "colour semantic 8/9 mismatch";
        }
        if (ok) ++good;
    }
    return good;
}

int s_failures;
void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++s_failures;
}

/* ---- Grass: the real grass program vs the CPU loop ----------------------
 * For every blade of a group built from game-layout arrays, every corner,
 * both passes and several draw/swing states, the grass program's clip
 * position and colour must equal, bit for bit, what the generated GPU-XF
 * program produces for the vertex the CPU loop emits (position from
 * grassCpuNearVertex/grassCpuFarVertex, colour-table entry by index), and
 * match the CPU XF path within the generated program's own tolerance. */
using namespace sms_wiiu_grass;

struct GrassGroupFixture {
    uint32_t count;
    std::vector<float> near;     // TVec3<f32>: x, apex y, z
    std::vector<int16_t> far;    // TVec3<s16>: x, base y, z
    std::vector<int16_t> apex;   // s16 apex
    float baseY;
    std::vector<GrassGpuRecord> nearRecords, farRecords;
};

void makeGrassGroup(GrassGroupFixture& f, uint32_t count) {
    // TMapObjGrassGroup::load: near = scale * rand around mPosition, far =
    // the s16 truncation, apex = s16(mPosition.y + near.y).
    f.count = count;
    f.baseY = -37.0f;
    f.near.resize(count * 3);
    f.far.resize(count * 3);
    f.apex.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const float x = rndf(-900.0f, 900.0f), z = rndf(-1500.0f, 300.0f);
        const float y = rndf(0.0f, 200.0f) + f.baseY + 100.0f;
        f.near[i * 3 + 0] = x;
        f.near[i * 3 + 1] = y;
        f.near[i * 3 + 2] = z;
        f.far[i * 3 + 0] = int16_t(x);
        f.far[i * 3 + 1] = int16_t(f.baseY);
        f.far[i * 3 + 2] = int16_t(z);
        f.apex[i] = int16_t(f.baseY + y);
    }
    f.nearRecords.resize(count * 3);
    f.farRecords.resize(count * 3);
    grassExpandGroup(count, f.near.data(), f.far.data(), f.apex.data(), f.baseY,
                     f.nearRecords.data(), f.farRecords.data());
}

int s_grassChecked;

bool runGrassCase(const GrassGroupFixture& f, bool ortho, bool far, float drawXf, float drawZf,
                  const float* swingf, std::string& err) {
    LatteVertexImage grass, generated;
    if (!buildGrassVertexImage(ortho, grass)) { err = std::string("grass build: ") + grass.reject; return false; }
    if (!buildGeneratedVertexImage(grassStateKey(ortho), {false, false, false}, generated)) {
        err = "generated build";
        return false;
    }
    if (grass.stride != sizeof(GrassGpuRecord)) { err = "grass stride"; return false; }
    Scene scene;
    makeScene(scene, ortho, false);
    // initDrawNear: GX_PNMTX0 holds the view matrix.
    memcpy(&scene.xfPos[0][0], &scene.xfPos[9][0], 12 * sizeof(float));
    latte::VsInterpreter vm;
    loadConstants(vm, scene);
    // The dynamic values exactly as the renderer receives them: the near pass
    // gets mDrawVec / unk20, the far pass mDrawVecS16 / unk24 (truncations
    // the game already made) as floats.
    GrassNearDynamic nd;
    GrassFarDynamic fd;
    nd.drawX = drawXf;
    nd.drawZ = drawZf;
    fd.drawX = int16_t(drawXf);
    fd.drawZ = int16_t(drawZf);
    float shaderSwing[kGrassSwingSlots];
    for (uint32_t k = 0; k < kGrassSwingSlots; ++k) {
        nd.swing[k] = swingf[k];
        fd.swing[k] = int16_t(swingf[k]);
        shaderSwing[k] = far ? float(fd.swing[k]) : nd.swing[k];
    }
    float block[kGrassUniformRegs * 4];
    grassPackUniforms(far ? float(fd.drawX) : nd.drawX, far ? float(fd.drawZ) : nd.drawZ, shaderSwing, block);
    for (uint32_t r = 0; r < kGrassUniformRegs; ++r)
        vm.setConst(kGrassUniformDrawReg + r, block[r * 4], block[r * 4 + 1], block[r * 4 + 2], block[r * 4 + 3]);
    if (far && !grassFarFitsS16(grassFarMaxAbs(f.count, f.far.data()), 12.0f, 50.0f)) {
        err = "fixture outside the s16 guard";
        return false;
    }
    const uint8_t (*table)[4] = nullptr;
    static const uint8_t colours[2][4] = {{0x3c, 0xc8, 0x3c, 0xff}, {0x28, 0x64, 0x28, 0xff}};
    table = colours;
    for (uint32_t i = 0; i < f.count; ++i) {
        for (GrassCorner c : {kGrassLeft, kGrassCenter, kGrassRight}) {
            float cpuPos[3];
            uint8_t colourIndex;
            if (far) {
                const GrassFarBlade b = {f.far[i * 3 + 0], f.far[i * 3 + 2], f.far[i * 3 + 1], f.apex[i]};
                const GrassFarVertex v = grassCpuFarVertex(b, fd, i, c);
                // GXPosition3s16 with frac 0: the decoder's float position.
                cpuPos[0] = float(v.x); cpuPos[1] = float(v.y); cpuPos[2] = float(v.z);
                colourIndex = v.color;
            } else {
                const GrassNearBlade b = {f.near[i * 3 + 0], f.near[i * 3 + 2], f.near[i * 3 + 1], f.baseY};
                const GrassNearVertex v = grassCpuNearVertex(b, nd, i, c);
                cpuPos[0] = v.x; cpuPos[1] = v.y; cpuPos[2] = v.z;
                colourIndex = v.color;
            }
            const GrassGpuRecord& rec = (far ? f.farRecords : f.nearRecords)[i * 3 + c];
            latte::VsResult g;
            if (!vm.run(grass.words, grass.regs, grass.attribs, grass.attribCount,
                        reinterpret_cast<const uint8_t*>(&rec), g)) {
                err = "grass program: " + g.error;
                return false;
            }
            HostVertex hv;
            memset(static_cast<void*>(&hv), 0, sizeof hv);
            memcpy(hv.pos, cpuPos, sizeof cpuPos);
            memcpy(hv.clr[0], table[colourIndex], 4);
            hv.mtx[0] = 0;
            latte::VsResult ref;
            if (!vm.run(generated.words, generated.regs, generated.attribs, generated.attribCount,
                        reinterpret_cast<const uint8_t*>(&hv), ref)) {
                err = "generated program: " + ref.error;
                return false;
            }
            char buf[400];
            if (memcmp(g.pos, ref.pos, sizeof g.pos) != 0) {
                snprintf(buf, sizeof buf, "%s blade %u corner %u: clip (%.9g %.9g %.9g %.9g) != generated (%.9g %.9g %.9g %.9g) for cpu pos (%.9g %.9g %.9g)",
                         far ? "far" : "near", i, unsigned(c), g.pos[0], g.pos[1], g.pos[2], g.pos[3],
                         ref.pos[0], ref.pos[1], ref.pos[2], ref.pos[3], cpuPos[0], cpuPos[1], cpuPos[2]);
                err = buf;
                return false;
            }
            float cpuClip[4];
            xfTransformPositionMath(&scene.xfPos[0][0], cpuPos, scene.view, cpuClip);
            for (int k = 0; k < 4; ++k)
                if (!close(g.pos[k], cpuClip[k], std::fabs(cpuClip[3]) + 1.0f)) {
                    snprintf(buf, sizeof buf, "%s blade %u corner %u: clip %d %.9g vs CPU XF %.9g",
                             far ? "far" : "near", i, unsigned(c), k, g.pos[k], cpuClip[k]);
                    err = buf;
                    return false;
                }
            if (!g.hasSemantic[8] || memcmp(g.semantic[8], ref.semantic[8], sizeof g.semantic[8]) != 0 ||
                !g.hasSemantic[9] || memcmp(g.semantic[9], ref.semantic[9], sizeof g.semantic[9]) != 0) {
                snprintf(buf, sizeof buf, "%s blade %u corner %u: colour semantic 8 (%g %g %g %g) != colour-table[%u]",
                         far ? "far" : "near", i, unsigned(c), g.semantic[8][0], g.semantic[8][1],
                         g.semantic[8][2], g.semantic[8][3], unsigned(colourIndex));
                err = buf;
                return false;
            }
            for (uint32_t id = 0; id < 8; ++id)
                if (!g.hasSemantic[id] || memcmp(g.semantic[id], ref.semantic[id], sizeof g.semantic[id]) != 0) {
                    err = "texcoord semantic differs from the generated program";
                    return false;
                }
            ++s_grassChecked;
        }
    }
    return true;
}

void runGrassOracle() {
    GrassGroupFixture f;
    makeGrassGroup(f, 137);  // blades 0, 1, 9, 10 ... 136: every swing slot many times
    struct Dyn { float dx, dz; const char* name; };
    const Dyn dyns[] = {
        {11.93f, -1.27f, "positive x / negative z draw vector"},
        {-8.51f, -8.43f, "negative draw vector"},
        {-0.4f, 11.99f, "draw vector truncating to zero in x"},
    };
    for (int ortho = 0; ortho < 2; ++ortho)
        for (int far = 0; far < 2; ++far)
            for (const Dyn& d : dyns) {
                float swing[kGrassSwingSlots];
                for (uint32_t k = 0; k < kGrassSwingSlots; ++k)
                    swing[k] = 50.0f * std::sin(0.7f + 0.2f * float(k) + d.dx);  // distinct, both signs
                std::string err;
                const bool ok = runGrassCase(f, ortho != 0, far != 0, d.dx, d.dz, swing, err);
                char what[200];
                snprintf(what, sizeof what, "grass %s pass, %s projection, %s: all %u blades x 3 corners",
                         far ? "far" : "near", ortho ? "orthographic" : "perspective", d.name, f.count);
                if (!ok) std::printf("  first error: %s\n", err.c_str());
                check(ok, what);
            }
    std::printf("  grass vertices checked against the CPU loop: %d\n", s_grassChecked);

    // Structure: one program serves both passes; float-only (no integer or
    // AR instructions), the generated TEV export layout and fetch layout.
    LatteVertexImage persp;
    buildGrassVertexImage(false, persp);
    bool floatOnly = true;
    // The ALU words start at the first ALU clause's address (CF word 1).
    const size_t aluStart = __builtin_bswap32(uint32_t(persp.words[1] >> 32)) & 0x3fffff;
    for (size_t i = aluStart; i < persp.words.size(); ++i) {
        const uint64_t op = persp.words[i];
        const uint32_t w1 = __builtin_bswap32(uint32_t(op));
        if (((w1 >> 15) & 7) != 0) continue;  // OP3: MULADD / CNDGT
        const uint32_t inst = (w1 >> 7) & 0x7ff;
        if (inst == ALU_OP2_INST_MOVA_INT || inst == ALU_OP2_INST_FLT_TO_INT || inst == ALU_OP2_INST_INT_TO_FLT ||
            inst == ALU_OP2_INST_ADD_INT || inst == ALU_OP2_INST_MULLO_INT || inst == ALU_OP2_INST_LSHL_INT ||
            inst == ALU_OP2_INST_LSHR_INT || inst == ALU_OP2_INST_SETGE_INT || inst == ALU_OP2_INST_AND_INT)
            floatOnly = false;
    }
    LatteVertexImage tev;
    buildGeneratedVertexImage(grassStateKey(false), {false, false, false}, tev);
    check(floatOnly, "grass program uses no integer or AR instructions");
    check(persp.regs.spi_vs_out_config == tev.regs.spi_vs_out_config &&
              !memcmp(persp.regs.spi_vs_out_id, tev.regs.spi_vs_out_id, sizeof persp.regs.spi_vs_out_id) &&
              persp.regs.sq_pgm_resources_vs == tev.regs.sq_pgm_resources_vs,
          "grass exports copy the generated TEV vertex shader (count, config, semantic ids)");
    check(persp.attribCount == 2 && persp.attribs[0].location == 0 && persp.attribs[0].offset == 0 &&
              persp.attribs[1].location == 1 && persp.attribs[1].offset == offsetof(GrassGpuRecord, drawBiased) &&
              persp.attribs[1].kind == kLatteAttribUscaled8x4 && persp.regs.num_sq_vtx_semantic == 2,
          "grass fetch: location 0 = xyz floats, location 1 = unsigned scaled metadata bytes");
}

} // namespace

int main() {
    const int kVertices = 64;
    const LatteVertexFlags hw = {false, false, false};
    const LatteVertexFlags cemu = {true, false, false};

    // 1. The generated program reproduces the CPU XF path (perspective,
    //    orthographic, widescreen XMap, Cemu's naming preamble).
    struct Case { bool ortho, wide; const LatteVertexFlags* flags; const char* name; };
    const Case cases[] = {
        {false, false, &hw, "generated GPU-XF program == CPU XF (perspective)"},
        {true, false, &hw, "generated GPU-XF program == CPU XF (orthographic)"},
        {false, true, &hw, "generated GPU-XF program == CPU XF (widescreen x-map)"},
        {false, false, &cemu, "generated GPU-XF program == CPU XF (Cemu XF naming)"},
    };
    for (const Case& c : cases) {
        std::string err;
        const int good = runGenerated(grassStateKey(c.ortho), *c.flags, 1, c.ortho, c.wide, kVertices, &err);
        if (good != kVertices) std::printf("  first error: %s\n", err.c_str());
        check(good == kVertices, c.name);
    }
    std::printf("  max |gpu-cpu| / (|w|+1) = %.3g\n", double(s_maxError));

    // 2. N1: fetch location n -> R(n + offset).  Only offset 1 reproduces
    //    the hardware-proven program's result.
    for (uint32_t offset = 0; offset <= 4; ++offset) {
        std::string err;
        const int good = runGenerated(grassStateKey(false), hw, offset, false, false, 8, &err);
        char what[160];
        if (offset == 1) {
            snprintf(what, sizeof what, "N1: fetch location n lands in R(n+1) (8/8 vertices match)");
            check(good == 8, what);
        } else {
            snprintf(what, sizeof what, "N1: R(n+%u) mapping is refuted (%d/8 match; %s)", offset,
                     good < 0 ? 0 : good, err.c_str());
            check(good <= 0, what);
        }
    }

    // 3. N5: the TEV pixel shader for initDrawNear's TEV state (one stage,
    //    order COLOR0A0 with no texture, colour = RASC, alpha = RASA) imports
    //    exactly semantic 8, and the generated TEV vertex shader exports the
    //    vertex colour on semantic 8.
    {
        const uint32_t order[1] = {(0u << 7) | (0u << 6) | (7u << 3) | 7u};
        const uint32_t colorEnv[1] = {(10u << 12) | (15u << 8) | (15u << 4) | 15u};
        const uint32_t alphaEnv[1] = {(5u << 13) | (7u << 10) | (7u << 7) | (7u << 4)};
        bool useCoord[8] = {}, useRas[2] = {};
        tevScanInputs(1, order, colorEnv, alphaEnv, 0, 0, useCoord, useRas);
        uint32_t semantic[10], coordSlot[8], rasSlot[2];
        memset(coordSlot, 0xff, sizeof coordSlot);
        memset(rasSlot, 0xff, sizeof rasSlot);
        const uint32_t inputs = tevImportSemantics(useCoord, useRas, semantic, coordSlot, rasSlot);
        check(inputs == 1 && semantic[0] == 8 && rasSlot[0] == 0,
              "N5: the grass TEV pixel shader imports exactly semantic 8 (colour channel 0)");

        LatteVertexImage tev;
        buildGeneratedVertexImage(grassStateKey(false), hw, tev);
        check(tev.regs.spi_vs_out_config == (9u << 1) && tev.regs.spi_vs_out_id[2] == 0xffff0908u,
              "N5: generated TEV vertex shader exports 10 params, PARAM8/9 -> semantics 8/9");

        // The fixed-path layout (outputMode 1: five params, colour in PARAM4)
        // is what round 1 copied for grass: it never produces semantic 8.
        VertexKey fixed = grassStateKey(false);
        fixed.outputMode = 1;
        LatteVertexImage fx;
        buildGeneratedVertexImage(fixed, hw, fx);
        Scene scene;
        makeScene(scene, false, false);
        latte::VsInterpreter vm;
        loadConstants(vm, scene);
        HostVertex v;
        memset(static_cast<void*>(&v), 0, sizeof v);
        v.clr[0][0] = 0x3c; v.clr[0][1] = 0xc8; v.clr[0][2] = 0x3c; v.clr[0][3] = 0xff;
        latte::VsResult r;
        const bool ran = vm.run(fx.words, fx.regs, fx.attribs, fx.attribCount, reinterpret_cast<const uint8_t*>(&v), r);
        check(ran && r.paramCount == 5 && !r.hasSemantic[8] && r.hasSemantic[4] &&
                  r.semantic[4][1] == float(0xc8) / 255.0f,
              "N5: the five-param fixed layout puts colour on semantic 4 and leaves semantic 8 unwritten");
    }

    // 4. Grass near and far programs against the CPU loop.
    runGrassOracle();

    if (s_failures) {
        std::printf("latte vertex oracle: %d FAILED\n", s_failures);
        return 1;
    }
    std::puts("latte vertex oracle: PASS");
    return 0;
}
