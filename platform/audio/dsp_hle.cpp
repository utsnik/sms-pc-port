// High-level emulation of the JAudio DSP task's mail protocol (PROTOCOL.md).
//
// The CPU side is decomp code (JSystem/dsptask.c, dspproc.c, osdsp_task.c,
// JASAudioThread.cpp): commands go out as a word count, DSPAssertInt, then the
// words; the DSP answers through the mailbox plus the DSP interrupt, whose
// handler (__DSPHandler) takes 0xDCD1000x task mails and hands 0xDCD10004
// "request" mails to the audio task's req_cb (syncDSP), which reads a second
// mail 0xF355xxxx: 0xF355FF00 = subframe rendered, anything else = command
// acknowledged (DspFinishWork(low 16 bits)).
//
// This file always builds the port_audio_dsp_* entry points. The SDK-named
// DSP functions (DSPSendMailToDSP, __DSP_boot_task, ...) are only defined
// with SMS_AUDIO_DSP_HLE, because the bring-up fake DSP in
// platform/misc/sdk_data.cpp defines them today; README.md has the switch-over.
#include "port_compat.h"
#include "port_os.h"
#include "port_platform.h"
#include <dolphin/dsp.h>

#include "dsp_mixer.h"
#ifdef TARGET_WIIU
/* The mailbox deque and command vector grow from game threads. Outside a
 * PortHeapScope the Wii U operator new takes the game's current JKR heap, so a
 * deque block landed in a heap the game later reset and reused for
 * JPAEmitterManager's emitter pool, and each subframe's mail pair
 * (0xDCD10004, 0xF355FF00) overwrote the free list: the 1-in-2 DSI at Mario's
 * first step on file select (2026-10-20). */
#include "../wiiu/port_heap.h"
#include "../wiiu/runtime.h"
#include <dolphin/os.h>
#include <dolphin/os/OSThread.h>
#else
#define SMS_PORT_HEAP_SCOPE() ((void)0)
#endif

#include <cstring>
#include <deque>
#include <new>
#include <string>
#include <vector>

extern "C" u8* port_aram_ptr(u32 addr);
extern "C" void __DSPHandler(int interrupt, void* context); // JSystem/osdsp_task.c

