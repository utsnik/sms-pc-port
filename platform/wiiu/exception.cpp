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
#include "gx2_texture_pack.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(PORT_GAME_SMG) && !defined(SMG_WIIU_ENABLE_RESCUE)
#define SMG_WIIU_ENABLE_RESCUE 0
#endif

/* The build maps OSCreateThread & co. to the GameCube adapters for every port
 * file (SMS_WIIU_OS_ADAPTER_DEFS); the crash path needs coreinit's own. */
extern "C" {
BOOL cafe_OSCreateThread(OSThread*, OSThreadEntryPointFn, int32_t, char*, void*, uint32_t, int32_t,
                         OSThreadAttributes) __asm__("OSCreateThread");
int32_t cafe_OSResumeThread(OSThread*) __asm__("OSResumeThread");
OSThread* cafe_OSGetCurrentThread(void) __asm__("OSGetCurrentThread");
}

extern "C" void* sms_wiiu_vi_owner_thread(void);
extern "C" void sms_wiiu_crash_return_to_menu(void);
extern "C" void sms_wiiu_exit(void);
extern "C" void sms_wiiu_audio_shutdown(void);
extern "C" void sms_wiiu_d1_tick(void);
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
volatile int s_fault_count;
char s_trace[2048];

#if defined(PORT_GAME_SMG) && SMG_WIIU_ENABLE_RESCUE
enum { kRescueStackBytes = 0x200 };

struct RescueCapture {
	uint32_t gpr[32];
	uint32_t srr0;
	uint32_t srr1;
	uint32_t dar;
	uint32_t dsisr;
	uint32_t lr;
	uint32_t stack_address;
	uint32_t stack_size;
	const char* type;
	void* faulting_thread;
	uint8_t stack[kRescueStackBytes];
};

RescueCapture s_rescue_capture;
volatile bool s_rescue_started;
void* s_rescue_owner_thread;

BOOL fatal_no_rescue(const char* type, OSContext* context);
#endif

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
 * whatever overwrote it may have left recognisable data next to it. Built in
 * one buffer and printed with WHBLogPrint: WHBLogPrintf from the handler sent
 * the first line again and again over the dump (2026-10-20). */
char s_dump[4096];

void dump_line(const char* fmt, ...)
{
	size_t used = std::strlen(s_dump);
	if (used > 1000) { // keep each print well inside one UDP datagram
		WHBLogPrint(s_dump);
		s_dump[0] = '\0';
		used = 0;
	}
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(s_dump + used, sizeof(s_dump) - used, fmt, ap);
	va_end(ap);
}

