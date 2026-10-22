#ifndef SMG_WIIU_HOST_ALLOC_H
#define SMG_WIIU_HOST_ALLOC_H

#include <stddef.h>

namespace smg_wiiu {

/* port_host_alloc's alignment (ruling R17). Galaxy's operator new passes
 * JKRHeap alignments: a negative value asks for the heap tail, and only the
 * magnitude matters off the JKR heaps. memalign needs a power of two; 8 is
 * the malloc minimum. */
inline size_t host_alloc_alignment(int requested)
{
    const size_t wanted = requested < 0 ? size_t(-(long)requested) : size_t(requested);
    size_t alignment = 8;
    while (alignment < wanted)
        alignment <<= 1;
    return alignment;
}

} // namespace smg_wiiu

#endif
