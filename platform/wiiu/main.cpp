#include "runtime.h"

#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>
#include <whb/proc.h>

extern void SMS_main(void);
extern "C" void sms_wiiu_install_exception_handlers(void);
extern "C" void sms_wiiu_audio_shutdown(void);

/* Keep the entry point in volatile storage so the target link retains the
 * complete game while still making the runtime hand-off explicit. */
static void (*volatile s_game_entry)(void) = SMS_main;

#include <coreinit/thread.h>
#include <coreinit/core.h>
#include <coreinit/memdefaultheap.h>
#include <cstdio>

extern "C" void sms_wiiu_set_game_core(unsigned core);
extern "C" {
BOOL cafe_OSCreateThread(OSThread*, OSThreadEntryPointFn, int32_t, char*, void*, uint32_t, int32_t,
                         OSThreadAttributes) __asm__("OSCreateThread");
int32_t cafe_OSResumeThread(OSThread*) __asm__("OSResumeThread");
BOOL cafe_OSJoinThread(OSThread*, int*) __asm__("OSJoinThread");
}

/* Experiment (SD marker "core2"): run the runtime and the game on a core-2
 * thread, so GX2Init and every game thread land there, to measure what core
 * 2's 512 KB L2 costs the game against core 1's 2 MB (GX split stage 0). */
static int s_argc;
static char** s_argv;
static int game_on_core2(int, const char**)
{
	port_init(s_argc, s_argv);
	port_log("SMS Wii U: game running on core %u (SD marker core2)\n", (unsigned)OSGetCoreId());
	s_game_entry();
	port_log("SMS Wii U: SMS_main returned on core 2\n");
	return 0;
}

int main(int argc, char** argv)
{
	WHBLogUdpInit();
	/* OSConsole too: Cemu writes it to log.txt (UDP does not reach a listener there). */
	WHBLogCafeInit();
	WHBLogPrint("SMS Wii U: main start (global constructors done)");
	sms_wiiu_install_exception_handlers();
	WHBProcInit();
	if (FILE* f = std::fopen("fs:/vol/external01/wiiu/apps/sms/core2", "rb")) {
		std::fclose(f);
		static OSThread thread __attribute__((aligned(16)));
		const uint32_t kStack = 2u << 20;
		void* stack = MEMAllocFromDefaultHeapEx(kStack, 16);
		sms_wiiu_set_game_core(2);
		s_argc = argc;
		s_argv = argv;
		if (stack && cafe_OSCreateThread(&thread, game_on_core2, 0, nullptr, static_cast<char*>(stack) + kStack, kStack,
		                                 16, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
			cafe_OSResumeThread(&thread);
			int ret = 0;
			cafe_OSJoinThread(&thread, &ret);
			sms_wiiu_audio_shutdown();
			WHBProcShutdown();
			return 0;
		}
		sms_wiiu_set_game_core(1);
		WHBLogPrint("SMS Wii U: core-2 thread failed; running on core 1");
	}
	port_init(argc, argv);
	port_log("SMS Wii U: main entered; transferring ProcUI ownership to SMS_main\n");
	s_game_entry();
	port_log("SMS Wii U: SMS_main returned; shutting down\n");

	sms_wiiu_audio_shutdown();
	WHBProcShutdown();
	return 0;
}