void dump(OSContext* context)
{
	s_dump[0] = '\0';
	for (int i = 0; i < 32; i += 8)
		dump_line("[crash] r%-2d %08X %08X %08X %08X %08X %08X %08X %08X\n", i,
		          (unsigned)context->gpr[i], (unsigned)context->gpr[i + 1], (unsigned)context->gpr[i + 2],
		          (unsigned)context->gpr[i + 3], (unsigned)context->gpr[i + 4], (unsigned)context->gpr[i + 5],
		          (unsigned)context->gpr[i + 6], (unsigned)context->gpr[i + 7]);
	uint32_t base = (context->gpr[3] & ~31u) - 0x100;
	if (readable(base) && readable(base + 0x200)) {
		for (uint32_t a = base; a < base + 0x200; a += 32) {
			const uint32_t* w = (const uint32_t*)a;
			dump_line("[crash] %08X: %08X %08X %08X %08X %08X %08X %08X %08X\n", (unsigned)a, (unsigned)w[0],
			          (unsigned)w[1], (unsigned)w[2], (unsigned)w[3], (unsigned)w[4], (unsigned)w[5],
			          (unsigned)w[6], (unsigned)w[7]);
		}
	}
	WHBLogPrint(s_dump);
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

#if defined(PORT_GAME_SMG) && SMG_WIIU_ENABLE_RESCUE
void capture_stack(RescueCapture* capture, uint32_t sp)
{
	capture->stack_address = sp;
	capture->stack_size = 0;
	if (sp < 0x10000000u || sp >= 0x50000000u || (sp & 3u) ||
	    sp > 0x50000000u - kRescueStackBytes)
		return;
	volatile const uint8_t* source = reinterpret_cast<volatile const uint8_t*>(sp);
	for (uint32_t i = 0; i < kRescueStackBytes; ++i)
		capture->stack[i] = source[i];
	capture->stack_size = kRescueStackBytes;
}

BOOL rescue_fatal(const char* type, OSContext* context)
{
	if (!s_rescue_started)
		return fatal_no_rescue(type, context);

	RescueCapture* capture = &s_rescue_capture;
	capture->type = type;
	capture->faulting_thread = context;
	if (!context) {
		capture->stack_address = 0;
		capture->stack_size = 0;
		__sync_synchronize();
		sms_wiiu_crash_pending = 1;
		sms_wiiu_crash_stage = 1;
		return FALSE;
	}
	for (int i = 0; i < 32; ++i)
		capture->gpr[i] = context->gpr[i];
	capture->srr0 = context->srr0;
	capture->srr1 = context->srr1;
	capture->dar = context->dar;
	capture->dsisr = context->dsisr;
	capture->lr = context->lr;
	capture_stack(capture, capture->gpr[1]);

	/* A resumed faulting thread does not continue at its original PC. */
	context->srr0 = (uint32_t)&park_forever;
	context->lr = 0;
	context->gpr[1] &= ~0xFu;
	context->gpr[1] -= 0x100;

	/* The callback does no formatting, symbol lookup, memory walking, or
	 * logging. Publish the complete snapshot before the rescue thread sees it. */
	__sync_synchronize();
	sms_wiiu_crash_pending = 1;
	sms_wiiu_crash_stage = context == s_rescue_owner_thread ? 1 : 10;
	return TRUE;
}
#endif

BOOL fatal(const char* type, OSContext* context)
{
	/* Keep the fault address on one short line before the detailed report.
	 * The rescue build stays capture-only here; its rescue thread prints it. */
#if !(defined(PORT_GAME_SMG) && SMG_WIIU_ENABLE_RESCUE)
	if (context) {
		char line[128];
		std::snprintf(line, sizeof(line), "[x] %s #%d srr0=%08X lr=%08X dar=%08X r1=%08X", type,
		              __sync_add_and_fetch(&s_fault_count, 1), (unsigned)context->srr0,
		              (unsigned)context->lr, (unsigned)context->dar, (unsigned)context->gpr[1]);
		WHBLogPrint(line);
	}
#endif
#if defined(PORT_GAME_SMG) && SMG_WIIU_ENABLE_RESCUE
	return rescue_fatal(type, context);
#else
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
#if defined(PORT_GAME_SMG) && !SMG_WIIU_ENABLE_RESCUE
	/* The release candidate records the fault and leaves the reliable fatal
	 * screen/reboot path intact; no recovery thread is started. */
	OSFatal(s_message);
	return FALSE;
#else
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
	std::snprintf(s_message, sizeof(s_message), "[crash] %s thread faulted: context=%p owner=%p",
	              main_thread ? "main" : "other", (void*)context, sms_wiiu_vi_owner_thread());
	WHBLogPrint(s_message);
	if (main_thread) {
		sms_wiiu_crash_stage = 1;
	} else {
		sms_wiiu_crash_stage = 10;
		sms_wiiu_crash_pending = 1;
	}
	return TRUE;
#endif
#endif
}

#if defined(PORT_GAME_SMG) && SMG_WIIU_ENABLE_RESCUE
BOOL fatal_no_rescue(const char* type, OSContext* context)
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
	OSFatal(s_message);
	return FALSE;
}
#endif

BOOL on_dsi(OSContext* c) { return fatal("DSI", c); }
BOOL on_isi(OSContext* c) { return fatal("ISI", c); }
BOOL on_program(OSContext* c) { return fatal("PROGRAM", c); }

} // namespace

