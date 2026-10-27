#ifndef SMS_WIIU_PIPELINED_PRESENT
#define SMS_WIIU_PIPELINED_PRESENT 1
#endif
#include "include/dolphin/gx/GXVert.h"
#include "../gx/include/sms_gx/gx_pc.h"
#include "runtime.h"
#include "gx2_texture_pack.h"
#include "gx2_producer_worker.h"

#include <dolphin/gx/GXCpu2Efb.h>
#include <dolphin/gx/GXFrameBuffer.h>
#include <dolphin/gx/GXManage.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <whb/proc.h>
#include <proc_ui/procui.h>
#include <cstdlib>
#include <cstdio>
#include <malloc.h>
#include <sys/stat.h>
#include <whb/gfx.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/state.h>
extern "C" void sms_wiiu_gx2_target_begin(void);
extern "C" void sms_wiiu_gx2_target_present(void);
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <coreinit/cache.h>
#include <coreinit/memory.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memheap.h>
#include <gx2/mem.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <coreinit/core.h>

namespace {

uint32_t s_present_count;
float s_widescreen = 1.0f;
bool s_logged;
bool s_gfx_initialized;
bool s_frame_open;

extern "C" int sms_wiiu_gx2_renderer_init(void);
extern "C" int sms_wiiu_gx2_display_list_enabled(void);
extern "C" void sms_wiiu_gx2_renderer_begin(void);
extern "C" void sms_wiiu_gx2_force_nodiff(int on);
extern "C" const GX2Surface* sms_wiiu_gx2_render_surface(void);
extern "C" void sms_wiiu_gx2_renderer_end(void);
extern "C" void sms_wiiu_gx2_renderer_shutdown(void);
extern "C" void sms_wiiu_gx2_texture_pack_pump(void);
extern "C" int sms_wiiu_gx2_renderer_has_draws(void);

void log_active_once()
{
	if (!s_logged) {
		s_logged = true;
		WHBLogPrintf("SMS Wii U: GX2 renderer active");
	}
}

/* ---- GamePad output and display diagnostics ------------------------------
 * The GamePad shows the TV image: the finished TV colour buffer is copied to
 * the DRC scan buffer too (GX2CopyColorBufferToScanBuffer resizes; the DRC
 * scale is set to the source size, as the LUS and DKR ports do on hardware).
 * The 16:9 TV target keeps the 4:3 pillarbox on the 854x480 panel.
 *
 * WHBGfx's own DRC colour buffer is then unused; it is filled once with a
 * sentinel and checked later as a MEM1 canary (anything writing into the
 * WHBGfx MEM1 heap shows up as a changed word).  GX2GetLastFrame samples
 * show what the TV and DRC scan buffers actually hold. */
constexpr float kCanary[4] = {0x5a / 255.0f, 0x3c / 255.0f, 0x96 / 255.0f, 0x12 / 255.0f};
uint32_t s_canary_word;
bool s_canary_armed;
bool s_was_foreground = true;
uint32_t s_presented;

void log_surface(const char* tag, const GX2Surface& s)
{
	port_log("[drc] %s image=%p bytes=%u %ux%u fmt=0x%x tile=%u pitch=%u swizzle=0x%x use=0x%x aa=%u\n",
	         tag, s.image, unsigned(s.imageSize), unsigned(s.width), unsigned(s.height),
	         unsigned(s.format), unsigned(s.tileMode), unsigned(s.pitch), unsigned(s.swizzle),
	         unsigned(s.use), unsigned(s.aa));
}

/* Eight words spread over the image, read by the CPU after a GX2DrawDone. */
void log_words(const char* tag, const void* image, uint32_t bytes)
{
	if (!image || bytes < 64) {
		port_log("[drc] %s words: no image\n", tag);
		return;
	}
	DCInvalidateRange(const_cast<void*>(image), bytes);
	const uint32_t* w = static_cast<const uint32_t*>(image);
	const uint32_t n = bytes / 4;
	uint32_t v[8];
	for (uint32_t i = 0; i < 8; ++i)
		v[i] = w[uint32_t(uint64_t(n) * (2 * i + 1) / 16)];
	port_log("[drc] %s words %08x %08x %08x %08x %08x %08x %08x %08x\n", tag,
	         unsigned(v[0]), unsigned(v[1]), unsigned(v[2]), unsigned(v[3]),
	         unsigned(v[4]), unsigned(v[5]), unsigned(v[6]), unsigned(v[7]));
}

/* SD marker "tribuf": triple-buffered TV and DRC scan buffers, so two swaps may
 * be outstanding before the next frame starts.  WHBGfxInit sizes, allocates
 * and registers the TV buffer itself, so GX2CalcTVSize and GX2SetTVBuffer are
 * link-wrapped to change its buffering mode; WHBGfx then allocates the larger
 * buffer in its foreground heap and frees it on release as before. */
bool s_tribuf;
void gf_init();

GX2BufferingMode scan_buffering()
{
	return s_tribuf ? GX2_BUFFERING_MODE_TRIPLE : GX2_BUFFERING_MODE_DOUBLE;
}

/* WHBGfxInit sizes and registers the DRC scan buffer with GX2GetSystemDRCMode(),
 * which returned 0 (DISABLED) on hardware: a 512-byte buffer in a disabled
 * mode, so the GamePad kept the system launch screen.  The LUS and DKR ports
 * use GX2GetSystemDRCScanMode() (1 = SINGLE).  Register a real DRC scan buffer
 * in that mode, allocated from WHBGfx's own foreground heap (found from the
 * TV scan buffer it holds), so WHBGfx's release callback frees it with the
 * heap; redone after every foreground acquire. */
bool s_drc_registered;
uint32_t s_drc_attempts;
void* s_drc_scan;

void drc_register_scan_buffer()
{
	if (s_drc_registered || s_drc_attempts >= 10)
		return;
	++s_drc_attempts;
	GX2DrcRenderMode mode = GX2GetSystemDRCScanMode();
	if (mode == GX2_DRC_RENDER_MODE_DISABLED)
		mode = GX2_DRC_RENDER_MODE_SINGLE;
	uint32_t bytes = 0, unk = 0;
	GX2CalcDRCSize(mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, scan_buffering(), &bytes, &unk);
	uint32_t fga = 0, fgs = 0;
	OSGetForegroundBucket(&fga, &fgs);
	GX2Texture last;
	std::memset(&last, 0, sizeof(last));
	const BOOL ok = GX2GetLastFrame(GX2_SCAN_TARGET_TV, &last);
	const uint32_t tvScan = reinterpret_cast<uint32_t>(last.surface.image);
	const bool inForeground = ok && tvScan >= fga && tvScan - fga < fgs;
	/* WHBGfx's foreground expanded heap spans the whole foreground frame heap,
	 * so any address inside the bucket's tail finds it; only an EXPH heap is
	 * used (an exp-heap call on the frame heap itself would corrupt it). */
	void* probes[3] = {inForeground ? last.surface.image : nullptr,
	                   reinterpret_cast<void*>(fga + fgs - 256), reinterpret_cast<void*>(fga + fgs / 2)};
	MEMHeapHandle heap = nullptr;
	for (void* probe : probes) {
		if (!probe || !fgs)
			continue;
		MEMHeapHandle h = MEMFindContainHeap(probe);
		const uint32_t tag = h ? *reinterpret_cast<const uint32_t*>(h) : 0;
		port_log("[drc] probe %p -> heap %p tag=%08x\n", probe, static_cast<void*>(h), unsigned(tag));
		if (h && tag == 0x45585048u) { /* 'EXPH' */
			heap = h;
			break;
		}
	}
	const char* from = "foreground";
	if (!heap && s_drc_attempts >= 10) {
		/* Last resort: WHBGfx's MEM1 heap (holds its colour buffers). */
		heap = MEMFindContainHeap(WHBGfxGetDRCColourBuffer()->surface.image);
		from = "MEM1";
	}
	port_log("[drc] scan register try=%u mode=%d bytes=%u tvScan ok=%d %p inFG=%d heap=%p (%s)\n",
	         unsigned(s_drc_attempts), int(mode), unsigned(bytes), int(ok), last.surface.image,
	         int(inForeground), static_cast<void*>(heap), from);
	if (!heap || !bytes)
		return;
	s_drc_scan = MEMAllocFromExpHeapEx(heap, bytes, GX2_SCAN_BUFFER_ALIGNMENT);
	if (!s_drc_scan) {
		port_log("[drc] scan register: alloc of %u bytes failed (free %u)\n", unsigned(bytes),
		         unsigned(MEMGetAllocatableSizeForExpHeapEx(heap, GX2_SCAN_BUFFER_ALIGNMENT)));
		return;
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, s_drc_scan, bytes);
	GX2SetDRCBuffer(s_drc_scan, bytes, mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, scan_buffering());
	GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
	GX2SetDRCScale(tv->surface.width, tv->surface.height);
	GX2SetDRCEnable(TRUE);
	s_drc_registered = true;
	port_log("[drc] DRC scan buffer registered: %p+%u mode=%d scale %ux%u\n", s_drc_scan,
	         unsigned(bytes), int(mode), unsigned(tv->surface.width), unsigned(tv->surface.height));
}

void drc_setup(bool first)
{
	GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
	GX2SetDRCScale(tv->surface.width, tv->surface.height);
	if (!first) {
		port_log("[drc] foreground regained; DRC scale %ux%u\n",
		         unsigned(tv->surface.width), unsigned(tv->surface.height));
		s_canary_armed = false;
		/* WHBGfx's release callback destroyed its foreground heap (and our
		 * buffer); its acquire callback registered its disabled one again. */
		s_drc_registered = false;
		s_drc_attempts = 0;
		s_drc_scan = nullptr;
		return;
	}
	uint32_t drcBytes = 0, unk = 0;
	const GX2DrcRenderMode drcMode = GX2GetSystemDRCMode();
	GX2CalcDRCSize(drcMode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE, &drcBytes, &unk);
	uint32_t m1a = 0, m1s = 0, fga = 0, fgs = 0;
	OSGetMemBound(OS_MEM1, &m1a, &m1s);
	OSGetForegroundBucket(&fga, &fgs);
	port_log("[drc] init tvScan=%d aspect=%d drcMode=%d drcScanMode=%d drcScanBytes=%u "
	         "MEM1=%08x+%u foreground=%08x+%u\n",
	         int(GX2GetSystemTVScanMode()), int(GX2GetSystemTVAspectRatio()), int(drcMode),
	         int(GX2GetSystemDRCScanMode()), unsigned(drcBytes), unsigned(m1a), unsigned(m1s),
	         unsigned(fga), unsigned(fgs));
	log_surface("tvColour", tv->surface);
	log_surface("tvDepth", WHBGfxGetTVDepthBuffer()->surface);
	GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
	log_surface("drcColour", drc->surface);
	log_surface("drcDepth", WHBGfxGetDRCDepthBuffer()->surface);
	port_log("[drc] contexts tv=%p drc=%p\n", static_cast<void*>(WHBGfxGetTVContextState()),
	         static_cast<void*>(WHBGfxGetDRCContextState()));
	if (drc->surface.image && drc->surface.imageSize >= 64) {
		GX2ClearColor(drc, kCanary[0], kCanary[1], kCanary[2], kCanary[3]);
		GX2SetContextState(WHBGfxGetTVContextState());
		GX2DrawDone();
		DCInvalidateRange(drc->surface.image, drc->surface.imageSize);
		s_canary_word = static_cast<const uint32_t*>(drc->surface.image)[drc->surface.imageSize / 8];
		s_canary_armed = true;
		port_log("[drc] canary armed in WHBGfx DRC colour buffer: word=%08x\n", unsigned(s_canary_word));
	}
}

void canary_check()
{
	GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
	if (!s_canary_armed || !drc->surface.image)
		return;
	DCInvalidateRange(drc->surface.image, drc->surface.imageSize);
	const uint32_t* w = static_cast<const uint32_t*>(drc->surface.image);
	const uint32_t n = drc->surface.imageSize / 4;
	uint32_t bad = 0, first = n, last = 0;
	for (uint32_t i = 0; i < n; ++i) {
		if (w[i] != s_canary_word) {
			if (first == n) first = i;
			last = i;
			++bad;
		}
	}
	if (!bad)
		port_log("[drc] canary intact (%u words)\n", unsigned(n));
	else
		port_log("[drc] CANARY HIT: %u of %u words changed, first %p=%08x last %p\n",
		         unsigned(bad), unsigned(n), static_cast<const void*>(w + first),
		         unsigned(w[first]), static_cast<const void*>(w + last));
}

/* ---- Self-screenshots ------------------------------------------------------
 * Only when "<app root>shots" (a directory) exists.  At a few presents the TV
 * colour buffer (2x downscaled) and the GamePad scan buffer are copied into
 * linear surfaces (GX2CopySurface), the GPU is drained, and both are written
 * as 24-bit BMPs to "<app root>shots/shotNN-{tv,drc}.bmp".  Runs on the
 * presenting thread between frames (rare, <= 10 times); every file is closed
 * before returning. */
int s_shots_marker = -1;
uint32_t s_shots_written;
constexpr uint32_t kShotsMax = 10;

bool shots_wanted(uint32_t p)
{
	if (s_shots_marker < 0) {
		char path[192];
		std::snprintf(path, sizeof(path), "%sshots", sms_wiiu_app_root());
		struct stat st;
		s_shots_marker = (stat(path, &st) == 0) ? 1 : 0;
		port_log("[shots] marker %s\n", s_shots_marker ? "present: screenshots ON" : "absent: off");
	}
	if (!s_shots_marker || s_shots_written >= kShotsMax)
		return false;
	/* SMS_SHOTS_FROM=<present> (autoplay file): 4 shots, 400 presents apart,
	 * from there instead (A/B pictures of one place in a level). */
	static int s_from = -1;
	if (s_from < 0) {
		const char* v = std::getenv("SMS_SHOTS_FROM");
		s_from = v ? std::atoi(v) : 0;
	}
	/* SMS_SHOTS_ABA=1: three consecutive presents per shot point instead, the
	 * middle frame drawn with the redundancy caches off (nodiff), at two
	 * points: one run gives the same game state both ways. */
	static int s_aba = -1;
	if (s_aba < 0) {
		const char* v = std::getenv("SMS_SHOTS_ABA");
		s_aba = v && *v >= '1' && *v <= '5' ? *v - '0' : 0;
	}
	if (s_from > 0 && s_aba)
		return p >= uint32_t(s_from) && (p - uint32_t(s_from)) % 400 < 3 && s_shots_written < 6;
	if (s_from > 0)
		return p >= uint32_t(s_from) && (p - uint32_t(s_from)) % 400 == 0 && s_shots_written < 4;
	/* The default schedule only with SMS_SHOTS=1: the shots/ folder alone
	 * (left on the card by an earlier batch) stalled timing runs ~0.5 s per
	 * shot. */
	static int s_default = -1;
	if (s_default < 0) {
		const char* v = std::getenv("SMS_SHOTS");
		s_default = v && *v == '1';
	}
	return s_default && (p == 120 || p == 600 || p == 1800 || (p >= 3600 && p % 3600 == 0));
}

void put32(uint8_t* d, uint32_t v) { d[0] = v; d[1] = v >> 8; d[2] = v >> 16; d[3] = v >> 24; }

/* img is a linear RGBA8 image; writes it downscaled by `shrink` as a BMP. */
bool write_bmp(const char* path, const uint8_t* img, uint32_t w, uint32_t h, uint32_t pitchPx, uint32_t shrink)
{
	const uint32_t ow = w / shrink, oh = h / shrink;
	const uint32_t rowBytes = (ow * 3 + 3) & ~3u;
	uint8_t hdr[54] = {'B', 'M'};
	put32(hdr + 2, 54 + rowBytes * oh);
	put32(hdr + 10, 54);
	put32(hdr + 14, 40);
	put32(hdr + 18, ow);
	put32(hdr + 22, oh); /* positive = bottom-up */
	hdr[26] = 1;
	hdr[28] = 24;
	put32(hdr + 34, rowBytes * oh);
	FILE* f = std::fopen(path, "wb");
	if (!f)
		return false;
	static uint8_t row[1920 * 3 + 4];
	bool ok = std::fwrite(hdr, 1, 54, f) == 54;
	for (uint32_t y = 0; ok && y < oh; ++y) {
		const uint8_t* src = img + size_t(oh - 1 - y) * shrink * pitchPx * 4;
		std::memset(row, 0, rowBytes);
		for (uint32_t x = 0; x < ow; ++x) {
			unsigned r = 0, g = 0, b = 0;
			for (uint32_t dy = 0; dy < shrink; ++dy)
				for (uint32_t dx = 0; dx < shrink; ++dx) {
					const uint8_t* px = src + (size_t(dy) * pitchPx + x * shrink + dx) * 4;
					r += px[0]; g += px[1]; b += px[2];
				}
			const unsigned n = shrink * shrink;
			row[x * 3 + 0] = b / n; row[x * 3 + 1] = g / n; row[x * 3 + 2] = r / n;
		}
		ok = std::fwrite(row, 1, rowBytes, f) == rowBytes;
	}
	if (std::fclose(f) != 0)
		ok = false;
	return ok;
}

/* Copies `src` (any tiling) into a freshly allocated linear surface, waits for
 * the GPU, writes the BMP, frees. */
void shot_surface(const GX2Surface& src, const char* path, uint32_t shrink)
{
	if (!src.image || src.width > 1920 || src.height > 1080 ||
	    (src.format & 0x3ff) != GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8) {
		port_log("[shots] skip %s: unsupported source (image=%p %ux%u fmt=0x%x)\n", path, src.image,
		         unsigned(src.width), unsigned(src.height), unsigned(src.format));
		return;
	}
	GX2Surface dst;
	std::memset(&dst, 0, sizeof(dst));
	dst.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	dst.width = src.width;
	dst.height = src.height;
	dst.depth = 1;
	dst.mipLevels = 1;
	dst.format = src.format;
	dst.aa = GX2_AA_MODE1X;
	dst.use = GX2_SURFACE_USE_TEXTURE;
	dst.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
	GX2CalcSurfaceSizeAndAlignment(&dst);
	dst.image = MEMAllocFromDefaultHeapEx(dst.imageSize, dst.alignment);
	if (!dst.image) {
		port_log("[shots] skip %s: out of memory (%u bytes)\n", path, unsigned(dst.imageSize));
		return;
	}
	std::memset(dst.image, 0, dst.imageSize);
	DCFlushRange(dst.image, dst.imageSize);
	GX2CopySurface(const_cast<GX2Surface*>(&src), 0, 0, &dst, 0, 0);
	GX2Flush();
	GX2DrawDone();
	DCInvalidateRange(dst.image, dst.imageSize);
	const bool ok = write_bmp(path, static_cast<const uint8_t*>(dst.image), dst.width, dst.height, dst.pitch, shrink);
	port_log("[shots] %s %s (%ux%u)\n", ok ? "wrote" : "FAILED", path, unsigned(dst.width / shrink),
	         unsigned(dst.height / shrink));
	MEMFreeToDefaultHeap(dst.image);
}

void take_shots(uint32_t p)
{
	if (!shots_wanted(p))
		return;
	char dir[192], path[224];
	std::snprintf(dir, sizeof(dir), "%sshots", sms_wiiu_app_root());
	mkdir(dir, 0777);
	/* Numbering continues after the shots already on the card, so a second
	 * run before they are fetched does not overwrite the first one's. */
	static uint32_t s_base = ~0u;
	if (s_base == ~0u) {
		s_base = 0;
		for (struct stat st; s_base < 90; ++s_base) {
			std::snprintf(path, sizeof(path), "%s/shot%02u-tv.bmp", dir, unsigned(s_base));
			if (stat(path, &st) != 0) break;
		}
	}
	const uint32_t n = s_base + s_shots_written++;
	std::snprintf(path, sizeof(path), "%s/shot%02u-tv.bmp", dir, unsigned(n));
	/* The render target through its own descriptor: it aliases WHBGfx's TV
	 * colour buffer, whose 1920x1080 descriptor decodes a smaller target (the
	 * per-scene size) as noise.  GX2GetLastFrame(TV) is no substitute: it
	 * returns the system's saved frame (the Wii U splash), not this app's. */
	shot_surface(*sms_wiiu_gx2_render_surface(), path, 2);
	GX2Texture last;
	std::memset(&last, 0, sizeof(last));
	if (GX2GetLastFrame(GX2_SCAN_TARGET_DRC, &last)) {
		std::snprintf(path, sizeof(path), "%s/shot%02u-drc.bmp", dir, unsigned(n));
		shot_surface(last.surface, path, 1);
	} else {
		port_log("[shots] DRC GX2GetLastFrame failed\n");
	}
	/* SMS_SHOTS_ABA: the frame after the first of each triple is the B frame. */
	/* SMS_SHOTS_ABA=<mode>: the B frame's renderer mode. 1: nodiff; 2: caches
	 * on, a full upload's volume to unused registers in one packet; 3: the same
	 * in 4-float packets; 4: nodiff with the full upload in 4-vector packets;
	 * 5: 3 plus GX2DrawDone after every draw (no GPU lag). */
	if (const char* aba = std::getenv("SMS_SHOTS_ABA")) {
		const bool b = (n - s_base) % 3 == 0;
		const int mode = b ? (*aba >= '1' && *aba <= '5' ? *aba - '0' : 1) : 0;
		sms_wiiu_gx2_force_nodiff(mode);
		port_log("[shots] shot%02u taken; next frame B mode=%d\n", unsigned(n), mode);
	}
}

/* Called after WHBGfxFinishRender (its GX2DrawDone has completed the frame). */
void present_diagnostics()
{
	++s_presented;
	const uint32_t p = s_presented;
	take_shots(p);
	if (!(p == 2 || p == 120 || p == 1800 || p == 3600 || p % 18000 == 0))
		return;
	GX2DrawDone(); /* the reads below assume a finished frame */
	uint32_t swaps = 0, flips = 0;
	OSTime lastFlip = 0, lastVsync = 0;
	GX2GetSwapStatus(&swaps, &flips, &lastFlip, &lastVsync);
	port_log("[drc] present=%u swaps=%u flips=%u retrace=%u gx2dl=%d\n", unsigned(p), unsigned(swaps),
	         unsigned(flips), unsigned(sms_wiiu_frame_count()), sms_wiiu_gx2_display_list_enabled());
	GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
	/* WHBGfx's buffer descriptors live in our .bss: a stray write shows here. */
	log_surface("tvColour", tv->surface);
	log_surface("drcColour", WHBGfxGetDRCColourBuffer()->surface);
	log_words("tvColour", tv->surface.image, tv->surface.imageSize);
	static const struct { GX2ScanTarget target; const char* name; } kScans[] = {
		{GX2_SCAN_TARGET_TV, "tvScan"}, {GX2_SCAN_TARGET_DRC, "drcScan"}};
	for (const auto& scan : kScans) {
		GX2Texture last;
		std::memset(&last, 0, sizeof(last));
		const BOOL ok = GX2GetLastFrame(scan.target, &last);
		port_log("[drc] %s GX2GetLastFrame=%d\n", scan.name, int(ok));
		if (ok) {
			log_surface(scan.name, last.surface);
			log_words(scan.name, last.surface.image, last.surface.imageSize);
		}
	}
	canary_check();
}

} // namespace

