// Port: the pause menu's progress tracker. At the menu's bottom left, under
// the HUD's counters: an area's name, between the L and R buttons, and its
// Shine Sprites and blue coins as "got / all there are". It opens on the area
// Mario is in; L and R step through the others he has been to.
//
// Everything it draws is the game's, as the save state menu's is
// (savestate_menu.cpp): the area's name in the file select's lettering
// (port_savestate_world_art), the system font's L and R glyphs (swapped for
// the controller in use, patch zzz-prompts-01), and the HUD's art from
// game_6: the brushstroke behind its counters, the Shine and blue coin icons,
// the counter digits, and the slash of the HUD's "3/8" counters. A count
// that is complete has the HUD counters' green brushstroke.
//
// The areas and their counts are the game's own: the Shine tables the map
// screen (TGuide) and the file select count with, the Airstrip's two Shines
// (0x56, 0x58) as those two count them, and Delfino Plaza's the rest of the
// 120. Blue coins are TFlagManager's 50 per area, the Airstrip's being
// counted with the Plaza's as the game files them.
#include <GC2D/MessageUtil.hpp>
#include <GC2D/ShineTable.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/JDrama/JDRGraphics.hpp>
#include <JSystem/JDrama/JDRRect.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JUtility/JUTResFont.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>
#include <System/StageUtil.hpp>
#include <dolphin/gx.h>
#include <dolphin/pad.h>
#include <stdio.h>
#include <string.h>

#include "port_pause_tracker.h"
#include "port_savestate.h"

namespace {

const int kWorlds = 10; // SMS_getShineStage's: 0 the Airstrip ... 9 Corona Mountain

// layout, in the pause menu's 2D units (the 4:3 frame, 640 wide): clear of
// the HUD's counters above and the stage name's panel to the right
const f32 kLeft     = 24.0f;
const f32 kNameY    = 312.0f; // the rows' centres
const f32 kShineY   = 345.0f;
const f32 kBlueY    = 378.0f;
const f32 kRowW     = 176.0f; // the name's row, L to R
const f32 kNameH    = 22.0f;
const f32 kButton   = 24.0f;  // the L and R glyphs
const f32 kIconH    = 29.0f;
const f32 kDigitH   = 23.0f;
const f32 kBrushW   = 186.0f;
const f32 kBrushH   = 29.0f;

const u32 kWhite    = 0xFFFFFFFF;
const u32 kBrush    = 0x6E28A0FF; // the pause menu's stage panel purple
const u32 kComplete = 0x28AA28FF; // the HUD counters' green

struct Tally {
	int shines, allShines;
	int blueCoins, allBlueCoins;
};

int sWorld = -1; // the area shown, -1 until the menu appears
int sHome;       // the area Mario is in
Tally sTally[kWorlds];

JUTTexture sBrushArt, sShineArt, sBlueCoinArt, sCrossArt, sSlashArt, sDigitArt[10];
bool sHaveArt;

bool loadArt(JUTTexture& tex, const char* name)
{
	char path[64];
	snprintf(path, sizeof path, "/game_6/timg/%s.bti", name);
	const ResTIMG* timg = (const ResTIMG*)JKRGetResource(path);
	if (!timg)
		return false;
	tex.storeTIMG(timg);
	return true;
}

// game_6 comes with every stage, so the art is looked up again each time.
bool loadAllArt()
{
	bool ok = loadArt(sBrushArt, "coin_back") && loadArt(sShineArt, "shine_icon")
	          && loadArt(sBlueCoinArt, "bluecoin_icon") && loadArt(sCrossArt, "coin_number_x")
	          && loadArt(sSlashArt, "balloon_slash");
	for (int i = 0; i < 10 && ok; i++) {
		char name[32];
		snprintf(name, sizeof name, "coin_number_%d", i);
		ok = loadArt(sDigitArt[i], name);
	}
	return ok;
}

void count()
{
	TFlagManager* flags = TFlagManager::getInstance();
	memset(sTally, 0, sizeof sTally);

	// whose each of the 120 Shines is: the areas' eight episodes and three
	// others, the Airstrip's two, and the Plaza the rest
	u8 owner[120];
	memset(owner, 1, sizeof owner);
	for (int w = 2; w <= 8; w++) {
		for (int i = 0; i < 8; i++)
			owner[scShineConvTable[w][i]] = w;
		for (int i = 0; i < 3; i++)
			owner[scEtcShineConvTable[w][i]] = w;
	}
	owner[0x56] = owner[0x58] = 0;
	for (int id = 0; id < 120; id++) {
		Tally& t = sTally[owner[id]];
		t.allShines++;
		if (flags->getShineFlag(id))
			t.shines++;
	}

	// blue coins: 30 in each area, 20 in the Plaza (the Airstrip's one with
	// them), 10 on Corona Mountain
	for (int w = 1; w < kWorlds; w++) {
		Tally& t       = sTally[w];
		t.allBlueCoins = w == 1 ? 20 : w == 9 ? 10 : 30;
		for (int i = 0; i < 50; i++)
			if (flags->getBlueCoinFlag(scNormalStageTable[w], i))
				t.blueCoins++;
	}
}

// The areas L and R reach: the one Mario is in, the Plaza, and those he has
// been to (the map screen's test, a flag each stage sets as it loads).
bool reachable(int world)
{
	return world == sHome || world == 1 || TFlagManager::getInstance()->getBool(0x103A5 + world);
}

void step(int by)
{
	int w = sWorld;
	for (int i = 0; i < kWorlds; i++) {
		w = (w + by + kWorlds) % kWorlds;
		if (reachable(w))
			break;
	}
	if (w == sWorld)
		return;
	sWorld = w;
	SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_CURSOR, 0, nullptr, 0);
}

