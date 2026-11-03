#define JPA_LIST_DEFER_INLINE
#define JPA_FIELD_BLOCK_DEFER_INLINE
#include "JSystem/JParticle/JPAResource.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include "JSystem/JParticle/JPABaseShape.hpp"
#include "JSystem/JParticle/JPAChildShape.hpp"
#include "JSystem/JParticle/JPAEmitter.hpp"
#include "JSystem/JParticle/JPAExTexShape.hpp"
#include "JSystem/JParticle/JPAExtraShape.hpp"
#include "JSystem/JParticle/JPAFieldBlock.hpp"
#include "JSystem/JParticle/JPAKeyBlock.hpp"
#include "JSystem/JParticle/JPAParticle.hpp"
#include "JSystem/JParticle/JPAResourceManager.hpp"
#include "JPADirectionalBatch.hpp"
#include <revolution/mtx.h>
#include <cstring>
#if defined(TARGET_WIIU)
#include "wiiu/jpa_counters.h"
extern "C" int sms_wiiu_sd_marker(const char*);
extern "C" const char* sms_wiiu_stage_path(void);
extern "C" void port_log(const char*, ...);
extern "C" void GXGetVtxDesc(GXAttr, GXAttrType*);
extern "C" void GXGetVtxAttrFmt(GXVtxFmt, GXAttr, GXCompCnt*, GXCompType*, u8*);
#endif

