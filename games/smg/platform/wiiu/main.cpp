#include "runtime.h"

#include "port/port.h"

#include <whb/log_udp.h>
#include <whb/proc.h>

#ifndef SMG_WIIU_RUN_GAME
#error "SMG_WIIU_RUN_GAME must be defined by the Wii U target"
#endif

void port_game_main(void);

/* Retain the whole game and its entry references without crossing the Stage 1
 * no-data boundary. */
static void (*volatile s_game_entry)(void) = port_game_main;

int main(int, char**)
{
    WHBLogUdpInit();
    WHBProcInit();
    smg_wiiu_runtime_init();
    port_log("SMG Wii U: stage1 main entered");

#if SMG_WIIU_RUN_GAME == 1
    s_game_entry();
#else
    (void)s_game_entry;
#endif

    while (WHBProcIsRunning()) {
        /* WHBProcIsRunning owns the sole ProcUI pump for Stage 1. */
    }

    WHBProcShutdown();
    WHBLogUdpDeinit();
    return 0;
}