// --- drawing: quads as JUTResFont draws its glyphs (savestate_menu.cpp) -----

void setupTextured()
{
	GXSetNumChans(1);
	GXSetNumTevStages(1);
	GXSetNumTexGens(1);
	GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
}

// A w x h quad of the texture loaded in GX_TEXMAP0, its left edge at x and
// centred on y.
void quad(f32 x, f32 y, f32 w, f32 h, u32 colour, u8 alpha)
{
	const u8 a = (u8)(((colour & 0xFF) * alpha) / 255);
	static const f32 kU[4] = { 0, 1, 1, 0 }, kV[4] = { 0, 0, 1, 1 };
	setupTextured();
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	for (int i = 0; i < 4; i++) {
		GXPosition3f32(x + kU[i] * w, y - h / 2 + kV[i] * h, 0.0f);
		GXColor4u8(colour >> 24, colour >> 16, colour >> 8, a);
		GXTexCoord2f32(kU[i], kV[i]);
	}
	GXEnd();
}

// The art h tall at its own proportions; its width.
f32 art(JUTTexture& tex, f32 x, f32 y, f32 h, u32 colour, u8 alpha)
{
	const ResTIMG* t = tex.getTexInfo();
	const f32 w      = t ? h * t->width / t->height : h;
	tex.load(GX_TEXMAP0);
	quad(x, y, w, h, colour, alpha);
	return w;
}

// "x16/41" in the HUD's counter digits; its width.
f32 counter(int got, int all, f32 x, f32 y, u8 alpha)
{
	const f32 step = kDigitH * 0.86f; // the digits' spacing, as the HUD's
	char text[16];
	snprintf(text, sizeof text, "%d/%d", got, all);
	const f32 left = x;
	art(sCrossArt, x, y, kDigitH, kWhite, alpha);
	x += step;
	for (const char* p = text; *p; p++) {
		if (*p == '/') {
			art(sSlashArt, x - step * 0.12f, y, kDigitH * 1.05f, kWhite, alpha);
			x += step * 0.75f;
		} else {
			art(sDigitArt[*p - '0'], x, y, kDigitH, kWhite, alpha);
			x += step;
		}
	}
	return x - left;
}

// One of the HUD's counters on its brushstroke.
void counterRow(JUTTexture& icon, int got, int all, f32 y, u8 alpha)
{
	sBrushArt.load(GX_TEXMAP0);
	quad(kLeft - 10.0f, y + 1.0f, kBrushW, kBrushH, got >= all ? kComplete : kBrush, (u8)(alpha * 0.85f));
	const f32 iconW = art(icon, kLeft, y, kIconH, kWhite, alpha);
	counter(got, all, kLeft + iconW + 2.0f, y + 1.0f, alpha);
}