#if defined(TARGET_WIIU)
namespace {
struct DirectionKey {
    u32 flags;
    GXColor prm, env;
    u8 alpha, tex;
    s16 age;
};

enum DirectionKeyFlags {
    DirectionKeyPrm = 1 << 0,
    DirectionKeyEnv = 1 << 1,
    DirectionKeyAlpha = 1 << 2,
    DirectionKeyTex = 1 << 3,
    DirectionKeyAge = 1 << 4,
};

u32 directionKeyFlags(JPAResource::ParticleFunc* funcs, int funcCount, bool ageMatters) {
    const JPAResource::ParticleFunc loadTexAnm = static_cast<JPAResource::ParticleFunc>(&JPALoadTexAnm);
    const JPAResource::ParticleFunc registEnv = static_cast<JPAResource::ParticleFunc>(&JPARegistEnv);
    u32 flags = ageMatters ? DirectionKeyAge : 0;
    for (int i = 1; i < funcCount; ++i) {
        JPAResource::ParticleFunc func = funcs[i];
        if (func == loadTexAnm) flags |= DirectionKeyTex;
        if (func == &JPALoadCalcTexCrdMtxAnm) flags |= DirectionKeyAge;
        if (func == &JPARegistPrmAlpha || func == &JPARegistPrmAlphaEnv) flags |= DirectionKeyPrm;
        if (func == registEnv || func == &JPARegistAlphaEnv || func == &JPARegistPrmAlphaEnv)
            flags |= DirectionKeyEnv;
        if (func == &JPARegistAlpha || func == &JPARegistAlphaEnv || func == &JPARegistPrmAlpha ||
            func == &JPARegistPrmAlphaEnv)
            flags |= DirectionKeyAlpha;
    }
    return flags;
}

DirectionKey directionKey(JPABaseParticle* particle, JPAResource::ParticleFunc* funcs, int funcCount,
                          bool ageMatters) {
    DirectionKey key;
    key.flags = directionKeyFlags(funcs, funcCount, ageMatters);
    key.prm = particle->mPrmClr;
    key.env = particle->mEnvClr;
    key.alpha = particle->mPrmColorAlphaAnm;
    key.tex = particle->mTexAnmIdx;
    key.age = ageMatters ? particle->mAge : 0;
    return key;
}

bool sameDirectionKey(const DirectionKey& a, const DirectionKey& b) {
    if (a.flags != b.flags) return false;
    if ((a.flags & DirectionKeyPrm) &&
        (a.prm.r != b.prm.r || a.prm.g != b.prm.g || a.prm.b != b.prm.b || a.prm.a != b.prm.a)) return false;
    if ((a.flags & DirectionKeyEnv) &&
        (a.env.r != b.env.r || a.env.g != b.env.g || a.env.b != b.env.b || a.env.a != b.env.a)) return false;
    if ((a.flags & DirectionKeyAlpha) && a.alpha != b.alpha) return false;
    if ((a.flags & DirectionKeyTex) && a.tex != b.tex) return false;
    return !(a.flags & DirectionKeyAge) || a.age == b.age;
}

bool directionalCallbackAllowed(JPAEmitterWorkData* work, JPAResource::ParticleFunc* funcs,
                                int funcCount, int shapeType, bool child) {
    return work->mpEmtr->mpPtclCallBack == NULL ||
        (!child && shapeType == 3 && funcCount > 0 && funcs[0] == &JPADrawRotDirection &&
         jpa_batch::callbackDrawsNothing(work->mpEmtr->mpPtclCallBack));
}

void countDirectionalParticle(JPAEmitterWorkData* work, JPABaseParticle* particle, int shapeType,
                               bool child, JPAResource::ParticleFunc* funcs, int funcCount) {
    if (shapeType == 3) sms_wiiu_jpa_type3();
    if (shapeType == 4) sms_wiiu_jpa_type4();
    if (shapeType != 3 && shapeType != 4) {
        sms_wiiu_jpa_reject_type();
    } else if (!directionalCallbackAllowed(work, funcs, funcCount, shapeType, child)) {
        sms_wiiu_jpa_reject_callback();
    } else if (work->mPrjType != 0) {
        sms_wiiu_jpa_reject_prj();
        sms_wiiu_jpa_reject_reason(SMS_WIIU_JPA_REJECT_PROJECTION);
    } else if (!jpa_batch::eligible(work, particle)) {
        sms_wiiu_jpa_reject_ineligible();
        sms_wiiu_jpa_reject_reason(SMS_WIIU_JPA_REJECT_OTHER);
    }
}

struct BillboardBatchMode {
    bool prmRgb;
    bool envRgb;
    bool alpha;
    bool envAlpha;
    bool alphaAnimated;
    bool alphaFromParticlePrm;
    bool rasterColorPrm;
    bool rasterColorEnv;
    bool rasterColorAlphaPrm;
    bool rasterColorAlphaEnv;
    bool rasterAlphaPrm;
    bool rasterAlphaEnv;
    bool twoColor;
    bool textureAnimated;
};

struct BillboardColorRejectResource {
    JPAResource* resource;
    unsigned count;
    GXTevColorArg color[4];
    GXTevAlphaArg alpha[4];
    bool prmRgb;
    bool envRgb;
    bool alphaVarying;
    bool envAlpha;
};

BillboardColorRejectResource sBillboardColorRejects[256];
char sBillboardColorRejectStage[128];
bool sHaveBillboardColorRejectStage;

bool jpadiagMarker() {
    static int marker = -1;
    if (marker < 0) marker = sms_wiiu_sd_marker("jpadiag") != 0;
    return marker != 0;
}

void resetBillboardColorRejects() {
    for (unsigned i = 0; i < sizeof(sBillboardColorRejects) / sizeof(sBillboardColorRejects[0]); ++i)
        sBillboardColorRejects[i].count = 0;
}

void noteBillboardColorReject(JPAResource* resource, const BillboardBatchMode& mode,
                              const GXTevColorArg* color, const GXTevAlphaArg* alpha) {
    if (!jpadiagMarker()) return;
    BillboardColorRejectResource* entry = NULL;
    for (unsigned i = 0; i < sizeof(sBillboardColorRejects) / sizeof(sBillboardColorRejects[0]); ++i) {
        if (sBillboardColorRejects[i].resource == resource) {
            entry = &sBillboardColorRejects[i];
            break;
        }
    }
    if (entry == NULL) {
        for (unsigned i = 0; i < sizeof(sBillboardColorRejects) / sizeof(sBillboardColorRejects[0]); ++i) {
            if (sBillboardColorRejects[i].count == 0) {
                entry = &sBillboardColorRejects[i];
                entry->resource = resource;
                for (int j = 0; j < 4; ++j) {
                    entry->color[j] = color[j];
                    entry->alpha[j] = alpha[j];
                }
                entry->prmRgb = mode.prmRgb;
                entry->envRgb = mode.envRgb;
                entry->alphaVarying = mode.alpha;
                entry->envAlpha = mode.envAlpha;
                break;
            }
        }
    }
    if (entry != NULL) ++entry->count;
}

void reportBillboardColorRejects() {
    if (!jpadiagMarker() || !sHaveBillboardColorRejectStage) return;
    unsigned total = 0;
    for (unsigned i = 0; i < sizeof(sBillboardColorRejects) / sizeof(sBillboardColorRejects[0]); ++i)
        if (sBillboardColorRejects[i].count != 0) ++total;
    port_log("[jpadiag] stage=%s colorResources=%u\n", sBillboardColorRejectStage, total);
    unsigned selected[8];
    unsigned selectedCount = 0;
    for (unsigned rank = 0; rank < 8; ++rank) {
        unsigned best = sizeof(sBillboardColorRejects) / sizeof(sBillboardColorRejects[0]);
        for (unsigned i = 0; i < sizeof(sBillboardColorRejects) / sizeof(sBillboardColorRejects[0]); ++i) {
            if (sBillboardColorRejects[i].count == 0) continue;
            bool already = false;
            for (unsigned j = 0; j < selectedCount; ++j)
                if (selected[j] == i) already = true;
            if (!already && (best == sizeof(sBillboardColorRejects) / sizeof(sBillboardColorRejects[0]) ||
                             sBillboardColorRejects[i].count > sBillboardColorRejects[best].count))
                best = i;
        }
        if (best == sizeof(sBillboardColorRejects) / sizeof(sBillboardColorRejects[0])) break;
        selected[selectedCount++] = best;
        const BillboardColorRejectResource& entry = sBillboardColorRejects[best];
        port_log("[jpadiag] rank=%u res=%u rejects=%u tevC=%u,%u,%u,%u tevA=%u,%u,%u,%u flags=prmRgb:%u,envRgb:%u,alpha:%u,envAlpha:%u\n",
                 rank + 1, (unsigned)entry.resource->getUsrIdx(), entry.count,
                 (unsigned)entry.color[0], (unsigned)entry.color[1],
                 (unsigned)entry.color[2], (unsigned)entry.color[3],
                 (unsigned)entry.alpha[0], (unsigned)entry.alpha[1],
                 (unsigned)entry.alpha[2], (unsigned)entry.alpha[3],
                 entry.prmRgb ? 1u : 0u, entry.envRgb ? 1u : 0u,
                 entry.alphaVarying ? 1u : 0u, entry.envAlpha ? 1u : 0u);
    }
}

void setBillboardColorRejectStage(const char* stage) {
    if (!jpadiagMarker()) return;
    if (!sHaveBillboardColorRejectStage) {
        std::strncpy(sBillboardColorRejectStage, stage, sizeof(sBillboardColorRejectStage) - 1);
        sBillboardColorRejectStage[sizeof(sBillboardColorRejectStage) - 1] = '\0';
        sHaveBillboardColorRejectStage = true;
    } else if (std::strcmp(sBillboardColorRejectStage, stage) != 0) {
        reportBillboardColorRejects();
        resetBillboardColorRejects();
        std::strncpy(sBillboardColorRejectStage, stage, sizeof(sBillboardColorRejectStage) - 1);
        sBillboardColorRejectStage[sizeof(sBillboardColorRejectStage) - 1] = '\0';
    }
}

typedef SmsWiiuJpaRejectReason BillboardRejectReason;

void rejectBillboardIneligible(BillboardRejectReason reason) {
    sms_wiiu_jpa_reject_ineligible();
    sms_wiiu_jpa_reject_reason(reason);
}

bool hasTevColorArg(const GXTevColorArg* args, GXTevColorArg wanted) {
    for (int i = 0; i < 4; ++i)
        if (args[i] == wanted) return true;
    return false;
}

bool hasTevAlphaArg(const GXTevAlphaArg* args, GXTevAlphaArg wanted) {
    for (int i = 0; i < 4; ++i)
        if (args[i] == wanted) return true;
    return false;
}

bool batchMarker();

bool batchMarker2() {
    static int marker = -1;
    if (marker < 0) marker = batchMarker() && sms_wiiu_sd_marker("jpabatch2") != 0;
    return marker != 0;
}

bool billboardTwoColorAllowed(JPAEmitterWorkData* work, const BillboardBatchMode& mode,
                              const GXTevColorArg* colorArgs, const GXTevAlphaArg* alphaArgs) {
    if (!mode.prmRgb || !mode.envRgb)
        return false;
    JPAExTexShape* exTex = work->mpRes->getEts();
    if (exTex != NULL && (exTex->isUseIndirect() || exTex->isUseSecTex())) return false;
    if (hasTevColorArg(colorArgs, GX_CC_A0) && (!mode.alpha || !mode.alphaFromParticlePrm))
        return false;
    if (hasTevColorArg(colorArgs, GX_CC_A1) && !mode.envAlpha)
        return false;
    if (hasTevAlphaArg(alphaArgs, GX_CA_A0) && (!mode.alpha || !mode.alphaFromParticlePrm))
        return false;
    if (hasTevAlphaArg(alphaArgs, GX_CA_A1) && !mode.envAlpha)
        return false;
    return hasTevColorArg(colorArgs, GX_CC_C0) && hasTevColorArg(colorArgs, GX_CC_C1) &&
        batchMarker2();
}

bool billboardBatchMode(JPAEmitterWorkData* work, JPAResource::ParticleFunc* funcs,
                        int funcCount, BillboardBatchMode* mode,
                        BillboardRejectReason* rejectReason) {
    mode->prmRgb = false;
    mode->envRgb = false;
    mode->alpha = false;
    mode->envAlpha = false;
    mode->alphaAnimated = false;
    mode->alphaFromParticlePrm = false;
    mode->rasterColorPrm = false;
    mode->rasterColorEnv = false;
    mode->rasterColorAlphaPrm = false;
    mode->rasterColorAlphaEnv = false;
    mode->rasterAlphaPrm = false;
    mode->rasterAlphaEnv = false;
    mode->twoColor = false;
    mode->textureAnimated = false;
    *rejectReason = SMS_WIIU_JPA_REJECT_OTHER;

    const JPAResource::ParticleFunc registEnv = static_cast<JPAResource::ParticleFunc>(&JPARegistEnv);
    const JPAResource::ParticleFunc loadTexAnm = static_cast<JPAResource::ParticleFunc>(&JPALoadTexAnm);
    const JPAResource::ParticleFunc setPointSize = static_cast<JPAResource::ParticleFunc>(&JPASetPointSize);
    const JPAResource::ParticleFunc setLineWidth = static_cast<JPAResource::ParticleFunc>(&JPASetLineWidth);
    for (int i = 1; i < funcCount; ++i) {
        JPAResource::ParticleFunc func = funcs[i];
        if (func == &JPADrawParticleCallBack) {
            if (work->mpEmtr->mpPtclCallBack != NULL &&
                !jpa_batch::callbackDrawsNothing(work->mpEmtr->mpPtclCallBack)) return false;
        } else if (func == loadTexAnm) {
            mode->textureAnimated = true;
        } else if (func == &JPALoadCalcTexCrdMtxAnm) {
            *rejectReason = SMS_WIIU_JPA_REJECT_TEXTURE;
            return false;
        } else if (func == setPointSize || func == setLineWidth) {
            *rejectReason = SMS_WIIU_JPA_REJECT_SCALE;
            return false;
        } else if (func == &JPARegistPrmAlpha) {
            mode->prmRgb = true;
            mode->alpha = true;
            mode->alphaAnimated = true;
            mode->alphaFromParticlePrm = true;
        } else if (func == &JPARegistPrmAlphaEnv) {
            mode->prmRgb = true;
            mode->envRgb = true;
            mode->alpha = true;
            mode->envAlpha = true;
            mode->alphaAnimated = true;
            mode->alphaFromParticlePrm = true;
        } else if (func == &JPARegistAlpha) {
            mode->alpha = true;
            mode->alphaAnimated = true;
        } else if (func == &JPARegistAlphaEnv) {
            mode->envRgb = true;
            mode->alpha = true;
            mode->envAlpha = true;
            mode->alphaAnimated = true;
        } else if (func == registEnv) {
            mode->envRgb = true;
            mode->envAlpha = true;
        } else {
            // Keep unknown particle-side state on the original per-particle
            // path rather than guessing at its effect on the GX registers.
            return false;
        }
    }

    const GXTevColorArg* colorArgs = work->mpRes->getBsp()->getTevColorArg();
    mode->rasterColorPrm = mode->prmRgb && hasTevColorArg(colorArgs, GX_CC_C0);
    mode->rasterColorEnv = mode->envRgb && hasTevColorArg(colorArgs, GX_CC_C1);
    mode->rasterColorAlphaPrm = mode->alpha && hasTevColorArg(colorArgs, GX_CC_A0);
    mode->rasterColorAlphaEnv = mode->envAlpha && hasTevColorArg(colorArgs, GX_CC_A1);
    // One raster colour carries one varying RGB source and one varying alpha
    // source. The other TEV colour register may remain emitter state, but two
    // particle-varying sources in either component cannot be rewritten exactly.
    const GXTevAlphaArg* alphaArgs = work->mpRes->getBsp()->getTevAlphaArg();
    if (mode->rasterColorPrm && mode->rasterColorEnv) {
        if (billboardTwoColorAllowed(work, *mode, colorArgs, alphaArgs)) {
            mode->twoColor = true;
            return true;
        }
        *rejectReason = SMS_WIIU_JPA_REJECT_COLOR_RGB;
        noteBillboardColorReject(work->mpRes, *mode, colorArgs, alphaArgs);
        return false;
    }
    if (mode->rasterColorAlphaPrm && mode->rasterColorAlphaEnv) {
        *rejectReason = SMS_WIIU_JPA_REJECT_COLOR_ALPHA;
        noteBillboardColorReject(work->mpRes, *mode, colorArgs, alphaArgs);
        return false;
    }
    mode->rasterAlphaPrm = mode->alpha && hasTevAlphaArg(alphaArgs, GX_CA_A0);
    mode->rasterAlphaEnv = mode->envAlpha && hasTevAlphaArg(alphaArgs, GX_CA_A1);
    if ((mode->rasterColorAlphaPrm || mode->rasterAlphaPrm) &&
        (mode->rasterColorAlphaEnv || mode->rasterAlphaEnv)) {
        *rejectReason = SMS_WIIU_JPA_REJECT_COLOR_ALPHA;
        noteBillboardColorReject(work->mpRes, *mode, colorArgs, alphaArgs);
        return false;
    }
    return true;
}

bool billboardCallbackAllowed(JPAEmitterWorkData* work, bool child) {
    if (work->mpEmtr->mpPtclCallBack == NULL) return true;
    return !child && jpa_batch::callbackDrawsNothing(work->mpEmtr->mpPtclCallBack);
}

void countBillboardParticle(JPAEmitterWorkData* work, JPABaseParticle* particle,
                            int shapeType, bool child, JPAResource::ParticleFunc* funcs,
                            int funcCount) {
    if (shapeType == 2) sms_wiiu_jpa_type2();
    if (shapeType != 2 || funcCount == 0 ||
        (funcs[0] != &JPADrawBillboard && funcs[0] != &JPADrawRotBillboard)) {
        sms_wiiu_jpa_reject_type();
    } else if (!billboardCallbackAllowed(work, child)) {
        sms_wiiu_jpa_reject_callback();
    } else if (work->mPrjType != 0) {
        sms_wiiu_jpa_reject_prj();
        sms_wiiu_jpa_reject_reason(SMS_WIIU_JPA_REJECT_PROJECTION);
    } else {
        BillboardBatchMode mode;
        BillboardRejectReason reason;
        if (particle->checkStatus(8)) {
            rejectBillboardIneligible(SMS_WIIU_JPA_REJECT_OTHER);
        } else if (!billboardBatchMode(work, funcs, funcCount, &mode, &reason)) {
            rejectBillboardIneligible(reason);
        }
    }
}

bool batchMarker() {
    static int marker = -1;
    if (marker < 0) marker = sms_wiiu_sd_marker("jpabatch") != 0;
    return marker != 0;
}

bool canBatchDirectional(JPAEmitterWorkData* work, JPAResource::ParticleFunc* funcs, int funcCount,
                         int shapeType, bool child) {
    return batchMarker() && (shapeType == 3 || shapeType == 4) && work->mPrjType == 0 &&
        directionalCallbackAllowed(work, funcs, funcCount, shapeType, child) && funcCount > 0 &&
        (funcs[0] == &JPADrawDirection || funcs[0] == &JPADrawRotDirection);
}

bool canBatchBillboard(JPAEmitterWorkData* work, JPAResource::ParticleFunc* funcs, int funcCount,
                       int shapeType, bool child, BillboardBatchMode* mode) {
    BillboardRejectReason rejectReason;
    return batchMarker() && shapeType == 2 && work->mPrjType == 0 && funcCount > 0 &&
        (funcs[0] == &JPADrawBillboard || funcs[0] == &JPADrawRotBillboard) &&
        billboardCallbackAllowed(work, child) &&
        billboardBatchMode(work, funcs, funcCount, mode, &rejectReason);
}

const char* jpaDrawFuncName(JPAResource::ParticleFunc func) {
    if (func == &JPADrawBillboard) return "JPADrawBillboard";
    if (func == &JPADrawRotBillboard) return "JPADrawRotBillboard";
    if (func == &JPADrawYBillboard) return "JPADrawYBillboard";
    if (func == &JPADrawRotYBillboard) return "JPADrawRotYBillboard";
    if (func == &JPADrawDirection) return "JPADrawDirection";
    if (func == &JPADrawRotDirection) return "JPADrawRotDirection";
    if (func == &JPADrawDBillboard) return "JPADrawDBillboard";
    if (func == &JPADrawRotation) return "JPADrawRotation";
    if (func == &JPADrawPoint) return "JPADrawPoint";
    if (func == &JPADrawLine) return "JPADrawLine";
    if (func == &JPADrawParticleCallBack) return "JPADrawParticleCallBack";
    return "unknown";
}

void noteParticle(JPAEmitterWorkData* work, JPABaseParticle* particle, int shapeType,
                  bool child, JPAResource::ParticleFunc* funcs, int funcCount) {
    sms_wiiu_jpa_particle();
    const bool directionalFunc = funcCount > 0 &&
        (funcs[0] == &JPADrawDirection || funcs[0] == &JPADrawRotDirection);
    if (directionalFunc) countDirectionalParticle(work, particle, shapeType, child, funcs, funcCount);
    else countBillboardParticle(work, particle, shapeType, child, funcs, funcCount);
    sms_wiiu_jpa_histogram(child, (unsigned)shapeType,
                           funcCount > 0 ? jpaDrawFuncName(funcs[0]) : "none",
                           work->mpEmtr->mpPtclCallBack != NULL, work->mpEmtr);
}

struct DirectionTracker {
    bool have;
    DirectionKey previous;
    DirectionTracker() : have(false) { }
    void observe(JPAEmitterWorkData* work, JPABaseParticle* particle, bool directional, bool ageMatters,
                 JPAResource::ParticleFunc* funcs, int funcCount) {
        if (!directional || !jpa_batch::eligible(work, particle)) return;
        const DirectionKey key = directionKey(particle, funcs, funcCount, ageMatters);
        if (have && !sameDirectionKey(previous, key)) sms_wiiu_jpa_split();
        previous = key;
        have = true;
    }
};
}
#endif

