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
/* How far the crash path got, reported by the audio thread's stats line: a
 * crashed main thread can no longer be asked. 1 = handler resumed the thread,
 * 2 = recovery entered, 3 = audio stopped, 4 = Menu requested, 5 = ProcUI
 * exiting; 10+ = a parked non-main thread. */
extern "C" volatile int sms_wiiu_crash_stage;
volatile int sms_wiiu_crash_stage;

namespace {

void park_forever(void)
{
	for (;;)
		OSSleepTicks(OSSecondsToTicks(1));
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

bool readable(uint32_t addr) { return addr >= 0x10000000u && addr < 0x50000000u && !(addr & 3u); }

/* Every GPR, then the words around r3 (for JSUPtrList::remove, the list):
 * whatever overwrote it may have left recognisable data next to it. */
void dump(OSContext* context)
{
	for (int i = 0; i < 32; i += 8)
		WHBLogPrintf("[crash] r%-2d %08X %08X %08X %08X %08X %08X %08X %08X", i,
		             (unsigned)context->gpr[i], (unsigned)context->gpr[i + 1], (unsigned)context->gpr[i + 2],
		             (unsigned)context->gpr[i + 3], (unsigned)context->gpr[i + 4], (unsigned)context->gpr[i + 5],
		             (unsigned)context->gpr[i + 6], (unsigned)context->gpr[i + 7]);
	uint32_t base = (context->gpr[3] & ~31u) - 0x100;
	if (!readable(base) || !readable(base + 0x200))
		return;
	for (uint32_t a = base; a < base + 0x200; a += 32) {
		const uint32_t* w = (const uint32_t*)a;
		WHBLogPrintf("[crash] %08X: %08X %08X %08X %08X %08X %08X %08X %08X", (unsigned)a, (unsigned)w[0],
		             (unsigned)w[1], (unsigned)w[2], (unsigned)w[3], (unsigned)w[4], (unsigned)w[5],
		             (unsigned)w[6], (unsigned)w[7]);
	}
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
		dump(context);
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
	/* Measured 2026-10-20 (SMS_CRASHTEST): a thread resumed this way never runs
	 * again; its saved pc stayed on the new srr0 and nothing after it executed.
	 * So the faulting thread only gets a harmless place to be, and the work is
	 * done elsewhere: a main-thread fault by the rescue thread, any other by
	 * the main thread at its next frame boundary. */
	context->srr0 = (uint32_t)&park_forever;
	context->lr = 0;
	context->gpr[1] &= ~0xFu; // keep the stack, 16-byte aligned
	context->gpr[1] -= 0x100;   // below the faulting frame's red zone
	WHBLogPrintf("[crash] %s thread faulted: context=%p owner=%p", main_thread ? "main" : "other",
	             (void*)context, sms_wiiu_vi_owner_thread());
	if (main_thread) {
		sms_wiiu_crash_stage = 1;
	} else {
		sms_wiiu_crash_stage = 10;
		sms_wiiu_crash_pending = 1;
	}
	return TRUE;
}

BOOL on_dsi(OSContext* c) { return fatal("DSI", c); }
BOOL on_isi(OSContext* c) { return fatal("ISI", c); }
BOOL on_program(OSContext* c) { return fatal("PROGRAM", c); }

} // namespace

namespace {
/* Waits on core 1 (ProcUI's core) at the top priority for a main-thread fault,
 * then takes the main thread's place: Menu, ProcUI, exit. */
OSThread s_rescue;
alignas(16) uint8_t s_rescue_stack[16 * 1024];

int rescue(int, const char**)
{
	while (sms_wiiu_crash_stage != 1)
		OSSleepTicks(OSMillisecondsToTicks(100));
	WHBLogPrint("[crash] rescue thread takes over");
	sms_wiiu_crash_return_to_menu();
	return 0;
}
} // namespace

extern "C" void sms_wiiu_install_exception_handlers(void)
{
	/* Priority 0 was refused (2026-10-20); the parked main thread no longer
	 * competes, so any priority above the game's (16) will do. */
	static const int kPriorities[] = {1, 8, 15};
	bool started = false;
	for (int prio : kPriorities) {
		if (OSCreateThread(&s_rescue, rescue, 0, nullptr, s_rescue_stack + sizeof s_rescue_stack,
		                   sizeof s_rescue_stack, prio, OS_THREAD_ATTRIB_AFFINITY_CPU1)) {
			OSResumeThread(&s_rescue);
			WHBLogPrintf("[crash] rescue thread started at priority %d", prio);
			started = true;
			break;
		}
	}
	if (!started)
		WHBLogPrint("[crash] could not start the rescue thread");
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
	sms_wiiu_crash_stage = 2;
	OSSetThreadPriority(OSGetCurrentThread(), 0);
	WHBLogPrint("[crash] returning to the Wii U Menu");
	sms_wiiu_audio_shutdown();
	sms_wiiu_crash_stage = 3;
	SYSLaunchMenu();
	sms_wiiu_crash_stage = 4;
	for (;;) {
		ProcUIStatus status = ProcUIProcessMessages(TRUE);
		if (status == PROCUI_STATUS_RELEASE_FOREGROUND)
			ProcUIDrawDoneRelease();
		else if (status == PROCUI_STATUS_EXITING)
			break;
	}
	sms_wiiu_crash_stage = 5;
	WHBLogPrint("[crash] ProcUI exiting");
	ProcUIShutdown();
	_Exit(0);
}
