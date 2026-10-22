#include "gx2_producer_worker.h"

#include "runtime.h"

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
    uint32_t counterKind;
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
static volatile uint32_t s_workerHandled;
static volatile uint32_t s_serviceDone;
static volatile uint32_t s_stopRequested;
static bool s_started;
static uint32_t s_wokenAt;
static Counters s_totals;
static Counters s_lastLog;
static uint32_t s_lastLogFrame;
static bool s_appendFailureLogged;
static bool s_serviceFailureLogged;
static bool s_wrongServiceCoreLogged;
static uint32_t s_workerDrainBlocked;

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
    add(&s_totals.producerWakeCalls, 1);
    wut_OSSendMessage(&s_wakeQueue, &wake, 0);
}

static bool pushService(const ServiceRecord& record, bool mustSucceed, bool workerWait,
                        uint32_t* workerBlockedMicros = nullptr) {
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
        /* A wait on core 1 drains this queue while its barrier is pending.
         * Sleep rather than OSYieldThread: Cemu crashed in a yield spin here. */
        if (s_stopRequested) return false;
        add(&s_totals.serviceFullWaits, 1);
        const OSTime blockedStart = OSGetTime();
        wut_OSSleepTicks(62156250 / 20000); // about 50 us
        const uint32_t blocked = microsSince(blockedStart);
        if (workerWait && workerBlockedMicros)
            *workerBlockedMicros += blocked;
        else if (!workerWait)
            add(&s_totals.core1BlockedMicros, blocked);
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
    for (;;) {
        acquireBarrier();
        const uint32_t target = s_byteWrite;
        const uint32_t request = s_requestId;
        const OSTime busyStart = OSGetTime();
        s_workerDrainBlocked = 0;
        consumeThrough(target);

        if (request != s_workerHandled && reached(target, s_requestTarget)) {
            const uint32_t kind = s_requestKind;
            s_hooks.boundary(kind);
            ServiceRecord barrier = {SERVICE_BARRIER, 0, request, nullptr, 0,
                                     nullptr, nullptr};
            if (!pushService(barrier, true, true, &s_workerDrainBlocked)) return;
            releaseBarrier();
            s_workerHandled = request;
        }
        const uint32_t busyMicros = microsSince(busyStart);
        add(&s_totals.workerBlockedMicros, s_workerDrainBlocked);
        add(&s_totals.workerBusyMicros,
            busyMicros > s_workerDrainBlocked ? busyMicros - s_workerDrainBlocked : 0);

        acquireBarrier();
        if (s_byteRead == s_byteWrite && request == s_requestId) return;
    }
}

static int workerMain(int, const char**) {
    for (;;) {
        WutMessage message = {nullptr, {0, 0, 0}};
        const OSTime blockedStart = OSGetTime();
        const BOOL received = wut_OSReceiveMessage(&s_wakeQueue, &message, kMessageBlock);
        add(&s_totals.workerBlockedMicros, microsSince(blockedStart));
        if (!received)
            continue;
        if (!message.message || s_stopRequested) break;
        drainWorker();
    }
    return 0;
}

