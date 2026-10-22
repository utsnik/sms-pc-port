#include "Game/AreaObj/AreaForm.hpp"
#include "Game/MapObj/SimpleBreakableObj.hpp"
#include "Game/System/ErrorArchive.hpp"
#include "Game/Util/JMapInfo.hpp"
#include "JSystem/JAudio2/JASInstEffect.hpp"

#include <revolution/gd.h>

/* These bases are header-only in the decomp, but their virtual key functions
 * still need one owner when the full Galaxy archive is linked. */
void AreaForm::init(const JMapInfoIter&) {}
bool AreaForm::isInVolume(const TVec3f&) const { return false; }
void SimpleBreakableObjBase::control() {}
void JASInstEffect::effect(int, int, JASInstParam*) const {}

extern "C" {

/* Stage 1 intentionally carries no converted game tables or archives. */
u8 cErrorArchive[1] = {0};
extern const u8 StoryEventBCSV[1] = {0};
extern const JMapData GalaxyIDBCSV = {0, 0, 0, 0};

void GDSetGenMode(u8, u8, u8) {}
void GDSetTevColor(GXTevRegID, GXColor) {}
void GDSetTexCoordGen(GXTexCoordID, GXTexGenType, GXTexGenSrc, GXBool, u32) {}
void GDSetChanAmbColor(GXChannelID, GXColor) {}
void GDSetFog(GXFogType, f32, f32, f32, f32, GXColor) {}

}

namespace JASystem { namespace AudioThread {
u32 getDSPSyncCount() { return 0; }
} }
