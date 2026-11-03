#include "Game/LiveActor/ShadowVolumeDrawer.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#if defined(TARGET_WIIU) && defined(PORT_GAME_SMG)
extern "C" int sms_wiiu_sd_marker(const char*);
#endif

namespace {
    static Color8 sShapeColor(0xC0, 0xC0, 0, 4);
    static Color8 sDebugShapeColor(0xFF, 0, 0, 0x80);
};  // namespace

ShadowVolumeDrawInit::ShadowVolumeDrawInit() : NameObj("\x83\x56\x83\x83\x83\x68\x83\x45\x83\x7b\x83\x8a\x83\x85\x81\x5b\x83\x80\x95\x60\x89\xe6\x8f\x89\x8a\xfa\x89\xbb") {
    MR::registerPreDrawFunction(MR::Functor(&MR::setupShadowVolumeDraw), MR::DrawType_ShadowVolume);
}

ShadowVolumeDrawer::ShadowVolumeDrawer(const char* pName) : ShadowDrawer(pName) {
    mStartDrawShapeOffset = 0.0f;
    mEndDrawShapeOffset = 0.0f;
    mIsCutDropShadow = false;
    MR::createSceneObj(SceneObj_ShadowVolumeDrawInit);
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_ShadowVolume);
}

void ShadowVolumeDrawer::setStartDrawShepeOffset(f32 offs) {
    mStartDrawShapeOffset = offs;
}

void ShadowVolumeDrawer::setEndDrawShepeOffset(f32 offs) {
    mEndDrawShapeOffset = offs;
}

void ShadowVolumeDrawer::onCutDropShadow() {
    mIsCutDropShadow = true;
}

void ShadowVolumeDrawer::offCutDropShadow() {
    mIsCutDropShadow = false;
}

void ShadowVolumeDrawer::calcBaseDropPosition(TVec3f* pVec) const {
    calcBaseDropPosition(pVec, getController());
}

void ShadowVolumeDrawer::calcBaseDropPosition(TVec3f* pPosition, const ShadowController* pController) const {
    TVec3f position;
    TVec3f direction;
    pController->getDropPos(&position);
    pController->getDropDir(&direction);
    pPosition->set(position + direction * mStartDrawShapeOffset);
}

f32 ShadowVolumeDrawer::calcBaseDropLength() const {
    return calcBaseDropLength(getController());
}

f32 ShadowVolumeDrawer::calcBaseDropLength(const ShadowController* pController) const {
    f32 length = pController->getDropLength();

    if (mIsCutDropShadow) {
        if (pController->isProjected()) {
            length = pController->getProjectionLength();
        }
    }

    f32 endOffset = mEndDrawShapeOffset;
    f32 startOffset = mStartDrawShapeOffset;
    length += -startOffset + endOffset;
    return length;
}

void ShadowVolumeDrawer::loadModelDrawMtx() const {
}

void ShadowVolumeDrawer::drawShape() const {
}

bool ShadowVolumeDrawer::isDraw() const {
    return getController()->isDraw();
}

bool ShadowVolumeDrawer::isShadowVolumeBatchable() const {
    return true;
}

u32 ShadowVolumeDrawer::getShadowVolumeBatchState() const {
    return 0;
}

namespace {
#if defined(TARGET_WIIU) && defined(PORT_GAME_SMG)
    s32 sShadowVolumeBatchMarker = -1;
#endif

    void setShadowVolumePassState(bool subtract) {
        GXSetCullMode(subtract ? GX_CULL_BACK : GX_CULL_FRONT);
        GXSetBlendMode(subtract ? GX_BM_SUBTRACT : GX_BM_BLEND,
                       subtract ? GX_BL_ZERO : GX_BL_ONE,
                       subtract ? GX_BL_ZERO : GX_BL_ONE, GX_LO_COPY);
    }
}

bool ShadowVolumeDrawer::isBatchEnabled() {
#if defined(TARGET_WIIU) && defined(PORT_GAME_SMG)
    if (sShadowVolumeBatchMarker < 0) {
        sShadowVolumeBatchMarker = sms_wiiu_sd_marker("svbatch") != 0;
    }
    return sShadowVolumeBatchMarker != 0;
#else
    return false;
#endif
}

void ShadowVolumeDrawer::drawBatch(NameObj* const* pObjects, u32 count) {
#if defined(TARGET_WIIU) && defined(PORT_GAME_SMG)
    u32 batchState = 0;
    bool haveBatchState = false;
    for (u32 i = 0; i < count; i++) {
        ShadowVolumeDrawer* volume = static_cast< ShadowVolumeDrawer* >(pObjects[i]);
        if (!volume->isShadowVolumeBatchable()) {
            for (u32 j = 0; j < count; j++) {
                static_cast< ShadowVolumeDrawer* >(pObjects[j])->draw();
            }
            return;
        }

        u32 state = volume->getShadowVolumeBatchState();
        if (!haveBatchState) {
            batchState = state;
            haveBatchState = true;
        } else if (state != batchState) {
            // A state change could make the two-pass reorder observable. Keep
            // the retail object-by-object order for mixed-state extensions.
            for (u32 j = 0; j < count; j++) {
                static_cast< ShadowVolumeDrawer* >(pObjects[j])->draw();
            }
            return;
        }
    }

    GXColor shapeColor = reinterpret_cast< GXColor& >(::sShapeColor);
    GXSetTevColor(GX_TEVREG0, shapeColor);
    GXSetColorUpdate(0);
    GXSetDstAlpha(0, 0);

    setShadowVolumePassState(false);
    for (u32 i = 0; i < count; i++) {
        ShadowVolumeDrawer* volume = static_cast< ShadowVolumeDrawer* >(pObjects[i]);
        if (volume->isDraw()) {
            volume->loadModelDrawMtx();
            volume->drawShape();
        }
    }

    setShadowVolumePassState(true);
    for (u32 i = 0; i < count; i++) {
        ShadowVolumeDrawer* volume = static_cast< ShadowVolumeDrawer* >(pObjects[i]);
        if (volume->isDraw()) {
            volume->loadModelDrawMtx();
            volume->drawShape();
        }
    }

    GXSetCullMode(GX_CULL_NONE);
    GXSetColorUpdate(1);
    GXSetDstAlpha(1, 0);
#else
    for (u32 i = 0; i < count; i++) {
        static_cast< ShadowVolumeDrawer* >(pObjects[i])->draw();
    }
#endif
}

void ShadowVolumeDrawer::draw() const {
    if (isDraw()) {
        loadModelDrawMtx();
        GXColor shapeColor = reinterpret_cast< GXColor& >(::sShapeColor);
        GXSetTevColor(GX_TEVREG0, shapeColor);
        GXSetColorUpdate(0);
        GXSetDstAlpha(0, 0);
        GXSetCullMode(GX_CULL_FRONT);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_COPY);
        drawShape();
        GXSetCullMode(GX_CULL_BACK);
        GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ZERO, GX_BL_ZERO, GX_LO_COPY);
        drawShape();
        GXSetCullMode(GX_CULL_NONE);
        GXSetColorUpdate(1);
        GXSetDstAlpha(1, 0);
    }
}

ShadowVolumeDrawInit::~ShadowVolumeDrawInit() {
}