static Counters delta(const Counters& now, const Counters& before) {
    Counters d = now;
    d.producerAppendMicros -= before.producerAppendMicros;
    d.producerAppendCalls -= before.producerAppendCalls;
    d.producerAppendBytes -= before.producerAppendBytes;
    d.producerWakeCalls -= before.producerWakeCalls;
    d.producerFlushHookCalls -= before.producerFlushHookCalls;
    for (uint32_t i = 0; i != WAIT_KIND_COUNT; ++i)
        d.waitMicros[i] -= before.waitMicros[i];
    d.workerBusyMicros -= before.workerBusyMicros;
    d.workerBlockedMicros -= before.workerBlockedMicros;
    d.core1ReplayMicros -= before.core1ReplayMicros;
    d.core1BlockedMicros -= before.core1BlockedMicros;
    d.core1FrontendMicros -= before.core1FrontendMicros;
    d.core1PeekMicros -= before.core1PeekMicros;
    d.core1PresentMicros -= before.core1PresentMicros;
    d.core1ListMicros -= before.core1ListMicros;
    d.core1CopyClearMicros -= before.core1CopyClearMicros;
    d.core1RetileMicros -= before.core1RetileMicros;
    d.core1CallbackMicros -= before.core1CallbackMicros;
    d.segments -= before.segments;
    d.listsCalled -= before.listsCalled;
    d.dlBegins -= before.dlBegins;
    d.serviceFullWaits -= before.serviceFullWaits;
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
    if (!sms_wiiu_sd_marker("gx2dl")) {
        port_log("SMS Wii U: [prod] gx2prod requires gx2dl; worker refused\n");
        return false;
    }
    if (!hooks.consume || !hooks.boundary) {
        port_log("SMS Wii U: [prod] missing consume/boundary hook; worker refused\n");
        return false;
    }

    core2 = core2 || sms_wiiu_sd_marker("gx2core2");
    s_hooks = hooks;
    s_byteRead = s_byteWrite = 0;
    s_serviceRead = s_serviceWrite = 0;
    s_requestId = s_workerHandled = s_serviceDone = 0;
    s_requestTarget = s_requestKind = 0;
    s_stopRequested = 0;
    s_wokenAt = 0;
    s_workerDrainBlocked = 0;
    s_appendFailureLogged = s_serviceFailureLogged = false;
    s_wrongServiceCoreLogged = false;
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

static void (*s_flushHook)();

bool append(const uint8_t* bytes, uint32_t size) {
    if (!s_started || !bytes || !size) return false;
    add(&s_totals.producerAppendCalls, 1);
    add(&s_totals.producerAppendBytes, size);
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
        service();
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
    /* Keep display-list calls flowing while the game is producing.  The
     * compare is intentionally cheap when the service ring is empty. */
    if (s_serviceRead != s_serviceWrite) service();
    return true;
}

void wake() {
    if (s_started && s_flushHook) s_flushHook();
    if (s_started) wakeWorker();
}

static void serviceImpl(bool allowNested);

void set_flush_hook(void (*fn)()) { s_flushHook = fn; }

void wait(uint32_t kind) {
    if (!s_started) return;
    if (s_flushHook) s_flushHook();  // the producer's buffered bytes first
    const OSTime startTime = OSGetTime();
    const uint32_t request = s_requestId + 1;
    s_requestTarget = s_byteWrite;
    s_requestKind = kind;
    releaseBarrier();
    s_requestId = request;
    wakeWorker();
    while (!reached(s_serviceDone, request)) {
        serviceImpl(true);
        if (!reached(s_serviceDone, request)) {
            const OSTime blockedStart = OSGetTime();
            wut_OSSleepTicks(62156250 / 20000); // about 50 us
            add(&s_totals.core1BlockedMicros, microsSince(blockedStart));
        }
    }
    if (kind < WAIT_KIND_COUNT)
        add(&s_totals.waitMicros[kind], microsSince(startTime));
}

bool enqueue_list(void* displayList, uint32_t bytes) {
    if (!s_started || !displayList || !bytes) return false;
    ServiceRecord record = {SERVICE_LIST, 0, 0, displayList, bytes,
                            nullptr, nullptr};
    const bool worker = OSGetCoreId() != 1;
    if (!pushService(record, true, worker, worker ? &s_workerDrainBlocked : nullptr)) return false;
    add(&s_totals.segments, 1);
    return true;
}

bool enqueueService(ServiceFn fn, void* arg, ServiceCounterKind kind) {
    if (!s_started || !fn) return false;
    ServiceRecord record = {SERVICE_CALLBACK, uint32_t(kind), 0, nullptr, 0, fn, arg};
    const bool worker = OSGetCoreId() != 1;
    if (!pushService(record, true, worker, worker ? &s_workerDrainBlocked : nullptr)) return false;
    add(&s_totals.segments, 1);
    return true;
}

void publish_token(uint16_t token) {
    if (s_started && s_hooks.publish_token) s_hooks.publish_token(token);
}

void record_dl_begin() {
    if (s_started) add(&s_totals.dlBegins, 1);
}

void record_service_failure() {
    if (s_started) add(&s_totals.serviceFailures, 1);
}

void record_flush_hook() {
    if (s_started) add(&s_totals.producerFlushHookCalls, 1);
}

void record_core1_blocked(uint32_t micros) {
    if (s_started) add(&s_totals.core1BlockedMicros, micros);
}

void record_core1_frontend(uint32_t micros) {
    if (s_started) add(&s_totals.core1FrontendMicros, micros);
}

void record_core1_peek(uint32_t micros) {
    if (s_started) add(&s_totals.core1PeekMicros, micros);
}

void record_core1_present(uint32_t micros) {
    if (s_started) add(&s_totals.core1PresentMicros, micros);
}

void bind_service(ServiceCallback callback) { s_serviceCallback = callback; }

static bool s_inService;

/* allowNested: wait() may be reached from inside a serviced callback and must
 * still make progress; append's opportunistic pump must not nest. */
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
    /* A record can reach game code (draw-sync callbacks) that issues GX and
     * so appends, which pumps this queue again: never nest, and retire each
     * record before running it so nothing runs twice. */
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
            const OSTime serviceStart = OSGetTime();
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
            add(&s_totals.core1ListMicros, microsSince(serviceStart));
        } else if (record.type == SERVICE_CALLBACK) {
            const OSTime serviceStart = OSGetTime();
            record.fn(record.arg);
            const uint32_t micros = microsSince(serviceStart);
            switch (record.counterKind) {
            case SERVICE_COUNTER_COPY_CLEAR: add(&s_totals.core1CopyClearMicros, micros); break;
            case SERVICE_COUNTER_RETILE: add(&s_totals.core1RetileMicros, micros); break;
            default: add(&s_totals.core1CallbackMicros, micros); break;
            }
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
    value.producerAppendCalls = load(&s_totals.producerAppendCalls);
    value.producerAppendBytes = load(&s_totals.producerAppendBytes);
    value.producerWakeCalls = load(&s_totals.producerWakeCalls);
    value.producerFlushHookCalls = load(&s_totals.producerFlushHookCalls);
    for (uint32_t i = 0; i != WAIT_KIND_COUNT; ++i)
        value.waitMicros[i] = load(&s_totals.waitMicros[i]);
    value.workerBusyMicros = load(&s_totals.workerBusyMicros);
    value.workerBlockedMicros = load(&s_totals.workerBlockedMicros);
    value.core1ReplayMicros = load(&s_totals.core1ReplayMicros);
    value.core1BlockedMicros = load(&s_totals.core1BlockedMicros);
    value.core1FrontendMicros = load(&s_totals.core1FrontendMicros);
    value.core1PeekMicros = load(&s_totals.core1PeekMicros);
    value.core1PresentMicros = load(&s_totals.core1PresentMicros);
    value.core1ListMicros = load(&s_totals.core1ListMicros);
    value.core1CopyClearMicros = load(&s_totals.core1CopyClearMicros);
    value.core1RetileMicros = load(&s_totals.core1RetileMicros);
    value.core1CallbackMicros = load(&s_totals.core1CallbackMicros);
    value.queueHighWaterBytes = load(&s_totals.queueHighWaterBytes);
    value.segments = load(&s_totals.segments);
    value.listsCalled = load(&s_totals.listsCalled);
    value.dlBegins = load(&s_totals.dlBegins);
    value.serviceFullWaits = load(&s_totals.serviceFullWaits);
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
    if (!s_started) return;
    const Counters now = counters();
    const Counters d = delta(now, s_lastLog);
    port_log("SMS Wii U: [prod] frame=%u mode=%u core=%u us/30 append=%u appendCalls=%u appendBytes=%u wakeCalls=%u flushHooks=%u waitPeek=%u waitDrawsync=%u waitDrawdone=%u waitPresent=%u worker=%u workerBlocked=%u replay=%u core1Blocked=%u frontend=%u peek=%u present=%u list=%u copyClear=%u retile=%u callback=%u queueHigh=%u segments=%u lists=%u dlBegins=%u fullWaits=%u appendFail=%u serviceFail=%u\n",
             frame, now.mode, now.workerCore, d.producerAppendMicros,
             d.producerAppendCalls, d.producerAppendBytes, d.producerWakeCalls,
             d.producerFlushHookCalls,
             d.waitMicros[WAIT_PEEK], d.waitMicros[WAIT_DRAWSYNC],
             d.waitMicros[WAIT_DRAWDONE], d.waitMicros[WAIT_PRESENT_DRAIN],
             d.workerBusyMicros, d.workerBlockedMicros, d.core1ReplayMicros,
             d.core1BlockedMicros, d.core1FrontendMicros, d.core1PeekMicros,
             d.core1PresentMicros, d.core1ListMicros, d.core1CopyClearMicros,
             d.core1RetileMicros, d.core1CallbackMicros,
             now.queueHighWaterBytes, d.segments, d.listsCalled, d.dlBegins, d.serviceFullWaits,
             d.appendFailures, d.serviceFailures);
    s_lastLog = now;
    s_lastLogFrame = frame;
}

} // namespace gx2prod
