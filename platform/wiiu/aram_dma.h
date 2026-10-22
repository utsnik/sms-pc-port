#ifndef SMS_PLATFORM_WIIU_ARAM_DMA_H
#define SMS_PLATFORM_WIIU_ARAM_DMA_H

#include "smg_arena.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t* port_mem1_base;
extern uint32_t port_mem1_size;
extern uint8_t* sms_wiiu_mem2_base;
extern uint32_t sms_wiiu_mem2_size;
#ifdef __cplusplus
}

namespace smg_wiiu {

typedef void (*ArStartDma)(uint32_t direction, uint32_t src, uint32_t dst,
                           uint32_t length);

inline bool aram_range_contains(uint32_t address, uint32_t length,
                                const uint8_t* base, uint32_t size)
{
	if (!base || !size)
		return false;
	const uint32_t lo = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base));
	if (address < lo || address - lo > size)
		return false;
	return length <= size - (address - lo);
}

/* What aram_dma_start did with one JKRAMCommand. */
struct AramDmaResult {
	bool started;             // ARStartDMA was called
	uint32_t forwarded;       // bytes forwarded to ARStartDMA
	uint32_t overhang_offset; // ARAM offset of the first dropped byte past the window
	uint32_t overhang_bytes;  // bytes dropped past the window (0: none)
};

/* JKRAramPiece::startDMA on Wii U (Overwrite.cpp). The main-RAM side must
 * lie in the MEM1 or MEM2 arena; the ARAM side starts at or above
 * ARGetBaseAddress() (0x4000). Ruling R18: retail aralt does not bound the
 * ARAM side, and JKRAram's ARAlloc(0xE00000) audio heap spans
 * [0x4000, 0xE04000), so on a Wii its top 0x4000 bytes land in the GDDR3
 * heap header after the window. Here the in-window part is forwarded and the
 * part past min(aram_size, kAramWindowBytes) is dropped and reported. */
inline AramDmaResult aram_dma_start(int direction, uint32_t src, uint32_t dst,
                                    uint32_t length, uint32_t aram_size,
                                    ArStartDma start)
{
	AramDmaResult result = {false, 0, 0, 0};
	if (!start || (direction != 0 && direction != 1) || length > kAramWindowBytes)
		return result;
	const uint32_t ram = direction == 0 ? src : dst;
	const uint32_t aram = direction == 0 ? dst : src;
	const bool main_ok = aram_range_contains(ram, length, port_mem1_base, port_mem1_size) ||
	                     aram_range_contains(ram, length, sms_wiiu_mem2_base,
	                                         sms_wiiu_mem2_size);
	if (!main_ok || aram < kAramBaseOffset)
		return result;
	const uint32_t window = aram_size < kAramWindowBytes ? aram_size : kAramWindowBytes;
	if (aram >= window) {
		result.overhang_offset = aram;
		result.overhang_bytes = length;
		return result;
	}
	result.forwarded = length <= window - aram ? length : window - aram;
	if (result.forwarded < length) {
		result.overhang_offset = window;
		result.overhang_bytes = length - result.forwarded;
	}
	/* aralt.c and Galaxy's shipped call site use (direction, src, dst).
	 * Do not normalize this to Sunshine's GameCube (ram, aram) convention. */
	start(static_cast<uint32_t>(direction), src, dst, result.forwarded);
	result.started = true;
	return result;
}

} // namespace smg_wiiu
#endif

#endif
