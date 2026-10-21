#include "port_compat.h"
#include "port_os.h"
#include "runtime.h"

#include <coreinit/memdefaultheap.h>
#include <dolphin/os/OSMessage.h>
#include <dolphin/os/OSMutex.h>
#include <dolphin/os/OSThread.h>

#include <cstdint>
#include <cstring>

namespace {

struct WutThread;
struct WutQueue {
	WutThread* head;
	WutThread* tail;
	void* parent;
	uint32_t reserved;
};
typedef int (*WutEntry)(int, const char**);

extern "C" {
BOOL wut_OSCreateThread(WutThread*, WutEntry, int32_t, char*, void*, uint32_t,
                        int32_t, uint8_t) __asm__("OSCreateThread");
void wut_OSExitThread(int32_t) __asm__("OSExitThread");
void wut_OSCancelThread(WutThread*) __asm__("OSCancelThread");
BOOL wut_OSJoinThread(WutThread*, int*) __asm__("OSJoinThread");
void wut_OSDetachThread(WutThread*) __asm__("OSDetachThread");
int32_t wut_OSResumeThread(WutThread*) __asm__("OSResumeThread");
uint32_t wut_OSSuspendThread(WutThread*) __asm__("OSSuspendThread");
WutThread* wut_OSGetCurrentThread(void) __asm__("OSGetCurrentThread");
BOOL wut_OSIsThreadTerminated(WutThread*) __asm__("OSIsThreadTerminated");
void wut_OSInitThreadQueue(WutQueue*) __asm__("OSInitThreadQueue");
void wut_OSSleepThread(WutQueue*) __asm__("OSSleepThread");
void wut_OSWakeupThread(WutQueue*) __asm__("OSWakeupThread");
void wut_OSYieldThread(void) __asm__("OSYieldThread");
BOOL wut_OSDisableInterrupts(void) __asm__("OSDisableInterrupts");
BOOL wut_OSRestoreInterrupts(BOOL) __asm__("OSRestoreInterrupts");
uint32_t wut_OSGetSymbolName(uint32_t, char*, uint32_t) __asm__("OSGetSymbolName");
}

/* GameCube code guards shared state by disabling interrupts, which only
 * excludes other threads on the same core. Run every game thread on core 1,
 * beside the main thread, so those single-core rules still hold. */
uint8_t kGameThreadAffinityCpu1 = 1u << 1;
/* SD marker "core2" (main.cpp): the whole game runs on core 2 instead, every
 * game thread with it, so the single-core interrupt rule still holds. */
extern "C" void sms_wiiu_set_game_core(unsigned core) { kGameThreadAffinityCpu1 = uint8_t(1u << core); }

const uint32_t kWutThreadBytes = 0x6a0;
const unsigned kMaxThreads = 64;
const uint32_t kMinThreadStack = 0x10000;
const unsigned kMaxQueues = 128;

struct ThreadRecord {
	OSThread* game;
	WutThread* wut;
	void* (*entry)(void*);
	void* arg;
	void* stack;
	uint32_t stack_size;
	void* host_stack;
	uint32_t host_stack_bytes;
	s32 priority;
	void* result;
	bool active;
};

struct QueueRecord {
	OSThreadQueue* game;
	WutQueue wut;
	bool active;
};

ThreadRecord s_threads[kMaxThreads];
QueueRecord s_queues[kMaxQueues];
OSThread s_main_thread;
unsigned s_scheduler_depth;

ThreadRecord* record_for_game(OSThread* game)
{
	for (unsigned i = 0; i < kMaxThreads; ++i)
		if (s_threads[i].active && s_threads[i].game == game)
			return &s_threads[i];
	return nullptr;
}

ThreadRecord* record_for_wut(WutThread* wut)
{
	for (unsigned i = 0; i < kMaxThreads; ++i)
		if (s_threads[i].active && s_threads[i].wut == wut)
			return &s_threads[i];
	return nullptr;
}

OSThread* current_game_thread()
{
	ThreadRecord* r = record_for_wut(wut_OSGetCurrentThread());
	return r ? r->game : &s_main_thread;
}

QueueRecord* queue_for(OSThreadQueue* game, bool create)
{
	for (unsigned i = 0; i < kMaxQueues; ++i)
		if (s_queues[i].active && s_queues[i].game == game)
			return &s_queues[i];
	if (!create)
		return nullptr;
	for (unsigned i = 0; i < kMaxQueues; ++i) {
		if (s_queues[i].active)
			continue;
		s_queues[i].game = game;
		s_queues[i].active = true;
		wut_OSInitThreadQueue(&s_queues[i].wut);
		return &s_queues[i];
	}
	port_log("[os] decomp queue side table exhausted\n");
	return nullptr;
}

void set_thread_terminated(ThreadRecord* r, void* result)
{
	if (!r)
		return;
	r->result = result;
	r->game->val = result;
	r->game->state = OS_THREAD_STATE_MORIBUND;
	QueueRecord* q = queue_for(&r->game->queueJoin, true);
	if (q)
		wut_OSWakeupThread(&q->wut);
}

int thread_entry(int, const char** argv)
{
	ThreadRecord* r = reinterpret_cast<ThreadRecord*>(
		const_cast<void*>(reinterpret_cast<const void*>(argv)));
	if (!r || !r->active)
		return 0;
	r->game->state = OS_THREAD_STATE_RUNNING;
	void* result = r->entry ? r->entry(r->arg) : nullptr;
	set_thread_terminated(r, result);
	return static_cast<int>(reinterpret_cast<uintptr_t>(result));
}

void unlock_mutex_without_switch(OSMutex* mutex, OSThread* current)
{
	if (mutex->thread != current)
		return;
	if (mutex->count > 1) {
		--mutex->count;
		return;
	}
	mutex->count = 0;
	mutex->thread = nullptr;
	QueueRecord* q = queue_for(&mutex->queue, true);
	if (q)
		wut_OSWakeupThread(&q->wut);
}

void message_enqueue(OSMessageQueue* mq, void* msg, bool front)
{
	void** slots = reinterpret_cast<void**>(mq->msgArray);
	long at;
	if (front) {
		mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
		at = mq->firstIndex;
	} else {
		at = (mq->firstIndex + mq->usedCount) % mq->msgCount;
	}
	slots[at] = msg;
	++mq->usedCount;
}

void* message_dequeue(OSMessageQueue* mq)
{
	void** slots = reinterpret_cast<void**>(mq->msgArray);
	void* result = slots[mq->firstIndex];
	mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
	--mq->usedCount;
	return result;
}

} // namespace

