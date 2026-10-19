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

int main(int argc, char** argv)
{
	WHBLogUdpInit();
	/* OSConsole too: Cemu writes it to log.txt (UDP does not reach a listener there). */
	WHBLogCafeInit();
	WHBLogPrint("SMS Wii U: main start (global constructors done)");
	sms_wiiu_install_exception_handlers();
	WHBProcInit();
	port_init(argc, argv);
	port_log("SMS Wii U: main entered; transferring ProcUI ownership to SMS_main\n");
	s_game_entry();
	port_log("SMS Wii U: SMS_main returned; shutting down\n");

	sms_wiiu_audio_shutdown();
	WHBProcShutdown();
	return 0;
}