#if defined(TARGET_WIIU)
extern "C" void sms_wiiu_jpa_diag_stage_tick() {
    if (jpadiagMarker()) {
        const char* stage = sms_wiiu_stage_path();
        setBillboardColorRejectStage(stage != NULL ? stage : "unknown");
    }
}

extern "C" void sms_wiiu_jpa_diag_flush() {
    reportBillboardColorRejects();
    resetBillboardColorRejects();
    sHaveBillboardColorRejectStage = false;
}
#endif

JPAResource::JPAResource() {
    mpDrawEmitterChildFuncList = NULL;
    mpDrawEmitterFuncList = NULL;
    mpCalcEmitterFuncList = NULL;
    mpDrawParticleChildFuncList = NULL;
    mpCalcParticleChildFuncList = NULL;
    mpDrawParticleFuncList = NULL;
    mpCalcParticleFuncList = NULL;
    mpBaseShape = NULL;
    mpExtraShape = NULL;
    mpChildShape = NULL;
    mpExTexShape = NULL;
    mpDynamicsBlock = NULL;
    mpFieldBlocks = NULL;
    mpKeyBlocks = NULL;
    mpTDB1 = NULL;
    mpDrawParticleChildFuncListNum = 0;
    mpCalcParticleChildFuncListNum = 0;
    mpDrawParticleFuncListNum = 0;
    mpCalcParticleFuncListNum = 0;
    mpDrawEmitterChildFuncListNum = 0;
    mpDrawEmitterFuncListNum = 0;
    mpCalcEmitterFuncListNum = 0;
    mTDB1Num = 0;
    mKeyBlockNum = 0;
    mFieldBlockNum = 0;
    mUsrIdx = 0;
}

static u8 jpa_pos[324] ATTRIBUTE_ALIGN(32) = {
    0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x32, 0xCE, 0x00, 0x00, 0xCE, 0x00, 0xE7, 0x00, 0x00, 0x19, 0x00, 0x00, 0x19, 0xCE, 0x00, 0xE7, 0xCE, 0x00,
    0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0xCE, 0xCE, 0x00, 0x00, 0x19, 0x00, 0x32, 0x19, 0x00, 0x32, 0xE7, 0x00, 0x00, 0xE7, 0x00,
    0xE7, 0x19, 0x00, 0x19, 0x19, 0x00, 0x19, 0xE7, 0x00, 0xE7, 0xE7, 0x00, 0xCE, 0x19, 0x00, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0xCE, 0xE7, 0x00,
    0x00, 0x32, 0x00, 0x32, 0x32, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE7, 0x32, 0x00, 0x19, 0x32, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00,
    0xCE, 0x32, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x32, 0x00, 0x32, 0x00, 0x00, 0x32,
    0xE7, 0x00, 0x00, 0x19, 0x00, 0x00, 0x19, 0x00, 0x32, 0xE7, 0x00, 0x32, 0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0xCE, 0x00, 0x32,
    0x00, 0x00, 0xE7, 0x32, 0x00, 0xE7, 0x32, 0x00, 0x19, 0x00, 0x00, 0x19, 0xE7, 0x00, 0xE7, 0x19, 0x00, 0xE7, 0x19, 0x00, 0x19, 0xE7, 0x00, 0x19,
    0xCE, 0x00, 0xE7, 0x00, 0x00, 0xE7, 0x00, 0x00, 0x19, 0xCE, 0x00, 0x19, 0x00, 0x00, 0xCE, 0x32, 0x00, 0xCE, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xE7, 0x00, 0xCE, 0x19, 0x00, 0xCE, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00, 0xCE, 0x00, 0xCE, 0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0xCE, 0x32, 0x00, 0xCE, 0x00, 0x00, 0x00, 0xE7, 0x00, 0x00, 0x19, 0x00, 0xCE, 0x19, 0x00, 0xCE, 0xE7,
    0x00, 0x00, 0xCE, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00, 0x00, 0xCE, 0xCE, 0x00, 0x19, 0x00, 0x00, 0x19, 0x32, 0x00, 0xE7, 0x32, 0x00, 0xE7, 0x00,
    0x00, 0x19, 0xE7, 0x00, 0x19, 0x19, 0x00, 0xE7, 0x19, 0x00, 0xE7, 0xE7, 0x00, 0x19, 0xCE, 0x00, 0x19, 0x00, 0x00, 0xE7, 0x00, 0x00, 0xE7, 0xCE,
    0x00, 0x32, 0x00, 0x00, 0x32, 0x32, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x32, 0xE7, 0x00, 0x32, 0x19, 0x00, 0x00, 0x19, 0x00, 0x00, 0xE7,
    0x00, 0x32, 0xCE, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCE,
};

static u8 jpa_crd[32] ATTRIBUTE_ALIGN(32) = {
    0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x00, 0x02, 0x01, 0x00, 0x01,
    0x00, 0x00, 0x01, 0x00, 0x01, 0x02, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x02, 0x02, 0x00, 0x02,
};

