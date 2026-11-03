#pragma once

#include "Game/LiveActor/ShadowController.hpp"
#include "Game/LiveActor/ShadowDrawer.hpp"
#include "Game/Util/Color.hpp"

class ShadowVolumeDrawInit : public NameObj {
public:
    ShadowVolumeDrawInit();

    virtual ~ShadowVolumeDrawInit();
};

class ShadowVolumeDrawer : public ShadowDrawer {
public:
    ShadowVolumeDrawer(const char*);

    static bool isBatchEnabled();
    static void drawBatch(NameObj* const*, u32);

    virtual ~ShadowVolumeDrawer() {
    }

    virtual void draw() const override;
    virtual void loadModelDrawMtx() const;
    virtual void drawShape() const;
    virtual bool isDraw() const;
    virtual bool isShadowVolumeBatchable() const;
    virtual u32 getShadowVolumeBatchState() const;

    void setStartDrawShepeOffset(f32);
    void setEndDrawShepeOffset(f32);
    void onCutDropShadow();
    void offCutDropShadow();
    void calcBaseDropPosition(TVec3f*) const;
    void calcBaseDropPosition(TVec3f*, const ShadowController*) const;
    f32 calcBaseDropLength() const;
    f32 calcBaseDropLength(const ShadowController*) const;

    /* 0x10 */ f32 mStartDrawShapeOffset;
    /* 0x14 */ f32 mEndDrawShapeOffset;
    /* 0x18 */ bool mIsCutDropShadow;
};