/* Linked with -Wl,--wrap for each; see s_tribuf. */
extern "C" {
void __real_GX2CalcTVSize(GX2TVRenderMode, GX2SurfaceFormat, GX2BufferingMode, uint32_t*, uint32_t*);
void __real_GX2SetTVBuffer(void*, uint32_t, GX2TVRenderMode, GX2SurfaceFormat, GX2BufferingMode);
void __wrap_GX2CalcTVSize(GX2TVRenderMode mode, GX2SurfaceFormat format, GX2BufferingMode buffering,
                          uint32_t* size, uint32_t* unk)
{
	__real_GX2CalcTVSize(mode, format, s_tribuf ? GX2_BUFFERING_MODE_TRIPLE : buffering, size, unk);
}
void __wrap_GX2SetTVBuffer(void* buffer, uint32_t size, GX2TVRenderMode mode, GX2SurfaceFormat format,
                           GX2BufferingMode buffering)
{
	if (s_tribuf)
		buffering = GX2_BUFFERING_MODE_TRIPLE;
	port_log("[present] TV scan buffer %p+%u mode=%d buffering=%d\n", buffer, unsigned(size), int(mode),
	         int(buffering));
	__real_GX2SetTVBuffer(buffer, size, mode, format, buffering);
}
}

/* 1 forces the renderer's full-surface EFB copy (the Cemu path) on hardware. */
extern "C" const int sms_wiiu_efb_full_copy = 0;