void JPAResource::init(JKRHeap* heap) {
    u32 is_glbl_clr_anm = mpBaseShape->isGlblClrAnm();
    u32 is_glbl_tex_anm = mpBaseShape->isGlblTexAnm();
    BOOL is_prm_anm = mpBaseShape->isPrmAnm();
    BOOL is_env_anm = mpBaseShape->isEnvAnm();
    BOOL is_tex_anm = mpBaseShape->isTexAnm();
    BOOL is_tex_crd_anm = mpBaseShape->isTexCrdAnm();
    BOOL is_prj_tex = mpBaseShape->isPrjTex();
    BOOL is_enable_scale_anm = mpExtraShape != NULL && mpExtraShape->isEnableScaleAnm();
    BOOL is_enable_alpha_anm = mpExtraShape != NULL && mpExtraShape->isEnableAlphaAnm();
    BOOL is_enable_alpha_flick = mpExtraShape != NULL && mpExtraShape->isEnableAlphaAnm() && mpExtraShape->isEnableAlphaFlick();
    BOOL is_enable_rotate_anm = mpExtraShape != NULL && mpExtraShape->isEnableRotateAnm();
    BOOL is_rotate_on = is_enable_rotate_anm || (mpChildShape != NULL && mpChildShape->isRotateOn());
    BOOL base_type_5_6 = mpBaseShape->getType() == 5 || mpBaseShape->getType() == 6;
    BOOL base_type_0 = mpBaseShape->getType() == 0;
    BOOL base_type_0_1 = mpBaseShape->getType() == 0 || mpBaseShape->getType() == 1;
    BOOL child_type_5_6 = mpChildShape != NULL && (mpChildShape->getType() == 5 || mpChildShape->getType() == 6);
    BOOL child_type_0 = mpChildShape != NULL && mpChildShape->getType() == 0;
    BOOL child_type_0_1 = mpChildShape != NULL && (mpChildShape->getType() == 0 || mpChildShape->getType() == 1);
    BOOL is_draw_parent = !mpBaseShape->isNoDrawParent();
    BOOL is_draw_child = !mpBaseShape->isNoDrawChild();

    if (is_glbl_tex_anm && is_tex_anm) {
        mpCalcEmitterFuncListNum++;
    }

    if (is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcEmitterFuncListNum++;
        }
        if (is_env_anm) {
            mpCalcEmitterFuncListNum++;
        }
        if (is_prm_anm || is_env_anm) {
            mpCalcEmitterFuncListNum++;
        }
    }

    if (mpCalcEmitterFuncListNum != 0) {
        mpCalcEmitterFuncList = (EmitterFunc*)JKRAllocFromHeap(heap, mpCalcEmitterFuncListNum * sizeof(void*), sizeof(void*));
    }

    int func_no = 0;

    if (is_glbl_tex_anm && is_tex_anm) {
        switch (mpBaseShape->getTexAnmType()) {
        case 0:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxNormal;
            break;
        case 1:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxRepeat;
            break;
        case 2:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxReverse;
            break;
        case 3:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxMerge;
            break;
        case 4:
            mpCalcEmitterFuncList[func_no] = &JPACalcTexIdxRandom;
            break;
        }
        func_no++;
    }

    if (is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcEmitterFuncList[func_no] = &JPACalcPrm;
            func_no++;
        }
        if (is_env_anm) {
            mpCalcEmitterFuncList[func_no] = &JPACalcEnv;
            func_no++;
        }
        if (is_prm_anm || is_env_anm) {
            switch (mpBaseShape->getClrAnmType()) {
            case 0:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxNormal;
                break;
            case 1:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxRepeat;
                break;
            case 2:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxReverse;
                break;
            case 3:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxMerge;
                break;
            case 4:
                mpCalcEmitterFuncList[func_no] = &JPACalcClrIdxRandom;
                break;
            }
        }
    }

    if (!is_glbl_tex_anm && is_tex_anm) {
        mpCalcParticleFuncListNum++;
    }

    if (!base_type_5_6 && (is_enable_alpha_anm || is_enable_alpha_flick)) {
        mpCalcParticleFuncListNum++;
    }

    if (!is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcParticleFuncListNum++;
        }
        if (is_env_anm) {
            mpCalcParticleFuncListNum++;
        }
        if (is_prm_anm || is_env_anm) {
            mpCalcParticleFuncListNum++;
        }
    } else {
        mpCalcParticleFuncListNum++;
    }

    if (is_enable_scale_anm) {
        if (mpBaseShape->getType() != 0) {
            if (mpExtraShape->isScaleXYDiff()) {
                if (mpExtraShape->getScaleAnmTypeX() == 0 && mpExtraShape->getScaleAnmTypeY() == 0) {
                    mpCalcParticleFuncListNum++;
                } else {
                    mpCalcParticleFuncListNum++;
                    mpCalcParticleFuncListNum++;
                }
            } else {
                mpCalcParticleFuncListNum++;
            }
        }
        mpCalcParticleFuncListNum++;
        mpCalcParticleFuncListNum++;
    }

    if (mpCalcParticleFuncListNum != 0) {
        mpCalcParticleFuncList = (ParticleFunc*)JKRAllocFromHeap(heap, mpCalcParticleFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (!is_glbl_tex_anm && is_tex_anm) {
        switch (mpBaseShape->getTexAnmType()) {
        case 0:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxNormal;
            break;
        case 1:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxRepeat;
            break;
        case 2:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxReverse;
            break;
        case 3:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxMerge;
            break;
        case 4:
            mpCalcParticleFuncList[func_no] = &JPACalcTexIdxRandom;
            break;
        }
        func_no++;
    }

    if (!base_type_5_6 && (is_enable_alpha_anm || is_enable_alpha_flick)) {
        if (is_enable_alpha_flick) {
            mpCalcParticleFuncList[func_no] = &JPACalcAlphaFlickAnm;
            func_no++;
        } else {
            mpCalcParticleFuncList[func_no] = &JPACalcAlphaAnm;
            func_no++;
        }
    }

    if (!is_glbl_clr_anm) {
        if (is_prm_anm) {
            mpCalcParticleFuncList[func_no] = &JPACalcPrm;
            func_no++;
        }
        if (is_env_anm) {
            mpCalcParticleFuncList[func_no] = &JPACalcEnv;
            func_no++;
        }
        if (is_prm_anm || is_env_anm) {
            switch (mpBaseShape->getClrAnmType()) {
            case 0:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxNormal;
                break;
            case 1:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxRepeat;
                break;
            case 2:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxReverse;
                break;
            case 3:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxMerge;
                break;
            case 4:
                mpCalcParticleFuncList[func_no] = &JPACalcClrIdxRandom;
                break;
            }
            func_no++;
        }
    } else {
        mpCalcParticleFuncList[func_no] = &JPACalcColorCopy;
        func_no++;
    }

    if (is_enable_scale_anm) {
        if (mpBaseShape->getType() != 0) {
            if (mpExtraShape->isScaleXYDiff()) {
                mpCalcParticleFuncList[func_no] = &JPACalcScaleY;
                func_no++;
                if (mpExtraShape->getScaleAnmTypeY() != 0 || mpExtraShape->getScaleAnmTypeX() != 0) {
                    switch (mpExtraShape->getScaleAnmTypeY()) {
                    case 0:
                        mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmNormal;
                        break;
                    case 1:
                        mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmRepeatY;
                        break;
                    case 2:
                        mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmReverseY;
                        break;
                    }
                    func_no++;
                }
            } else {
                mpCalcParticleFuncList[func_no] = &JPACalcScaleCopy;
                func_no++;
            }
        }
        mpCalcParticleFuncList[func_no] = &JPACalcScaleX;
        func_no++;
        switch (mpExtraShape->getScaleAnmTypeX()) {
        case 0:
            mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmNormal;
            break;
        case 1:
            mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmRepeatX;
            break;
        case 2:
            mpCalcParticleFuncList[func_no] = &JPACalcScaleAnmReverseX;
            break;
        }
    }

    if (mpChildShape != NULL && mpChildShape->isScaleOutOn()) {
        mpCalcParticleChildFuncListNum++;
    }

    if (mpChildShape != NULL && mpChildShape->isAlphaOutOn()) {
        mpCalcParticleChildFuncListNum++;
    }

    if (mpCalcParticleChildFuncListNum != 0) {
        mpCalcParticleChildFuncList = (ParticleFunc*)JKRAllocFromHeap(heap, mpCalcParticleChildFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (mpChildShape != NULL && mpChildShape->isScaleOutOn()) {
        mpCalcParticleChildFuncList[func_no] = &JPACalcChildScaleOut;
        func_no++;
    }

    if (mpChildShape != NULL && mpChildShape->isAlphaOutOn()) {
        mpCalcParticleChildFuncList[func_no] = &JPACalcChildAlphaOut;
    }

    if (is_draw_parent && base_type_5_6) {
        mpDrawEmitterFuncListNum++;
    }

    mpDrawEmitterFuncListNum++;

    if (mpExTexShape != NULL) {
        mpDrawEmitterFuncListNum++;
    }

    if (is_glbl_tex_anm || !is_tex_anm) {
        mpDrawEmitterFuncListNum++;
    }

    mpDrawEmitterFuncListNum++;

    if (base_type_0_1) {
        mpDrawEmitterFuncListNum++;
    }

    if (base_type_0_1 && !is_enable_scale_anm) {
        mpDrawEmitterFuncListNum++;
    }

    if (is_glbl_clr_anm || (!is_prm_anm && !is_enable_alpha_anm) || !is_env_anm) {
        mpDrawEmitterFuncListNum++;
    }

    if (mpDrawEmitterFuncListNum != 0) {
        mpDrawEmitterFuncList = (EmitterFunc*)JKRAllocFromHeap(heap, mpDrawEmitterFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (is_draw_parent && base_type_5_6) {
        if (mpBaseShape->getType() == 5) {
            mpDrawEmitterFuncList[func_no] = &JPADrawStripe;
            func_no++;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPADrawStripeX;
            func_no++;
        }
    }

    mpDrawEmitterFuncList[func_no] = &JPADrawEmitterCallBackB;
    func_no++;

    if (mpExTexShape != NULL) {
        mpDrawEmitterFuncList[func_no] = &JPALoadExTex;
        func_no++;
    }

    if (!is_tex_anm) {
        mpDrawEmitterFuncList[func_no] = &JPALoadTex;
        func_no++;
    } else if (is_glbl_tex_anm) {
        mpDrawEmitterFuncList[func_no] = &JPALoadTexAnm;
        func_no++;
    }

    if (base_type_0_1) {
        mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxIdt;
        func_no++;
    } else if (is_prj_tex) {
        mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxPrj;
        func_no++;
    } else if (is_tex_crd_anm) {
        if (base_type_5_6) {
            mpDrawEmitterFuncList[func_no] = &JPAGenCalcTexCrdMtxAnm;
            func_no++;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxAnm;
            func_no++;
        }
    } else {
        mpDrawEmitterFuncList[func_no] = &JPAGenTexCrdMtxIdt;
        func_no++;
    }

    if (base_type_0_1) {
        mpDrawEmitterFuncList[func_no] = &JPALoadPosMtxCam;
        func_no++;
    }

    if (base_type_0_1 && !is_enable_scale_anm) {
        if (base_type_0) {
            mpDrawEmitterFuncList[func_no] = &JPASetPointSize;
            func_no++;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPASetLineWidth;
            func_no++;
        }
    }

    if (is_glbl_clr_anm) {
        if (base_type_5_6 || !is_enable_alpha_anm) {
            mpDrawEmitterFuncList[func_no] = &JPARegistPrmEnv;
        } else if (is_enable_alpha_anm) {
            mpDrawEmitterFuncList[func_no] = &JPARegistEnv;
        }
    } else if (!is_prm_anm && !is_enable_alpha_anm) {
        if (!is_env_anm) {
            mpDrawEmitterFuncList[func_no] = &JPARegistPrmEnv;
        } else {
            mpDrawEmitterFuncList[func_no] = &JPARegistPrm;
        }
    } else if (!is_env_anm) {
        mpDrawEmitterFuncList[func_no] = &JPARegistEnv;
    }

    if (is_draw_child && child_type_5_6) {
        mpDrawEmitterChildFuncListNum++;
    }

    mpDrawEmitterChildFuncListNum++;

    if (child_type_0_1) {
        mpDrawEmitterChildFuncListNum++;
    }

    if (mpChildShape != NULL && !mpChildShape->isAlphaOutOn() && !mpChildShape->isAlphaInherited() && !mpChildShape->isColorInherited()) {
        mpDrawEmitterChildFuncListNum++;
    }

    if (mpDrawEmitterChildFuncListNum != 0) {
        mpDrawEmitterChildFuncList = (EmitterFunc*)JKRAllocFromHeap(heap, mpDrawEmitterChildFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (is_draw_child && child_type_5_6) {
        if (mpChildShape->getType() == 5) {
            mpDrawEmitterChildFuncList[func_no] = &JPADrawStripe;
            func_no++;
        } else {
            mpDrawEmitterChildFuncList[func_no] = &JPADrawStripeX;
            func_no++;
        }
    }

    mpDrawEmitterChildFuncList[func_no] = &JPADrawEmitterCallBackB;
    func_no++;

    if (child_type_0_1) {
        mpDrawEmitterChildFuncList[func_no] = &JPALoadPosMtxCam;
        func_no++;
    }

    if (mpChildShape != NULL && !mpChildShape->isAlphaOutOn() && !mpChildShape->isAlphaInherited() && !mpChildShape->isColorInherited()) {
        mpDrawEmitterChildFuncList[func_no] = &JPARegistChildPrmEnv;
    }

    if (is_draw_parent && !base_type_5_6) {
        mpDrawParticleFuncListNum++;
    }

    mpDrawParticleFuncListNum++;

    if (!is_glbl_tex_anm && is_tex_anm) {
        mpDrawParticleFuncListNum++;
    }

    if ((base_type_0_1 && is_enable_scale_anm) || (is_tex_crd_anm && !is_prj_tex)) {
        mpDrawParticleFuncListNum++;
    }

    if ((!is_glbl_clr_anm && (is_prm_anm || is_env_anm || is_enable_alpha_anm)) || (is_glbl_clr_anm && is_enable_alpha_anm && !base_type_5_6)) {
        mpDrawParticleFuncListNum++;
    }

    if (mpDrawParticleFuncListNum != 0) {
        mpDrawParticleFuncList = (ParticleFunc*)JKRAllocFromHeap(heap, mpDrawParticleFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (is_draw_parent && !base_type_5_6) {
        switch (mpBaseShape->getType()) {
        case 2:
            if (is_enable_rotate_anm) {
                mpDrawParticleFuncList[func_no] = &JPADrawRotBillboard;
            } else {
                mpDrawParticleFuncList[func_no] = &JPADrawBillboard;
            }
            break;
        case 10:
            if (is_enable_rotate_anm) {
                mpDrawParticleFuncList[func_no] = &JPADrawRotYBillboard;
            } else {
                mpDrawParticleFuncList[func_no] = &JPADrawYBillboard;
            }
            break;
        case 3:
        case 4:
            if (is_enable_rotate_anm) {
                mpDrawParticleFuncList[func_no] = &JPADrawRotDirection;
            } else {
                mpDrawParticleFuncList[func_no] = &JPADrawDirection;
            }
            break;
        case 9:
            mpDrawParticleFuncList[func_no] = &JPADrawDBillboard;
            break;
        case 7:
        case 8:
            mpDrawParticleFuncList[func_no] = &JPADrawRotation;
            break;
        case 0:
            mpDrawParticleFuncList[func_no] = &JPADrawPoint;
            break;
        case 1:
            mpDrawParticleFuncList[func_no] = &JPADrawLine;
            break;
        }
        func_no++;
    }

    mpDrawParticleFuncList[func_no] = &JPADrawParticleCallBack;
    func_no++;

    if (!is_glbl_tex_anm && is_tex_anm) {
        mpDrawParticleFuncList[func_no] = &JPALoadTexAnm;
        func_no++;
    }

    if (base_type_0_1 && is_enable_scale_anm) {
        if (base_type_0) {
            mpDrawParticleFuncList[func_no] = &JPASetPointSize;
            func_no++;
        } else {
            mpDrawParticleFuncList[func_no] = &JPASetLineWidth;
            func_no++;
        }
    } else if (is_tex_crd_anm && !is_prj_tex) {
        mpDrawParticleFuncList[func_no] = &JPALoadCalcTexCrdMtxAnm;
        func_no++;
    }

    if (!is_glbl_clr_anm) {
        if (is_prm_anm) {
            if (is_env_anm) {
                mpDrawParticleFuncList[func_no] = &JPARegistPrmAlphaEnv;
            } else {
                mpDrawParticleFuncList[func_no] = &JPARegistPrmAlpha;
            }
        } else if (is_enable_alpha_anm) {
            if (is_env_anm) {
                mpDrawParticleFuncList[func_no] = &JPARegistAlphaEnv;
            } else {
                mpDrawParticleFuncList[func_no] = &JPARegistAlpha;
            }
        } else if (is_env_anm) {
            mpDrawParticleFuncList[func_no] = &JPARegistEnv;
        }
    } else if (is_enable_alpha_anm && !base_type_5_6) {
        mpDrawParticleFuncList[func_no] = &JPARegistAlpha;
    }

    if (is_draw_child && mpChildShape != NULL && !child_type_5_6) {
        mpDrawParticleChildFuncListNum++;
    }

    mpDrawParticleChildFuncListNum++;

    if (child_type_0_1) {
        mpDrawParticleChildFuncListNum++;
    }

    if (mpChildShape != NULL && (mpChildShape->isAlphaOutOn() || mpChildShape->isAlphaInherited() || mpChildShape->isColorInherited())) {
        mpDrawParticleChildFuncListNum++;
    }

    if (mpDrawParticleChildFuncListNum != 0) {
        mpDrawParticleChildFuncList = (ParticleFunc*)JKRAllocFromHeap(heap, mpDrawParticleChildFuncListNum * sizeof(void*), sizeof(void*));
    }

    func_no = 0;

    if (is_draw_child && mpChildShape != NULL && !child_type_5_6) {
        switch (mpChildShape->getType()) {
        case 2:
            if (is_rotate_on) {
                mpDrawParticleChildFuncList[func_no] = &JPADrawRotBillboard;
            } else {
                mpDrawParticleChildFuncList[func_no] = &JPADrawBillboard;
            }
            break;
        case 10:
            if (is_rotate_on) {
                mpDrawParticleChildFuncList[func_no] = &JPADrawRotYBillboard;
            } else {
                mpDrawParticleChildFuncList[func_no] = &JPADrawYBillboard;
            }
            break;
        case 3:
        case 4:
            if (is_rotate_on) {
                mpDrawParticleChildFuncList[func_no] = &JPADrawRotDirection;
            } else {
                mpDrawParticleChildFuncList[func_no] = &JPADrawDirection;
            }
            break;
        case 9:
            mpDrawParticleChildFuncList[func_no] = &JPADrawDBillboard;
            break;
        case 7:
        case 8:
            mpDrawParticleChildFuncList[func_no] = &JPADrawRotation;
            break;
        case 0:
            mpDrawParticleChildFuncList[func_no] = &JPADrawPoint;
            break;
        case 1:
            mpDrawParticleChildFuncList[func_no] = &JPADrawLine;
            break;
        }
        func_no++;
    }

    mpDrawParticleChildFuncList[func_no] = &JPADrawParticleCallBack;
    func_no++;

    if (child_type_0_1) {
        if (child_type_0) {
            mpDrawParticleChildFuncList[func_no] = &JPASetPointSize;
            func_no++;
        } else {
            mpDrawParticleChildFuncList[func_no] = &JPASetLineWidth;
            func_no++;
        }
    }

    if (mpChildShape != NULL && (mpChildShape->isAlphaOutOn() || mpChildShape->isAlphaInherited() || mpChildShape->isColorInherited())) {
        mpDrawParticleChildFuncList[func_no] = &JPARegistPrmAlphaEnv;
    }
}

bool JPAResource::calc(JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
    work->mpEmtr = emtr;
    work->mpRes = this;
    work->mEmitCount = 0;

    if (!emtr->processTillStartFrame()) {
        return false;
    }

    if (emtr->processTermination()) {
        return true;
    }

    if (emtr->checkStatus(2)) {
        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->execute(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
            emtr->mpEmtrCallBack->executeAfter(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
        }

    } else {
        calcKey(work);

        for (int i = mFieldBlockNum - 1; i >= 0; i--) {
            mpFieldBlocks[i]->initOpParam();
        }

        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->execute(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
        }

        calcWorkData_c(work);

        for (int i = mpCalcEmitterFuncListNum - 1; i >= 0; i--) {
            (*mpCalcEmitterFuncList[i])(work);
        }

        for (int i = mFieldBlockNum - 1; i >= 0; i--) {
            mpFieldBlocks[i]->prepare(work);
        }

        if (!emtr->checkStatus(8)) {
            mpDynamicsBlock->create(work);
        }

        if (emtr->mpEmtrCallBack != NULL) {
            emtr->mpEmtrCallBack->executeAfter(emtr);
            if (emtr->checkStatus(0x100)) {
                return true;
            }
        }

        JPANode< JPABaseParticle >* node = emtr->mAlivePtclBase.getFirst();
        JPANode< JPABaseParticle >* next;
        while (node != emtr->mAlivePtclBase.getEnd()) {
            next = node->getNext();
            if (node->getObject()->calc_p(work)) {
                emtr->mpPtclPool->push_front(emtr->mAlivePtclBase.erase(node));
            }
            node = next;
        }

        JPANode< JPABaseParticle >* child = emtr->mAlivePtclChld.getFirst();
        while (child != emtr->mAlivePtclChld.getEnd()) {
            JPANode< JPABaseParticle >* childNext = child->getNext();
            if (child->getObject()->calc_c(work)) {
                emtr->mpPtclPool->push_front(emtr->mAlivePtclChld.erase(child));
            }
            child = childNext;
        }

        emtr->mTick++;
    }

    return false;
}

void JPAResource::draw(JPAEmitterWorkData* work, JPABaseEmitter* emtr) {
    work->mpEmtr = emtr;
    work->mpRes = this;
    work->mDrawCount = 0;
    calcWorkData_d(work);
    mpBaseShape->setGX(work);
    for (s32 i = 1; i <= emtr->getDrawTimes(); i++) {
        work->mDrawCount++;
        if (mpBaseShape->isDrawPrntAhead() && mpChildShape != NULL)
            drawC(work);
        drawP(work);
        if (!mpBaseShape->isDrawPrntAhead() && mpChildShape != NULL)
            drawC(work);
    }
}

#if defined(TARGET_WIIU)
// A run is counted before GXBegin, so the FIFO receives exactly one primitive
// with a fixed count. The template TEX0 indices remain indexed throughout.
static void drawDirectionalList(JPAEmitterWorkData* work, JPAResource::ParticleFunc* funcs,
                                int funcCount, bool reverse, bool ageMatters,
                                int shapeType, bool child,
                                const signed char* template_xyz, const void* pos_array,
                                const void* tex_array) {
    typedef JPANode< JPABaseParticle > Node;
    Node* const end = work->mpAlivePtcl->getEnd();
    Node* node = reverse ? work->mpAlivePtcl->getLast() : work->mpAlivePtcl->getFirst();
    bool havePrevious = false;
    DirectionKey previous;
    while (node != end) {
        work->mpCurNode = node;
        JPABaseParticle* particle = node->getObject();
        Node* next = reverse ? node->getPrev() : node->getNext();
        noteParticle(work, particle, shapeType, child, funcs, funcCount);
        if (!jpa_batch::eligible(work, particle)) {
            for (int i = funcCount - 1; i >= 0; --i) (*funcs[i])(work, particle);
            node = next;
            havePrevious = false;
            continue;
        }
        const DirectionKey key = directionKey(particle, funcs, funcCount, ageMatters);
        if (havePrevious && !sameDirectionKey(previous, key)) sms_wiiu_jpa_split();
        previous = key;
        havePrevious = true;
        unsigned runLength = 1;
        for (Node* scan = next; scan != end; scan = reverse ? scan->getPrev() : scan->getNext()) {
            const unsigned maxRunLength = shapeType == 4 ? 8191 : 16383;
            if (runLength == maxRunLength) break; // GXBegin's vertex count is u16.
            work->mpCurNode = scan;
            JPABaseParticle* candidate = scan->getObject();
            if (!jpa_batch::eligible(work, candidate) ||
                !sameDirectionKey(key, directionKey(candidate, funcs, funcCount, ageMatters))) break;
            noteParticle(work, candidate, shapeType, child, funcs, funcCount);
            ++runLength;
        }
        work->mpCurNode = node;
        for (int i = funcCount - 1; i > 0; --i) (*funcs[i])(work, particle);
        if (key.flags & DirectionKeyTex)
            work->mpResMgr->load(work->mpRes->getTexIdx(particle->mTexAnmIdx), GX_TEXMAP0);
        Mtx identity;
        PSMTXIdentity(identity);
        GXLoadPosMtxImm(identity, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        const bool rotated = funcs[0] == &JPADrawRotDirection;
        Mtx lastMatrix;
        Node* emit = node;
        static const u8 tex_template[2][4] = {{0, 1, 2, 3}, {0, 1, 2, 3}};
        // jpa_dl_x's second quad uses POS indices 0x48-0x4B; the array stride is 3 bytes.
        const signed char* xyz_template[2] = {template_xyz, template_xyz + 0x48 * 3};
        const unsigned quadCount = shapeType == 4 ? 2 : 1;
        GXBegin(GX_QUADS, GX_VTXFMT0, runLength * quadCount * 4);
        for (unsigned i = 0; i < runLength; ++i) {
            work->mpCurNode = emit;
            jpa_batch::Vertex corners[4];
            jpa_batch::materialize(work, emit->getObject(), xyz_template[0], corners, lastMatrix, rotated);
            for (unsigned corner = 0; corner < 4; ++corner) {
                GXPosition3f32(corners[corner].x, corners[corner].y, corners[corner].z);
                GXTexCoord1x8(tex_template[0][corner]);
            }
            if (shapeType == 4) {
                for (unsigned corner = 0; corner < 4; ++corner) {
                    corners[corner] = jpa_batch::transform(lastMatrix, xyz_template[1] + corner * 3);
                    GXPosition3f32(corners[corner].x, corners[corner].y, corners[corner].z);
                    GXTexCoord1x8(tex_template[1][corner]);
                }
            }
            if (i != 0) {
                sms_wiiu_jpa_batched();
            }
            emit = reverse ? emit->getPrev() : emit->getNext();
        }
        GXEnd();
        sms_wiiu_jpa_draw();
        GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
        GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S8, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S8, 0);
        GXSetArray(GX_VA_POS, pos_array, 3);
        GXSetArray(GX_VA_TEX0, tex_array, 2);
        GXLoadPosMtxImm(work->mPosCamMtx, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        node = emit;
    }
    if (havePrevious) sms_wiiu_jpa_split(); // phase/emitter boundary
}

static GXColor billboardColor(JPAEmitterWorkData* work, JPABaseParticle* particle,
                              const BillboardBatchMode& mode) {
    GXColor color = {255, 255, 255, 255};
    JPABaseEmitter* emitter = work->mpEmtr;
    if (mode.rasterColorPrm) {
        color.r = COLOR_MULTI(particle->mPrmClr.r, emitter->mGlobalPrmClr.r);
        color.g = COLOR_MULTI(particle->mPrmClr.g, emitter->mGlobalPrmClr.g);
        color.b = COLOR_MULTI(particle->mPrmClr.b, emitter->mGlobalPrmClr.b);
    } else if (mode.rasterColorEnv) {
        color.r = COLOR_MULTI(particle->mEnvClr.r, emitter->mGlobalEnvClr.r);
        color.g = COLOR_MULTI(particle->mEnvClr.g, emitter->mGlobalEnvClr.g);
        color.b = COLOR_MULTI(particle->mEnvClr.b, emitter->mGlobalEnvClr.b);
    }
    if (mode.rasterColorAlphaPrm || mode.rasterAlphaPrm) {
        u8 alpha;
        alpha = mode.alphaFromParticlePrm ? particle->mPrmClr.a : emitter->mPrmClr.a;
        alpha = COLOR_MULTI(alpha, emitter->mGlobalPrmClr.a);
        if (mode.alphaAnimated) alpha = COLOR_MULTI(alpha, particle->mPrmColorAlphaAnm);
        color.a = alpha;
    } else if (mode.rasterColorAlphaEnv || mode.rasterAlphaEnv) {
        const u8 alpha = COLOR_MULTI(particle->mEnvClr.a, emitter->mGlobalEnvClr.a);
        color.a = alpha;
    }
    return color;
}

static void billboardMatrix(JPAEmitterWorkData* work, JPABaseParticle* particle,
                            bool rotated, Mtx matrix) {
    JGeometry::TVec3< f32 > view;
    PSMTXMultVec(work->mPosCamMtx, &particle->mPosition, &view);
    const f32 particleX = work->mGlobalPtclScl.x * particle->mParticleScaleX;
    const f32 particleY = work->mGlobalPtclScl.y * particle->mParticleScaleY;
    if (rotated) {
        const f32 sinRot = JMASSin(particle->mRotateAngle);
        const f32 cosRot = JMath::sSinCosTable.cosShort(particle->mRotateAngle);
        matrix[0][0] = cosRot * particleX;
        matrix[0][1] = -sinRot * particleY;
        matrix[1][0] = sinRot * particleX;
        matrix[1][1] = cosRot * particleY;
    } else {
        matrix[0][0] = particleX;
        matrix[0][1] = 0.0f;
        matrix[1][0] = 0.0f;
        matrix[1][1] = particleY;
    }
    matrix[0][2] = matrix[1][2] = 0.0f;
    matrix[2][0] = matrix[2][1] = 0.0f;
    matrix[2][2] = 1.0f;
    matrix[0][3] = view.x;
    matrix[1][3] = view.y;
    matrix[2][3] = view.z;
}

static GXColor billboardPrmColor(JPAEmitterWorkData* work, JPABaseParticle* particle) {
    GXColor color = particle->mPrmClr;
    JPABaseEmitter* emitter = work->mpEmtr;
    color.r = COLOR_MULTI(color.r, emitter->mGlobalPrmClr.r);
    color.g = COLOR_MULTI(color.g, emitter->mGlobalPrmClr.g);
    color.b = COLOR_MULTI(color.b, emitter->mGlobalPrmClr.b);
    color.a = COLOR_MULTI(color.a, emitter->mGlobalPrmClr.a);
    color.a = COLOR_MULTI(color.a, particle->mPrmColorAlphaAnm);
    return color;
}

static GXColor billboardEnvColor(JPAEmitterWorkData* work, JPABaseParticle* particle) {
    GXColor color = particle->mEnvClr;
    JPABaseEmitter* emitter = work->mpEmtr;
    color.r = COLOR_MULTI(color.r, emitter->mGlobalEnvClr.r);
    color.g = COLOR_MULTI(color.g, emitter->mGlobalEnvClr.g);
    color.b = COLOR_MULTI(color.b, emitter->mGlobalEnvClr.b);
    color.a = COLOR_MULTI(color.a, emitter->mGlobalEnvClr.a);
    return color;
}

static void restoreBillboardParticleTev(JPAEmitterWorkData* work, JPABaseParticle* particle,
                                        JPAResource::ParticleFunc* funcs, int funcCount) {
    const JPAResource::ParticleFunc registEnv = static_cast<JPAResource::ParticleFunc>(&JPARegistEnv);
    const JPAResource::ParticleFunc registAlphaEnv = static_cast<JPAResource::ParticleFunc>(&JPARegistAlphaEnv);
    bool prmWritten = false;
    bool envWritten = false;
    for (int i = funcCount - 1; i > 0; --i) {
        const JPAResource::ParticleFunc func = funcs[i];
        if (func == &JPARegistPrmAlphaEnv) {
            if (!prmWritten) {
                GXSetTevColor(GX_TEVREG0, billboardPrmColor(work, particle));
                prmWritten = true;
            }
            if (!envWritten) {
                GXSetTevColor(GX_TEVREG1, billboardEnvColor(work, particle));
                envWritten = true;
            }
        } else if (func == &JPARegistPrmAlpha) {
            if (!prmWritten) {
                GXSetTevColor(GX_TEVREG0, billboardPrmColor(work, particle));
                prmWritten = true;
            }
        } else if (func == registAlphaEnv) {
            if (!prmWritten) {
                GXColor prm = work->mpEmtr->mPrmClr;
                prm.r = COLOR_MULTI(prm.r, work->mpEmtr->mGlobalPrmClr.r);
                prm.g = COLOR_MULTI(prm.g, work->mpEmtr->mGlobalPrmClr.g);
                prm.b = COLOR_MULTI(prm.b, work->mpEmtr->mGlobalPrmClr.b);
                prm.a = COLOR_MULTI(prm.a, work->mpEmtr->mGlobalPrmClr.a);
                prm.a = COLOR_MULTI(prm.a, particle->mPrmColorAlphaAnm);
                GXSetTevColor(GX_TEVREG0, prm);
                prmWritten = true;
            }
            if (!envWritten) {
                GXSetTevColor(GX_TEVREG1, billboardEnvColor(work, particle));
                envWritten = true;
            }
        } else if (func == &JPARegistAlpha) {
            if (!prmWritten) {
                GXColor prm = work->mpEmtr->mPrmClr;
                prm.r = COLOR_MULTI(prm.r, work->mpEmtr->mGlobalPrmClr.r);
                prm.g = COLOR_MULTI(prm.g, work->mpEmtr->mGlobalPrmClr.g);
                prm.b = COLOR_MULTI(prm.b, work->mpEmtr->mGlobalPrmClr.b);
                prm.a = COLOR_MULTI(prm.a, work->mpEmtr->mGlobalPrmClr.a);
                prm.a = COLOR_MULTI(prm.a, particle->mPrmColorAlphaAnm);
                GXSetTevColor(GX_TEVREG0, prm);
                prmWritten = true;
            }
        } else if (func == registEnv) {
            if (!envWritten) {
                GXSetTevColor(GX_TEVREG1, billboardEnvColor(work, particle));
                envWritten = true;
            }
        }
        if (prmWritten && envWritten) break;
    }
}

static void setBillboardBatchTev(JPAEmitterWorkData* work, const BillboardBatchMode& mode,
                                 GXTexMapID texMap) {
    if (!mode.rasterColorPrm && !mode.rasterColorEnv &&
        !mode.rasterColorAlphaPrm && !mode.rasterColorAlphaEnv &&
        !mode.rasterAlphaPrm && !mode.rasterAlphaEnv)
        return;
    const GXTevColorArg* sourceColor = work->mpRes->getBsp()->getTevColorArg();
    const GXTevAlphaArg* sourceAlpha = work->mpRes->getBsp()->getTevAlphaArg();
    if (mode.twoColor) {
        GXTevColorArg color[4] = {sourceColor[0], sourceColor[1], sourceColor[2], sourceColor[3]};
        GXTevAlphaArg alpha[4] = {sourceAlpha[0], sourceAlpha[1], sourceAlpha[2], sourceAlpha[3]};
        for (int i = 0; i < 4; ++i) {
            if (color[i] == GX_CC_C0) color[i] = GX_CC_RASC;
            if (color[i] == GX_CC_A0) color[i] = GX_CC_RASA;
            if (alpha[i] == GX_CA_A0) alpha[i] = GX_CA_RASA;
        }
        GXSetNumChans(2);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR1A1);
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG1);
        GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG1);
        GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, texMap, GX_COLOR0A0);
        GXSetTevColorIn(GX_TEVSTAGE1, color[0], color[1], color[2], color[3]);
        GXSetTevAlphaIn(GX_TEVSTAGE1, alpha[0], alpha[1], alpha[2], alpha[3]);
        GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevDirect(GX_TEVSTAGE0);
        GXSetTevDirect(GX_TEVSTAGE1);
        GXSetNumTevStages(2);
        GXSetNumIndStages(0);
        return;
    }
    GXTevColorArg color[4] = {sourceColor[0], sourceColor[1], sourceColor[2], sourceColor[3]};
    for (int i = 0; i < 4; ++i) {
        if (mode.rasterColorPrm && color[i] == GX_CC_C0) color[i] = GX_CC_RASC;
        if (mode.rasterColorEnv && color[i] == GX_CC_C1) color[i] = GX_CC_RASC;
        if (mode.rasterColorAlphaPrm && color[i] == GX_CC_A0) color[i] = GX_CC_RASA;
        if (mode.rasterColorAlphaEnv && color[i] == GX_CC_A1) color[i] = GX_CC_RASA;
    }
    GXTevAlphaArg alpha[4] = {sourceAlpha[0], sourceAlpha[1], sourceAlpha[2], sourceAlpha[3]};
    for (int i = 0; i < 4; ++i) {
        if (mode.rasterAlphaPrm && alpha[i] == GX_CA_A0) alpha[i] = GX_CA_RASA;
        if (mode.rasterAlphaEnv && alpha[i] == GX_CA_A1) alpha[i] = GX_CA_RASA;
    }
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, texMap, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, color[0], color[1], color[2], color[3]);
    GXSetTevAlphaIn(GX_TEVSTAGE0, alpha[0], alpha[1], alpha[2], alpha[3]);
}

static void restoreBillboardBatchTev(JPAEmitterWorkData* work, const BillboardBatchMode& mode,
                                     GXTexMapID texMap) {
    if (!mode.rasterColorPrm && !mode.rasterColorEnv &&
        !mode.rasterColorAlphaPrm && !mode.rasterColorAlphaEnv &&
        !mode.rasterAlphaPrm && !mode.rasterAlphaEnv)
        return;
    const GXTevColorArg* color = work->mpRes->getBsp()->getTevColorArg();
    const GXTevAlphaArg* alpha = work->mpRes->getBsp()->getTevAlphaArg();
    if (mode.twoColor) {
        GXSetNumChans(0);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetTevColorIn(GX_TEVSTAGE0, color[0], color[1], color[2], color[3]);
        GXSetTevAlphaIn(GX_TEVSTAGE0, alpha[0], alpha[1], alpha[2], alpha[3]);
        GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevDirect(GX_TEVSTAGE0);
        GXSetTevDirect(GX_TEVSTAGE1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, texMap, GX_COLOR_NULL);
        GXSetNumTevStages(1);
        GXSetNumIndStages(0);
        return;
    }
    GXSetTevColorIn(GX_TEVSTAGE0, color[0], color[1], color[2], color[3]);
    GXSetTevAlphaIn(GX_TEVSTAGE0, alpha[0], alpha[1], alpha[2], alpha[3]);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, texMap, GX_COLOR_NULL);
}

static void drawBillboardList(JPAEmitterWorkData* work, JPAResource::ParticleFunc* funcs,
                              int funcCount, bool reverse, bool child,
                              const BillboardBatchMode& mode, const signed char* template_xyz,
                              const void* pos_array, const void* tex_array, GXTexMapID texMap) {
    typedef JPANode< JPABaseParticle > Node;
    GXAttrType savedPosDesc, savedTexDesc, savedClr0Desc, savedClr1Desc;
    GXCompCnt savedPosCnt, savedTexCnt, savedClr0Cnt, savedClr1Cnt;
    GXCompType savedPosType, savedTexType, savedClr0Type, savedClr1Type;
    u8 savedPosFrac, savedTexFrac, savedClr0Frac, savedClr1Frac;
    GXGetVtxDesc(GX_VA_POS, &savedPosDesc);
    GXGetVtxDesc(GX_VA_TEX0, &savedTexDesc);
    GXGetVtxDesc(GX_VA_CLR0, &savedClr0Desc);
    GXGetVtxDesc(GX_VA_CLR1, &savedClr1Desc);
    GXGetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, &savedPosCnt, &savedPosType, &savedPosFrac);
    GXGetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, &savedTexCnt, &savedTexType, &savedTexFrac);
    GXGetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, &savedClr0Cnt, &savedClr0Type, &savedClr0Frac);
    GXGetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, &savedClr1Cnt, &savedClr1Type, &savedClr1Frac);
    Node* const end = work->mpAlivePtcl->getEnd();
    Node* node = reverse ? work->mpAlivePtcl->getLast() : work->mpAlivePtcl->getFirst();
    while (node != end) {
        Node* runEnd = node;
        const u8 texture = node->getObject()->mTexAnmIdx;
        unsigned visible = 0;
        JPABaseParticle* lastParticle = NULL;
        JPABaseParticle* lastVisibleParticle = NULL;
        while (runEnd != end) {
            JPABaseParticle* particle = runEnd->getObject();
            if (runEnd != node && mode.textureAnimated && particle->mTexAnmIdx != texture)
                break;
            lastParticle = particle;
            work->mpCurNode = runEnd;
            noteParticle(work, particle, 2, child, funcs, funcCount);
            if (!particle->checkStatus(8)) {
                ++visible;
                lastVisibleParticle = particle;
            }
            runEnd = reverse ? runEnd->getPrev() : runEnd->getNext();
        }

        if (visible != 0) {
            if (mode.textureAnimated)
                work->mpResMgr->load(work->mpRes->getTexIdx(texture), texMap);

            Mtx identity;
            PSMTXIdentity(identity);
            GXLoadPosMtxImm(identity, GX_PNMTX0);
            GXSetCurrentMtx(GX_PNMTX0);
            GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
            GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
            if (mode.rasterColorPrm || mode.rasterColorEnv ||
                mode.rasterColorAlphaPrm || mode.rasterColorAlphaEnv ||
                mode.rasterAlphaPrm || mode.rasterAlphaEnv) {
                GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
                GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
            }
            if (mode.twoColor) {
                GXSetVtxDesc(GX_VA_CLR1, GX_DIRECT);
                GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
            }
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S8, 0);
            setBillboardBatchTev(work, mode, texMap);
            GXBegin(GX_QUADS, GX_VTXFMT0, visible * 4);
            unsigned emitted = 0;
            for (Node* scan = node; scan != runEnd;
                 scan = reverse ? scan->getPrev() : scan->getNext()) {
                JPABaseParticle* particle = scan->getObject();
                if (particle->checkStatus(8)) continue;
                Mtx matrix;
                billboardMatrix(work, particle, funcs[0] == &JPADrawRotBillboard, matrix);
                for (unsigned corner = 0; corner < 4; ++corner) {
                    const jpa_batch::Vertex vertex =
                        jpa_batch::transform(matrix, template_xyz + corner * 3);
                    GXPosition3f32(vertex.x, vertex.y, vertex.z);
                    if (mode.twoColor) {
                        const GXColor prm = billboardPrmColor(work, particle);
                        const GXColor env = billboardEnvColor(work, particle);
                        GXColor4u8(prm.r, prm.g, prm.b, prm.a);
                        GXColor4u8(env.r, env.g, env.b, env.a);
                    } else if (mode.rasterColorPrm || mode.rasterColorEnv ||
                        mode.rasterColorAlphaPrm || mode.rasterColorAlphaEnv ||
                        mode.rasterAlphaPrm || mode.rasterAlphaEnv) {
                        const GXColor color = billboardColor(work, particle, mode);
                        GXColor4u8(color.r, color.g, color.b, color.a);
                    }
                    GXTexCoord1x8(corner);
                }
                if (emitted++ != 0) sms_wiiu_jpa_batched();
            }
            GXEnd();
            sms_wiiu_jpa_draw();
            GXSetVtxDesc(GX_VA_POS, savedPosDesc);
            GXSetVtxDesc(GX_VA_TEX0, savedTexDesc);
            GXSetVtxDesc(GX_VA_CLR0, savedClr0Desc);
            GXSetVtxDesc(GX_VA_CLR1, savedClr1Desc);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, savedPosCnt, savedPosType, savedPosFrac);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, savedTexCnt, savedTexType, savedTexFrac);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, savedClr0Cnt, savedClr0Type, savedClr0Frac);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, savedClr1Cnt, savedClr1Type, savedClr1Frac);
            GXSetArray(GX_VA_POS, pos_array, 3);
            GXSetArray(GX_VA_TEX0, tex_array, 2);
            restoreBillboardBatchTev(work, mode, texMap);
            if (lastVisibleParticle != NULL) {
                Mtx matrix;
                billboardMatrix(work, lastVisibleParticle, funcs[0] == &JPADrawRotBillboard, matrix);
                GXLoadPosMtxImm(matrix, GX_PNMTX0);
            }
        }
        if (lastParticle != NULL) restoreBillboardParticleTev(work, lastParticle, funcs, funcCount);
        node = runEnd;
    }
}
#endif

