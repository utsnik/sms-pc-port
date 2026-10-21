#include "../../wiiu/gx2_texture_pack.h"

#include <cstdio>
#include <cstdlib>

using namespace gx;

static int checks;
static void check(bool ok, const char* name) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}

static TexturePackRequest request(const uint8_t* p, uint32_t id) {
    TexturePackRequest r = {};
    r.source = p; r.width = 8; r.height = 8; r.format = 6; r.levels = 1;
    r.writeClock = 10; r.requestId = id;
    return r;
}

int main() {
    uint8_t bytes[8] = {};
    TexturePackRequestQueue q;
    TexturePackRequest a = request(bytes, 1), b = request(bytes + 1, 2);
    check(q.push(a) && q.contains(a) && q.size() == 1, "queue push and dedup key");
    check(q.push(a) && q.size() == 1, "queue duplicate does not grow");
    check(q.push(b) && q.size() == 2, "queue accepts distinct source");
    TexturePackRequest out = {};
    check(q.pop(&out) && out.source == bytes, "queue fifo");
    check(q.pop(&out) && out.source == bytes + 1 && !q.pop(&out), "queue drains");

    TexturePackKey exact = {8, 8, 6, 0, 0x11, 0x22, 0};
    TexturePackKey different = exact; different.dataHash ^= 1;
    TexturePackKey wildcard = exact; wildcard.wildcardTlut = 1; wildcard.tlutHash = 0;
    check(texturePackKeyEqual(exact, exact) && !texturePackKeyEqual(exact, different), "collision safe exact key");
    check(texturePackWildcardMatch(wildcard, exact) && !texturePackWildcardMatch(wildcard, different), "wildcard match");
    TexturePackKey opposite = exact; opposite.mipmapped = 1;
    check(!texturePackKeySameSource(exact, opposite), "mip state is part of source key");
    check(texturePackKeySameSource(exact, different), "hash is not queue metadata");

    TexturePackLru lru;
    lru.reset(100);
    check(lru.add(1, 60, 1) && lru.add(2, 30, 1), "lru add within budget");
    lru.touch(2, 2);
    uint32_t evicted[4] = {};
    check(lru.evict(2, 30, evicted, 4) == 0, "lru protects previous frame");
    check(lru.evict(3, 30, evicted, 4) == 1 && evicted[0] == 1, "lru evicts oldest safe entry");
    check(lru.residentBytes() + 30 <= lru.budget(), "lru chunk budget");

    TexturePackUploadBudget budget(256);
    check(budget.consume(128) && !budget.consume(129) && budget.bytes == 128, "bounded upload chunk");
    TexturePackRequest changed = a; changed.retry = 1;
    check(changed.retry == 1, "changed write retry state");
    std::printf("gx2_texture_pack_runtime_test: %d passed\n", checks);
    return 0;
}