extern "C" void sms_wiiu_gfx_init(void)
{
	if (s_gfx_initialized)
		return;
	s_tribuf = sms_wiiu_sd_marker("tribuf") != 0;
	port_log("SMS Wii U: scan buffers %s (SD marker tribuf)\n", s_tribuf ? "triple" : "double");
	gf_init();
	s_gfx_initialized = WHBGfxInit() != FALSE;
	if (!s_gfx_initialized)
		WHBLogPrintf("SMS Wii U: WHBGfxInit failed");
	else if (!sms_wiiu_gx2_renderer_init())
		WHBLogPrintf("SMS Wii U: GX2 renderer setup failed");
	else {
		WHBLogPrintf("SMS Wii U: [gx2dl] display-list path %s",
		             sms_wiiu_gx2_display_list_enabled() ? "enabled" : "disabled");
		drc_setup(true);
		WHBGfxBeginRender();
		sms_wiiu_gx2_target_begin();
		sms_wiiu_gx2_renderer_begin();
		s_frame_open = true;
	}
}

/* Orderly exit on the main thread after ProcUI reported EXITING: WHBGfx owns
 * its foreground-release handling; release it, then end the process like a
 * return from main. */
extern "C" void sms_wiiu_audio_shutdown(void);
extern "C" void sms_wiiu_vsync_pace_stop(void);

