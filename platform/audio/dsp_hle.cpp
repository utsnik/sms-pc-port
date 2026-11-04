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
#if defined(TARGET_WIIU) || defined(SMG_AUDIO_ASYNC_TEST)
#define SMG_DSP_ASYNC 1
#endif
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
#elif defined(SMG_AUDIO_ASYNC_TEST)
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdlib>
#define SMS_PORT_HEAP_SCOPE() ((void)0)
#else
#define SMS_PORT_HEAP_SCOPE() ((void)0)
#endif

#if defined(PORT_GAME_SMG) && defined(SMG_DSP_ASYNC)
extern "C" void* port_host_alloc(size_t, int);
extern "C" void port_host_free(void*);
#endif

#include <cstring>
#include <deque>
#include <new>
#include <string>
#include <vector>

extern "C" u8* port_aram_ptr(u32 addr);
#if defined(PORT_GAME_SMG)
extern "C" const u8* port_varam_ptr(u32 addr);
#endif
extern "C" void __DSPHandler(int interrupt, void* context); // JSystem/osdsp_task.c

#ifdef TARGET_WIIU
extern "C" void __real_DCFlushRange(void*, uint32_t); // coreinit's, not the GX-stamping wrapper (os_core.cpp)
#endif

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
#if defined(SMG_DSP_ASYNC)
	std::vector<u32> workerWords;
#endif
	// frame state from the 0x82 command
	int16_t* outA;
	int16_t* outB;
	void* voices;
	u32 nvoices;
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

#if defined(PORT_GAME_SMG)
/* smg-audio: D3. Galaxy's DSP addresses ARAM relative to the base JASDsp::boot
 * sends with DsetVARAM (0x8E, dspproc.cpp): JKRHeap::getAltAramStartAdr(), the
 * MEM2 arena low mark that HeapMemoryWatcher::createRootHeap reserves and
 * RVL_SDK aralt's ARInit maps ARAM offset 0 to. port_varam_ptr resolves that
 * main-memory address (NULL outside the window); before 0x8E nothing resolves. */
u32 s_varam;
const uint8_t* aram_lookup(uint32_t a) { return s_varam ? port_varam_ptr(s_varam + a) : NULL; }
#else
const uint8_t* aram_lookup(uint32_t a) { return port_aram_ptr(a); }
#endif

#if defined(PORT_GAME_SMG)
/* Both the target's asynchronous queue and the host's synchronous fallback
 * publish these counters.  The ordering contract is mail -> render -> reply. */
volatile u32 s_subframeMails, s_subframeRenders, s_subframeReplies;
#endif

void post(u32 m)
{
	SMS_PORT_HEAP_SCOPE();
	g.fromDsp.push_back(m);
}

volatile u32 s_syncFrames;
/* fromWorker: called by the mixer thread. It is an argument, not a shared flag:
 * a ReleaseHalt arriving on core 1 while the worker ran the 0x82 frame command
 * read the worker's flag, re-ran the 0x82 inline and dropped the release, so no
 * subframe was ever rendered and the game was silent from the first frame. */
void execute(bool fromWorker = false);

#if defined(SMG_DSP_ASYNC)
/* wut's coreinit ABI is deliberately kept separate from the decomp's
 * GameCube OS types. These declarations mirror the boundary in
 * platform/wiiu/os_thread.cpp and are bound to wut by symbol name. */
struct WutThread {
#ifdef TARGET_WIIU
	unsigned char bytes[0x6a0];
} __attribute__((aligned(32)));
struct WutMessageQueue {
	unsigned char bytes[0x100];
} __attribute__((aligned(32)));
#else
	std::thread* thread;
	bool started;
};
struct WutMessageQueue {
	std::mutex mutex;
	std::condition_variable condition;
	std::deque<void*> messages;
};
#endif
typedef int (*WutEntry)(int, const char**);

extern "C" {
#ifdef TARGET_WIIU
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
#else
typedef WutThread TestThread;
BOOL host_OSCreateThread(TestThread*, void* (*)(void*), void*, void*, u32, s32, u16);
long host_OSResumeThread(TestThread*);
int host_OSJoinThread(TestThread*, void**);
#endif
}