namespace {
#if !defined(PORT_GAME_SMG) || SMG_WIIU_ENABLE_RESCUE
#if defined(PORT_GAME_SMG) && SMG_WIIU_ENABLE_RESCUE
uint32_t captured_word(const RescueCapture& capture, uint32_t address)
{
	if (!capture.stack_size || address < capture.stack_address ||
	    address > capture.stack_address + capture.stack_size - sizeof(uint32_t))
		return 0;
	uint32_t value;
	std::memcpy(&value, capture.stack + (address - capture.stack_address), sizeof(value));
	return value;
}

void trace_capture(const RescueCapture& capture)
{
	uint32_t sp = capture.gpr[1];
	s_trace[0] = '\0';
	append_symbol("PC", capture.srr0);
	append_symbol("LR", capture.lr);
	for (int i = 0; i < 16; ++i) {
		if (!capture.stack_size || sp < capture.stack_address ||
		    sp > capture.stack_address + capture.stack_size - 8)
			break;
		uint32_t next = captured_word(capture, sp);
		if (next <= sp || next >= 0x50000000u || next > capture.stack_address + capture.stack_size - 8)
			break;
		append_symbol("BT", captured_word(capture, next + 4));
		sp = next;
	}
}

void dump_capture(const RescueCapture& capture)
{
	s_dump[0] = '\0';
	for (int i = 0; i < 32; i += 8)
		dump_line("[crash] r%-2d %08X %08X %08X %08X %08X %08X %08X %08X\n", i,
		          (unsigned)capture.gpr[i], (unsigned)capture.gpr[i + 1], (unsigned)capture.gpr[i + 2],
		          (unsigned)capture.gpr[i + 3], (unsigned)capture.gpr[i + 4], (unsigned)capture.gpr[i + 5],
		          (unsigned)capture.gpr[i + 6], (unsigned)capture.gpr[i + 7]);
	for (uint32_t address = capture.stack_address;
	     capture.stack_size && address < capture.stack_address + capture.stack_size; address += 32) {
		dump_line("[crash] %08X: %08X %08X %08X %08X %08X %08X %08X %08X\n", (unsigned)address,
		          (unsigned)captured_word(capture, address), (unsigned)captured_word(capture, address + 4),
		          (unsigned)captured_word(capture, address + 8), (unsigned)captured_word(capture, address + 12),
		          (unsigned)captured_word(capture, address + 16), (unsigned)captured_word(capture, address + 20),
		          (unsigned)captured_word(capture, address + 24), (unsigned)captured_word(capture, address + 28));
	}
	WHBLogPrint(s_dump);
}

bool report_capture(const RescueCapture& capture)
{
	if (!capture.faulting_thread) {
		std::snprintf(s_message, sizeof(s_message), "SMS Wii U %s exception (no context)\n", capture.type);
		WHBLogPrint(s_message);
		OSFatal(s_message);
		return false;
	}
	std::snprintf(s_message, sizeof(s_message), "[x] %s #%d srr0=%08X lr=%08X dar=%08X r1=%08X",
	              capture.type, __sync_add_and_fetch(&s_fault_count, 1), (unsigned)capture.srr0,
	              (unsigned)capture.lr, (unsigned)capture.dar, (unsigned)capture.gpr[1]);
	WHBLogPrint(s_message);
	std::snprintf(s_message, sizeof(s_message),
	              "SMS Wii U %s exception\nSRR0=%08X SRR1=%08X\nDAR=%08X DSISR=%08X\n"
	              "LR=%08X R1=%08X R3=%08X R4=%08X\n",
	              capture.type, (unsigned)capture.srr0, (unsigned)capture.srr1,
	              (unsigned)capture.dar, (unsigned)capture.dsisr, (unsigned)capture.lr,
	              (unsigned)capture.gpr[1], (unsigned)capture.gpr[3], (unsigned)capture.gpr[4]);
	WHBLogPrint(s_message);
	trace_capture(capture);
	WHBLogPrint(s_trace);
	dump_capture(capture);
	std::snprintf(s_message, sizeof(s_message), "[crash] %s thread faulted: context=%p owner=%p",
	              capture.faulting_thread == s_rescue_owner_thread ? "main" : "other",
	              capture.faulting_thread, s_rescue_owner_thread);
	WHBLogPrint(s_message);
	return true;
}
#endif

/* Waits on core 1 (ProcUI's core) at the top priority for a main-thread fault,
 * then takes the main thread's place: Menu, ProcUI, exit. */
OSThread s_rescue;
alignas(16) uint8_t s_rescue_stack[16 * 1024];

int rescue(int, const char**)
{
#if defined(PORT_GAME_SMG) && SMG_WIIU_ENABLE_RESCUE
	for (;;) {
		while (sms_wiiu_crash_stage != 1 && sms_wiiu_crash_stage != 10)
			OSSleepTicks(OSMillisecondsToTicks(100));
		__sync_synchronize();
		RescueCapture capture = s_rescue_capture;
		if (!report_capture(capture))
			return 0;
		if (capture.faulting_thread == s_rescue_owner_thread) {
			WHBLogPrint("[crash] rescue thread takes over");
			sms_wiiu_crash_stage = 1;
			sms_wiiu_crash_return_to_menu();
			return 0;
		}
		sms_wiiu_crash_stage = 11;
	}
#else
	while (sms_wiiu_crash_stage != 1)
		OSSleepTicks(OSMillisecondsToTicks(100));
	WHBLogPrint("[crash] rescue thread takes over");
	sms_wiiu_crash_return_to_menu();
	return 0;
#endif
}
#endif
} // namespace

