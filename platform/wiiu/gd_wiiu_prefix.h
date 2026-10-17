/* Force-included before the decompiled GD sources on the Wii U (the PC build
 * uses platform/gx/src/gd_host_prefix.h). GD only writes command bytes into
 * memory; it needs the cached->physical conversion (no GameCube GPU reads these
 * lists here, so the address is kept as is) and __cntlzw. */
#ifndef SMS_WIIU_GD_PREFIX_H
#define SMS_WIIU_GD_PREFIX_H

#include <stdint.h>
#include <dolphin/os.h>

#undef OSCachedToPhysical
#define OSCachedToPhysical(caddr) ((uint32_t)(uintptr_t)(caddr))

#ifndef __cntlzw
static inline unsigned int sms_wiiu_cntlzw(unsigned int x) { return x ? (unsigned int)__builtin_clz(x) : 32u; }
#define __cntlzw(x) sms_wiiu_cntlzw((unsigned int)(x))
#endif

#endif
