#include "runtime.h"

#include <whb/log.h>
#include <whb/log_udp.h>
#include <whb/proc.h>

#ifndef SMS_WIIU_RUN_GAME
#define SMS_WIIU_RUN_GAME 0
#endif

extern "C" void SMS_main(void);

/* Keeping the entry point in volatile storage makes the Stage 1 ELF retain
 * the complete game even though normal Stage 1 builds never call it. */
static void (*volatile s_game_entry)(void) = SMS_main;

int main(int argc, char** argv)
{
	WHBLogUdpInit();
	WHBProcInit();
	sms_wiiu_runtime_init();
	WHBLogPrintf("SMS Wii U: stage1 main entered");

	bool game_entered = false;
	while (WHBProcIsRunning()) {
#if SMS_WIIU_RUN_GAME == 1
		if (!game_entered) {
			game_entered = true;
			port_init(argc, argv);
			s_game_entry();
		}
#else
		(void)argc;
		(void)argv;
		(void)game_entered;
		/* Read through volatile storage so the linker-visible reference is
		 * retained even under whole-program dead-code elimination. */
		(void)s_game_entry;
#endif
	}

	WHBProcShutdown();
	return 0;
}
