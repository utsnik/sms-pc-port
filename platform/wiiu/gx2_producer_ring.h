#ifndef SMS_WIIU_GX2_PRODUCER_RING_H
#define SMS_WIIU_GX2_PRODUCER_RING_H

#include <stdint.h>

namespace gx2prod {

struct ByteReservation {
    uint32_t write;
    uint32_t used;
    uint32_t offset;
    uint32_t capacity;
};

/* Pure ring arithmetic shared by the Wii U producer and its host test.  A
 * reservation never crosses the physical end of the ring; the next reserve
 * starts at offset zero after that short segment is committed. */
inline bool reserveBytes(uint32_t write, uint32_t read, uint32_t ringSize,
                         uint32_t requested, ByteReservation* out) {
    if (!ringSize || !requested || !out) return false;
    const uint32_t used = write - read;
    if (used >= ringSize) return false;
    const uint32_t offset = write % ringSize;
    const uint32_t contiguous = ringSize - offset;
    const uint32_t available = ringSize - used;
    const uint32_t capacity = requested < contiguous ? requested : contiguous;
    const uint32_t amount = capacity < available ? capacity : available;
    if (!amount) return false;
    *out = {write, used, offset, amount};
    return true;
}

inline bool commitBytes(const ByteReservation& reservation, uint32_t size,
                        uint32_t* nextWrite) {
    if (!nextWrite || size > reservation.capacity) return false;
    *nextWrite = reservation.write + size;
    return true;
}

} // namespace gx2prod

#endif
