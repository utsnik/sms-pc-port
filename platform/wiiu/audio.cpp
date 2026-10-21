// Audio Interface (AI) for the Wii U: the DMA engine that plays JAudio's DAC
// buffers, output through SDL2's audio queue (wut's SDL2 drives AX).
//
// JAudio (JASAiCtrl.cpp / JASAudioThread.cpp) programs one DMA block at a time
// with AIInitDMA; the hardware latches the registers when a block starts and
// raises the DMA interrupt, whose callback (syncAudio) wakes the audio thread
// to mix the next block. The DSP side is platform/audio/dsp_hle.cpp (software
// mixer on the audio thread); this file is the clock.
//
// platform/audio/ai.cpp is the PC version. It is not used here: it loads SDL
// with dlopen, paces from pthreads and the emulated CPU's interrupt poll (on
// the Wii U port_irq_kick is a no-op and sources are polled once per video
// frame, which would run the 57 Hz DAC at 30 Hz), and it waits on
// OSThread::state, which the Wii U thread adapter never updates.
//
// Here a pacer thread runs beside the game on core 1 (so the callback's
// OSDisableInterrupts exclusion holds against game threads, see
// os_thread.cpp). Every few milliseconds it tops SDL's queue up to a small
// target: a block "starts" (its samples are queued, the DMA callback runs).
// The pacer has a lower priority than JAudio's audio thread (2), so the
// callback's message makes the audio thread preempt it and mix at once.
// SDL is opened with AUDIO_S16SYS (big-endian here), the DAC buffers' own
// byte order, so samples are not swapped; only the pair order is, since DMA
// pairs are (bus 2, bus 1) = (right, left).
// Without a device the pacer runs blocks from the system clock instead, so
// JAudio's sequencer keeps running (it is what frees stopped sequences).
#include "port_compat.h"
#include "port_os.h"
#include "runtime.h"

#include <dolphin/ai.h>
#include <dolphin/os.h>

#include <cstdint>
#include <cstring>

extern "C" int port_audio_dsp_frame_pending(void);
extern "C" int sms_wiiu_sdl_audio_open(int rate);
extern "C" void sms_wiiu_sdl_audio_queue(const void* data, uint32_t bytes);
extern "C" uint32_t sms_wiiu_sdl_audio_queued(void);
extern "C" void sms_wiiu_sdl_audio_close(void);
extern "C" u64 port_audio_dsp_subframes(void);
extern "C" void port_audio_dsp_shutdown(void);

extern "C" {
/* wut symbols bound by name (the game's OS names are remapped by CMake). */
int64_t sms_wiiu_audio_system_time(void) __asm__("OSGetSystemTime");
void sms_wiiu_audio_sleep_ticks(int64_t ticks) __asm__("OSSleepTicks");
}

