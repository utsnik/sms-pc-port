#include <JSystem/JGeometry/TVec.hpp>

#include "Game/System/WPad.hpp"
#include "Game/System/WPadHVSwing.hpp"

#if defined(TARGET_WIIU)
extern "C" uint32_t sms_wiiu_spin_frames_since_press(void);
#endif

void WPadHVSwing_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
}

WPadHVSwing::WPadHVSwing(const WPad* pPad, u32 channel) {
    this->pPad = pPad;
    mChannel = channel;
    mDistanceSwingThreshold = 1.0;
    mIsSwing = false;
    mIsSwingLatched = 0;
    mSwingLatchedFrames = 0;
    mSwingBelowThresholdFrames = 0;
    mIsTriggerSwing = 0;
    mSwingThreshold = 0.4f;
    mSwingDetected = false;
    mSwingTriggered = false;
    mSwingHoldFrames = 0;
    mSwingCooldownFrames = 0;
}

void WPadHVSwing::updateSwing() {
    TVec3f pastAccel;
    TVec3f curAccel;

    if (!pPad->getPastAcceleration(&pastAccel, 20, mChannel) || !pPad->getAcceleration(&curAccel, mChannel)) {
        mIsSwing = false;
        return;
    }

    mIsSwing = curAccel.distance(pastAccel) >= mDistanceSwingThreshold;

    if (!mIsSwing) {
        mSwingBelowThresholdFrames++;
    } else {
        mSwingBelowThresholdFrames = 0;
    }

    if (mIsSwingLatched) {
        if (mSwingBelowThresholdFrames > 6) {
            mIsSwingLatched = false;
        }
    } else if (mIsSwing) {
        mIsSwingLatched = true;
    }

    if (mIsSwingLatched) {
        mSwingLatchedFrames++;
    } else {
        mSwingLatchedFrames = 0;
    }
}

void WPadHVSwing::updateCentrifugal() {
    TVec3f pastAccel;
    TVec3f curAccel;

    if (!pPad->getPastAcceleration(&pastAccel, 20, mChannel) || !pPad->getAcceleration(&curAccel, mChannel)) {
        mSwingDetected = false;
        mSwingTriggered = false;
        mSwingHoldFrames = 0;
        mSwingCooldownFrames = 0;
    }

    float accelDeltaSum = 0.0f;
    float peakDelta = 0.0f;

    if (pPad->getEnableAccelPastCount(mChannel) >= 15) {
        for (int i = 1; i < 15; i++) {
            TVec3f prevSample, curSample;
            pPad->getPastAcceleration(&prevSample, i - 1, mChannel);
            pPad->getPastAcceleration(&curSample, i, mChannel);
            accelDeltaSum += (prevSample.y - curSample.y);
            if (peakDelta < accelDeltaSum) {
                peakDelta = accelDeltaSum;
            }
        }
    }

    mSwingDetected = peakDelta > mSwingThreshold;

    if (!mSwingDetected) {
        mSwingCooldownFrames++;
    } else {
        mSwingCooldownFrames = 0;
    }

    if (mSwingTriggered) {
        if (mSwingCooldownFrames > 8) {
            mSwingTriggered = false;
        }
    } else if (mSwingDetected) {
        mSwingTriggered = true;
    }

    if (mSwingTriggered) {
        mSwingHoldFrames++;
    } else {
        mSwingHoldFrames = 0;
    }
}

void WPadHVSwing::update() {
    updateSwing();
    updateCentrifugal();

    mIsTriggerSwing = false;
    if (mSwingLatchedFrames == 1) {
        mIsTriggerSwing = true;
    } else if (mSwingHoldFrames >= 30) {
        bool isOffPulseBoundary = (mSwingHoldFrames - 30) % 15 != 0;

        if (!isOffPulseBoundary && mSwingCooldownFrames < 15) {
            mIsTriggerSwing = true;
        }
    }

#if defined(TARGET_WIIU)
    /* mChannel is the device (core/Nunchuk); the spin belongs to player 1's core. */
    if (mChannel == WPAD_DEV_CORE && pPad->mChannel == WPAD_CHAN0) {
        const uint32_t spinFrames = sms_wiiu_spin_frames_since_press();
        if (spinFrames < 6) {
            mIsSwing = true;
            mIsTriggerSwing = spinFrames == 0;
        }
    }
#endif
}
