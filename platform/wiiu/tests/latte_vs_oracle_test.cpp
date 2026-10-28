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

    if (s_failures) {
        std::printf("latte vertex oracle: %d FAILED\n", s_failures);
        return 1;
    }
    std::puts("latte vertex oracle: PASS");
    return 0;
}