void JPAResource::drawP(JPAEmitterWorkData* work) {
    work->mpEmtr->clearStatus(0x80);

    work->mGlobalPtclScl.x = work->mpEmtr->mGlobalPScl.x * mpBaseShape->getBaseSizeX();
    work->mGlobalPtclScl.y = work->mpEmtr->mGlobalPScl.y * mpBaseShape->getBaseSizeY();

    if (mpBaseShape->getType() == 0) {
        work->mGlobalPtclScl.x *= 1.02f;
    } else if (mpBaseShape->getType() == 1) {
        work->mGlobalPtclScl.x *= 1.02f;
        work->mGlobalPtclScl.y *= 0.4f;
    }

    if (mpExtraShape != NULL && mpExtraShape->isEnableScaleAnm()) {
        work->mPivot.x = mpExtraShape->getScaleCenterX() - 1.0f;
        work->mPivot.y = mpExtraShape->getScaleCenterY() - 1.0f;
    } else {
        work->mPivot.x = work->mPivot.y = 0.0f;
    }

    work->mDirType = mpBaseShape->getDirType();
    work->mRotType = mpBaseShape->getRotType();
    work->mDLType = mpBaseShape->getType() == 4 || mpBaseShape->getType() == 8;
    work->mPlaneType = work->mDLType ? 2 : mpBaseShape->getBasePlaneType();
    work->mPrjType = mpBaseShape->isPrjTex() ? (mpBaseShape->isTexCrdAnm() ? 2 : 1) : 0;

    work->mpAlivePtcl = &work->mpEmtr->mAlivePtclBase;
    setPTev();

    for (int i = mpDrawEmitterFuncListNum - 1; i >= 0; i--) {
        (*mpDrawEmitterFuncList[i])(work);
    }

#if defined(TARGET_WIIU)
    DirectionTracker directionTracker;
    const int shapeType = mpBaseShape->getType();
    BillboardBatchMode billboardMode;
    const bool directionalFunc = mpDrawParticleFuncListNum > 0 &&
        (mpDrawParticleFuncList[0] == &JPADrawDirection || mpDrawParticleFuncList[0] == &JPADrawRotDirection);
    const bool directional = (shapeType == 3 || shapeType == 4) && directionalFunc;
    if (canBatchBillboard(work, mpDrawParticleFuncList, mpDrawParticleFuncListNum, shapeType, false,
                          &billboardMode)) {
        const int center = mpExtraShape != NULL
            ? (mpExtraShape->getScaleCenterX() + 3 * mpExtraShape->getScaleCenterY()) * 0xC : 0x30;
        drawBillboardList(work, mpDrawParticleFuncList, mpDrawParticleFuncListNum,
                          mpBaseShape->isDrawFwdAhead(), false, billboardMode,
                          reinterpret_cast<const signed char*>(jpa_pos + center),
                          jpa_pos + center, jpa_crd + (mpBaseShape->getTilingS() + 2 * mpBaseShape->getTilingT()) * 8,
                          GX_TEXMAP0);
    } else if (canBatchDirectional(work, mpDrawParticleFuncList, mpDrawParticleFuncListNum, shapeType, false)) {
        const int center = mpExtraShape != NULL
            ? (mpExtraShape->getScaleCenterX() + 3 * mpExtraShape->getScaleCenterY()) * 0xC : 0x30;
        const int offset = shapeType == 3 ? center + mpBaseShape->getBasePlaneType() * 0x6C : center;
        const int crd_offset = (mpBaseShape->getTilingS() + 2 * mpBaseShape->getTilingT()) * 8;
        drawDirectionalList(work, mpDrawParticleFuncList, mpDrawParticleFuncListNum,
                            mpBaseShape->isDrawFwdAhead(), mpBaseShape->isTexCrdAnm() && !mpBaseShape->isPrjTex(),
                            shapeType, false,
                            reinterpret_cast<const signed char*>(jpa_pos + offset),
                            jpa_pos + offset, jpa_crd + crd_offset);
    } else
#endif
    if (mpBaseShape->isDrawFwdAhead()) {
        JPANode< JPABaseParticle >* node = work->mpEmtr->mAlivePtclBase.getLast();
        for (; node != work->mpEmtr->mAlivePtclBase.getEnd(); node = node->getPrev()) {
            work->mpCurNode = node;
#if defined(TARGET_WIIU)
            noteParticle(work, node->getObject(), shapeType, false, mpDrawParticleFuncList, mpDrawParticleFuncListNum);
            directionTracker.observe(work, node->getObject(), directional,
                                     mpBaseShape->isTexCrdAnm() && !mpBaseShape->isPrjTex(),
                                     mpDrawParticleFuncList, mpDrawParticleFuncListNum);
#endif
            if (mpDrawParticleFuncList != NULL) {
                for (int i = mpDrawParticleFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleFuncList[i])(work, node->getObject());
                }
            }
        }
    } else {
        JPANode< JPABaseParticle >* node = work->mpEmtr->mAlivePtclBase.getFirst();
        for (; node != work->mpEmtr->mAlivePtclBase.getEnd(); node = node->getNext()) {
            work->mpCurNode = node;
#if defined(TARGET_WIIU)
            noteParticle(work, node->getObject(), shapeType, false, mpDrawParticleFuncList, mpDrawParticleFuncListNum);
            directionTracker.observe(work, node->getObject(), directional,
                                     mpBaseShape->isTexCrdAnm() && !mpBaseShape->isPrjTex(),
                                     mpDrawParticleFuncList, mpDrawParticleFuncListNum);
#endif
            if (mpDrawParticleFuncList != NULL) {
                for (int i = mpDrawParticleFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleFuncList[i])(work, node->getObject());
                }
            }
        }
    }