namespace {

const u32 kRate        = 32000;
const u32 kBlockFrames = 560;      // JAudio's DAC block (0x460 samples), 17.5 ms
const u32 kBlockBytes  = kBlockFrames * 4;
const int kBaseBlocks  = 3;        // blocks kept queued (52 ms)
const int kMaxExtra    = 5;        // more after the queue ran dry: up to 8 (140 ms)
const int64_t kTicksPerSec = 62156250; // Wii U timebase (bus clock / 4)
const int64_t kPollTicks   = kTicksPerSec / 250;  // 4 ms
const int64_t kBlockTicks  = kTicksPerSec * kBlockFrames / kRate;
const u8 kPacerPriority    = 4;    // below the audio (2) and DVD (3) threads

struct Ai {
	bool inited;
	bool enabled; // not SMS_NO_AUDIO
	volatile bool running;
	volatile bool quit;    // shutdown requested
	volatile bool stopped; // the pacer has left its loop
	u32 start, length;
	u32 latchedStart, latchedLen;
	AIDCallback cb;
	u32 dspRate, streamRate;
	bool dev; // an SDL output device is open
	int extra;      // blocks queued beyond kBaseBlocks
	bool fed;       // a block has reached the queue
	bool dry;       // in a dry spell (queue empty while running)
	int64_t nextDue; // null clock: when the next block starts
	int64_t steadySince;
	// The device stopped draining the queue (no output in the emulator, the
	// HOME menu): blocks run from the clock until it drains again.
	bool stalled;
	u32 queuedAfter;  // queue level after the last top-up
	int64_t lastDrain; // when the queue last went down
	// Stats
	u32 blocks, underruns, pendingWaits;
	int64_t statsSince;
	u64 statsSubframes;
	u32 minQueued, maxQueued;
	u64 sumSquares, samples; // level of what was played
	u32 peak;
};

Ai g;
OSThread s_pacer;
u8 s_pacer_stack[0x10000] __attribute__((aligned(32)));
int16_t s_mix[kBlockFrames * 2] __attribute__((aligned(32)));

int target_blocks() { return kBaseBlocks + g.extra; }

// One DMA block starts: latch the registers, queue the samples, interrupt.
void dma_block()
{
	g.latchedStart = g.start;
	g.latchedLen   = g.length;
	const int16_t* src = (const int16_t*)(uintptr_t)g.latchedStart;
	u32 frames         = g.latchedLen / 4;
	if (frames > kBlockFrames)
		frames = kBlockFrames;
	if (g.dev && src && frames && !(g.stalled && sms_wiiu_sdl_audio_queued() > 16 * kBlockBytes)) {
		for (u32 i = 0; i < frames; i++) {
			s_mix[i * 2]     = src[i * 2 + 1];
			s_mix[i * 2 + 1] = src[i * 2];
		}
		for (u32 i = 0; i < frames * 2; i++) {
			int32_t v = s_mix[i];
			g.sumSquares += (u64)(v * v);
			u32 a = (u32)(v < 0 ? -v : v);
			if (a > g.peak)
				g.peak = a;
		}
		g.samples += frames * 2;
		sms_wiiu_sdl_audio_queue(s_mix, frames * 4);
		g.fed = true;
	}
	g.blocks++;
	if (g.cb) {
		BOOL level = OSDisableInterrupts(); // the callback ran in interrupt context
		g.cb();
		OSRestoreInterrupts(level);
	}
}

u32 isqrt(u64 v)
{
	u64 r = 0, bit = 1ull << 62;
	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (u32)r;
}

extern "C" void sms_wiiu_hang_check(void);
extern "C" void sms_wiiu_ax_load(u32* ppc, u32* dsp);
extern "C" volatile int sms_wiiu_crash_stage;

void log_stats(int64_t now)
{
	int64_t span = now - g.statsSince;
	if (span < kTicksPerSec * 10)
		return;
	u64 sub = port_audio_dsp_subframes();
	u32 ms  = (u32)(span * 1000 / kTicksPerSec);
	port_log("[audio] %u blocks in %u ms (%u.%02u/s, 57.14 nominal), %u DSP subframes, %u underruns, "
	         "queue %u..%u bytes, target %d blocks, %u frame waits, level rms %u peak %u\n",
	         g.blocks, ms, g.blocks * 1000u / ms, (g.blocks * 100000u / ms) % 100,
	         (unsigned)(sub - g.statsSubframes), g.underruns, g.minQueued, g.maxQueued, target_blocks(),
	         g.pendingWaits, (unsigned)(g.samples ? isqrt(g.sumSquares / g.samples) : 0), g.peak);
	{
		u32 ppc, dsp;
		sms_wiiu_ax_load(&ppc, &dsp);
		port_log("[audio] AX load ppc=%u dsp=%u\n", (unsigned)ppc, (unsigned)dsp);
	}
	g.sumSquares = g.samples = 0;
	g.peak       = 0;
	g.statsSince     = now;
	g.statsSubframes = sub;
	sms_wiiu_hang_check();
	if (sms_wiiu_crash_stage)
		port_log("[crash] recovery stage %d\n", sms_wiiu_crash_stage);
	g.blocks = g.underruns = g.pendingWaits = 0;
	g.minQueued = 0xFFFFFFFFu;
	g.maxQueued = 0;
}

void* pacer(void*)
{
	// Opened here, not in AIInit: opening blocks on AX/IPC, and AIInit runs on
	// JAudio's audio thread inside Kernel::init, before Dvd::init allocates
	// the buffer of the DVD message queue. Blocking there let the main thread
	// reach Dvd::dvdProcInit first and initialise that queue with no buffer
	// (signal 11 in send_message at the first sequence load).
	// A/B switch (SD marker "noaudio"): no SDL device, so no AX voices, to
	// measure what the system's AX thread costs core 1.
	const bool noAudio = sms_wiiu_sd_marker("noaudio");
	if (noAudio)
		port_log("[audio] SD marker noaudio: SDL audio not opened\n");
	g.dev = !noAudio && sms_wiiu_sdl_audio_open(kRate) != 0;
	if (!g.dev)
		port_log("[audio] no audio device: pacing AI DMA from the system clock\n");
	g.statsSince = sms_wiiu_audio_system_time();
	g.minQueued  = 0xFFFFFFFFu;
	while (!g.quit) {
		sms_wiiu_audio_sleep_ticks(kPollTicks);
		if (g.quit)
			break;
		int64_t now = sms_wiiu_audio_system_time();
		log_stats(now);
		if (!g.running) {
			g.nextDue = 0;
			continue;
		}
		// The audio thread is mid-frame (subframes still to render): on
		// hardware the DSP finishes a frame within a block period, and
		// starting blocks faster than JAudio makes frames repeats DSPBuf's
		// stale samples. The audio thread outranks this one, so this is rare.
		if (port_audio_dsp_frame_pending()) {
			g.pendingWaits++;
			continue;
		}
		u32 queued = g.dev ? sms_wiiu_sdl_audio_queued() : 0;
		if (g.dev) {
			if (queued < g.queuedAfter || !g.lastDrain)
				g.lastDrain = now;
			bool stalled = queued && now - g.lastDrain > kTicksPerSec / 2;
			if (stalled != g.stalled) {
				g.stalled = stalled;
				port_log("[audio] output %s; %s\n", stalled ? "stopped draining" : "drains again",
				         stalled ? "pacing AI DMA from the system clock" : "pacing from the queue");
				g.nextDue = 0;
			}
		}
		if (!g.dev || g.stalled) {
			if (!g.nextDue || now - g.nextDue > kBlockTicks * 4)
				g.nextDue = now; // a stalled game does not get a burst later
			while (now >= g.nextDue && !port_audio_dsp_frame_pending()) {
				dma_block();
				g.nextDue += kBlockTicks;
			}
			g.queuedAfter = g.dev ? sms_wiiu_sdl_audio_queued() : 0;
			continue;
		}
		if (queued < g.minQueued)
			g.minQueued = queued;
		if (queued > g.maxQueued)
			g.maxQueued = queued;
		if (queued == 0 && g.fed) {
			if (!g.dry) {
				g.dry = true;
				g.underruns++;
				if (g.extra < kMaxExtra) {
					g.extra++;
					port_log("[audio] the output ran dry; queueing %d blocks (%u ms) from now on\n",
					         target_blocks(), (unsigned)(target_blocks() * kBlockFrames * 1000 / kRate));
				}
			}
			g.steadySince = now;
		} else if (queued) {
			g.dry = false;
			// One extra block back after 30 s without running dry.
			if (g.extra > 0 && now - g.steadySince > kTicksPerSec * 30) {
				g.extra--;
				g.steadySince = now;
			}
		}
		// Top up. Each callback wakes the audio thread, which preempts this
		// thread and mixes before the next block is started.
		int n = 0;
		while (queued < (u32)target_blocks() * kBlockBytes && n < kBaseBlocks + kMaxExtra &&
		       !port_audio_dsp_frame_pending() && g.running) {
			dma_block();
			n++;
			queued = sms_wiiu_sdl_audio_queued();
		}
		g.queuedAfter = queued;
	}
	g.stopped = true;
	return NULL;
}

} // namespace