extern "C" void sms_wiiu_exit(void)
{
	/* Stop the audio pacer and close SDL audio before the foreground and AX
	 * go away. */
	/* Step logs: an exit to the Menu froze once (2026-10-19 21:49) with no
	 * "exiting" line, so a repeat must show which step never returned. */
	WHBLogPrintf("SMS Wii U: exit: audio shutdown");
	gx::texturePackShutdown();
	sms_wiiu_audio_shutdown();
	sms_wiiu_vsync_pace_stop();
	if (s_gfx_initialized) {
		if (s_frame_open) {
			WHBLogPrintf("SMS Wii U: exit: renderer end");
			sms_wiiu_gx2_renderer_end();
			s_frame_open = false;
		}
		sms_wiiu_gx2_renderer_shutdown();
		s_gfx_initialized = false;
		WHBLogPrintf("SMS Wii U: exit: WHBGfxShutdown");
		WHBGfxShutdown();
	}
	WHBLogPrintf("SMS Wii U: exiting");
	WHBProcShutdown();
	WHBLogUdpDeinit();
	/* _Exit, not std::exit: the game's global destructors run while its other
	 * threads are still alive, and a relaunch froze right after "exiting". */
	std::_Exit(0);
}

extern "C" int sms_wiiu_gfx_is_initialized(void)
{
	return s_gfx_initialized ? 1 : 0;
}