#ifndef OS_THREAD_ATTRIB_AFFINITY_CPU0
#define OS_THREAD_ATTRIB_AFFINITY_CPU0 (1u << 0)
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
#ifdef TARGET_WIIU
	WutMessage commandSlots[kCommandQueueCount] __attribute__((aligned(32)));
	WutMessage replySlots[kReplyQueueCount] __attribute__((aligned(32)));
	WutThread mixerThread;
	OSThread replyThread;
	u8 mixerStack[0x10000] __attribute__((aligned(32)));
	u8 replyStack[0x10000] __attribute__((aligned(32)));
#else
	WutThread mixerThread;
	TestThread replyThread;
#endif
	volatile u32 framePending;
	volatile u32 subframesRendered;
	bool checked;
	bool active;
	bool mixerStarted;
	bool replyStarted;
};

AsyncState s_async;
// Async-path debug counters (logged with the 10 s audio stats).
volatile u32 s_dbgMails, s_dbgAsserts, s_dbgLastAssertMail, s_dbgSkippedMails;
volatile u32 s_dbgQueued[4], s_dbgExec[4], s_dbgReleaseIdle, s_dbgReplies, s_dbgReplyPosts;
#if defined(SMG_AUDIO_ASYNC_TEST)
volatile u32 s_asyncCommandAllocs, s_asyncCommandFrees;
volatile u32 s_asyncReplyAllocs, s_asyncReplyFrees;
#endif

#if defined(PORT_GAME_SMG) && defined(SMG_DSP_ASYNC)
template <typename T>
T* async_alloc()
{
	void* storage = port_host_alloc(sizeof(T), alignof(T));
	return storage ? new (storage) T : nullptr;
}

template <typename T>
void async_free(T* object)
{
	if (!object)
		return;
	object->~T();
	port_host_free(object);
}
#endif

inline int dbg_kind(bool release, const std::vector<u32>& w)
{
	if (release) return 0;
	if (w.empty()) return 3;
	const u32 op = w[0] >> 24;
	return op == 0x82 ? 1 : op == 0x81 ? 2 : 3;
}

BOOL send_ptr(WutMessageQueue* q, void* p)
{
#ifdef TARGET_WIIU
	WutMessage m = {p, {0, 0, 0}};
	return wut_OSSendMessage(q, &m, kWutMessageBlock);
#else
	{
		std::lock_guard<std::mutex> lock(q->mutex);
		q->messages.push_back(p);
	}
	q->condition.notify_one();
	return 1;
#endif
}

BOOL receive_ptr(WutMessageQueue* q, void** p)
{
#ifdef TARGET_WIIU
	WutMessage m = {nullptr, {0, 0, 0}};
	const BOOL ok = wut_OSReceiveMessage(q, &m, kWutMessageBlock);
	*p = m.message;
	return ok;
#else
	std::unique_lock<std::mutex> lock(q->mutex);
	q->condition.wait(lock, [q] { return !q->messages.empty(); });
	*p = q->messages.front();
	q->messages.pop_front();
	return 1;
#endif
}
bool s_workerRelease;

void queue_reply(u32 first, u32 second)
{
	SMS_PORT_HEAP_SCOPE();
#if defined(PORT_GAME_SMG)
	Reply* reply = async_alloc<Reply>();
#else
	Reply* reply = new Reply;
#endif
	if (!reply)
		return;
#if defined(SMG_AUDIO_ASYNC_TEST)
	++s_asyncReplyAllocs;
#endif
	reply->first = first;
	reply->second = second;
	if (!send_ptr(&s_async.replyQueue, reply)) {
#if defined(PORT_GAME_SMG)
		async_free(reply);
#if defined(SMG_AUDIO_ASYNC_TEST)
		++s_asyncReplyFrees;
#endif
#else
		delete reply;
#endif
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
#if defined(PORT_GAME_SMG)
		const bool subframe_reply = reply->second == (kPrefix | 0xFF00u);
#else
		const bool subframe_reply = false;
#endif
		__DSPHandler(0, NULL);
#if defined(PORT_GAME_SMG)
		if (subframe_reply)
			++s_subframeReplies;
#endif
		OSRestoreInterrupts(level);
		{
			SMS_PORT_HEAP_SCOPE();
#if defined(PORT_GAME_SMG)
			async_free(reply);
#if defined(SMG_AUDIO_ASYNC_TEST)
			++s_asyncReplyFrees;
#endif
#else
			delete reply;
#endif
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
#if defined(PORT_GAME_SMG)
			async_free(command);
#if defined(SMG_AUDIO_ASYNC_TEST)
			++s_asyncCommandFrees;
#endif
#else
			delete command;
#endif
		}
	}
	return 0;
}