namespace {

const u32 kPrefix = 0xF3550000u; // JASystem::DSPInterface::JAS_DSP_PREFIX << 16

#ifdef TARGET_WIIU
volatile u32 s_dsp_setup_ready;
#endif

struct Hle {
	std::deque<u32> fromDsp; // mailbox DSP -> CPU
	// command assembly
	u32 lastMail;
	bool inCmd;
	bool release; // header word 0: DSPReleaseHalt's two map words
	u32 want;
	std::vector<u32> words;
#ifdef TARGET_WIIU
	std::vector<u32> workerWords;
#endif
	// frame state from the 0x82 command
	int16_t* outA;
	int16_t* outB;
	u32 subframes, sub, subSamples;
	u16 master;
	u64 subframesRendered;
	bool traced;
};

#ifdef TARGET_WIIU
/* No global constructor: the deque allocates, and code that runs before main()
 * is where a Wii U boot dies silently. Build the state on first use. */
Hle& hle_state()
{
	alignas(Hle) static unsigned char storage[sizeof(Hle)];
	static Hle* state;
	if (!state) {
		SMS_PORT_HEAP_SCOPE();
		state = new (storage) Hle();
	}
	return *state;
}
#define g (hle_state())
#else
Hle g;
#endif

const uint8_t* aram_lookup(uint32_t a) { return port_aram_ptr(a); }

void post(u32 m)
{
	SMS_PORT_HEAP_SCOPE();
	g.fromDsp.push_back(m);
}

#ifdef TARGET_WIIU
/* wut's coreinit ABI is deliberately kept separate from the decomp's
 * GameCube OS types. These declarations mirror the boundary in
 * platform/wiiu/os_thread.cpp and are bound to wut by symbol name. */
struct WutThread {
	unsigned char bytes[0x6a0];
} __attribute__((aligned(32)));
struct WutMessageQueue {
	unsigned char bytes[0x100];
} __attribute__((aligned(32)));
typedef int (*WutEntry)(int, const char**);

extern "C" {
BOOL wut_OSCreateThread(WutThread*, WutEntry, int32_t, char*, void*, uint32_t,
	                        int32_t, uint8_t) __asm__("OSCreateThread");
int32_t wut_OSResumeThread(WutThread*) __asm__("OSResumeThread");
BOOL wut_OSJoinThread(WutThread*, int*) __asm__("OSJoinThread");
/* wut's OSMessage: a pointer and three words (16 bytes). The queue calls copy
 * whole messages, so a bare pointer passed as one made the receiver read the
 * Reply's first mail word (0xDCD10000) as its pointer: a DSI at boot
 * (2026-10-21 03:12). */
struct WutMessage {
	void* message;
	uint32_t args[3];
};
void wut_OSInitMessageQueue(WutMessageQueue*, WutMessage*, int32_t) __asm__("OSInitMessageQueue");
BOOL wut_OSSendMessage(WutMessageQueue*, WutMessage*, int32_t) __asm__("OSSendMessage");
BOOL wut_OSReceiveMessage(WutMessageQueue*, WutMessage*, int32_t) __asm__("OSReceiveMessage");

int sms_wiiu_OSCreateThread(OSThread*, void* (*)(void*), void*, void*, u32, s32, u16);
long sms_wiiu_OSResumeThread(OSThread*);
int sms_wiiu_OSJoinThread(OSThread*, void**);
int sms_wiiu_sd_marker(const char* name);
}

#ifndef OS_THREAD_ATTRIB_AFFINITY_CPU0
#define OS_THREAD_ATTRIB_AFFINITY_CPU0 (1u << 0)
#endif
#ifndef OS_THREAD_ATTRIB_AFFINITY_CPU2
#define OS_THREAD_ATTRIB_AFFINITY_CPU2 (1u << 2)
#endif

const int32_t kWutMessageBlock = 1;
const int kCommandQueueCount = 32;
const int kReplyQueueCount = 32;
const s32 kMixerPriority = 2;
const s32 kReplyPriority = 1; // above JAudio's priority 2

struct Command {
	std::vector<u32> words;
	bool release;
};

struct Reply {
	u32 first;
	u32 second;
};

struct AsyncState {
	WutMessageQueue commandQueue;
	WutMessageQueue replyQueue;
	WutMessage commandSlots[kCommandQueueCount] __attribute__((aligned(32)));
	WutMessage replySlots[kReplyQueueCount] __attribute__((aligned(32)));
	WutThread mixerThread;
	OSThread replyThread;
	u8 mixerStack[0x10000] __attribute__((aligned(32)));
	u8 replyStack[0x10000] __attribute__((aligned(32)));
	volatile u32 framePending;
	volatile u32 subframesRendered;
	bool checked;
	bool active;
	bool mixerStarted;
	bool replyStarted;
};

AsyncState s_async;
volatile u32 s_syncFrames;
// Async-path debug counters (logged with the 10 s audio stats).
volatile u32 s_dbgMails, s_dbgAsserts, s_dbgLastAssertMail, s_dbgSkippedMails;
volatile u32 s_dbgQueued[4], s_dbgExec[4], s_dbgReleaseIdle, s_dbgReplies, s_dbgReplyPosts;
inline int dbg_kind(bool release, const std::vector<u32>& w)
{
	if (release) return 0;
	if (w.empty()) return 3;
	const u32 op = w[0] >> 24;
	return op == 0x82 ? 1 : op == 0x81 ? 2 : 3;
}

BOOL send_ptr(WutMessageQueue* q, void* p)
{
	WutMessage m = {p, {0, 0, 0}};
	return wut_OSSendMessage(q, &m, kWutMessageBlock);
}

BOOL receive_ptr(WutMessageQueue* q, void** p)
{
	WutMessage m = {nullptr, {0, 0, 0}};
	const BOOL ok = wut_OSReceiveMessage(q, &m, kWutMessageBlock);
	*p = m.message;
	return ok;
}
bool s_workerRelease;

/* fromWorker: called by the mixer thread. It is an argument, not a shared flag:
 * a ReleaseHalt arriving on core 1 while the worker ran the 0x82 frame command
 * read the worker's flag, re-ran the 0x82 inline and dropped the release, so no
 * subframe was ever rendered and the game was silent from the first frame. */
void execute(bool fromWorker = false);

void queue_reply(u32 first, u32 second)
{
	SMS_PORT_HEAP_SCOPE();
	Reply* reply = new Reply;
	if (!reply)
		return;
	reply->first = first;
	reply->second = second;
	if (!send_ptr(&s_async.replyQueue, reply)) {
		delete reply;
		port_log("[audio] DSP reply queue failed\n");
	}
}

void* reply_thread(void*)
{
	for (;;) {
		Reply* reply = nullptr;
		if (!receive_ptr(&s_async.replyQueue, reinterpret_cast<void**>(&reply)))
			continue;
		if (!reply)
			break;
		++s_dbgReplyPosts;
		BOOL level = OSDisableInterrupts();
		post(reply->first);
		post(reply->second);
		__DSPHandler(0, NULL);
		OSRestoreInterrupts(level);
		{
			SMS_PORT_HEAP_SCOPE();
			delete reply;
		}
	}
	return nullptr;
}

int mixer_thread(int, const char** argv)
{
	AsyncState* state = reinterpret_cast<AsyncState*>(
		const_cast<void*>(reinterpret_cast<const void*>(argv)));
	for (;;) {
		Command* command = nullptr;
		if (!receive_ptr(&state->commandQueue, reinterpret_cast<void**>(&command)))
			continue;
		if (!command)
			break;
		{
			SMS_PORT_HEAP_SCOPE();
			g.workerWords.swap(command->words);
			s_workerRelease = command->release;
			++s_dbgExec[dbg_kind(s_workerRelease, g.workerWords)];
			execute(true);
			g.workerWords.clear();
			delete command;
		}
	}
	return 0;
}

bool start_async()
{
	if (s_async.checked)
		return s_async.active;
	s_async.checked = true;
	if (sms_wiiu_sd_marker("syncmix")) {
		port_log("[audio] DSP HLE mode: synchronous (SD marker syncmix)\n");
		return false;
	}
	std::memset(&s_async, 0, sizeof(s_async));
	s_async.checked = true;
	wut_OSInitMessageQueue(&s_async.commandQueue, s_async.commandSlots, kCommandQueueCount);
	wut_OSInitMessageQueue(&s_async.replyQueue, s_async.replySlots, kReplyQueueCount);
	if (!sms_wiiu_OSCreateThread(&s_async.replyThread, reply_thread, nullptr,
	                            s_async.replyStack + sizeof(s_async.replyStack),
	                            sizeof(s_async.replyStack), kReplyPriority, 0)) {
		port_log("[audio] DSP HLE: reply thread creation failed; using synchronous mode\n");
		return false;
	}
	s_async.replyStarted = true;
	s_async.active = true;
	sms_wiiu_OSResumeThread(&s_async.replyThread);
	// The mixer runs on core 2 (AX, texture-pack loader).  On core 0, at
	// priority 2, it preempted the GX2 producer worker (15-22 pct of core 0 in
	// Gelato and Pianta; Gelato 49.5 -> 33.9 ms, 2026-10-27).  SD marker
	// "mixcore0" puts it back.
	const bool mixCore2 = sms_wiiu_sd_marker("mixcore0") == 0;
	if (!wut_OSCreateThread(&s_async.mixerThread, mixer_thread, 0,
	                        reinterpret_cast<char*>(&s_async),
	                        s_async.mixerStack + sizeof(s_async.mixerStack),
	                        sizeof(s_async.mixerStack), kMixerPriority,
	                        mixCore2 ? OS_THREAD_ATTRIB_AFFINITY_CPU2 : OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
		s_async.active = false;
		send_ptr(&s_async.replyQueue, nullptr);
		sms_wiiu_OSJoinThread(&s_async.replyThread, nullptr);
		s_async.replyStarted = false;
		port_log("[audio] DSP HLE: mixer thread creation failed; using synchronous mode\n");
		return false;
	}
	s_async.mixerStarted = true;
	wut_OSResumeThread(&s_async.mixerThread);
	port_log("[audio] DSP HLE mode: asynchronous (mixer core %d, replies core 1)\n", mixCore2 ? 2 : 0);
	return true;
}

void stop_async()
{
	if (!s_async.active)
		return;
	/* Stop follows all queued commands, so the reply stop marker follows all
	 * mails produced by the mixer. */
	send_ptr(&s_async.commandQueue, nullptr);
	if (s_async.mixerStarted) {
		wut_OSJoinThread(&s_async.mixerThread, nullptr);
		s_async.mixerStarted = false;
	}
	s_async.active = false;
	if (s_async.replyStarted) {
		send_ptr(&s_async.replyQueue, nullptr);
		sms_wiiu_OSJoinThread(&s_async.replyThread, nullptr);
		s_async.replyStarted = false;
	}
	port_log("[audio] DSP HLE asynchronous threads stopped\n");
}
#endif

// Raise the DSP interrupt: runs the decomp's __DSPHandler in interrupt
// context at the next check point (the sender still has interrupts off).
void raise_irq(u32 first, u32 second)
{
#ifdef TARGET_WIIU
	if (s_async.active) {
		++s_dbgReplies;
		queue_reply(first, second);
		return;
	}
#endif
	port_irq_defer([first, second]() {
		post(first);
		post(second);
		__DSPHandler(0, NULL);
	});
}

void reply(u32 low16) { raise_irq(0xDCD10004u, kPrefix | low16); }

// Debug: SMS_AUDIO_ARAM_DUMP=path[,subframe] writes the 16 MiB ARAM image once
// (default after 40 s of audio) for offline mixer runs (tests/, PROTOCOL.md).
void maybe_dump_aram()
{
	static int state; // 0 unchecked, 1 armed, 2 done
	static u64 at;
	static const char* path;
	if (state == 0) {
		path  = getenv("SMS_AUDIO_ARAM_DUMP");
		state = path && *path ? 1 : 2;
		const char* c = path ? strchr(path, ',') : NULL;
		at            = c ? strtoull(c + 1, NULL, 10) : 16000;
	}
	if (state != 1 || g.subframesRendered < at)
		return;
	state = 2;
	std::string fn(path, strchr(path, ',') ? strchr(path, ',') - path : strlen(path));
	if (FILE* f = fopen(fn.c_str(), "wb")) {
		fwrite(port_aram_ptr(0), 1, 16u << 20, f);
		fclose(f);
		port_log("[audio] ARAM image written to %s\n", fn.c_str());
	}
}

void render_subframe()
{
	maybe_dump_aram();
	u32 n = g.subSamples;
#ifdef TARGET_WIIU
	// A/B switch (SD marker "nomix"): silence instead of the mixer, to
	// measure what mixing on the audio thread costs core 1.
	static int s_noMix = -1;
	if (s_noMix < 0) {
		s_noMix = sms_wiiu_sd_marker("nomix");
		if (s_noMix)
			port_log("[audio] SD marker nomix: DSP mixer skipped (silence)\n");
	}
	if (s_noMix) {
		std::memset(g.outA + g.sub * n, 0, n * sizeof(int16_t));
		std::memset(g.outB + g.sub * n, 0, n * sizeof(int16_t));
	} else
#endif
	port_dspmix_render(g.outA + g.sub * n, g.outB + g.sub * n, (int)n, g.master);
	g.sub++;
	g.subframesRendered++;
#ifdef TARGET_WIIU
	if (s_async.active) {
		s_async.subframesRendered++;
		if (g.sub >= g.subframes) {
			__sync_synchronize();  // the samples are visible to core 1 before the flag clears
			s_async.framePending = 0;
		}
	}
#endif
	reply(0xFF00);
}

void execute(bool fromWorker)
{
	#ifdef TARGET_WIIU
	if (s_async.active && !fromWorker) {
		SMS_PORT_HEAP_SCOPE();
		Command* command = new Command;
		if (!command)
			return;
		command->words = g.words;
		command->release = g.release;
		++s_dbgQueued[dbg_kind(g.release, g.words)];
		/* A frame is pending from its 0x82 command, as in the synchronous
		 * path: the pacer must not start another DMA block in the window
		 * before the mixer reaches the frame's first subframe. */
		if (!g.release && !g.words.empty() && (g.words[0] >> 24) == 0x82 && g.words.size() >= 3 &&
		    ((g.words[0] >> 16) & 0xFF) != 0)
			s_async.framePending = 1;
		if (!send_ptr(&s_async.commandQueue, command)) {
			delete command;
			port_log("[audio] DSP command queue failed\n");
		}
		return;
	}
	const std::vector<u32>& w = fromWorker ? g.workerWords : g.words;
	bool release = fromWorker ? s_workerRelease : g.release;
	#else
	const std::vector<u32>& w = g.words;
	#endif
	u32 op = w[0] >> 24;
	if (
#ifdef TARGET_WIIU
		release
#else
		g.release
#endif
	) {
		// DSPReleaseHalt: the 64-bit active-voice map. The microcode renders
		// the next subframe of the current frame and reports 0xFF00.
		if (g.outA && g.sub < g.subframes)
			render_subframe();
#ifdef TARGET_WIIU
		else
			++s_dbgReleaseIdle;
#endif
		return;
	}
	switch (op) {
	case 0x81: // DsetupTable(n, CH_BUF, DSPRES_FILTER, DSPADPCM_FILTER, FX_BUF)
		if (w.size() >= 5) {
			port_dspmix_set_aram(aram_lookup);
			port_dspmix_setup(w[0] & 0xFFFF, (void*)(uintptr_t)w[1], (const uint32_t*)(uintptr_t)w[2],
			                  (const uint32_t*)(uintptr_t)w[3], (void*)(uintptr_t)w[4]);
#ifdef TARGET_WIIU
			__sync_synchronize();
			s_dsp_setup_ready = 1;
#endif
			port_log("[audio] DSP setup: %u voices at 0x%08x, FX lines at 0x%08x\n", (unsigned)(w[0] & 0xFFFF),
			         (unsigned)w[1], (unsigned)w[4]);
		}
		reply(w[0] >> 16);
		break;
	case 0x82: // DsyncFrame(subframes, out A, out B) | mixer level
		if (w.size() >= 3) {
			g.subframes = (w[0] >> 16) & 0xFF;
			g.master    = (u16)(w[0] & 0xFFFF);
			g.outA      = (int16_t*)(uintptr_t)w[1];
			g.outB      = (int16_t*)(uintptr_t)w[2];
			// The two outputs are the halves of one frame buffer.
			u32 frame    = (w[2] - w[1]) / 2;
			g.subSamples = g.subframes ? frame / g.subframes : 0;
			g.sub        = 0;
			++s_syncFrames;
#ifdef TARGET_WIIU
			if (s_async.active)
				s_async.framePending = g.subframes != 0;
#endif
			if (!g.traced) {
				g.traced = true;
				port_log("[audio] first DSP frame: %u subframes x %u samples, mixer level 0x%04x\n",
				         (unsigned)g.subframes, (unsigned)g.subSamples, (unsigned)g.master);
			}
		}
		break;
	default:
		port_log("[audio] DSP: unknown command 0x%08x (%zu words)\n", (unsigned)w[0], w.size());
		reply(w[0] >> 16);
		break;
	}
}

} // namespace

#ifdef TARGET_WIIU
extern "C" void port_audio_dsp_wait_for_setup(void)
{
	const OSTime deadline = OSGetTime() + OSMicrosecondsToTicks(1000000);
	while (!s_dsp_setup_ready && OSGetTime() < deadline)
		OSYieldThread();
	if (!s_dsp_setup_ready)
		port_log("[audio] DSP setup wait timed out after 1000 ms\n");
}
#endif

extern "C" void port_audio_dsp_boot(DSPTaskInfo* task)
{
	// Task start: the microcode mails 0xDCD10000 (the handler then runs the
	// task's init_cb, DspHandShake, which waits for one more mail).
	(void)task;
#ifdef TARGET_WIIU
	start_async();
#endif
	raise_irq(0xDCD10000u, kPrefix);
	port_log("[audio] DSP HLE: audio task booted (software mixer)\n");
}

extern "C" u32 port_audio_dsp_check_mail_from(void)
{
	return g.fromDsp.empty() ? 0 : 0x80000000u | (g.fromDsp.front() >> 16);
}

extern "C" u32 port_audio_dsp_read_mail_from(void)
{
	if (g.fromDsp.empty())
		return 0;
	u32 m = g.fromDsp.front();
	g.fromDsp.pop_front();
	return m;
}

extern "C" void port_audio_dsp_mail_to(u32 mail)
{
	g.lastMail = mail;
#ifdef TARGET_WIIU
	++s_dbgMails;
	if (!g.inCmd)
		++s_dbgSkippedMails;
#endif
	if (!g.inCmd)
		return;
	SMS_PORT_HEAP_SCOPE();
	g.words.push_back(mail);
	if (g.words.size() == g.want) {
		g.inCmd = false;
		execute();
	}
}

extern "C" void port_audio_dsp_assert_int(void)
{
	// DSPSendCommands2 sends the word count, then asserts; 0 means 2 words.
#ifdef TARGET_WIIU
	++s_dbgAsserts;
	s_dbgLastAssertMail = g.lastMail;
#endif
	g.release = g.lastMail == 0;
	g.want    = g.lastMail ? g.lastMail : 2;
	g.inCmd   = true;
	g.words.clear();
}

extern "C" u64 port_audio_dsp_subframes(void)
{
#ifdef TARGET_WIIU
	if (s_async.active)
		return s_async.subframesRendered;
#endif
	return g.subframesRendered;
}

// True while a frame started by 0x82 still has subframes to render. The AI
// layer does not start another DMA block until it is done: on hardware the
// DSP always finishes a frame within one block period, and consuming blocks
// faster than JAudio produces frames makes DSPBuf repeat stale samples (gaps)
// while the sequencer falls behind.
#ifdef TARGET_WIIU
namespace JASystem { namespace AudioThread {
u32 getDSPSyncCount();
extern OSThread jac_audioThread;
} }
extern "C" int sms_wiiu_OSIsThreadTerminated(OSThread*);
#endif
extern "C" void port_audio_dsp_debug_log(void)
{
#ifdef TARGET_WIIU
	if (!s_async.active)
		return;
	port_log("[audio] async: queued rel/82/81/other %u/%u/%u/%u exec %u/%u/%u/%u releaseIdle %u replies %u posts %u "
	         "pending %u sub %u/%u fromDsp %u mails %u skipped %u asserts %u lastAssertMail %u inCmd %d want %u have %u "
	         "intcount %u audioThreadDead %d\n",
	         (unsigned)s_dbgQueued[0], (unsigned)s_dbgQueued[1], (unsigned)s_dbgQueued[2], (unsigned)s_dbgQueued[3],
	         (unsigned)s_dbgExec[0], (unsigned)s_dbgExec[1], (unsigned)s_dbgExec[2], (unsigned)s_dbgExec[3],
	         (unsigned)s_dbgReleaseIdle, (unsigned)s_dbgReplies, (unsigned)s_dbgReplyPosts,
	         (unsigned)s_async.framePending, (unsigned)g.sub, (unsigned)g.subframes, (unsigned)g.fromDsp.size(),
	         (unsigned)s_dbgMails, (unsigned)s_dbgSkippedMails, (unsigned)s_dbgAsserts, (unsigned)s_dbgLastAssertMail,
	         (int)g.inCmd, (unsigned)g.want, (unsigned)g.words.size(),
	         (unsigned)JASystem::AudioThread::getDSPSyncCount(),
	         sms_wiiu_OSIsThreadTerminated(&JASystem::AudioThread::jac_audioThread));
#endif
}

// 0x82 frame commands seen so far (queued, in the asynchronous mode).
extern "C" u32 port_audio_dsp_frames_started(void)
{
#ifdef TARGET_WIIU
	if (s_async.active)
		return s_dbgQueued[1];
#endif
	return s_syncFrames;
}

extern "C" int port_audio_dsp_frame_pending(void)
{
#ifdef TARGET_WIIU
	if (s_async.active) {
		const int pending = s_async.framePending != 0;
		__sync_synchronize();  // reads of the frame's samples come after the flag
		return pending;
	}
#endif
	return g.outA && g.sub < g.subframes;
}

#ifdef TARGET_WIIU
extern "C" void port_audio_dsp_shutdown(void) { stop_async(); }
#endif

#ifdef SMS_AUDIO_DSP_HLE
// SDK surface (replaces the handshake-only fake in platform/misc/sdk_data.cpp).
DSPTaskInfo* __DSP_curr_task;
DSPTaskInfo* __DSP_first_task;
DSPTaskInfo* __DSP_last_task;
extern "C" void DSPInit(void) {}
extern "C" u32 DSPCheckMailToDSP(void) { return 0; } // always drained at once
extern "C" u32 DSPCheckMailFromDSP(void) { return port_audio_dsp_check_mail_from(); }
extern "C" u32 DSPReadMailFromDSP(void) { return port_audio_dsp_read_mail_from(); }
extern "C" void DSPSendMailToDSP(u32 mail) { port_audio_dsp_mail_to(mail); }
extern "C" void DSPAssertInt(void) { port_audio_dsp_assert_int(); }
extern "C" void __DSP_boot_task(DSPTaskInfo* task)
{
	__DSP_curr_task = task;
	port_audio_dsp_boot(task);
}
// SMS runs only the audio task: no task switching to emulate.
extern "C" void __DSP_insert_task(DSPTaskInfo*) {}
extern "C" void __DSP_exec_task(DSPTaskInfo*, DSPTaskInfo*) {}
extern "C" void __DSP_remove_task(DSPTaskInfo*) {}
#endif
