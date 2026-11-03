#include "../../wiiu/gx2_producer_ring.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

static bool writeSplit(const std::vector<uint8_t>& input, uint32_t ringSize,
                       uint32_t* write, uint32_t read, std::vector<uint8_t>* ring,
                       std::vector<uint8_t>* staging) {
    const uint32_t chunk = 8;
    uint32_t pos = 0;
    while (pos < input.size()) {
        gx2prod::ByteReservation reservation = {};
        if (!gx2prod::reserveBytes(*write, read, ringSize, chunk, &reservation)) {
            staging->insert(staging->end(), input.begin() + pos, input.end());
            return false;
        }
        const uint32_t take = std::min<uint32_t>(reservation.capacity, input.size() - pos);
        std::copy(input.begin() + pos, input.begin() + pos + take,
                  ring->begin() + reservation.offset);
        assert(gx2prod::commitBytes(reservation, take, write));
        pos += take;
    }
    return true;
}

static void appendStaging(const std::vector<uint8_t>& staging, uint32_t ringSize,
                          uint32_t* write, std::vector<uint8_t>* ring) {
    for (uint8_t byte : staging) {
        (*ring)[*write % ringSize] = byte;
        ++*write;
    }
}

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

    // Mirror pipePut's reservation loop: a command fills the short tail,
    // wraps through another reservation, then stages the remainder when the
    // ring becomes full.  Reassembling the committed bytes and the staged
    // fallback must preserve the original command exactly.
    const uint32_t ringSize = 16;
    const uint32_t initialWrite = 14;
    const uint32_t initialRead = 9;
    std::vector<uint8_t> ring(ringSize, 0);
    std::vector<uint8_t> command(13);
    for (uint32_t i = 0; i < command.size(); ++i) command[i] = uint8_t(0xA0 + i);
    std::vector<uint8_t> staging;
    uint32_t splitWrite = initialWrite;
    assert(!writeSplit(command, ringSize, &splitWrite, initialRead, &ring, &staging));
    assert(splitWrite == initialWrite + 11);
    assert(staging.size() == 2);

    std::vector<uint8_t> reassembled;
    for (uint32_t at = initialWrite; at != splitWrite; ++at)
        reassembled.push_back(ring[at % ringSize]);
    // The real append waits for the worker to consume the committed prefix,
    // then copies the staging fallback through the normal wrapping path.
    uint32_t resumedWrite = splitWrite;
    appendStaging(staging, ringSize, &resumedWrite, &ring);
    for (uint32_t at = splitWrite; at != resumedWrite; ++at)
        reassembled.push_back(ring[at % ringSize]);
    assert(reassembled == command);
    return 0;
}