#if defined(SMG_AUDIO_ASYNC_TEST)
BOOL host_OSCreateThread(TestThread* thread, void* (*entry)(void*), void* arg,
                         void*, u32, s32, u16)
{
	thread->thread = new std::thread([thread, entry, arg] {
		entry(arg);
		thread->started = false;
	});
	thread->started = true;
	return 1;
}

long host_OSResumeThread(TestThread*) { return 0; }

int host_OSJoinThread(TestThread* thread, void**)
{
	if (thread->thread && thread->thread->joinable())
		thread->thread->join();
	delete thread->thread;
	thread->thread = nullptr;
	thread->started = false;
	return 1;
}
#endif

bool start_async()
{
	if (s_async.checked)
		return s_async.active;
	s_async.checked = true;
#if !defined(SMG_AUDIO_ASYNC_TEST)
	if (sms_wiiu_sd_marker("syncmix")) {
		port_log("[audio] DSP HLE mode: synchronous (SD marker syncmix)\n");
		return false;
	}
#endif
	#ifdef TARGET_WIIU
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
	if (!wut_OSCreateThread(&s_async.mixerThread, mixer_thread, 0,
	                        reinterpret_cast<char*>(&s_async),
	                        s_async.mixerStack + sizeof(s_async.mixerStack),
	                        sizeof(s_async.mixerStack), kMixerPriority,
	                        OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
		s_async.active = false;
		send_ptr(&s_async.replyQueue, nullptr);
		sms_wiiu_OSJoinThread(&s_async.replyThread, nullptr);
		s_async.replyStarted = false;
		port_log("[audio] DSP HLE: mixer thread creation failed; using synchronous mode\n");
		return false;
	}
	s_async.mixerStarted = true;
	wut_OSResumeThread(&s_async.mixerThread);
	#else
	s_async.checked = true;
	s_async.replyThread.thread = nullptr;
	s_async.replyThread.started = false;
	s_async.mixerThread.thread = new std::thread([] {
		mixer_thread(0, reinterpret_cast<const char**>(&s_async));
	});
	s_async.mixerThread.started = true;
	if (!host_OSCreateThread(&s_async.replyThread, reply_thread, nullptr, nullptr,
	                         0, 0, 0))
		return false;
	s_async.replyStarted = true;
	s_async.mixerStarted = true;
	s_async.active = true;
	#endif
	port_log("[audio] DSP HLE mode: asynchronous (mixer core 0, replies core 1)\n");
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
#ifdef TARGET_WIIU
		wut_OSJoinThread(&s_async.mixerThread, nullptr);
#else
		if (s_async.mixerThread.thread && s_async.mixerThread.thread->joinable())
			s_async.mixerThread.thread->join();
		delete s_async.mixerThread.thread;
		s_async.mixerThread.thread = nullptr;
		s_async.mixerThread.started = false;
#endif
		s_async.mixerStarted = false;
	}
	s_async.active = false;
	if (s_async.replyStarted) {
		send_ptr(&s_async.replyQueue, nullptr);
#ifdef TARGET_WIIU
		sms_wiiu_OSJoinThread(&s_async.replyThread, nullptr);
#else
		host_OSJoinThread(&s_async.replyThread, nullptr);
#endif
		s_async.replyStarted = false;
	}
	port_log("[audio] DSP HLE asynchronous threads stopped\n");
}
#else
// Host builds (platform/audio/tests) run the synchronous path only.
#endif

// Raise the DSP interrupt: runs the decomp's __DSPHandler in interrupt
// context at the next check point (the sender still has interrupts off).
void raise_irq(u32 first, u32 second)
{
#if defined(SMG_DSP_ASYNC)
	if (s_async.active) {
		++s_dbgReplies;
		queue_reply(first, second);
		return;
	}
#endif
	port_irq_defer([first, second]() {
		post(first);
		post(second);
#if defined(PORT_GAME_SMG)
		const bool subframe_reply = second == (kPrefix | 0xFF00u);
#else
		const bool subframe_reply = false;
#endif
		__DSPHandler(0, NULL);
#if defined(PORT_GAME_SMG)
		if (subframe_reply)
			++s_subframeReplies;
#endif
	});
}