// Called from the main thread on exit to the Menu: stop block generation,
// wait (bounded) for the pacer to leave its loop, then close the SDL device.
// If the pacer does not stop in time the device is left open (the process is
// about to end anyway) rather than closed under a live SDL_QueueAudio.
extern "C" void sms_wiiu_audio_shutdown(void)
{
	g.running = false;
	g.quit    = true;
	if (g.inited && g.enabled) {
		for (int i = 0; i < 100 && !g.stopped; i++)
			sms_wiiu_audio_sleep_ticks(kTicksPerSec / 100); // up to 1 s
		if (g.stopped) {
			g.dev = false;
			sms_wiiu_sdl_audio_close();
		}
		port_log("[audio] shut down (pacer %s)\n", g.stopped ? "stopped, SDL closed" : "did not stop; SDL left open");
	}
	port_audio_dsp_shutdown();
}

extern "C" void AIInit(u8*)
{
	if (g.inited)
		return;
	g.inited  = true;
	g.enabled = !port_no_audio;
	if (!g.enabled) {
		port_log("[audio] SMS_NO_AUDIO: AI DMA idle (no mixing, no output)\n");
		return;
	}
	if (!OSCreateThread(&s_pacer, pacer, NULL, s_pacer_stack + sizeof s_pacer_stack, sizeof s_pacer_stack,
	                    kPacerPriority, 1)) {
		port_log("[audio] could not start the AI pacer thread\n");
		return;
	}
	OSResumeThread(&s_pacer);
}
extern "C" BOOL AICheckInit(void) { return g.inited; }
extern "C" void AIReset(void) {}
extern "C" AIDCallback AIRegisterDMACallback(AIDCallback cb)
{
	AIDCallback old = g.cb;
	g.cb            = cb;
	return old;
}
extern "C" void AIInitDMA(u32 start_addr, u32 length)
{
	g.start  = start_addr;
	g.length = length;
}
extern "C" BOOL AIGetDMAEnableFlag(void) { return g.running; }
extern "C" void AIStartDMA(void)
{
	if (g.enabled)
		g.running = true;
}
extern "C" void AIStopDMA(void) { g.running = false; }
extern "C" u32 AIGetDMABytesLeft(void) { return 0; }
extern "C" u32 AIGetDMAStartAddr(void) { return g.latchedStart; }
extern "C" u32 AIGetDMALength(void) { return g.latchedLen; }
extern "C" void AISetDSPSampleRate(u32 rate) { g.dspRate = rate; }
extern "C" u32 AIGetDSPSampleRate(void) { return g.dspRate; }
// DVD audio streaming (DTK): retail SMS never streams.
extern "C" void AISetStreamSampleRate(u32 rate) { g.streamRate = rate; }
extern "C" u32 AIGetStreamSampleRate(void) { return g.streamRate; }
