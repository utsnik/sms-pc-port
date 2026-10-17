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
}

const uint32_t kWutThreadBytes = 0x6a0;
const unsigned kMaxThreads = 64;
const unsigned kMaxQueues = 128;

struct ThreadRecord {
	OSThread* game;
	WutThread* wut;
	void* (*entry)(void*);
	void* arg;
	void* stack;
	uint32_t stack_size;
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
	std::memset(r, 0, sizeof(*r));
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
	                        priority, 0)) {
		r->active = false;
		return FALSE;
	}
	return TRUE;
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
		wut_OSRestoreInterrupts(level);
		if (q)
			wut_OSSleepThread(&q->wut);
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
	wut_OSRestoreInterrupts(level);
	if (q)
		wut_OSSleepThread(&q->wut);
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
			wut_OSRestoreInterrupts(level);
			return FALSE;
		}
		QueueRecord* q = queue_for(&mq->queueSend, true);
		wut_OSRestoreInterrupts(level);
		if (q)
			wut_OSSleepThread(&q->wut);
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
		wut_OSRestoreInterrupts(level);
		if (q)
			wut_OSSleepThread(&q->wut);
	}
}

} // extern "C"