#if defined(TARGET_WIIU)
    if (directionTracker.have) sms_wiiu_jpa_split();
#endif

    GXSetMisc(GX_MT_XF_FLUSH, 0);

    if (work->mpEmtr->mpEmtrCallBack != NULL) {
        work->mpEmtr->mpEmtrCallBack->drawAfter(work->mpEmtr);
    }
}

void JPAResource::drawC(JPAEmitterWorkData* work) {
    work->mpEmtr->setStatus(0x80);

    if (mpChildShape->isScaleInherited()) {
        work->mGlobalPtclScl.x = work->mpEmtr->mGlobalPScl.x * mpBaseShape->getBaseSizeX();
        work->mGlobalPtclScl.y = work->mpEmtr->mGlobalPScl.y * mpBaseShape->getBaseSizeY();
    } else {
        work->mGlobalPtclScl.x = work->mpEmtr->mGlobalPScl.x * mpChildShape->getScaleX();
        work->mGlobalPtclScl.y = work->mpEmtr->mGlobalPScl.y * mpChildShape->getScaleY();
    }

    if (mpChildShape->getType() == 0) {
        work->mGlobalPtclScl.x *= 1.02f;
    } else if (mpChildShape->getType() == 1) {
        work->mGlobalPtclScl.x *= 1.02f;
        work->mGlobalPtclScl.y *= 0.4f;
    }

    work->mPivot.x = work->mPivot.y = 0.0f;

    work->mDirType = mpChildShape->getDirType();
    work->mRotType = mpChildShape->getRotType();
    work->mDLType = mpChildShape->getType() == 4 || mpChildShape->getType() == 8;
    work->mPlaneType = work->mDLType ? 2 : mpChildShape->getBasePlaneType();
    work->mPrjType = 0;

    work->mpAlivePtcl = &work->mpEmtr->mAlivePtclChld;
    setCTev(work);

    for (int i = mpDrawEmitterChildFuncListNum - 1; i >= 0; i--) {
        (*mpDrawEmitterChildFuncList[i])(work);
    }

#if defined(TARGET_WIIU)
    DirectionTracker directionTracker;
    const int shapeType = mpChildShape->getType();
    BillboardBatchMode billboardMode;
    const bool directionalFunc = mpDrawParticleChildFuncListNum > 0 &&
        (mpDrawParticleChildFuncList[0] == &JPADrawDirection || mpDrawParticleChildFuncList[0] == &JPADrawRotDirection);
    const bool directional = (shapeType == 3 || shapeType == 4) && directionalFunc;
    if (canBatchBillboard(work, mpDrawParticleChildFuncList, mpDrawParticleChildFuncListNum, shapeType, true,
                          &billboardMode)) {
        drawBillboardList(work, mpDrawParticleChildFuncList, mpDrawParticleChildFuncListNum,
                          mpBaseShape->isDrawFwdAhead(), true, billboardMode,
                          reinterpret_cast<const signed char*>(jpa_pos + 0x30),
                          jpa_pos + 0x30, jpa_crd, GX_TEXMAP1);
    } else if (canBatchDirectional(work, mpDrawParticleChildFuncList, mpDrawParticleChildFuncListNum, shapeType, true)) {
        const int offset = shapeType == 3 ? 0x30 + mpChildShape->getBasePlaneType() * 0x6C : 0x30;
        drawDirectionalList(work, mpDrawParticleChildFuncList, mpDrawParticleChildFuncListNum,
                            mpBaseShape->isDrawFwdAhead(), false, shapeType,
                            true,
                            reinterpret_cast<const signed char*>(jpa_pos + offset),
                            jpa_pos + offset, jpa_crd);
    } else
#endif
    if (mpBaseShape->isDrawFwdAhead()) {
        JPANode< JPABaseParticle >* node = work->mpEmtr->mAlivePtclChld.getLast();
        for (; node != work->mpEmtr->mAlivePtclChld.getEnd(); node = node->getPrev()) {
            work->mpCurNode = node;
#if defined(TARGET_WIIU)
            noteParticle(work, node->getObject(), shapeType, true, mpDrawParticleChildFuncList, mpDrawParticleChildFuncListNum);
            directionTracker.observe(work, node->getObject(), directional, false,
                                     mpDrawParticleChildFuncList, mpDrawParticleChildFuncListNum);
#endif
            if (mpDrawParticleChildFuncList != NULL) {
                for (int i = mpDrawParticleChildFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleChildFuncList[i])(work, node->getObject());
                }
            }
        }
    } else {
        JPANode< JPABaseParticle >* node = work->mpEmtr->mAlivePtclChld.getFirst();
        for (; node != work->mpEmtr->mAlivePtclChld.getEnd(); node = node->getNext()) {
            work->mpCurNode = node;
#if defined(TARGET_WIIU)
            noteParticle(work, node->getObject(), shapeType, true, mpDrawParticleChildFuncList, mpDrawParticleChildFuncListNum);
            directionTracker.observe(work, node->getObject(), directional, false,
                                     mpDrawParticleChildFuncList, mpDrawParticleChildFuncListNum);
#endif
            if (mpDrawParticleChildFuncList != NULL) {
                for (int i = mpDrawParticleChildFuncListNum - 1; i >= 0; i--) {
                    (*mpDrawParticleChildFuncList[i])(work, node->getObject());
                }
            }
        }
    }

