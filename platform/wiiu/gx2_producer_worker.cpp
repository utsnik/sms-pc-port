#include "gx2_producer_worker.h"
#include "gx2_producer_ring.h"

#include "runtime.h"
#include "port_heap.h"

#include <coreinit/core.h>
#include <coreinit/time.h>

#include <cstring>

namespace gx2prod {
namespace {

/* Keep wut's types out of the public header and away from the decomp SDK's
 * same-named types.  These sizes and alignments match the existing Wii U
 * worker boundary in gx2_texture_pack.cpp. */
struct WutThread { unsigned char bytes[0x6a0]; } __attribute__((aligned(32)));
struct WutMessageQueue { unsigned char bytes[0x100]; } __attribute__((aligned(32)));
struct WutMessage { void* message; uint32_t args[3]; };
typedef int (*WutEntry)(int, const char**);

extern "C" {
BOOL wut_OSCreateThread(WutThread*, WutEntry, int32_t, char*, void*, uint32_t,
                        int32_t, uint8_t) __asm__("OSCreateThread");
int32_t wut_OSResumeThread(WutThread*) __asm__("OSResumeThread");
BOOL wut_OSJoinThread(WutThread*, int*) __asm__("OSJoinThread");
void wut_OSInitMessageQueue(WutMessageQueue*, WutMessage*, int32_t)
    __asm__("OSInitMessageQueue");
BOOL wut_OSSendMessage(WutMessageQueue*, WutMessage*, int32_t)
    __asm__("OSSendMessage");
BOOL wut_OSReceiveMessage(WutMessageQueue*, WutMessage*, int32_t)
    __asm__("OSReceiveMessage");
void wut_OSSleepTicks(int64_t) __asm__("OSSleepTicks");
void wut_OSYieldThread(void) __asm__("OSYieldThread");
int64_t wut_OSGetSystemTime(void) __asm__("OSGetSystemTime");
int sms_wiiu_pipe2_enabled(void);
}

static const uint32_t kByteRingSize = 2u << 20;
static const uint32_t kServiceRingSize = 4096;
static const uint32_t kWakeSlots = 4;
static const uint32_t kWorkerStackSize = 1u << 20;
static const uint8_t kAffinityCpu0 = 1u << 0;
static const uint8_t kAffinityCpu2 = 1u << 2;
static const int32_t kMessageBlock = 1;

enum ServiceType { SERVICE_LIST, SERVICE_CALLBACK, SERVICE_BARRIER };
struct ServiceRecord {
    uint32_t type;
    uint32_t request;
    void* list;
    uint32_t bytes;
    ServiceFn fn;
    void* arg;
};

static WutThread s_worker;
static WutMessageQueue s_wakeQueue;
static WutMessage s_wakeSlots[kWakeSlots] __attribute__((aligned(32)));
static uint8_t s_workerStack[kWorkerStackSize] __attribute__((aligned(32)));
static uint8_t s_byteRing[kByteRingSize] __attribute__((aligned(64)));
static ServiceRecord s_serviceRing[kServiceRingSize] __attribute__((aligned(64)));

static Hooks s_hooks;
static ServiceCallback s_serviceCallback;
static volatile uint32_t s_byteRead;
static volatile uint32_t s_byteWrite;
static volatile uint32_t s_serviceRead;
static volatile uint32_t s_serviceWrite;
static volatile uint32_t s_requestId;
static volatile uint32_t s_requestTarget;
static volatile uint32_t s_requestKind;
static ServiceFn s_requestFn;
static void* s_requestArg;
static volatile uint32_t s_workerHandled;
static volatile uint32_t s_serviceDone;
static volatile uint32_t s_pendingRequest;
static volatile uint32_t s_stopRequested;
static bool s_started;
static uint32_t s_wokenAt;
static Counters s_totals;
static Counters s_lastLog;
static uint32_t s_lastLogFrame;
static bool s_appendFailureLogged;
static bool s_serviceFailureLogged;
static bool s_wrongServiceCoreLogged;
static bool s_byteReservation;
static uint32_t s_reservedWrite;
static uint32_t s_reservedUsed;
static uint32_t s_reservedCapacity;
static void (*s_flushHook)();
static bool s_inService;

static constexpr uint32_t kDrawownMaxOwners = 1024;
static constexpr uint32_t kDrawownOtherOwner = kDrawownMaxOwners;
static constexpr uint32_t kDrawownMaxStack = 32;
static constexpr uint32_t kDrawownWindowFrames = 120;
struct DrawownWorkerOwner {
    const char* name;
    uint64_t busyTicks;
    uint64_t draws;
};
static bool s_drawownEnabled;
static DrawownWorkerOwner s_drawownOwners[kDrawownMaxOwners + 1];
static uint32_t s_drawownStack[kDrawownMaxStack];
static uint32_t s_drawownStackDepth;
static uint32_t s_drawownCurrent = kDrawownOtherOwner;
static int64_t s_drawownStart;
static bool s_drawownTiming;

static inline int64_t drawownNow() { return wut_OSGetSystemTime(); }

static void drawownAccumulate(int64_t now) {
    if (!s_drawownEnabled || !s_drawownTiming) return;
    const int64_t elapsed = now - s_drawownStart;
    if (elapsed > 0)
        s_drawownOwners[s_drawownCurrent].busyTicks += uint64_t(elapsed);
    s_drawownStart = now;
}

static void drawownPause() {
    if (!s_drawownEnabled || !s_drawownTiming) return;
    const int64_t now = drawownNow();
    drawownAccumulate(now);
    s_drawownTiming = false;
}

static void drawownResume() {
    if (!s_drawownEnabled || s_drawownTiming) return;
    s_drawownStart = drawownNow();
    s_drawownTiming = true;
}

static void drawownSwitch(uint32_t owner) {
    if (!s_drawownEnabled) return;
    if (owner > kDrawownMaxOwners) owner = kDrawownOtherOwner;
    const int64_t now = drawownNow();
    drawownAccumulate(now);
    s_drawownCurrent = owner;
    s_drawownStart = now;
    s_drawownTiming = true;
}

static inline uint32_t drawownMicros(uint64_t ticks) {
    return uint32_t(OSTicksToMicroseconds(ticks));
}

static inline void releaseBarrier() { __sync_synchronize(); }
static inline void acquireBarrier() { __sync_synchronize(); }
static inline bool reached(uint32_t value, uint32_t target) {
    return static_cast<int32_t>(value - target) >= 0;
}
static inline uint32_t microsSince(OSTime start) {
    return static_cast<uint32_t>(OSTicksToMicroseconds(OSGetTime() - start));
}
static inline void add(volatile uint32_t* value, uint32_t amount) {
    __sync_fetch_and_add(value, amount);
}
static inline uint32_t load(const volatile uint32_t* value) {
    return __sync_fetch_and_add(const_cast<volatile uint32_t*>(value), 0);
}

static void updateHighWater(uint32_t used) {
    uint32_t old = s_totals.queueHighWaterBytes;
    while (old < used && !__sync_bool_compare_and_swap(
                              &s_totals.queueHighWaterBytes, old, used))
        old = s_totals.queueHighWaterBytes;
}

static void wakeWorker() {
    WutMessage wake = {reinterpret_cast<void*>(uintptr_t(1)), {0, 0, 0}};
    /* Nonblocking sends coalesce naturally when a wake is already queued. */
    wut_OSSendMessage(&s_wakeQueue, &wake, 0);
}

static bool pushService(const ServiceRecord& record, bool mustSucceed) {
    for (;;) {
        const uint32_t write = s_serviceWrite;
        acquireBarrier();
        const uint32_t read = s_serviceRead;
        if (write - read < kServiceRingSize) {
            s_serviceRing[write % kServiceRingSize] = record;
            releaseBarrier();
            s_serviceWrite = write + 1;
            return true;
        }
        if (!mustSucceed) {
            add(&s_totals.serviceFailures, 1);
            if (!s_serviceFailureLogged) {
                s_serviceFailureLogged = true;
                port_log("SMS Wii U: [prod] core-1 service queue overflow; probe failed\n");
            }
            return false;
        }
        /* A wait on core 1 drains this queue while its barrier is pending. */
        if (s_stopRequested) return false;
        wut_OSSleepTicks(62156250 / 20000); // about 50 us
    }
}

static void consumeThrough(uint32_t target) {
    uint32_t read = s_byteRead;
    while (read != target) {
        const uint32_t offset = read % kByteRingSize;
        uint32_t bytes = target - read;
        const uint32_t contiguous = kByteRingSize - offset;
        if (bytes > contiguous) bytes = contiguous;
        s_hooks.consume(s_byteRing + offset, bytes);
        read += bytes;
        releaseBarrier();
        s_byteRead = read;
    }
}

static void drainWorker() {
    drawownResume();
    for (;;) {
        acquireBarrier();
        const uint32_t target = s_byteWrite;
        const uint32_t request = s_requestId;
        const OSTime busyStart = OSGetTime();
        consumeThrough(target);

        if (request != s_workerHandled && reached(target, s_requestTarget)) {
            const uint32_t kind = s_requestKind;
            const ServiceFn fn = s_requestFn;
            void* const arg = s_requestArg;
            s_hooks.boundary(kind);
            drawownPause();
            if (fn) {
                ServiceRecord operation = {SERVICE_CALLBACK, 0, nullptr, 0, fn, arg};
                if (!pushService(operation, true)) return;
            }
            ServiceRecord barrier = {SERVICE_BARRIER, request, nullptr, 0,
                                     nullptr, nullptr};
            if (!pushService(barrier, true)) return;
            releaseBarrier();
            s_workerHandled = request;
            /* A frame operation is the handoff between two renderer states.
             * Do not parse the next frame until core 1 has replayed the
             * operation and performed its resource rotation. */
            if (kind == WAIT_END_FRAME) {
                while (!reached(s_serviceDone, request) && !s_stopRequested)
                    wut_OSSleepTicks(62156250 / 20000);
            }
        }
        add(&s_totals.workerBusyMicros, microsSince(busyStart));

        acquireBarrier();
        if (s_byteRead == s_byteWrite && request == s_requestId) {
            drawownPause();
            return;
        }
        drawownResume();
    }
}

extern "C" void sms_wiiu_profiler_worker_start(void);
static int workerMain(int, const char**) {
    sms_port_heap_enter();
    sms_wiiu_profiler_worker_start();
    for (;;) {
        WutMessage message = {nullptr, {0, 0, 0}};
        if (!wut_OSReceiveMessage(&s_wakeQueue, &message, kMessageBlock))
            continue;
        if (!message.message || s_stopRequested) break;
        drainWorker();
    }
    return 0;
}

static Counters delta(const Counters& now, const Counters& before) {
    Counters d = now;
    d.producerAppendMicros -= before.producerAppendMicros;
    for (uint32_t i = 0; i != WAIT_KIND_COUNT; ++i)
        d.waitMicros[i] -= before.waitMicros[i];
    d.workerBusyMicros -= before.workerBusyMicros;
    d.core1ReplayMicros -= before.core1ReplayMicros;
    d.segments -= before.segments;
    d.listsCalled -= before.listsCalled;
    d.appendFailures -= before.appendFailures;
    d.serviceFailures -= before.serviceFailures;
    return d;
}

} // namespace

bool start(const Hooks& hooks, bool core2) {
    if (s_started) return true;
    std::memset(&s_totals, 0, sizeof s_totals);
    std::memset(&s_lastLog, 0, sizeof s_lastLog);
    s_lastLogFrame = 0;
    if (!sms_wiiu_sd_marker("gx2prod")) return false;
    if (!hooks.consume || !hooks.boundary) {
        port_log("SMS Wii U: [prod] missing consume/boundary hook; worker refused\n");
        return false;
    }

    core2 = core2 || sms_wiiu_sd_marker("gx2core2");
    s_hooks = hooks;
    s_byteRead = s_byteWrite = 0;
    s_serviceRead = s_serviceWrite = 0;
    s_requestId = s_workerHandled = s_serviceDone = 0;
    s_pendingRequest = 0;
    s_requestTarget = s_requestKind = 0;
    s_requestFn = nullptr;
    s_requestArg = nullptr;
    s_stopRequested = 0;
    s_wokenAt = 0;
    s_appendFailureLogged = s_serviceFailureLogged = false;
    s_wrongServiceCoreLogged = false;
    s_byteReservation = false;
    s_reservedWrite = s_reservedUsed = s_reservedCapacity = 0;
    std::memset(&s_worker, 0, sizeof s_worker);
    std::memset(&s_wakeQueue, 0, sizeof s_wakeQueue);
    wut_OSInitMessageQueue(&s_wakeQueue, s_wakeSlots, kWakeSlots);

    const uint32_t core = core2 ? 2u : 0u;
    const uint8_t affinity = core2 ? kAffinityCpu2 : kAffinityCpu0;
    if (!wut_OSCreateThread(&s_worker, workerMain, 0, nullptr,
                            s_workerStack + sizeof s_workerStack,
                            sizeof s_workerStack, 16, affinity)) {
        port_log("SMS Wii U: [prod] worker creation failed on core %u\n", core);
        return false;
    }
    s_totals.mode = 1;
    s_totals.workerCore = core;
    s_started = true;
    wut_OSResumeThread(&s_worker);
    if (core2)
        port_log("SMS Wii U: [prod] worker core=2 (gx2core2; shares core with default AX and texture-pack work)\n");
    else
        port_log("SMS Wii U: [prod] worker core=0 (default; core 1 owns game/GX2 service, core 2 hosts AX and texture-pack work)\n");
    return true;
}

bool enabled() { return s_started; }

void drawown_enable(bool enabled) {
    s_drawownEnabled = enabled && s_started;
    s_drawownStackDepth = 0;
    s_drawownCurrent = kDrawownOtherOwner;
    s_drawownStart = 0;
    s_drawownTiming = false;
    if (s_drawownEnabled) {
        for (uint32_t i = 0; i <= kDrawownMaxOwners; ++i) {
            s_drawownOwners[i].name = nullptr;
            s_drawownOwners[i].busyTicks = 0;
            s_drawownOwners[i].draws = 0;
        }
        s_drawownOwners[kDrawownOtherOwner].name = "other";
    }
}

void drawown_owner_name(uint32_t owner, const char* name) {
    if (owner < kDrawownMaxOwners && name && *name)
        s_drawownOwners[owner].name = name;
}

void drawown_worker_push(uint32_t owner) {
    if (!s_drawownEnabled) return;
    if (s_drawownStackDepth < kDrawownMaxStack)
        s_drawownStack[s_drawownStackDepth++] = s_drawownCurrent;
    drawownSwitch(owner);
}

void drawown_worker_pop() {
    if (!s_drawownEnabled) return;
    const uint32_t owner = s_drawownStackDepth ? s_drawownStack[--s_drawownStackDepth]
                                              : kDrawownOtherOwner;
    drawownSwitch(owner);
}

void drawown_worker_draw() {
    if (s_drawownEnabled)
        ++s_drawownOwners[s_drawownCurrent].draws;
}

void drawown_report(uint32_t frame) {
    if (!s_drawownEnabled || !frame || frame % kDrawownWindowFrames) return;
    __sync_synchronize();
    uint64_t totalTicks = 0;
    for (uint32_t i = 0; i <= kDrawownMaxOwners; ++i)
        totalTicks += s_drawownOwners[i].busyTicks;

    uint32_t order[kDrawownMaxOwners + 1];
    for (uint32_t i = 0; i <= kDrawownMaxOwners; ++i) order[i] = i;
    const uint32_t limit = 16;
    uint32_t reported = 0;
    for (uint32_t rank = 0; rank < limit; ++rank) {
        uint32_t best = rank;
        for (uint32_t i = rank + 1; i <= kDrawownMaxOwners; ++i) {
            const DrawownWorkerOwner& candidate = s_drawownOwners[order[i]];
            const DrawownWorkerOwner& current = s_drawownOwners[order[best]];
            if (candidate.busyTicks > current.busyTicks)
                best = i;
        }
        const uint32_t tmp = order[rank];
        order[rank] = order[best];
        order[best] = tmp;
        const DrawownWorkerOwner& owner = s_drawownOwners[order[rank]];
        if (!owner.busyTicks && !owner.draws) break;
        ++reported;
    }

    port_log("[wown] frame=%u worker_us=%u\n", frame,
             unsigned(drawownMicros(totalTicks) / kDrawownWindowFrames));
    for (uint32_t rank = 0; rank < reported; ++rank) {
        const DrawownWorkerOwner& owner = s_drawownOwners[order[rank]];
        const char* name = owner.name ? owner.name : "other";
        port_log("[wown]  owner=%s us=%u draws=%u\n", name,
                 unsigned(drawownMicros(owner.busyTicks) / kDrawownWindowFrames),
                 unsigned(owner.draws * 10 / kDrawownWindowFrames));
    }
    for (uint32_t i = 0; i <= kDrawownMaxOwners; ++i) {
        s_drawownOwners[i].busyTicks = 0;
        s_drawownOwners[i].draws = 0;
    }
}

void set_flush_hook(void (*fn)()) { s_flushHook = fn; }

static void serviceImpl(bool allowNested);
void waitPending(uint32_t kind);

bool append(const uint8_t* bytes, uint32_t size) {
    if (!s_started || !bytes || !size) return false;
    const OSTime startTime = OSGetTime();
    if (size > kByteRingSize) {
        add(&s_totals.appendFailures, 1);
        if (!s_appendFailureLogged) {
            s_appendFailureLogged = true;
            port_log("SMS Wii U: [prod] append of %u bytes exceeds %u-byte queue; probe failed\n",
                     size, kByteRingSize);
        }
        add(&s_totals.producerAppendMicros, microsSince(startTime));
        return false;
    }

    uint32_t write = 0, used;
    for (;;) {
        write = s_byteWrite;
        acquireBarrier();
        used = write - s_byteRead;
        if (size <= kByteRingSize - used) break;
        /* The worker can itself be waiting for room in the ordered service
         * ring, so core 1 must pump that ring while applying byte backpressure. */
        wakeWorker();
        serviceImpl(true);
        wut_OSSleepTicks(62156250 / 20000); // about 50 us
    }

    const uint32_t offset = write % kByteRingSize;
    uint32_t first = kByteRingSize - offset;
    if (first > size) first = size;
    std::memcpy(s_byteRing + offset, bytes, first);
    if (first != size) std::memcpy(s_byteRing, bytes + first, size - first);
    releaseBarrier();
    s_byteWrite = write + size;
    updateHighWater(used + size);
    add(&s_totals.producerAppendMicros, microsSince(startTime));

    /* Wake immediately from empty, then every 32 KiB while the producer is
     * ahead.  Synchronous boundaries always issue an unconditional wake. */
    if (!used || s_byteWrite - s_wokenAt >= (32u << 10)) {
        s_wokenAt = s_byteWrite;
        wakeWorker();
    }
    // Keep ordered display-list services moving while the game is recording;
    // this also prevents a busy producer from filling the service ring before
    // its next synchronous boundary.
    if (s_serviceRead != s_serviceWrite)
        serviceImpl(false);
    return true;
}

bool reserve(uint32_t requested, uint8_t** out, uint32_t* capacity) {
    if (!s_started || !requested || !out || !capacity || s_byteReservation)
        return false;

    const uint32_t write = s_byteWrite;
    acquireBarrier();
    const uint32_t read = s_byteRead;
    ByteReservation reservation = {};
    if (!reserveBytes(write, read, kByteRingSize, requested, &reservation)) return false;

    s_byteReservation = true;
    s_reservedWrite = reservation.write;
    s_reservedUsed = reservation.used;
    s_reservedCapacity = reservation.capacity;
    *out = s_byteRing + reservation.offset;
    *capacity = reservation.capacity;
    return true;
}

bool commit(uint32_t size) {
    if (!s_started || !s_byteReservation || size > s_reservedCapacity)
        return false;

    const uint32_t write = s_reservedWrite;
    const uint32_t used = s_reservedUsed;
    ByteReservation reservation = {write, used, write % kByteRingSize, s_reservedCapacity};
    uint32_t nextWrite = 0;
    if (!commitBytes(reservation, size, &nextWrite)) return false;
    s_byteReservation = false;
    s_reservedWrite = s_reservedUsed = s_reservedCapacity = 0;
    if (!size)
        return true;

    /* Match append(): all command bytes become visible before byteWrite is
     * published, and the worker only consumes through that published index. */
    releaseBarrier();
    s_byteWrite = nextWrite;
    updateHighWater(used + size);
    if (!used || s_byteWrite - s_wokenAt >= (32u << 10)) {
        s_wokenAt = s_byteWrite;
        wakeWorker();
    }
    if (s_serviceRead != s_serviceWrite)
        serviceImpl(false);
    return true;
}

void wake() {
    if (s_started) {
        if (s_flushHook) s_flushHook();
        wakeWorker();
    }
}

void wait(uint32_t kind) {
    if (!s_started) return;
    if (s_byteReservation && s_flushHook) s_flushHook();
    /* A pipe2 worker may reach GX2DrawDone/GXFlush through the worker-side
     * parser. It cannot wait for a barrier that only core 1 can service. */
    if (sms_wiiu_pipe2_enabled() && OSGetCoreId() != 1)
        return;
    if (s_pendingRequest)
        waitPending(kind);
    if (s_flushHook) s_flushHook();
    const OSTime startTime = OSGetTime();
    const uint32_t request = s_requestId + 1;
    s_requestTarget = s_byteWrite;
    s_requestKind = kind;
    releaseBarrier();
    s_requestId = request;
    wakeWorker();
    while (!reached(s_serviceDone, request)) {
        serviceImpl(true);
        if (!reached(s_serviceDone, request))
            wut_OSSleepTicks(62156250 / 20000); // about 50 us
    }
    if (kind < WAIT_KIND_COUNT)
        add(&s_totals.waitMicros[kind], microsSince(startTime));
}

bool queueBoundary(uint32_t kind, ServiceFn fn, void* arg) {
    if (!s_started || !fn || s_pendingRequest)
        return false;
    if (s_flushHook) s_flushHook();
    const uint32_t request = s_requestId + 1;
    s_requestTarget = s_byteWrite;
    s_requestKind = kind;
    s_requestFn = fn;
    s_requestArg = arg;
    releaseBarrier();
    s_requestId = request;
    s_pendingRequest = request;
    wakeWorker();
    return true;
}

void waitPending(uint32_t kind) {
    if (!s_started || !s_pendingRequest) return;
    if (s_byteReservation && s_flushHook) s_flushHook();
    const OSTime startTime = OSGetTime();
    const uint32_t request = s_pendingRequest;
    while (!reached(s_serviceDone, request)) {
        serviceImpl(true);
        if (!reached(s_serviceDone, request))
            wut_OSSleepTicks(62156250 / 20000);
    }
    s_pendingRequest = 0;
    if (kind < WAIT_KIND_COUNT)
        add(&s_totals.waitMicros[kind], microsSince(startTime));
}

bool enqueue_list(void* displayList, uint32_t bytes) {
    if (!s_started || !displayList || !bytes) return false;
    ServiceRecord record = {SERVICE_LIST, 0, displayList, bytes,
                            nullptr, nullptr};
    if (!pushService(record, true)) return false;
    add(&s_totals.segments, 1);
    return true;
}

bool enqueueService(ServiceFn fn, void* arg) {
    if (!s_started || !fn) return false;
    ServiceRecord record = {SERVICE_CALLBACK, 0, nullptr, 0, fn, arg};
    if (!pushService(record, true)) return false;
    add(&s_totals.segments, 1);
    return true;
}

void publish_token(uint16_t token) {
    if (s_started && s_hooks.publish_token) s_hooks.publish_token(token);
}

void bind_service(ServiceCallback callback) { s_serviceCallback = callback; }

static void serviceImpl(bool allowNested) {
    if (!s_started) return;
    if (OSGetCoreId() != 1) {
        add(&s_totals.serviceFailures, 1);
        if (!s_wrongServiceCoreLogged) {
            s_wrongServiceCoreLogged = true;
            port_log("SMS Wii U: [prod] service called on core %u, expected core 1\n",
                     static_cast<unsigned>(OSGetCoreId()));
        }
        return;
    }
    if (s_inService && !allowNested) return;
    const bool outer = !s_inService;
    s_inService = true;
    const OSTime startTime = OSGetTime();
    uint32_t serviced = 0;
    for (;;) {
        const uint32_t read = s_serviceRead;
        acquireBarrier();
        if (read == s_serviceWrite) break;
        const ServiceRecord record = s_serviceRing[read % kServiceRingSize];
        if (record.type != SERVICE_BARRIER) {
            releaseBarrier();
            s_serviceRead = read + 1;
        }
        if (record.type == SERVICE_LIST) {
            if (s_serviceCallback) {
                s_serviceCallback(record.list, record.bytes);
                add(&s_totals.listsCalled, 1);
            } else {
                add(&s_totals.serviceFailures, 1);
                if (!s_serviceFailureLogged) {
                    s_serviceFailureLogged = true;
                    port_log("SMS Wii U: [prod] display list reached core 1 without a service callback\n");
                }
            }
        } else if (record.type == SERVICE_CALLBACK) {
            record.fn(record.arg);
        }
        if (record.type == SERVICE_BARRIER) {
            /* Retire the queue slot before publishing completion.  A waiter
             * observing s_serviceDone may return immediately. */
            releaseBarrier();
            s_serviceRead = read + 1;
            releaseBarrier();
            s_serviceDone = record.request;
        }
        ++serviced;
    }
    if (outer) s_inService = false;
    if (serviced) add(&s_totals.core1ReplayMicros, microsSince(startTime));
}

void service() { serviceImpl(false); }

void stop() {
    if (!s_started) return;
    wait(BOUNDARY_STOP);
    s_stopRequested = 1;
    releaseBarrier();
    WutMessage stopMessage = {nullptr, {0, 0, 0}};
    wut_OSSendMessage(&s_wakeQueue, &stopMessage, 0);
    int result = 0;
    wut_OSJoinThread(&s_worker, &result);
    service();
    s_started = false;
}

void snapshot(Counters* out) {
    if (!out) return;
    Counters value = {};
    value.mode = s_totals.mode;
    value.workerCore = s_totals.workerCore;
    value.producerAppendMicros = load(&s_totals.producerAppendMicros);
    for (uint32_t i = 0; i != WAIT_KIND_COUNT; ++i)
        value.waitMicros[i] = load(&s_totals.waitMicros[i]);
    value.workerBusyMicros = load(&s_totals.workerBusyMicros);
    value.core1ReplayMicros = load(&s_totals.core1ReplayMicros);
    value.queueHighWaterBytes = load(&s_totals.queueHighWaterBytes);
    value.segments = load(&s_totals.segments);
    value.listsCalled = load(&s_totals.listsCalled);
    value.appendFailures = load(&s_totals.appendFailures);
    value.serviceFailures = load(&s_totals.serviceFailures);
    *out = value;
}

Counters counters() {
    Counters result;
    snapshot(&result);
    return result;
}

void note_frame(uint32_t frame) {
    if (!frame || frame % 30 || frame == s_lastLogFrame) return;
    if (!s_started) {
        port_log("SMS Wii U: [prod] frame=%u mode=0 core=0 us/30 append=0 waitPeek=0 waitDrawsync=0 waitDrawdone=0 waitPresent=0 waitEnd=0 worker=0 replay=0 queueHigh=0 segments=0 lists=0 appendFail=0 serviceFail=0\n",
                 frame);
        s_lastLogFrame = frame;
        return;
    }
    const Counters now = counters();
    const Counters d = delta(now, s_lastLog);
    port_log("SMS Wii U: [prod] frame=%u mode=%u core=%u us/30 append=%u waitPeek=%u waitDrawsync=%u waitDrawdone=%u waitPresent=%u waitEnd=%u worker=%u replay=%u queueHigh=%u segments=%u lists=%u appendFail=%u serviceFail=%u\n",
             frame, now.mode, now.workerCore, d.producerAppendMicros,
             d.waitMicros[WAIT_PEEK], d.waitMicros[WAIT_DRAWSYNC],
             d.waitMicros[WAIT_DRAWDONE], d.waitMicros[WAIT_PRESENT_DRAIN],
             d.waitMicros[WAIT_END_FRAME], d.workerBusyMicros, d.core1ReplayMicros,
             now.queueHighWaterBytes, d.segments, d.listsCalled,
             d.appendFailures, d.serviceFailures);
    s_lastLog = now;
    s_lastLogFrame = frame;
    drawown_report(frame);
}

} // namespace gx2prod
