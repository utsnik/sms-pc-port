#include "runtime.h"

#include "port/port.h"

#include <whb/log_udp.h>
#include <whb/proc.h>
#include <revolution/nand.h>

void port_game_main(void);

/* Retain the whole game and its entry references without crossing the Stage 1
 * no-data boundary. */
static void (*volatile s_game_entry)(void) = port_game_main;

int main(int, char**)
{
    WHBLogUdpInit();
    WHBProcInit();
    smg_wiiu_runtime_init();
    port_log("SMG Wii U: stage2 main entered");

    port_os_init();
    port_os_threads_init();
    port_dvd_init();
    smg_wiiu_load_dol_data();
    port_vi_init();
    sms_wiiu_gfx_init();
    port_pad_init();
    const s32 nand_result = NANDInit();
    port_log("SMG Wii U: NAND initialized result=%d", nand_result);
    smg_wiiu_audio_init();
    s_game_entry();

    WHBProcShutdown();
    WHBLogUdpDeinit();
    return 0;
}