#if defined(TARGET_WIIU)
    if (directionTracker.have) sms_wiiu_jpa_split();
#endif

    GXSetMisc(GX_MT_XF_FLUSH, 0);

    if (work->mpEmtr->mpEmtrCallBack != NULL) {
        work->mpEmtr->mpEmtrCallBack->drawAfter(work->mpEmtr);
    }
}

void JPAResource::setPTev() {
    GXTexCoordID tex_coord = GX_TEXCOORD1;
    u8 tev_stages = 1;
    u8 tex_gens = 1;
    u8 ind_stages = 0;

    int base_plane_type = (mpBaseShape->getType() == 3 || mpBaseShape->getType() == 7) ? mpBaseShape->getBasePlaneType() : 0;
    int center_offset;
    if (mpExtraShape != NULL) {
        center_offset = (mpExtraShape->getScaleCenterX() + 3 * mpExtraShape->getScaleCenterY()) * 0xC;
    } else {
        center_offset = 0x30;
    }
    int pos_offset = center_offset + base_plane_type * 0x6C;
    int crd_offset = (mpBaseShape->getTilingS() + 2 * mpBaseShape->getTilingT()) * 8;
    GXSetArray(GX_VA_POS, jpa_pos + pos_offset, 3);
    GXSetArray(GX_VA_TEX0, jpa_crd + crd_offset, 2);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);

    if (mpExTexShape != NULL) {
        if (mpExTexShape->isUseIndirect()) {
            GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP2);
            GXSetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);
            GXSetIndTexMtx(GX_ITM_0, (f32(*)[3])mpExTexShape->getIndTexMtx(), mpExTexShape->getExpScale());
            GXSetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU, GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF, 0, 0, GX_ITBA_OFF);
            ind_stages++;
            tex_gens++;
            tex_coord = GX_TEXCOORD2;
        }
        if (mpExTexShape->isUseSecTex()) {
            GXSetTevOrder(GX_TEVSTAGE1, tex_coord, GX_TEXMAP3, GX_COLOR_NULL);
            GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO);
            GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO);
            GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            tev_stages++;
            tex_gens++;
        }
    }

    GXSetNumTevStages(tev_stages);
    GXSetNumIndStages(ind_stages);
    if (mpBaseShape->isClipOn()) {
        GXSetMisc(GX_MT_XF_FLUSH, 8);
        GXSetClipMode(GX_CLIP_ENABLE);
    } else {
        GXSetClipMode(GX_CLIP_DISABLE);
    }
    GXSetNumTexGens(tex_gens);
}