extern "C" int64_t wut_OSGetTime_stub(void) __asm__("OSGetTime");
/* Where a presented frame waits: FinishRender ends with GX2DrawDone (the GPU
 * finishing the frame), BeginRender waits for the next vsync. */
static int64_t s_presentFinish, s_presentBegin;
static uint32_t s_presentFrames;

/* SD marker "gpuframe": the GPU's own cost per frame under gx2prod, where
 * perfdiag's per-draw stamps miss the worker's lists, copies, clears and the
 * present.  Core 1 stamps the top of pipe at frame start and before each list,
 * and the bottom of pipe after each list or serviced callback and before the
 * swap.  A top stamp later than every earlier bottom stamp is time the GPU sat
 * drained, waiting for commands.  Frames are read four frames later, when the
 * GPU has long written them. */
extern "C" void GX2SampleBottomGPUCycle(uint64_t* result);
extern "C" void GX2SampleTopGPUCycle(uint64_t* result);
namespace {
constexpr uint32_t kGfFrames = 4, kGfStamps = 512;
bool s_gf;
uint64_t* s_gfStamp;
/* Stamp kinds: 0 top of pipe; bottom after 1 a list, 2 a copy or clear,
 * 3 a retile, 4 another callback, 5 the frame's draws (before the present),
 * 6 the present (before the swap), 7 a copy's copy, 8 its colour clear,
 * 9 its depth clear, 10 the frame-start clears. */
constexpr uint32_t kGfKinds = 11, kGfSegs = 160;
uint8_t s_gfKind[kGfFrames][kGfStamps];
uint64_t s_gfKindCost[kGfKinds], s_gfSegCost[kGfSegs];
uint8_t s_gfSegKind[kGfSegs];
uint32_t s_gfCount[kGfFrames], s_gfFrame;
uint64_t s_gfPrevEnd;
uint64_t s_gfSpan, s_gfSpanMax, s_gfIdle, s_gfStartGap;
uint32_t s_gfStamps, s_gfFrames, s_gfNotReady, s_gfOverflow;

void gf_init()
{
	s_gf = sms_wiiu_sd_marker("gpuframe") != 0;
	if (s_gf) {
		s_gfStamp = static_cast<uint64_t*>(MEMAllocFromDefaultHeapEx(kGfFrames * kGfStamps * 8, 64));
		if (s_gfStamp) {
			memset(s_gfStamp, 0, kGfFrames * kGfStamps * 8);
			DCFlushRange(s_gfStamp, kGfFrames * kGfStamps * 8);
		} else {
			s_gf = false;
		}
	}
	port_log("SMS Wii U: GPU frame stamps %s (SD marker gpuframe)\n", s_gf ? "on" : "off");
}

void gf_stamp(uint32_t kind, bool end = false)
{
	const uint32_t f = s_gfFrame % kGfFrames;
	uint32_t& n = s_gfCount[f];
	if (n >= kGfStamps - (end ? 0 : 1)) {
		++s_gfOverflow;
		return;
	}
	uint64_t* slot = s_gfStamp + f * kGfStamps + n;
	s_gfKind[f][n++] = uint8_t(kind);
	if (kind) GX2SampleBottomGPUCycle(slot);
	else GX2SampleTopGPUCycle(slot);
}

/* Reads the frame about to reuse its slot, then clears the slot for it. */
void gf_collect(uint32_t f)
{
	const uint32_t n = s_gfCount[f];
	s_gfCount[f] = 0;
	if (!n) return;
	uint64_t* st = s_gfStamp + f * kGfStamps;
	DCInvalidateRange(st, n * 8);
	bool ready = true;
	for (uint32_t i = 0; i < n; ++i)
		if (!st[i]) ready = false;
	if (ready && s_gfKind[f][n - 1]) {
		const uint64_t end = st[n - 1];
		uint64_t busyUntil = s_gfPrevEnd;
		uint64_t idle = 0;
		const bool count = s_gfPrevEnd && end > s_gfPrevEnd;
		for (uint32_t i = 0; i < n; ++i) {
			if (const uint32_t kind = s_gfKind[f][i]) {
				/* Busy time since the GPU last caught up goes to this stamp's kind. */
				if (st[i] > busyUntil) {
					if (count && busyUntil) {
						s_gfKindCost[kind < kGfKinds ? kind : 0] += st[i] - busyUntil;
						if (i < kGfSegs) { s_gfSegCost[i] += st[i] - busyUntil; s_gfSegKind[i] = uint8_t(kind); }
					}
					busyUntil = st[i];
				}
			} else if (busyUntil && st[i] > busyUntil) {
				if (i == 0) s_gfStartGap += st[i] - busyUntil;
				idle += st[i] - busyUntil;
				busyUntil = st[i];
			}
		}
		if (s_gfPrevEnd && end > s_gfPrevEnd) {
			const uint64_t span = end - s_gfPrevEnd;
			s_gfSpan += span;
			s_gfIdle += idle;
			if (span > s_gfSpanMax) s_gfSpanMax = span;
			s_gfStamps += n;
			++s_gfFrames;
		}
		s_gfPrevEnd = end;
	} else {
		++s_gfNotReady;
		s_gfPrevEnd = 0;
	}
	memset(st, 0, n * 8);
	DCFlushRange(st, n * 8);
}

/* GPU timer: 27 MHz, so 2700 cycles per 0.1 ms. */
void gf_end_frame()
{
	gf_stamp(6, true);
	++s_gfFrame;
	gf_collect(s_gfFrame % kGfFrames);
	if (s_gfFrames >= 30) {
		port_log("SMS Wii U: [gpuframe] per-frame x0.1ms span=%u busy=%u idle=%u startGap=%u maxSpan=%u stamps=%u /window: frames=%u notReady=%u overflow=%u\n",
		         unsigned(s_gfSpan / s_gfFrames / 2700),
		         unsigned((s_gfSpan - (s_gfIdle < s_gfSpan ? s_gfIdle : s_gfSpan)) / s_gfFrames / 2700),
		         unsigned(s_gfIdle / s_gfFrames / 2700), unsigned(s_gfStartGap / s_gfFrames / 2700),
		         unsigned(s_gfSpanMax / 2700), unsigned(s_gfStamps / s_gfFrames), unsigned(s_gfFrames),
		         unsigned(s_gfNotReady), unsigned(s_gfOverflow));
		const uint64_t d = uint64_t(s_gfFrames) * 270;  // x0.01 ms per frame
		port_log("SMS Wii U: [gpuframe] busy by kind x0.01ms: lists=%u copyClear=%u retile=%u callback=%u preDraws=%u present=%u "
		         "copy=%u copyColourClear=%u copyDepthClear=%u frameClears=%u\n",
		         unsigned(s_gfKindCost[1] / d), unsigned(s_gfKindCost[2] / d), unsigned(s_gfKindCost[3] / d),
		         unsigned(s_gfKindCost[4] / d), unsigned(s_gfKindCost[5] / d), unsigned(s_gfKindCost[6] / d),
		         unsigned(s_gfKindCost[7] / d), unsigned(s_gfKindCost[8] / d), unsigned(s_gfKindCost[9] / d),
		         unsigned(s_gfKindCost[10] / d));
		/* The costliest stamp positions: frames run the same sequence, so a
		 * position names the same list or copy from frame to frame. */
		char buf[256];
		int len = 0;
		for (int r = 0; r < 8; ++r) {
			uint32_t best = 0;
			for (uint32_t i = 1; i < kGfSegs; ++i)
				if (s_gfSegCost[i] > s_gfSegCost[best]) best = i;
			if (!s_gfSegCost[best]) break;
			len += std::snprintf(buf + len, sizeof(buf) - len, " #%u(k%u)=%u", unsigned(best),
			                     unsigned(s_gfSegKind[best]), unsigned(s_gfSegCost[best] / d));
			s_gfSegCost[best] = 0;
		}
		port_log("SMS Wii U: [gpuframe] top positions x0.01ms:%s\n", buf);
		memset(s_gfKindCost, 0, sizeof(s_gfKindCost));
		memset(s_gfSegCost, 0, sizeof(s_gfSegCost));
		s_gfSpan = s_gfSpanMax = s_gfIdle = s_gfStartGap = 0;
		s_gfStamps = s_gfFrames = s_gfNotReady = s_gfOverflow = 0;
	}
}
} // namespace

