#ifndef SMS_PLATFORM_WIIU_ARAM_DMA_H
#define SMS_PLATFORM_WIIU_ARAM_DMA_H

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

inline bool aram_dma_command_valid(int direction, uint32_t src, uint32_t dst,
                                   uint32_t length)
{
	if ((direction != 0 && direction != 1) || length > 0x00E00000u)
		return false;
	const uint32_t ram = direction == 0 ? src : dst;
	const uint32_t aram = direction == 0 ? dst : src;
	const bool main_ok = aram_range_contains(ram, length, port_mem1_base, port_mem1_size) ||
	                     aram_range_contains(ram, length, sms_wiiu_mem2_base,
	                                         sms_wiiu_mem2_size);
	return main_ok && aram < 0x04000000u && length <= 0x04000000u - aram;
}

} // namespace smg_wiiu
#endif

#endif