void JPAResource::setCTev(JPAEmitterWorkData* work) {
    int base_plane_type = (mpChildShape->getType() == 3 || mpChildShape->getType() == 7) ? mpChildShape->getBasePlaneType() : 0;
    int pos_offset = 0x30 + base_plane_type * 0x6C;
    GXSetArray(GX_VA_POS, jpa_pos + pos_offset, 3);
    GXSetArray(GX_VA_TEX0, jpa_crd, 2);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR_NULL);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 0x3C, GX_FALSE, 0x7D);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetNumTevStages(1);
    GXSetNumIndStages(0);
    if (mpChildShape->isClipOn()) {
        GXSetMisc(GX_MT_XF_FLUSH, 8);
        GXSetClipMode(GX_CLIP_ENABLE);
    } else {
        GXSetClipMode(GX_CLIP_DISABLE);
    }
    GXSetNumTexGens(1);
    work->mpResMgr->CALL_INLINE_FUNC(load, work->mpRes->getTexIdx(mpChildShape->getTexIdx()), GX_TEXMAP1);
}

void JPAResource::calc_p(JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
    if (mpCalcParticleFuncList != NULL) {
        for (int i = mpCalcParticleFuncListNum - 1; i >= 0; i--) {
            (*mpCalcParticleFuncList[i])(work, ptcl);
        }
    }
}

void JPAResource::calc_c(JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
    if (mpCalcParticleChildFuncList != NULL) {
        for (int i = mpCalcParticleChildFuncListNum - 1; i >= 0; i--) {
            (*mpCalcParticleChildFuncList[i])(work, ptcl);
        }
    }
}

void JPAResource::calcField(JPAEmitterWorkData* work, JPABaseParticle* ptcl) {
    for (int i = mFieldBlockNum - 1; i >= 0; i--) {
        mpFieldBlocks[i]->calc(work, ptcl);
    }
}

void JPAResource::calcKey(JPAEmitterWorkData* work) {
    for (int i = mKeyBlockNum - 1; i >= 0; i--) {
        f32 val = mpKeyBlocks[i]->calc(work->mpEmtr->mTick);
        switch (mpKeyBlocks[i]->getID()) {
        case 0:
            work->mpEmtr->mRate = val;
            break;
        case 1:
            work->mpEmtr->mVolumeSize = val;
            break;
        case 3:
            work->mpEmtr->mVolumeMinRad = val;
            break;
        case 4:
            work->mpEmtr->mLifeTime = val;
            break;
        case 6:
            work->mpEmtr->mAwayFromCenterSpeed = val;
            break;
        case 7:
            work->mpEmtr->mAwayFromAxisSpeed = val;
            break;
        case 8:
            work->mpEmtr->mDirSpeed = val;
            break;
        case 9:
            work->mpEmtr->mSpread = val;
            break;
        case 10:
            work->mpEmtr->mScaleOut = val;
            break;
        }
    }
}

void JPAResource::calcWorkData_c(JPAEmitterWorkData* work) {
    work->mVolumeSize = work->mpEmtr->mVolumeSize;
    work->mVolumeMinRad = work->mpEmtr->mVolumeMinRad;
    work->mVolumeSweep = work->mpEmtr->mVolumeSweep;
    work->mVolumeX = 0;
    work->mVolumeAngleNum = 0;
    work->mVolumeAngleMax = 1;
    work->mDivNumber = mpDynamicsBlock->getDivNumber() * 2 + 1;
    Mtx local_scl_mtx, local_rot_mtx, global_mtx;
    PSMTXScale(local_scl_mtx, work->mpEmtr->mLocalScl.x, work->mpEmtr->mLocalScl.y, work->mpEmtr->mLocalScl.z);
    JPAGetXYZRotateMtx(work->mpEmtr->mLocalRot.x * 182, work->mpEmtr->mLocalRot.y * 182, work->mpEmtr->mLocalRot.z * 182, local_rot_mtx);
    PSMTXScale(global_mtx, work->mpEmtr->mGlobalScl.x, work->mpEmtr->mGlobalScl.y, work->mpEmtr->mGlobalScl.z);
    PSMTXConcat(work->mpEmtr->mGlobalRot, global_mtx, global_mtx);
    global_mtx[0][3] = work->mpEmtr->mGlobalTrs.x;
    global_mtx[1][3] = work->mpEmtr->mGlobalTrs.y;
    global_mtx[2][3] = work->mpEmtr->mGlobalTrs.z;
    PSMTXCopy(work->mpEmtr->mGlobalRot, work->mRotationMtx);
    PSMTXConcat(work->mRotationMtx, local_rot_mtx, work->mGlobalRot);
    PSMTXConcat(work->mGlobalRot, local_scl_mtx, work->mGlobalSR);
    work->mEmitterPos.set(work->mpEmtr->mLocalTrs);
    work->mGlobalScl.mul(work->mpEmtr->mGlobalScl, work->mpEmtr->mLocalScl);
    JPAGetDirMtx(work->mpEmtr->mLocalDir, work->mDirectionMtx);
    work->mPublicScale.set(work->mpEmtr->mGlobalScl);
    PSMTXMultVec(global_mtx, &work->mpEmtr->mLocalTrs, &work->mGlobalPos);
}

void JPAResource::calcWorkData_d(JPAEmitterWorkData* work) {
    Mtx mtx;
    JPAGetXYZRotateMtx(work->mpEmtr->mLocalRot.x * 0xB6, work->mpEmtr->mLocalRot.y * 0xB6, work->mpEmtr->mLocalRot.z * 0xB6, mtx);
    PSMTXConcat(work->mpEmtr->mGlobalRot, mtx, work->mGlobalRot);
    PSMTXMultVecSR(work->mGlobalRot, &work->mpEmtr->mLocalDir, &work->mGlobalEmtrDir);
}

#include "JSystem/JParticle/JPAFieldBlockInline.hpp"
#include "JSystem/JParticle/JPAListInline.hpp"

template JPANode< JPABaseParticle >* JPAList< JPABaseParticle >::erase(JPANode< JPABaseParticle >*);
template void JPAList< JPABaseParticle >::push_front(JPANode< JPABaseParticle >*);
