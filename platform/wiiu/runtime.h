#ifndef SMS_PLATFORM_WIIU_RUNTIME_H
#define SMS_PLATFORM_WIIU_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage 1 owns a fixed, process-lifetime arena.  It deliberately does not
 * depend on the wut malloc wrappers; the game is not entered by default. */
void sms_wiiu_runtime_init(void);

extern uint8_t* port_mem1_base;
extern uint32_t port_mem1_size;

void port_init(int argc, char** argv);
void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void* port_low_alloc(unsigned long size);
int64_t port_time_ticks(void);

#ifdef __cplusplus
}
#endif

#endif