extern "C" void sms_wiiu_gpuframe_stamp(int kind)
{
	if (s_gf && OSGetCoreId() == 1) gf_stamp(uint32_t(kind));
}

/* Pipelined present: WHBGfxFinishRender ends in GX2DrawDone and
 * WHBGfxBeginRender waits for the previous flip, so the CPU never built a frame
 * while the GPU drew one.  Here the swap is queued without draining, and the
 * next frame starts once at most one swap (two with tribuf) is still outstanding.  The vertex
 * arenas are fenced and GXPeekARGB is asynchronous, so one frame in flight is
 * safe. */
static void present_finish_pipelined(void)
{
	if (s_gf) gf_end_frame();
	GX2SwapScanBuffers();
	GX2Flush();
	static bool s_enabled;
	if (!s_enabled) {
		GX2SetTVEnable(TRUE);
		GX2SetDRCEnable(TRUE);
		s_enabled = true;
	}
}
/* Where beginRender's time goes, per 30 frames: the swap wait (and how many
 * vsyncs it took), the context-state switch, and swaps still pending on entry. */
static int64_t s_beginSwapWait, s_beginContext, s_beginMax;
static uint32_t s_beginVsyncs, s_beginPending, s_beginWaited;
static void present_begin_pipelined(void)
{
	const int64_t t0 = wut_OSGetTime_stub();
	for (uint32_t wait = 0;; ++wait) {
		uint32_t swaps = 0, flips = 0;
		OSTime lastFlip = 0, lastVsync = 0;
		GX2GetSwapStatus(&swaps, &flips, &lastFlip, &lastVsync);
		if (!wait)
			s_beginPending += swaps - flips;
		if (flips + (s_tribuf ? 2 : 1) >= swaps)
			break;
		if (!wait)
			++s_beginWaited;
		if (wait >= 10) {
			port_log("[present] wait for swap timed out (swaps=%u flips=%u)\n", unsigned(swaps), unsigned(flips));
			break;
		}
		GX2WaitForVsync();
		++s_beginVsyncs;
	}
	const int64_t t1 = wut_OSGetTime_stub();
	GX2SetContextState(WHBGfxGetTVContextState());
	if (s_gf) gf_stamp(0);
	const int64_t t2 = wut_OSGetTime_stub();
	s_beginSwapWait += t1 - t0;
	s_beginContext += t2 - t1;
	if (t2 - t0 > s_beginMax)
		s_beginMax = t2 - t0;
}

