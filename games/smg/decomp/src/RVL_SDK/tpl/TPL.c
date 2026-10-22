#include "revolution/tpl.h"
#include "revolution/os.h"

#if defined(TARGET_PC) && !PORT_HOST_BIG_ENDIAN
/* Offsets and pointers in the palette are 32-bit fields (PTR32). */
#define TPL_PTR(T, v) ((T)(uintptr_t)(v))

void TPLBind(TPLPalettePtr ptr) {
    u16 i;
    u32 base = (u32)(uintptr_t)ptr;
    TPLDescriptorPtr desc;

    if (ptr->versionNumber != 2142000) {
        OSPanic(__FILE__, 0x19, "invalid version number for texture palette");
    }

    ptr->descriptorArray = ptr->descriptorArray + base;
    desc = TPL_PTR(TPLDescriptorPtr, ptr->descriptorArray);

    for (i = 0; i < ptr->numDescriptors; i++) {
        if (desc[i].textureHeader) {
            TPLHeaderPtr th;
            desc[i].textureHeader = desc[i].textureHeader + base;
            th = TPL_PTR(TPLHeaderPtr, desc[i].textureHeader);

            if (!th->unpacked) {
                th->data = th->data + base;
                th->unpacked = 1;
            }
        }

        if (desc[i].CLUTHeader) {
            TPLClutHeaderPtr ch;
            desc[i].CLUTHeader = desc[i].CLUTHeader + base;
            ch = TPL_PTR(TPLClutHeaderPtr, desc[i].CLUTHeader);

            if (!ch->unpacked) {
                ch->data = ch->data + base;
                ch->unpacked = 1;
            }
        }
    }
}

TPLDescriptorPtr TPLGet(TPLPalettePtr ptr, u32 id) {
    id %= ptr->numDescriptors;
    return &TPL_PTR(TPLDescriptorPtr, ptr->descriptorArray)[id];
}
#else
void TPLBind(TPLPalettePtr ptr) {
    u16 i;
    TPLDescriptorPtr descriptors = ptr->descriptorArray;

    if (ptr->versionNumber != 2142000) {
        OSPanic(__FILE__, 0x19, "invalid version number for texture palette");
    }

    ptr->descriptorArray = (TPLDescriptorPtr)(((u32)(ptr->descriptorArray)) + ((u32)ptr));
    descriptors = ptr->descriptorArray;

    for (i = 0; i < ptr->numDescriptors; i++) {
        TPLHeaderPtr textureHeader = (TPLHeaderPtr)(uintptr_t)descriptors[i].textureHeader;
        TPLClutHeaderPtr clutHeader = (TPLClutHeaderPtr)(uintptr_t)descriptors[i].CLUTHeader;
        if (textureHeader) {
            textureHeader = (TPLHeaderPtr)(((u32)(uintptr_t)textureHeader) + ((u32)ptr));
            descriptors[i].textureHeader = (uint32_t)(uintptr_t)textureHeader;
        
            if (!textureHeader->unpacked) {
                textureHeader->data = (uint32_t)((u32)(textureHeader->data) + (u32)ptr);
                textureHeader->unpacked = 1;
            }
        }

        if (clutHeader) {
            clutHeader = (TPLClutHeaderPtr)((u32)(uintptr_t)clutHeader + (u32)ptr);
            descriptors[i].CLUTHeader = (uint32_t)(uintptr_t)clutHeader;

            if (!clutHeader->unpacked) {
                clutHeader->data = (uint32_t)((u32)(clutHeader->data) + (u32)ptr);
                clutHeader->unpacked = 1;
            }
        }
    }
}

TPLDescriptorPtr TPLGet(TPLPalettePtr ptr, u32 id) {
    id %= ptr->numDescriptors;
    TPLDescriptorPtr descriptors = ptr->descriptorArray;
    return &descriptors[id];
}
#endif