extern "C" {

void port_os_threads_init(void)
{
	std::memset(s_threads, 0, sizeof(s_threads));
	std::memset(s_queues, 0, sizeof(s_queues));
	std::memset(&s_main_thread, 0, sizeof(s_main_thread));
	s_main_thread.state = OS_THREAD_STATE_RUNNING;
	s_main_thread.priority = 0;
	s_main_thread.base = 0;
	port_log("[os] target thread adapter ready; wut thread ABI isolated\n");
}

void sms_wiiu_OSInitThreadQueue(OSThreadQueue* queue)
{
	if (!queue)
		return;
	queue->head = nullptr;
	queue->tail = nullptr;
	queue_for(queue, true);
}

OSThread* sms_wiiu_OSGetCurrentThread(void) { return current_game_thread(); }

BOOL sms_wiiu_OSIsThreadTerminated(OSThread* game)
{
	ThreadRecord* r = record_for_game(game);
	return r ? (r->game->state == OS_THREAD_STATE_MORIBUND ||
	            wut_OSIsThreadTerminated(r->wut))
	         : FALSE;
}

s32 sms_wiiu_OSDisableScheduler(void) { return s_scheduler_depth++; }
s32 sms_wiiu_OSEnableScheduler(void)
{
	s32 old = static_cast<s32>(s_scheduler_depth);
	if (s_scheduler_depth)
		--s_scheduler_depth;
	return old;
}

void sms_wiiu_OSYieldThread(void) { wut_OSYieldThread(); }

int sms_wiiu_OSCreateThread(OSThread* game, void* (*entry)(void*), void* arg,
                            void* stack, u32 stack_size, s32 priority, u16 attr)
{
	if (!game || !entry || stack_size < 0x1000)
		return FALSE;
	ThreadRecord* r = record_for_game(game);
	if (!r) {
		for (unsigned i = 0; i < kMaxThreads; ++i) {
			if (s_threads[i].active)
				continue;
			r = &s_threads[i];
			break;
		}
	}
	if (!r)
		return FALSE;
	/* The game sizes stacks for the GameCube SDK (the card worker has 4 KiB).
	 * Port code that runs on these threads (stdio, std::string, 64 KiB read
	 * buffers) needs far more, and an overflow lands in the JKR heap blocks
	 * allocated next to the stack. Small stacks are replaced by one from the
	 * system heap, reused when the record is. */
	void* host_stack = r->host_stack;
	uint32_t host_stack_bytes = r->host_stack_bytes;
	if (stack_size < kMinThreadStack) {
		if (host_stack_bytes < kMinThreadStack) {
			if (host_stack)
				MEMFreeToDefaultHeap(host_stack);
			host_stack = MEMAllocFromDefaultHeapEx(kMinThreadStack, 0x40);
			host_stack_bytes = host_stack ? kMinThreadStack : 0;
			if (!host_stack)
				return FALSE;
		}
		stack = static_cast<uint8_t*>(host_stack) + host_stack_bytes;
		stack_size = host_stack_bytes;
	}
	std::memset(r, 0, sizeof(*r));
	r->host_stack = host_stack;
	r->host_stack_bytes = host_stack_bytes;
	r->game = game;
	r->entry = entry;
	r->arg = arg;
	r->stack = stack;
	r->stack_size = stack_size;
	r->priority = priority;
	r->active = true;
	r->wut = reinterpret_cast<WutThread*>(MEMAllocFromDefaultHeapEx(kWutThreadBytes, 0x40));
	if (!r->wut) {
		r->active = false;
		return FALSE;
	}
	std::memset(game, 0, sizeof(*game));
	game->state = OS_THREAD_STATE_READY;
	game->priority = priority;
	game->base = priority;
	game->attr = attr;
	game->queueJoin.head = nullptr;
	game->queueJoin.tail = nullptr;
	if (!wut_OSCreateThread(r->wut, thread_entry, 0,
	                        reinterpret_cast<char*>(r), stack, stack_size,
	                        priority, kGameThreadAffinityCpu1)) {
		r->active = false;
		return FALSE;
	}
	return TRUE;
}

/* Diagnostic: each game thread's state and the top of its saved stack, with
 * symbol names from the loader, so a blocked loader thread names its wait. */
extern "C" void sms_wiiu_dump_threads(void)
{
	for (unsigned i = 0; i < kMaxThreads; ++i) {
		ThreadRecord& r = s_threads[i];
		if (!r.active || !r.wut)
			continue;
		const uint32_t* ctx = reinterpret_cast<const uint32_t*>(r.wut);
		uint32_t sp = ctx[0x0C / 4];
		uint32_t lr = ctx[0x8C / 4];
		uint32_t pc = ctx[0x98 / 4];
		char pcs[48] = {}, lrs[48] = {};
		wut_OSGetSymbolName(pc, pcs, sizeof(pcs));
		wut_OSGetSymbolName(lr, lrs, sizeof(lrs));
		port_log("[thr] #%u game=%p state=%u prio=%d pc=%08x %s lr=%08x %s\n", i,
		         static_cast<void*>(r.game), static_cast<unsigned>(r.game->state),
		         static_cast<int>(r.priority), pc, pcs, lr, lrs);
		const uint32_t lo = reinterpret_cast<uint32_t>(r.stack) - r.stack_size;
		const uint32_t hi = reinterpret_cast<uint32_t>(r.stack);
		for (int depth = 0; depth < 8 && sp >= lo && sp + 8 <= hi && (sp & 3u) == 0; ++depth) {
			const uint32_t* frame = reinterpret_cast<const uint32_t*>(sp);
			uint32_t next = frame[0];
			if (next <= sp || next > hi)
				break;
			uint32_t ret = reinterpret_cast<const uint32_t*>(next)[1];
			char rs[48] = {};
			wut_OSGetSymbolName(ret, rs, sizeof(rs));
			port_log("[thr]   #%u frame %d ret=%08x %s\n", i, depth, ret, rs);
			sp = next;
		}
	}
}

void sms_wiiu_OSExitThread(OSThread* game)
{
	ThreadRecord* r = record_for_game(game);
	if (r) {
		set_thread_terminated(r, game->val);
		wut_OSExitThread(static_cast<int32_t>(reinterpret_cast<uintptr_t>(game->val)));
	}
}

void sms_wiiu_OSCancelThread(OSThread* game)
{
	ThreadRecord* r = record_for_game(game);
	if (r)
		wut_OSCancelThread(r->wut);
}

int sms_wiiu_OSJoinThread(OSThread* game, void** value)
{
	ThreadRecord* r = record_for_game(game);
	if (!r)
		return FALSE;
	int result = 0;
	if (!wut_OSJoinThread(r->wut, &result))
		return FALSE;
	if (value)
		*value = r->result;
	game->state = OS_THREAD_STATE_MORIBUND;
	return TRUE;
}

void sms_wiiu_OSDetachThread(OSThread* game)
{
	ThreadRecord* r = record_for_game(game);
	if (r)
		wut_OSDetachThread(r->wut);
}

long sms_wiiu_OSResumeThread(OSThread* game)
{
	ThreadRecord* r = record_for_game(game);
	return r ? wut_OSResumeThread(r->wut) : -1;
}

s32 sms_wiiu_OSSuspendThread(OSThread* game)
{
	ThreadRecord* r = record_for_game(game);
	return r ? static_cast<s32>(wut_OSSuspendThread(r->wut)) : -1;
}

void sms_wiiu_OSSleepThread(OSThreadQueue* queue)
{
	QueueRecord* q = queue_for(queue, true);
	if (q)
		wut_OSSleepThread(&q->wut);
}

void sms_wiiu_OSWakeupThread(OSThreadQueue* queue)
{
	QueueRecord* q = queue_for(queue, false);
	if (q)
		wut_OSWakeupThread(&q->wut);
	if (queue) {
		queue->head = nullptr;
		queue->tail = nullptr;
	}
}

long sms_wiiu_OSGetThreadPriority(OSThread* game)
{
	ThreadRecord* r = record_for_game(game);
	return r ? r->priority : (game ? game->priority : 31);
}

BOOL sms_wiiu_OSDisableInterrupts(void) { return wut_OSDisableInterrupts(); }
BOOL sms_wiiu_OSEnableInterrupts(void) { return wut_OSRestoreInterrupts(TRUE); }
BOOL sms_wiiu_OSRestoreInterrupts(BOOL level) { return wut_OSRestoreInterrupts(level); }

void sms_wiiu_OSInitMutex(OSMutex* mutex)
{
	if (!mutex)
		return;
	std::memset(mutex, 0, sizeof(*mutex));
}

void sms_wiiu_OSLockMutex(OSMutex* mutex)
{
	if (!mutex)
		return;
	OSThread* current = current_game_thread();
	for (;;) {
		BOOL level = wut_OSDisableInterrupts();
		if (!mutex->thread) {
			mutex->thread = current;
			mutex->count = 1;
			wut_OSRestoreInterrupts(level);
			return;
		}
		if (mutex->thread == current) {
			++mutex->count;
			wut_OSRestoreInterrupts(level);
			return;
		}
		QueueRecord* q = queue_for(&mutex->queue, true);
		// Sleep before re-enabling interrupts: a wakeup sent in between found no
		// sleeper and was lost (the audio thread slept with a message queued).
		if (q)
			wut_OSSleepThread(&q->wut);
		wut_OSRestoreInterrupts(level);
	}
}

void sms_wiiu_OSUnlockMutex(OSMutex* mutex)
{
	if (!mutex)
		return;
	BOOL level = wut_OSDisableInterrupts();
	unlock_mutex_without_switch(mutex, current_game_thread());
	wut_OSRestoreInterrupts(level);
}

BOOL sms_wiiu_OSTryLockMutex(OSMutex* mutex)
{
	if (!mutex)
		return FALSE;
	OSThread* current = current_game_thread();
	BOOL level = wut_OSDisableInterrupts();
	BOOL ok = !mutex->thread || mutex->thread == current;
	if (ok) {
		mutex->thread = current;
		++mutex->count;
	}
	wut_OSRestoreInterrupts(level);
	return ok;
}

void sms_wiiu_OSInitCond(struct OSCond* cond)
{
	if (cond)
		std::memset(cond, 0, sizeof(*cond));
}

void sms_wiiu_OSWaitCond(struct OSCond* cond, OSMutex* mutex)
{
	if (!cond || !mutex)
		return;
	BOOL level = wut_OSDisableInterrupts();
	unlock_mutex_without_switch(mutex, current_game_thread());
	QueueRecord* q = queue_for(&cond->queue, true);
	if (q)
		wut_OSSleepThread(&q->wut);  // before re-enabling interrupts (lost wakeup)
	wut_OSRestoreInterrupts(level);
	sms_wiiu_OSLockMutex(mutex);
}

void sms_wiiu_OSSignalCond(struct OSCond* cond)
{
	if (!cond)
		return;
	QueueRecord* q = queue_for(&cond->queue, false);
	if (q)
		wut_OSWakeupThread(&q->wut);
}

void sms_wiiu_OSInitMessageQueue(OSMessageQueue* mq, void* array, long count)
{
	if (!mq)
		return;
	std::memset(mq, 0, sizeof(*mq));
	mq->msgArray = array;
	mq->msgCount = count;
	queue_for(&mq->queueSend, true);
	queue_for(&mq->queueReceive, true);
}

/* Non-blocking sends refused by a full queue. JAudio forwards each DSP 0xFF00
 * reply to its audio thread with one; a refused one stalls audio for good. */
volatile uint32_t sms_wiiu_msg_drops;
/* The queue JAudio's audio thread waits on, learned from its first receive,
 * so the audio pacer can start a DMA block only once that queue is empty. */
extern OSThread sms_jac_audioThread __asm__("_ZN8JASystem11AudioThread15jac_audioThreadE");
OSMessageQueue* volatile sms_wiiu_audio_mq;

int send_message(OSMessageQueue* mq, void* msg, long flags, bool front)
{
	if (!mq || mq->msgCount <= 0)
		return FALSE;
	for (;;) {
		BOOL level = wut_OSDisableInterrupts();
		if (mq->usedCount < mq->msgCount) {
			message_enqueue(mq, msg, front);
			QueueRecord* q = queue_for(&mq->queueReceive, false);
			wut_OSRestoreInterrupts(level);
			if (q)
				wut_OSWakeupThread(&q->wut);
			return TRUE;
		}
		if (flags == OS_MESSAGE_NOBLOCK) {
			++sms_wiiu_msg_drops;
			wut_OSRestoreInterrupts(level);
			return FALSE;
		}
		QueueRecord* q = queue_for(&mq->queueSend, true);
		// Sleep before re-enabling interrupts: a wakeup sent in between found no
		// sleeper and was lost (the audio thread slept with a message queued).
		if (q)
			wut_OSSleepThread(&q->wut);
		wut_OSRestoreInterrupts(level);
	}
}

int sms_wiiu_OSSendMessage(OSMessageQueue* mq, void* msg, long flags)
{
	return send_message(mq, msg, flags, false);
}

int sms_wiiu_OSJamMessage(OSMessageQueue* mq, void* msg, long flags)
{
	return send_message(mq, msg, flags, true);
}

int sms_wiiu_OSReceiveMessage(OSMessageQueue* mq, void* msg, long flags)
{
	if (!mq || mq->msgCount <= 0)
		return FALSE;
	if (!sms_wiiu_audio_mq && current_game_thread() == &sms_jac_audioThread)
		sms_wiiu_audio_mq = mq;
	for (;;) {
		BOOL level = wut_OSDisableInterrupts();
		if (mq->usedCount > 0) {
			void* value = message_dequeue(mq);
			QueueRecord* q = queue_for(&mq->queueSend, false);
			wut_OSRestoreInterrupts(level);
			if (msg)
				*reinterpret_cast<void**>(msg) = value;
			if (q)
				wut_OSWakeupThread(&q->wut);
			return TRUE;
		}
		if (flags == OS_MESSAGE_NOBLOCK) {
			wut_OSRestoreInterrupts(level);
			return FALSE;
		}
		QueueRecord* q = queue_for(&mq->queueReceive, true);
		// Sleep before re-enabling interrupts: a wakeup sent in between found no
		// sleeper and was lost (the audio thread slept with a message queued).
		if (q)
			wut_OSSleepThread(&q->wut);
		wut_OSRestoreInterrupts(level);
	}
}

// Messages the audio thread has not taken yet (0 before it first waits).
int sms_wiiu_audio_mq_used(void)
{
	OSMessageQueue* mq = sms_wiiu_audio_mq;
	return mq ? int(mq->usedCount) : 0;
}

} // extern "C"
