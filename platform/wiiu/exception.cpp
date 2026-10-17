/* Fatal exception reporting for the first hardware boots: registers go to the
 * UDP log and to the OSFatal screen. Kept free of decomp headers, whose OS types
 * clash with coreinit's. ALIGNMENT is not hooked: GameCube code can do
 * misaligned float accesses that the OS otherwise handles. */
#include <coreinit/core.h>
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <whb/log.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

char s_message[512];
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
	OSFatal(s_message);
	return FALSE;
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
