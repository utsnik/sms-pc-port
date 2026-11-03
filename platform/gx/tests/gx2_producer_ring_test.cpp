#include "../../wiiu/gx2_producer_ring.h"

#include <cassert>
#include <cstdint>

int main() {
    gx2prod::ByteReservation r = {};
    uint32_t next = 0;

    // A reservation at the physical end is shortened, then the next one
    // wraps to zero without exposing the unreserved tail to the producer.
    assert(gx2prod::reserveBytes(14, 8, 16, 8, &r));
    assert(r.offset == 14 && r.used == 6 && r.capacity == 2);
    assert(gx2prod::commitBytes(r, 2, &next) && next == 16);
    assert(gx2prod::reserveBytes(next, 8, 16, 8, &r));
    assert(r.offset == 0 && r.used == 8 && r.capacity == 8);
    assert(gx2prod::commitBytes(r, 8, &next) && next == 24);

    // A full ring is a hard reservation failure; the caller can retain its
    // staging fallback and append after the consumer advances the read index.
    assert(!gx2prod::reserveBytes(24, 8, 16, 4, &r));
    assert(gx2prod::reserveBytes(24, 9, 16, 4, &r));
    assert(r.offset == 8 && r.capacity == 1);
    assert(gx2prod::commitBytes(r, 1, &next) && next == 25);
    return 0;
}
