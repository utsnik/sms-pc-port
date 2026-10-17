// OSThread scheduler check: a host thread whose OSThread was cancelled never
// runs game code again, even when the OSThread is recreated and scheduled
// before the old host gets back the CPU lock.
//
// THP's reader is a static OSThread: stopping one movie cancels it and the
// next movie recreates it. Its old host could still be spinning or queued for
// g_cpu; the turn is told by the OSThread pointer alone, and the cancelled
// check only ran after a condition-variable wait, so the old host resumed the
// old function beside the new one (a Windows player's crash after the FLUDD
// movies, jumping to 0x500). Here a worker above the main thread's priority
// blocks on a queue, the main thread runs on long enough for the worker's host
// to stop spinning and queue for the lock, then cancels, recreates and wakes
// it, and the old incarnation must never take a message.
#include <dolphin/os.h>
#include <dolphin/os/OSMessage.h>
#include <dolphin/os/OSThread.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include "os/crash.h"

extern "C" void port_os_threads_init(void);

// What os_thread.cpp needs from the rest of the port.
extern "C" void port_log(const char* fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vfprintf(stderr, fmt, a);
	va_end(a);
}
extern "C" void* port_low_alloc(unsigned long n) { return calloc(1, n); }
extern "C" int port_vi_idle_advance(void) { return 0; }
void port_crash_thread_init() {}
void port_crash_emit(PortCrashLine&) {}
bool port_crash_module(uintptr_t, uintptr_t*, const char**) { return false; }
void port_crash_set_thread_describer(PortCrashDescriber) {}

static OSThread worker; // static, like THPRead.c's ReadThread
static OSMessageQueue queue;
static OSMessage slots[4];
static std::atomic<int> generation(0); // the incarnation allowed to run
static std::atomic<int> stale(0);      // messages an older incarnation took

static void* body(void* arg)
{
	const int mine = (int)(intptr_t)arg;
	for (;;) {
		OSMessage m;
		OSReceiveMessage(&queue, &m, OS_MESSAGE_BLOCK);
		if (generation.load() != mine)
			stale.fetch_add(1);
	}
	return NULL;
}

// Runs on (holding the CPU) for `ms`, without an OS call.
static void busy(double ms)
{
	timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	do
		clock_gettime(CLOCK_MONOTONIC, &b);
	while ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6 < ms);
}

int main()
{
	alarm(60); // a hang fails the test rather than the runner
	const int rounds = 300;
	static char stack[1 << 16];
	port_os_threads_init();
	OSInitMessageQueue(&queue, slots, 4);
	for (int i = 0; i < rounds; i++) {
		generation.store(i);
		// Priority 8 over the main thread's 16, as THP's reader is over the game.
		OSCreateThread(&worker, body, (void*)(intptr_t)i, stack + sizeof stack, sizeof stack, 8, 0);
		OSResumeThread(&worker);                                 // runs, and blocks on the queue
		OSSendMessage(&queue, (OSMessage)1, OS_MESSAGE_NOBLOCK); // takes it, and blocks again
		busy(1.0);               // its host stops spinning and queues for the lock
		OSCancelThread(&worker); // the movie stops, and the next one's reader is made at once
		generation.store(i + 1);
	}
	OSCancelThread(&worker);
	busy(20);
	printf("%d rounds: a cancelled incarnation took %d messages\n", rounds, stale.load());
	return stale.load() == 0 ? 0 : 1;
}