f32 textWidth(const char* s, f32 size)
{
	JUTFont* font = gpSystemFont;
	f32 w         = 0.0f;
	for (; *s; s++)
		w += font->getWidth((u8)*s) * (size / font->getWidth());
	return w;
}

// The area's name between the L and R buttons, which sit at the row's ends
// (as wide as the counters' brushstrokes, clear of the menu's options): the
// file select's lettering, else the pause menu's own name for it in the
// system font, made smaller where it would not fit.
void nameRow(u8 alpha)
{
	JUTFont* font    = gpSystemFont;
	const bool more  = sWorld != sHome || reachable((sWorld + 1) % kWorlds)
	                  || reachable((sWorld + kWorlds - 1) % kWorlds);
	const f32 gap    = 4.0f;
	const f32 room   = more ? kRowW - 2.0f * (kButton + gap) : kRowW;
	const f32 middle = kLeft + kRowW / 2;
	int tw = 0, th = 0, artW = 0;
	const void* data = port_savestate_world_art(sWorld, 0, &tw, &th, &artW);
	if (data) {
		f32 h = kNameH;
		if (artW * h / th > room)
			h = room * th / artW;
		GXTexObj obj;
		GXInitTexObj(&obj, (void*)data, tw, th, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
		GXLoadTexObj(&obj, GX_TEXMAP0);
		quad(middle - artW * h / th / 2, kNameY, tw * h / th, h, kWhite, alpha);
	} else if (const char* name
	           = SMSGetMessageData(JKRFileLoader::getGlbResource("/common/2d/stagename.bmg"), sWorld)) {
		f32 size = kNameH * 0.8f;
		if (textWidth(name, size) > room)
			size *= room / textWidth(name, size);
		const f32 x = middle - textWidth(name, size) / 2, y = kNameY + size * 0.4f;
		font->setGX();
		font->setCharColor(JUtility::TColor(0, 0, 0, alpha / 2));
		font->drawString_size_scale(x + 1.5f, y + 1.5f, size, size, name, strlen(name), false);
		font->setCharColor(JUtility::TColor(255, 255, 255, alpha));
		font->drawString_size_scale(x, y, size, size, name, strlen(name), false);
	}
	if (more) {
		font->setGX();
		font->setCharColor(JUtility::TColor(255, 255, 255, alpha));
		font->drawChar_scale(kLeft, kNameY + kButton / 2 - 3.0f, kButton, kButton, '<', false);
		font->drawChar_scale(kLeft + kRowW - kButton, kNameY + kButton / 2 - 3.0f, kButton, kButton, '>', false);
	}
}

} // namespace

void PauseTrackerReset()
{
	sWorld = -1;
	if (!port_pause_tracker || !gpMarDirector)
		return;
	sHaveArt = loadAllArt();
	sHome    = SMS_getShineStage(gpMarDirector->mMap);
	if (sHome >= kWorlds)
		sHome = 1; // somewhere outside retail's areas: start at the Plaza
	count();
	sWorld = sHome;
}

void PauseTrackerMove(TMarioGamePad* pad)
{
	if (sWorld < 0 || !pad)
		return;
	if (pad->mButton.mTrigger & PAD_TRIGGER_L)
		step(-1);
	else if (pad->mButton.mTrigger & PAD_TRIGGER_R)
		step(1);
}

void PauseTrackerDraw(const JDrama::TRect& viewport, u8 alpha)
{
	if (sWorld < 0 || !sHaveArt || !alpha)
		return;
	J2DOrthoGraph graph(viewport);
	graph.setPort(); // the menu's screen left its own projection
	graph.setup2D();
	nameRow(alpha);
	const Tally& t = sTally[sWorld];
	f32 y          = kShineY;
	if (t.allShines) {
		counterRow(sShineArt, t.shines, t.allShines, y, alpha);
		y = kBlueY;
	}
	if (t.allBlueCoins)
		counterRow(sBlueCoinArt, t.blueCoins, t.allBlueCoins, y, alpha);
}
