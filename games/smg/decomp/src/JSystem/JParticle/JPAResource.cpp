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
#if defined(TARGET_WIIU)
#include "wiiu/jpa_counters.h"
extern "C" int sms_wiiu_sd_marker(const char*);
#endif

#if defined(TARGET_WIIU)
namespace {
struct DirectionKey {
    GXColor prm, env;
    u8 alpha, tex;
    s16 age;
};

DirectionKey directionKey(JPABaseParticle* particle, bool ageMatters) {
    DirectionKey key;
    key.prm = particle->mPrmClr;
    key.env = particle->mEnvClr;
    key.alpha = particle->mPrmColorAlphaAnm;
    key.tex = particle->mTexAnmIdx;
    key.age = ageMatters ? particle->mAge : 0;
    return key;
}

bool sameDirectionKey(const DirectionKey& a, const DirectionKey& b) {
    return a.prm.r == b.prm.r && a.prm.g == b.prm.g && a.prm.b == b.prm.b && a.prm.a == b.prm.a &&
           a.env.r == b.env.r && a.env.g == b.env.g && a.env.b == b.env.b && a.env.a == b.env.a &&
           a.alpha == b.alpha && a.tex == b.tex && a.age == b.age;
}

void countDirectionalParticle(JPAEmitterWorkData* work, JPABaseParticle* particle, int shapeType) {
    if (shapeType == 3) sms_wiiu_jpa_type3();
    if (shapeType == 4) sms_wiiu_jpa_type4();
    if (shapeType != 3 && shapeType != 4) {
        sms_wiiu_jpa_reject_type();
    } else if (work->mpEmtr->mpPtclCallBack != NULL) {
        sms_wiiu_jpa_reject_callback();
    } else if (work->mPrjType != 0) {
        sms_wiiu_jpa_reject_prj();
    } else if (!jpa_batch::eligible(work, particle)) {
        sms_wiiu_jpa_reject_ineligible();
    }
}

bool batchMarker() {
    static int marker = -1;
    if (marker < 0) marker = sms_wiiu_sd_marker("jpabatch") != 0;
    return marker != 0;
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
    if (directionalFunc) countDirectionalParticle(work, particle, shapeType);
    sms_wiiu_jpa_histogram(child, (unsigned)shapeType,
                           funcCount > 0 ? jpaDrawFuncName(funcs[0]) : "none",
                           work->mpEmtr->mpPtclCallBack != NULL, work->mpEmtr);
}

struct DirectionTracker {
    bool have;
    DirectionKey previous;
    DirectionTracker() : have(false) { }
    void observe(JPAEmitterWorkData* work, JPABaseParticle* particle, bool directional, bool ageMatters) {
        if (!directional || !jpa_batch::eligible(work, particle)) return;
        const DirectionKey key = directionKey(particle, ageMatters);
        if (have && !sameDirectionKey(previous, key)) sms_wiiu_jpa_split();
        previous = key;
        have = true;
    }
};
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
        const DirectionKey key = directionKey(particle, ageMatters);
        if (havePrevious && !sameDirectionKey(previous, key)) sms_wiiu_jpa_split();
        previous = key;
        havePrevious = true;
        unsigned runLength = 1;
        for (Node* scan = next; scan != end; scan = reverse ? scan->getPrev() : scan->getNext()) {
            const unsigned maxRunLength = shapeType == 4 ? 8191 : 16383;
            if (runLength == maxRunLength) break; // GXBegin's vertex count is u16.
            work->mpCurNode = scan;
            JPABaseParticle* candidate = scan->getObject();
            if (!jpa_batch::eligible(work, candidate) || !sameDirectionKey(key, directionKey(candidate, ageMatters))) break;
            noteParticle(work, candidate, shapeType, child, funcs, funcCount);
            ++runLength;
        }
        work->mpCurNode = node;
        for (int i = funcCount - 1; i > 0; --i) (*funcs[i])(work, particle);
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
    const bool directionalFunc = mpDrawParticleFuncListNum > 0 &&
        (mpDrawParticleFuncList[0] == &JPADrawDirection || mpDrawParticleFuncList[0] == &JPADrawRotDirection);
    const bool directional = (shapeType == 3 || shapeType == 4) && directionalFunc;
    if (batchMarker() && (shapeType == 3 || shapeType == 4) && work->mPrjType == 0 &&
        work->mpEmtr->mpPtclCallBack == NULL && mpDrawParticleFuncListNum > 0 &&
        (mpDrawParticleFuncList[0] == &JPADrawDirection || mpDrawParticleFuncList[0] == &JPADrawRotDirection)) {
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
            directionTracker.observe(work, node->getObject(), directional, mpBaseShape->isTexCrdAnm() && !mpBaseShape->isPrjTex());
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
            directionTracker.observe(work, node->getObject(), directional, mpBaseShape->isTexCrdAnm() && !mpBaseShape->isPrjTex());
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
    const bool directionalFunc = mpDrawParticleChildFuncListNum > 0 &&
        (mpDrawParticleChildFuncList[0] == &JPADrawDirection || mpDrawParticleChildFuncList[0] == &JPADrawRotDirection);
    const bool directional = (shapeType == 3 || shapeType == 4) && directionalFunc;
    if (batchMarker() && (shapeType == 3 || shapeType == 4) && work->mPrjType == 0 &&
        work->mpEmtr->mpPtclCallBack == NULL && mpDrawParticleChildFuncListNum > 0 &&
        (mpDrawParticleChildFuncList[0] == &JPADrawDirection || mpDrawParticleChildFuncList[0] == &JPADrawRotDirection)) {
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
            directionTracker.observe(work, node->getObject(), directional, false);
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
            directionTracker.observe(work, node->getObject(), directional, false);
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
