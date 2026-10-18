/* Force-included before the decompiled GD sources on the Wii U (the PC build
 * uses platform/gx/src/gd_host_prefix.h). GD only writes command bytes into
 * memory; it needs the cached->physical conversion and __cntlzw. */
#ifndef SMS_WIIU_GD_PREFIX_H
#define SMS_WIIU_GD_PREFIX_H

#include <stdint.h>
#include <dolphin/os.h>

/* The GX decoder resolves CP array bases as an offset into its memory window
 * (physToPtr), so GD must write window offsets too: the raw pointer made every
 * J3D vertex array resolve to garbage and all stage geometry decoded as 0,0,0. */
#ifdef __cplusplus
extern "C" {
#endif
uint32_t GXPC_PtrToPhys(const void* p);
#ifdef __cplusplus
}
#endif
#undef OSCachedToPhysical
#define OSCachedToPhysical(caddr) GXPC_PtrToPhys((const void*)(caddr))

#ifndef __cntlzw
static inline unsigned int sms_wiiu_cntlzw(unsigned int x) { return x ? (unsigned int)__builtin_clz(x) : 32u; }
#define __cntlzw(x) sms_wiiu_cntlzw((unsigned int)(x))
#endif

#endif