#if defined(PORT_GAME_SMG)
/* Galaxy's audio pacer, which drives the hang check in Sunshine, had not
 * started when the first console runs hung during init: check from a thread
 * of its own every 10 s. */
extern "C" void sms_wiiu_hang_check(void);
extern "C" void smg_wiiu_flush_reports(void);
static OSThread s_watchdog;
alignas(16) static uint8_t s_watchdog_stack[8 * 1024];

static int watchdog(int, const char**)
{
	unsigned ticks = 0;
	for (;;) {
		OSSleepTicks(OSMillisecondsToTicks(1000));
		sms_wiiu_d1_tick();
		if (++ticks == 10) {
			ticks = 0;
			smg_wiiu_flush_reports();
			sms_wiiu_hang_check();
		}
	}
	return 0;
}
#endif

extern "C" void sms_wiiu_install_exception_handlers(void)
{
#if defined(PORT_GAME_SMG)
	if (cafe_OSCreateThread(&s_watchdog, watchdog, 0, nullptr, s_watchdog_stack + sizeof s_watchdog_stack,
	                        sizeof s_watchdog_stack, 2, OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
		cafe_OSResumeThread(&s_watchdog);
		WHBLogPrint("[watchdog] thread started");
	} else {
		WHBLogPrint("[watchdog] could not start the thread");
	}
#endif
	/* Priority 0 was refused (2026-10-20); the parked main thread no longer
	 * competes, so any priority above the game's (16) will do. */
#if !defined(PORT_GAME_SMG) || SMG_WIIU_ENABLE_RESCUE
	#if defined(PORT_GAME_SMG) && SMG_WIIU_ENABLE_RESCUE
	s_rescue_owner_thread = sms_wiiu_vi_owner_thread();
	#endif
	static const int kPriorities[] = {1, 8, 15};
	bool started = false;
	for (int prio : kPriorities) {
		if (cafe_OSCreateThread(&s_rescue, rescue, 0, nullptr, s_rescue_stack + sizeof s_rescue_stack,
		                   sizeof s_rescue_stack, prio, OS_THREAD_ATTRIB_AFFINITY_CPU1)) {
			cafe_OSResumeThread(&s_rescue);
			WHBLogPrintf("[crash] rescue thread started at priority %d", prio);
			started = true;
			break;
		}
	}
	#if defined(PORT_GAME_SMG) && SMG_WIIU_ENABLE_RESCUE
	s_rescue_started = started;
	#endif
	if (!started)
		WHBLogPrint("[crash] could not start the rescue thread");
#else
	WHBLogPrint("[crash] rescue thread disabled for the SMG no-rescue build");
#endif
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, on_dsi);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, on_isi);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, on_program);
}

/* Runs on the main thread after a crash: ask for the Menu, hand over the
 * foreground when ProcUI asks, and end the process without running the game's
 * global destructors (other game threads are still alive). WHBProc/std::exit
 * were not used here: with them the Menu started (its music played) but the
 * app never let go of the screen. */
extern "C" void sms_wiiu_vsync_pace_stop(void);

extern "C" void sms_wiiu_crash_return_to_menu(void)
{
	sms_wiiu_vsync_pace_stop();
	sms_wiiu_crash_stage = 2;
	OSSetThreadPriority(cafe_OSGetCurrentThread(), 1);
	WHBLogPrint("[crash] returning to the Wii U Menu");
	gx::texturePackShutdown();
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
