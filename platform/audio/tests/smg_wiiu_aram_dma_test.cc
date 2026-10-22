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

int main()
{
	port_mem1_base = reinterpret_cast<uint8_t*>(0x10000000u);
	port_mem1_size = 0x1000;
	sms_wiiu_mem2_base = reinterpret_cast<uint8_t*>(0x20000000u);
	sms_wiiu_mem2_size = 0x2000;

	JKRAMCommand ram_to_aram = {0, 0x100, 0x10000100u, smg_wiiu::kAramBaseOffset};
	smg_wiiu::aram_dma_start(ram_to_aram.direction, ram_to_aram.src,
	                         ram_to_aram.dst, ram_to_aram.length,
	                         smg_wiiu::kAramAddressableBytes, ARStartDMA_recording);
	if (check(g_record.count == 1, "MEM1 RAM-to-ARAM command was dropped"))
		return 1;
	if (check(g_record.direction == 0 && g_record.ram == ram_to_aram.src &&
		              g_record.aram == ram_to_aram.dst && g_record.length == ram_to_aram.length,
	          "RAM-to-ARAM ARStartDMA arguments were changed"))
		return 1;

	JKRAMCommand aram_to_ram = {1, 0x80, smg_wiiu::kAramBaseOffset, 0x20001f80u};
	smg_wiiu::aram_dma_start(aram_to_ram.direction, aram_to_ram.src,
	                         aram_to_ram.dst, aram_to_ram.length,
	                         smg_wiiu::kAramAddressableBytes, ARStartDMA_recording);
	if (check(g_record.count == 2 && g_record.direction == 1 &&
              g_record.ram == aram_to_ram.src && g_record.aram == aram_to_ram.dst,
          "ARAM-to-MEM2 ARStartDMA must receive the shipped src,dst order"))
		return 1;

	JKRAMCommand crosses_mem1 = {0, 0x100, 0x10000f80u, 0x00004000u};
	JKRAMCommand crosses_mem2 = {1, 0x100, 0x00004000u, 0x20001f80u};
	JKRAMCommand old_gamecube_address = {0, 0x20, 0x80000000u, 0x00004000u};
	JKRAMCommand crosses_aram = {0, 0x100, 0x20000ff0u,
	                             smg_wiiu::kAramAddressableBytes - 0x80u};
	smg_wiiu::aram_dma_start(crosses_mem1.direction, crosses_mem1.src,
	                         crosses_mem1.dst, crosses_mem1.length,
	                         smg_wiiu::kAramAddressableBytes, ARStartDMA_recording);
	smg_wiiu::aram_dma_start(crosses_mem2.direction, crosses_mem2.src,
	                         crosses_mem2.dst, crosses_mem2.length,
	                         smg_wiiu::kAramAddressableBytes, ARStartDMA_recording);
	smg_wiiu::aram_dma_start(old_gamecube_address.direction, old_gamecube_address.src,
	                         old_gamecube_address.dst, old_gamecube_address.length,
	                         smg_wiiu::kAramAddressableBytes, ARStartDMA_recording);
	smg_wiiu::aram_dma_start(crosses_aram.direction, crosses_aram.src,
	                         crosses_aram.dst, crosses_aram.length,
	                         smg_wiiu::kAramAddressableBytes, ARStartDMA_recording);
	if (check(g_record.count == 2, "an out-of-range DMA command reached ARStartDMA"))
		return 1;

	puts("aram-dma-test: PASS: MEM1/MEM2 directions and boundary drops verified");
	return 0;
}
