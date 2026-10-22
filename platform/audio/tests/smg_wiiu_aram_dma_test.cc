#include "../../../platform/wiiu/aram_dma.h"

#include <stdint.h>
#include <stdio.h>

extern "C" {
uint8_t* port_mem1_base;
uint32_t port_mem1_size;
uint8_t* sms_wiiu_mem2_base;
uint32_t sms_wiiu_mem2_size;
}

struct JKRAMCommand {
	int32_t direction;
	uint32_t length;
	uint32_t src;
	uint32_t dst;
};

struct DMARecord {
	unsigned count;
	uint32_t direction;
	uint32_t ram;
	uint32_t aram;
	uint32_t length;
};

static DMARecord g_record;

static void ARStartDMA_recording(uint32_t direction, uint32_t ram, uint32_t aram,
	                              uint32_t length)
{
	++g_record.count;
	g_record.direction = direction;
	g_record.ram = ram;
	g_record.aram = aram;
	g_record.length = length;
}

static int check(bool condition, const char* message)
{
	if (condition)
		return 0;
	fprintf(stderr, "aram-dma-test: FAIL: %s\n", message);
	return 1;
}

static smg_wiiu::AramDmaResult start_dma(const JKRAMCommand& c)
{
	/* Overwrite.cpp passes ARGetSize(): retail aralt's __AR_Size, the
	 * 0xE00000 MEM2 window createRootHeap carved out. */
	return smg_wiiu::aram_dma_start(c.direction, c.src, c.dst, c.length,
	                                smg_wiiu::kAramWindowBytes, ARStartDMA_recording);
}

int main()
{
	port_mem1_base = reinterpret_cast<uint8_t*>(0x10000000u);
	port_mem1_size = 0x1000;
	sms_wiiu_mem2_base = reinterpret_cast<uint8_t*>(0x20000000u);
	sms_wiiu_mem2_size = 0x2000;
	const uint32_t window = smg_wiiu::kAramWindowBytes;

	JKRAMCommand ram_to_aram = {0, 0x100, 0x10000100u, smg_wiiu::kAramBaseOffset};
	smg_wiiu::AramDmaResult r = start_dma(ram_to_aram);
	if (check(g_record.count == 1 && r.started && r.forwarded == 0x100 && !r.overhang_bytes,
	          "MEM1 RAM-to-ARAM command was dropped"))
		return 1;
	if (check(g_record.direction == 0 && g_record.ram == ram_to_aram.src &&
		              g_record.aram == ram_to_aram.dst && g_record.length == ram_to_aram.length,
	          "RAM-to-ARAM ARStartDMA arguments were changed"))
		return 1;

	JKRAMCommand aram_to_ram = {1, 0x80, smg_wiiu::kAramBaseOffset, 0x20001f80u};
	r = start_dma(aram_to_ram);
	if (check(g_record.count == 2 && g_record.direction == 1 &&
              g_record.ram == aram_to_ram.src && g_record.aram == aram_to_ram.dst &&
              r.started && !r.overhang_bytes,
          "ARAM-to-MEM2 ARStartDMA must receive the shipped src,dst order"))
		return 1;

	/* The last in-window bytes are delivered whole. */
	JKRAMCommand window_tail = {0, 0x80, 0x20000f00u, window - 0x80u};
	r = start_dma(window_tail);
	if (check(g_record.count == 3 && g_record.length == 0x80 && r.started && !r.overhang_bytes,
	          "a DMA ending exactly at the 0xE00000 window was not forwarded whole"))
		return 1;

	/* Retail overhang: JKRAram's ARAlloc(0xE00000) audio heap spans
	 * [0x4000, 0xE04000). The in-window part is copied; the 0x4000 beyond
	 * the window (the GDDR3 heap header on a Wii) is dropped and reported. */
	JKRAMCommand straddle = {0, 0x100, 0x20000e00u, window - 0x80u};
	r = start_dma(straddle);
	if (check(g_record.count == 4 && g_record.direction == 0 &&
	              g_record.ram == straddle.src && g_record.aram == straddle.dst &&
	              g_record.length == 0x80 && r.started && r.forwarded == 0x80,
	          "a DMA straddling the window was not clipped to its in-window part"))
		return 1;
	if (check(r.overhang_offset == window && r.overhang_bytes == 0x80,
	          "the straddling DMA's overhang (offset, size) was not reported"))
		return 1;
	JKRAMCommand straddle_read = {1, 0x100, window - 0x40u, 0x20001e00u};
	r = start_dma(straddle_read);
	if (check(g_record.count == 5 && g_record.direction == 1 && g_record.length == 0x40 &&
	              g_record.ram == straddle_read.src && g_record.aram == straddle_read.dst &&
	              r.overhang_offset == window && r.overhang_bytes == 0xC0,
	          "an ARAM-to-RAM read straddling the window was not clipped and reported"))
		return 1;

	JKRAMCommand beyond = {0, 0x40, 0x20000e00u, window + 0x1000u};
	r = start_dma(beyond);
	if (check(g_record.count == 5 && !r.started && r.overhang_offset == window + 0x1000u &&
	              r.overhang_bytes == 0x40,
	          "a DMA wholly beyond the window was forwarded or not reported"))
		return 1;

	JKRAMCommand crosses_mem1 = {0, 0x100, 0x10000f80u, 0x00004000u};
	JKRAMCommand crosses_mem2 = {1, 0x100, 0x00004000u, 0x20001f80u};
	JKRAMCommand old_gamecube_address = {0, 0x20, 0x80000000u, 0x00004000u};
	JKRAMCommand reserved_low = {0, 0x20, 0x10000100u, 0x00003fe0u};
	JKRAMCommand too_long = {0, window + 0x20u, 0x10000100u, 0x00004000u};
	const JKRAMCommand invalid[] = {crosses_mem1, crosses_mem2, old_gamecube_address,
	                                reserved_low, too_long};
	for (const JKRAMCommand& c : invalid) {
		r = start_dma(c);
		if (check(!r.started && !r.overhang_bytes, "an invalid DMA was forwarded or reported as overhang"))
			return 1;
	}
	if (check(g_record.count == 5, "an out-of-range DMA command reached ARStartDMA"))
		return 1;

	/* port_varam_ptr's bound (runtime.cpp calls smg_wiiu::varam_lookup). */
	static uint8_t varam[0x100];
	const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(varam));
	bool beyond_window = false;
	const uint8_t* p = smg_wiiu::varam_lookup(base, window, base + window - 1u, &beyond_window);
	if (check(p == reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(base + window - 1u)) &&
	              !beyond_window,
	          "the last VARAM window byte did not resolve"))
		return 1;
	p = smg_wiiu::varam_lookup(base, window, base + window, &beyond_window);
	if (check(!p && beyond_window, "VARAM offset 0xE00000 resolved (aliases the GDDR3 heap)"))
		return 1;
	beyond_window = false;
	p = smg_wiiu::varam_lookup(base, window + 0x4000u, base + window + 0x10u, &beyond_window);
	if (check(!p && beyond_window, "a bound larger than the window was honoured"))
		return 1;
	beyond_window = false;
	p = smg_wiiu::varam_lookup(base, window, base + 0x10u, &beyond_window);
	if (check(!p && !beyond_window, "a reserved low VARAM offset resolved"))
		return 1;
	p = smg_wiiu::varam_lookup(0, window, 0x4000u, &beyond_window);
	if (check(!p, "VARAM resolved before ARInit bound a base"))
		return 1;

	puts("aram-dma-test: PASS: MEM1/MEM2 directions, 0xE00000 window clip + overhang report, "
	     "invalid drops, VARAM bound");
	return 0;
}