/* Real vsyncs, counted by GX2's vsync event. The VI emulation advances its
 * retrace count by the vsyncs that really passed and waits for one when none
 * has, so JDrama's waitForRetrace(2) holds the game to 30 fps as a GameCube's
 * VI did (20 fps when a frame overruns). Without it every emulated retrace
 * was free: the airstrip ran at ~40 fps, 1.33x speed (2026-10-20). The SD
 * marker "nopace" keeps the old free-running count for A/B runs. */
static volatile uint32_t s_vsyncs;
static uint32_t s_vsyncsSeen;
static int s_pace = -1;
static uint32_t s_paceCalls, s_paceWaits, s_paceFields;

static void on_vsync(GX2EventType, void*) { ++s_vsyncs; }

extern "C" uint32_t sms_wiiu_vsync_fields(void)
{
	if (!s_gfx_initialized || !ProcUIInForeground())
		return 1;
	if (s_pace < 0) {
		s_pace = !sms_wiiu_sd_marker("nopace");
		if (s_pace)
			GX2SetEventCallback(GX2_EVENT_TYPE_VSYNC, on_vsync, nullptr);
		s_vsyncsSeen = s_vsyncs;
		port_log("SMS Wii U: retrace pacing %s (SD marker nopace)\n", s_pace ? "on real vsyncs" : "off");
	}
	if (!s_pace)
		return 1;
	int32_t fields = int32_t(s_vsyncs - s_vsyncsSeen);
	if (fields <= 0) {
		GX2WaitForVsync();
		++s_paceWaits;
		/* The event callback may not have run yet: the wait itself is a field. */
		fields = int32_t(s_vsyncs - s_vsyncsSeen);
		if (fields < 1)
			fields = 1;
	}
	s_vsyncsSeen += uint32_t(fields);
	s_paceFields += uint32_t(fields);
	if (++s_paceCalls == 600) {
		port_log("SMS Wii U: [pace] 600 retraces = %u vsyncs, %u waited (vsync events %u)\n",
		         unsigned(s_paceFields), unsigned(s_paceWaits), unsigned(s_vsyncs));
		s_paceCalls = s_paceWaits = s_paceFields = 0;
	}
	return uint32_t(fields);
}

/* Every exit path drops the callback, or GX2 calls into an unmapped process. */
extern "C" void sms_wiiu_vsync_pace_stop(void)
{
	if (s_pace > 0)
		GX2SetEventCallback(GX2_EVENT_TYPE_VSYNC, nullptr, nullptr);
	s_pace = 0;
}