void reply(u32 low16) { raise_irq(0xDCD10004u, kPrefix | low16); }

#if !defined(PORT_GAME_SMG)
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
#endif

void render_subframe()
{
#if !defined(PORT_GAME_SMG)
	maybe_dump_aram();
#endif
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
#ifdef TARGET_WIIU
	/* On GameCube the DSP writes these blocks by DMA. The Wii U mixer writes
	 * through the CPU cache, while JASDSPBuf invalidates the ranges before the
	 * game consumes them; flush them first so RAM contains the DSP results. */
#if defined(PORT_GAME_SMG)
	DCFlushRange(g.outA + g.sub * n, n * sizeof(int16_t));
	DCFlushRange(g.outB + g.sub * n, n * sizeof(int16_t));
	if (g.voices)
		DCFlushRange(g.voices, g.nvoices * 0x180);
#else
	__real_DCFlushRange(g.outA + g.sub * n, n * sizeof(int16_t));
	__real_DCFlushRange(g.outB + g.sub * n, n * sizeof(int16_t));
	if (g.voices)
		__real_DCFlushRange(g.voices, g.nvoices * 0x180);
#endif
#endif
	g.sub++;
	g.subframesRendered++;
#if defined(PORT_GAME_SMG)
	++s_subframeRenders;
#endif
#if defined(SMG_DSP_ASYNC)
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
#if defined(SMG_DSP_ASYNC)
	if (s_async.active && !fromWorker) {
		SMS_PORT_HEAP_SCOPE();
#if defined(PORT_GAME_SMG)
		Command* command = async_alloc<Command>();
#else
		Command* command = new Command;
#endif
		if (!command)
			return;
#if defined(SMG_AUDIO_ASYNC_TEST)
		++s_asyncCommandAllocs;
#endif
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
#if defined(PORT_GAME_SMG)
			async_free(command);
#if defined(SMG_AUDIO_ASYNC_TEST)
			++s_asyncCommandFrees;
#endif
#else
			delete command;
#endif
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
	#if defined(SMG_DSP_ASYNC)
		release
#else
		g.release
#endif
	) {
		// DSPReleaseHalt: the 64-bit active-voice map. The microcode renders
		// the next subframe of the current frame and reports 0xFF00.
#if defined(PORT_GAME_SMG)
	/* smg-audio: D2. Galaxy releases the voices sixteen at a time
		 * (JASDSPChannel::updateAll), one DSPReleaseHalt2(group) word each:
		 * group << 16 | the group's 16-voice map. The subframe is rendered and
		 * reported once, after group 3; groups 0-2 get no reply. */
		if (w.empty() || ((w[0] >> 16) & 0xFF) != 3)
			return;
		++s_subframeMails;
#endif
		if (g.outA && g.sub < g.subframes) {
			render_subframe();
		}
#if defined(SMG_DSP_ASYNC)
		else
			++s_dbgReleaseIdle;
#endif
		return;
	}
	switch (op) {
	case 0x81: // DsetupTable(n, CH_BUF, DSPRES_FILTER, DSPADPCM_FILTER, FX_BUF)
		if (w.size() >= 5) {
			port_dspmix_set_aram(aram_lookup);
			g.voices  = (void*)(uintptr_t)w[1];
			g.nvoices = w[0] & 0xFFFF;
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
		#if defined(SMG_DSP_ASYNC)
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
#if defined(PORT_GAME_SMG)
	case 0x8E: // smg-audio: D3. DsetVARAM(base): sent once by JASDsp::boot, before 0x81
		if (w.size() >= 2) {
			s_varam = w[1];
			port_log("[audio] DSP VARAM base 0x%08x (ARAM addresses are relative to it)\n", (unsigned)s_varam);
		}
		reply(w[0] >> 16); // DsetVARAM spins until DspFinishWork(0x8E00)
		break;
#endif
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
#if defined(SMG_DSP_ASYNC)
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
#if defined(PORT_GAME_SMG)
	/* Only the command vector is port memory. In synchronous mode execute()
	 * runs the game's __DSPHandler (raise_irq), whose allocations belong to
	 * the game heaps, so it must not run inside the port scope. */
	{
		SMS_PORT_HEAP_SCOPE();
		g.words.push_back(mail);
	}
#else
	SMS_PORT_HEAP_SCOPE();
	g.words.push_back(mail);
#endif
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
#if defined(PORT_GAME_SMG)
	// smg-audio: D1. Galaxy's DSPSendCommands2 (JAudio2 dsptask.cpp) sends
	// ONE word for a count of 0: DSPReleaseHalt2's group/map word.
	g.want    = g.lastMail ? g.lastMail : 1;
#else
	g.want    = g.lastMail ? g.lastMail : 2;
#endif
	g.inCmd   = true;
	g.words.clear();
}

extern "C" u64 port_audio_dsp_subframes(void)
{
#if defined(SMG_DSP_ASYNC)
	if (s_async.active)
		return s_async.subframesRendered;
#endif
	return g.subframesRendered;
}

#if defined(PORT_GAME_SMG)
extern "C" void port_audio_dsp_subframe_stats(uint32_t* mails, uint32_t* renders,
                                               uint32_t* replies)
{
	if (mails)
		*mails = s_subframeMails;
	if (renders)
		*renders = s_subframeRenders;
	if (replies)
		*replies = s_subframeReplies;
}

extern "C" void port_audio_dsp_subframe_stats_ex(uint32_t* releases,
                                                   uint32_t* renders,
                                                   uint32_t* replies,
                                                   uint32_t* release_idle)
{
	if (releases)
		*releases = s_subframeMails;
	if (renders)
		*renders = s_subframeRenders;
	if (replies)
		*replies = s_subframeReplies;
	if (release_idle)
#if defined(SMG_DSP_ASYNC)
		*release_idle = s_dbgReleaseIdle;
#else
		*release_idle = 0;
#endif
}
#endif

#if defined(SMG_AUDIO_ASYNC_TEST)
extern "C" void port_audio_dsp_async_alloc_stats(uint32_t* command_allocs,
                                                    uint32_t* command_frees,
                                                    uint32_t* reply_allocs,
                                                    uint32_t* reply_frees)
{
	if (command_allocs)
		*command_allocs = s_asyncCommandAllocs;
	if (command_frees)
		*command_frees = s_asyncCommandFrees;
	if (reply_allocs)
		*reply_allocs = s_asyncReplyAllocs;
	if (reply_frees)
		*reply_frees = s_asyncReplyFrees;
}
#endif

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
#if defined(PORT_GAME_SMG)
		port_log("[audio] async: queued rel/82/81/other %u/%u/%u/%u exec %u/%u/%u/%u releaseIdle %u replies %u posts %u subframe mail/render/reply %u/%u/%u "
	         "pending %u sub %u/%u fromDsp %u mails %u skipped %u asserts %u lastAssertMail %u inCmd %d want %u have %u "
	         "intcount %u audioThreadDead %d\n",
	         (unsigned)s_dbgQueued[0], (unsigned)s_dbgQueued[1], (unsigned)s_dbgQueued[2], (unsigned)s_dbgQueued[3],
	         (unsigned)s_dbgExec[0], (unsigned)s_dbgExec[1], (unsigned)s_dbgExec[2], (unsigned)s_dbgExec[3],
		         (unsigned)s_dbgReleaseIdle, (unsigned)s_dbgReplies, (unsigned)s_dbgReplyPosts,
		         (unsigned)s_subframeMails, (unsigned)s_subframeRenders, (unsigned)s_subframeReplies,
	         (unsigned)s_async.framePending, (unsigned)g.sub, (unsigned)g.subframes, (unsigned)g.fromDsp.size(),
	         (unsigned)s_dbgMails, (unsigned)s_dbgSkippedMails, (unsigned)s_dbgAsserts, (unsigned)s_dbgLastAssertMail,
	         (int)g.inCmd, (unsigned)g.want, (unsigned)g.words.size(),
	         (unsigned)JASystem::AudioThread::getDSPSyncCount(),
	         sms_wiiu_OSIsThreadTerminated(&JASystem::AudioThread::jac_audioThread));
#else
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
#if defined(SMG_DSP_ASYNC)
	if (s_async.active) {
		const int pending = s_async.framePending != 0;
		__sync_synchronize();  // reads of the frame's samples come after the flag
		return pending;
	}
#endif
	return g.outA && g.sub < g.subframes;
}

#if defined(SMG_DSP_ASYNC)
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
