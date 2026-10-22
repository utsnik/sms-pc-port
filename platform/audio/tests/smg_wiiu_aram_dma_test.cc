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

static void startDMA(JKRAMCommand* command)
{
	if (!smg_wiiu::aram_dma_command_valid(command->direction, command->src,
	                                     command->dst, command->length))
		return;
	const uint32_t ram = command->direction == 0 ? command->src : command->dst;
	const uint32_t aram = command->direction == 0 ? command->dst : command->src;
	ARStartDMA_recording(command->direction, ram, aram, command->length);
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

	JKRAMCommand ram_to_aram = {0, 0x100, 0x10000100u, 0x00002000u};
	startDMA(&ram_to_aram);
	if (check(g_record.count == 1, "MEM1 RAM-to-ARAM command was dropped"))
		return 1;
	if (check(g_record.direction == 0 && g_record.ram == ram_to_aram.src &&
	              g_record.aram == ram_to_aram.dst && g_record.length == ram_to_aram.length,
	          "RAM-to-ARAM ARStartDMA arguments were changed"))
		return 1;

	JKRAMCommand aram_to_ram = {1, 0x80, 0x00003000u, 0x20001f80u};
	startDMA(&aram_to_ram);
	if (check(g_record.count == 2 && g_record.direction == 1 &&
              g_record.ram == aram_to_ram.dst && g_record.aram == aram_to_ram.src,
	          "ARAM-to-MEM2 ARStartDMA arguments were changed"))
		return 1;

	JKRAMCommand crosses_mem1 = {0, 0x100, 0x10000f80u, 0x00004000u};
	JKRAMCommand crosses_mem2 = {1, 0x100, 0x00004000u, 0x20001f80u};
	JKRAMCommand old_gamecube_address = {0, 0x20, 0x80000000u, 0x00004000u};
	JKRAMCommand crosses_aram = {0, 0x100, 0x20000ff0u, 0x03ffff80u};
	startDMA(&crosses_mem1);
	startDMA(&crosses_mem2);
	startDMA(&old_gamecube_address);
	startDMA(&crosses_aram);
	if (check(g_record.count == 2, "an out-of-range DMA command reached ARStartDMA"))
		return 1;

	puts("aram-dma-test: PASS: MEM1/MEM2 directions and boundary drops verified");
	return 0;
}