extern "C" void sms_wiiu_gfx_frame(void)
{
	log_active_once();
	/* After RELEASE_FOREGROUND, WHBProcIsRunning still returns true while the app
	 * runs in the background; any GX2 call then freezes the console (DKR). */
	if (!s_gfx_initialized)
		return;
	if (!ProcUIInForeground()) {
		s_was_foreground = false;
		return;
	}
	if (!s_was_foreground) {
		s_was_foreground = true;
		drc_setup(false);
	}
	if (!s_frame_open) {
		WHBGfxBeginRender();
		sms_wiiu_gx2_target_begin();
		sms_wiiu_gx2_renderer_begin();
		s_frame_open = true;
	}
	/* Retraces with nothing drawn keep the last picture on screen. */
	if (!sms_wiiu_gx2_renderer_has_draws())
		return;
	sms_wiiu_gx2_renderer_end();
	const int64_t t0 = wut_OSGetTime_stub();
	if (s_gf) gf_stamp(5);
	sms_wiiu_gx2_target_present();
	if (SMS_WIIU_PIPELINED_PRESENT) present_finish_pipelined();
	else WHBGfxFinishRender();
	const int64_t t1 = wut_OSGetTime_stub();
	drc_register_scan_buffer();
	present_diagnostics();
	if (SMS_WIIU_PIPELINED_PRESENT) present_begin_pipelined();
	else WHBGfxBeginRender();
	const int64_t t2 = wut_OSGetTime_stub();
	s_presentFinish += t1 - t0;
	s_presentBegin += t2 - t1;
	if (gx2prod::enabled()) {
		const uint32_t finishUs = uint32_t(OSTicksToMicroseconds(t1 - t0));
		const uint32_t beginUs = uint32_t(OSTicksToMicroseconds(t2 - t1));
		gx2prod::record_core1_present(finishUs + beginUs);
		gx2prod::record_core1_blocked(finishUs + beginUs);
	}
	if (++s_presentFrames == 30) {
		const int64_t tick100us = (248625000 / 4) / 10000;
		port_log("SMS Wii U: [present] per-frame x0.1ms finish(GPU drain)=%u beginRender(vsync)=%u "
		         "swapWait=%u context=%u maxBegin=%u /30: waited=%u vsyncs=%u pendingAtEntry=%u\n",
		         unsigned(s_presentFinish / 30 / tick100us), unsigned(s_presentBegin / 30 / tick100us),
		         unsigned(s_beginSwapWait / 30 / tick100us), unsigned(s_beginContext / 30 / tick100us),
		         unsigned(s_beginMax / tick100us), unsigned(s_beginWaited), unsigned(s_beginVsyncs),
		         unsigned(s_beginPending));
		s_presentFinish = s_presentBegin = 0;
		s_beginSwapWait = s_beginContext = s_beginMax = 0;
		s_beginWaited = s_beginVsyncs = s_beginPending = 0;
		s_presentFrames = 0;
	}
	sms_wiiu_gx2_texture_pack_pump();
	sms_wiiu_gx2_target_begin();
	sms_wiiu_gx2_renderer_begin();
	s_frame_open = true;
}

extern "C" {

int GXPC_ParseArgs(int*, char**) { return 0; }
void GXPC_SetHeadless(int) {}
void GXPC_PrintDisplayInfo(void) {}
int GXPC_InitAuto(float scale) { log_active_once(); return sms_wiiu_gx2_renderer_init() ? 1 : 0; }
int GXPC_IsHeadless(void) { return 1; }
void GXPC_SetAutoPresent(int) {}
void GXPC_Present(const void*) { log_active_once(); ++s_present_count; sms_wiiu_frame_boundary(); }
void GXPC_SkipNextPresent(int) {}
double GXPC_LastPresentSeconds(void) { return 0.0; }
uint32_t GXPC_FrameCount(void) { return sms_wiiu_frame_count(); }
void GXPC_SetWindowIcon(const uint8_t*, int, int) {}
int GXPC_MouseCaptured(void) { return 0; }
int GXPC_GameInputBlocked(void) { return 0; }
void GXPC_SetWidescreen(float width) { s_widescreen = width; }
void GXPC_SetStretch2D(int) {}
void GXPC_SetWide2D(int) {}
void GXPC_SetHud(int) {}
void GXPC_HudPaneBegin(float, float) {}
void GXPC_HudPaneEnd(void) {}
float GXPC_GetWidescreen(void) { return s_widescreen; }
void sms_gx_set_event_callback(void (*)(const union SDL_Event*)) {}
void sms_gx_pump_events(void) {}
int sms_gx_rumble(unsigned short) { return 0; }
int GXPC_Init(GXPCGetProcFn, float) { log_active_once(); return 1; }
void GXPC_Shutdown(void) {}
void GXPC_PrefetchResource(const void*, uint32_t, const char*) {}
void GXPC_PreloadTextures(void) {}
void GXPC_PrepareStage(int) {}

int GXPC_PresentXFB(const void*, int, int)
{
	log_active_once();
	++s_present_count;
	sms_wiiu_frame_boundary();
	return 1;
}

void GXPC_EndPresent(void) {}
void GXPC_ReadEFB(uint8_t*, int* w, int* h)
{
	if (w) *w = 0;
	if (h) *h = 0;
}
int GXPC_ReadXFB(const void*, uint8_t*, int* w, int* h)
{
	if (w) *w = 0;
	if (h) *h = 0;
	return 0;
}
void GXPC_GetStats(GXPCStats* out) { if (out) std::memset(out, 0, sizeof(*out)); }
void GXPC_GetLastFrameStats(GXPCStats* out) { if (out) std::memset(out, 0, sizeof(*out)); }
double GXPC_GxSeconds(void) { return 0.0; }
void GXPC_GetTimes(GXPCTimes* out) { if (out) std::memset(out, 0, sizeof(*out)); }
void GXPC_SetDetailedTimers(int) {}
void GXPC_SetIdleClock(double (*)(void)) {}
void GXPC_DrawOverlay(const uint8_t*, int, int, int, int, int, int, int) {}
void GXPC_OverlayToggle(void) {}
void GXPC_OverlayDraw(int, int) {}
void GXPC_OverlayFrame(void) {}
int GXPC_OverlayVisible(void) { return 0; }
void GXPC_CycleSpeed(void) {}
int GXPC_GetSpeed(void) { return 1; }

/* Ancillary weak interfaces used only when optional host menu sources slip
 * into a source list.  Stage 1 has no screenshots or prompt device. */
__attribute__((weak)) int GXPC_InputStyle(void) { return 0; }
__attribute__((weak)) uint8_t* GXPC_SaveStateThumbnail(size_t* size)
{
	if (size) *size = 0;
	return nullptr;
}
__attribute__((weak)) void GXPC_SetSaveStateApi(const void*) {}

} // extern "C"

/* Presents so far: pad.cpp's frame-clocked autoplay (SMS_AUTOPLAY_CLOCK=frames). */
extern "C" uint32_t sms_wiiu_presents(void) { return s_presented; }
