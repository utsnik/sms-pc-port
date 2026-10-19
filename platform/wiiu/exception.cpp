/* Fatal exception reporting for the first hardware boots: registers go to the
 * UDP log and to the OSFatal screen. Kept free of decomp headers, whose OS types
 * clash with coreinit's. ALIGNMENT is not hooked: GameCube code can do
 * misaligned float accesses that the OS otherwise handles. */
#include <coreinit/core.h>
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sysapp/launch.h>
#include <whb/proc.h>
#include <proc_ui/procui.h>
#include <cstdlib>
#include <whb/log.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" void* sms_wiiu_vi_owner_thread(void);
extern "C" void sms_wiiu_crash_return_to_menu(void);
extern "C" void sms_wiiu_exit(void);
extern "C" void sms_wiiu_audio_shutdown(void);
extern "C" volatile int sms_wiiu_crash_pending;
volatile int sms_wiiu_crash_pending;

namespace {

/* A non-main thread that faulted: ask the main thread to leave, then wait.
 * If the main thread never reaches a frame boundary (it may be waiting on this
 * thread), fall back to the error screen after 10 seconds. */
void park_crashed_thread(void)
{
	sms_wiiu_crash_pending = 1;
	WHBLogPrint("[crash] game thread parked; the main thread returns to the Menu");
	OSSleepTicks(OSSecondsToTicks(10));
	OSFatal("SMS Wii U: crashed (see the UDP log); the main thread did not exit");
}

char s_message[512];
volatile int s_crashed;
char s_trace[2048];

/* The faulting PC is often inside a system RPL (outside .text at 0x02000000),
 * so name every address and walk the stack back to the game's own frames. */
void append_symbol(const char* tag, uint32_t addr)
{
	char name[96];
	size_t used = std::strlen(s_trace);
	name[0] = '\0';
	OSGetSymbolName(addr, name, sizeof(name));
	if (used < sizeof(s_trace))
		std::snprintf(s_trace + used, sizeof(s_trace) - used, "%s %08X %s\n", tag, (unsigned)addr, name);
}

void trace(OSContext* context)
{
	uint32_t sp = context->gpr[1];
	s_trace[0] = '\0';
	append_symbol("PC", context->srr0);
	append_symbol("LR", context->lr);
	for (int i = 0; i < 16; i++) {
		if (sp < 0x10000000u || sp >= 0x50000000u || (sp & 3u)) break;
		uint32_t next = *(const uint32_t*)sp;
		if (next <= sp || next >= 0x50000000u) break;
		append_symbol("BT", *(const uint32_t*)(next + 4));
		sp = next;
	}
}

BOOL fatal(const char* type, OSContext* context)
{
	if (!context) {
		std::snprintf(s_message, sizeof(s_message), "SMS Wii U %s exception (no context)\n", type);
	} else {
		std::snprintf(s_message, sizeof(s_message),
		              "SMS Wii U %s exception core=%u\nSRR0=%08X SRR1=%08X\nDAR=%08X DSISR=%08X\n"
		              "LR=%08X R1=%08X R3=%08X R4=%08X\n",
		              type, (unsigned)OSGetCoreId(), (unsigned)context->srr0, (unsigned)context->srr1,
		              (unsigned)context->dar, (unsigned)context->dsisr, (unsigned)context->lr,
		              (unsigned)context->gpr[1], (unsigned)context->gpr[3], (unsigned)context->gpr[4]);
	}
	WHBLogPrint(s_message);
	if (context) {
		trace(context);
		WHBLogPrint(s_trace);
	}
	/* A second fault (or none of the recovery paths apply): the old behaviour,
	 * an error screen that needs a reboot. */
	if (!context || s_crashed++) {
		OSFatal(s_message);
		return FALSE;
	}
	/* Crash safety: resume the faulting thread in a recovery function instead
	 * of OSFatal. The main thread (which owns ProcUI and GX2) goes straight to
	 * the Menu; any other thread parks, and the main thread leaves at its next
	 * frame boundary. OSThread starts with its OSContext, so the context
	 * address identifies the thread. */
	bool main_thread = (void*)context == sms_wiiu_vi_owner_thread();
	context->srr0 = main_thread ? (uint32_t)&sms_wiiu_crash_return_to_menu : (uint32_t)&park_crashed_thread;
	context->lr = 0;
	context->gpr[1] &= ~0xFu; // keep the stack, 16-byte aligned
	context->gpr[1] -= 0x100;   // below the faulting frame's red zone
	return TRUE;
}

BOOL on_dsi(OSContext* c) { return fatal("DSI", c); }
BOOL on_isi(OSContext* c) { return fatal("ISI", c); }
BOOL on_program(OSContext* c) { return fatal("PROGRAM", c); }

} // namespace

extern "C" void sms_wiiu_install_exception_handlers(void)
{
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, on_dsi);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, on_isi);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, on_program);
}

/* Runs on the main thread after a crash: ask for the Menu, hand over the
 * foreground when ProcUI asks, and end the process without running the game's
 * global destructors (other game threads are still alive). WHBProc/std::exit
 * were not used here: with them the Menu started (its music played) but the
 * app never let go of the screen. */
extern "C" void sms_wiiu_crash_return_to_menu(void)
{
	OSSetThreadPriority(OSGetCurrentThread(), 0);
	WHBLogPrint("[crash] returning to the Wii U Menu");
	sms_wiiu_audio_shutdown();
	SYSLaunchMenu();
	for (;;) {
		ProcUIStatus status = ProcUIProcessMessages(TRUE);
		if (status == PROCUI_STATUS_RELEASE_FOREGROUND)
			ProcUIDrawDoneRelease();
		else if (status == PROCUI_STATUS_EXITING)
			break;
	}
	WHBLogPrint("[crash] ProcUI exiting");
	ProcUIShutdown();
	_Exit(0);
}
