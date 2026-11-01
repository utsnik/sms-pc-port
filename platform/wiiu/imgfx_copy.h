#ifndef SMS_WIIU_IMGFX_COPY_H
#define SMS_WIIU_IMGFX_COPY_H

#include <stdint.h>

namespace sms_wiiu_imgfx {

enum CopyOwner : uint32_t {
    kCopyOwnerUntagged = 0,
    kCopyOwnerBloom = 1,
    kCopyOwnerSimpleBloom = 2,
    kCopyOwnerDepthOfField = 3,
    kCopyOwnerScreenBlur = 4,
    kCopyOwnerFullScreenBlur = 5,
    kCopyOwnerCaptureScreen = 6,
    kCopyOwnerWaterFilter = 7,
    kCopyOwnerScreenAlpha = 8,
};

struct CopySizes {
    bool native;
    uint32_t logicalW, logicalH;
    uint32_t sourceW, sourceH;
    uint32_t physicalW, physicalH;
};

inline CopySizes selectCopySizes(bool marker, uint32_t owner, uint32_t sourceW, uint32_t sourceH,
                                 bool mipmap, uint32_t destinationW, uint32_t destinationH,
                                 float gameScaleX, float gameScaleY, bool cemu,
                                 uint32_t fullW, uint32_t fullH) {
    CopySizes result = {};
    result.native = marker && owner == kCopyOwnerBloom;
    result.logicalW = result.native && destinationW ? destinationW : (mipmap ? (sourceW + 1) / 2 : sourceW);
    result.logicalH = result.native && destinationH ? destinationH : (mipmap ? (sourceH + 1) / 2 : sourceH);
    result.sourceW = result.native ? sourceW : uint32_t(float(sourceW) * gameScaleX);
    result.sourceH = result.native ? sourceH : uint32_t(float(sourceH) * gameScaleY);
    result.physicalW = result.native ? result.logicalW : (cemu ? fullW : result.sourceW);
    result.physicalH = result.native ? result.logicalH : (cemu ? fullH : result.sourceH);
    return result;
}

}  // namespace sms_wiiu_imgfx

#endif
