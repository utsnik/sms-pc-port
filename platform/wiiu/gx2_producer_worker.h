#ifndef SMS_WIIU_GX2_PRODUCER_WORKER_H
#define SMS_WIIU_GX2_PRODUCER_WORKER_H

#include <stdint.h>

namespace gx2prod {

enum WaitKind {
    WAIT_PEEK = 0,
    WAIT_DRAWSYNC = 1,
    WAIT_DRAWDONE = 2,
    WAIT_PRESENT_DRAIN = 3,
    WAIT_KIND_COUNT = 4,
    WAIT_FLUSH = 4,
    BOUNDARY_STOP = 5,

    kWaitPeek = WAIT_PEEK,
    kWaitDrawSync = WAIT_DRAWSYNC,
    kWaitDrawDone = WAIT_DRAWDONE,
    kWaitPresent = WAIT_PRESENT_DRAIN,
    kWaitFlush = WAIT_FLUSH
};

/* All Hooks run on the producer worker.  consume() may be given two pieces
 * for one producer append when the byte ring wraps.  The parser therefore
 * retains any incomplete command exactly as the same-thread pipe does. */
struct Hooks {
    void (*consume)(const uint8_t* bytes, uint32_t size);
    void (*boundary)(uint32_t kind);
    void (*publish_token)(uint16_t token);
};

typedef void (*ServiceCallback)(void* displayList, uint32_t bytes);
typedef void (*ServiceFn)(void* arg);

enum ServiceCounterKind {
    SERVICE_COUNTER_CALLBACK = 0,
    SERVICE_COUNTER_COPY_CLEAR = 1,
    SERVICE_COUNTER_RETILE = 2
};

struct Counters {
    uint32_t mode;
    uint32_t workerCore;
    uint32_t producerAppendMicros;
    uint32_t producerAppendCalls;
    uint32_t producerAppendBytes;
    uint32_t producerWakeCalls;
    uint32_t producerFlushHookCalls;
    uint32_t waitMicros[WAIT_KIND_COUNT];
    uint32_t workerBusyMicros;
    uint32_t workerBlockedMicros;
    uint32_t core1ReplayMicros;
    uint32_t core1BlockedMicros;
    uint32_t core1FrontendMicros;
    uint32_t core1PeekMicros;
    uint32_t core1PresentMicros;
    uint32_t core1ListMicros;
    uint32_t core1CopyClearMicros;
    uint32_t core1RetileMicros;
    uint32_t core1CallbackMicros;
    uint32_t queueHighWaterBytes;
    uint32_t segments;
    uint32_t listsCalled;
    uint32_t dlBegins;
    uint32_t serviceFullWaits;
    uint32_t appendFailures;
    uint32_t serviceFailures;
};

/* start() is inert unless both gx2prod and gx2dl SD markers are present.
 * core2 is the caller's cached gx2core2 selection; the module also checks
 * that marker so an integration cannot accidentally ignore it. */
bool start(const Hooks& hooks, bool core2 = false);
bool enabled();

/* Producer/core-1 API.  False is a hard probe failure (normally a full
 * byte ring); callers must not silently fall back to same-thread parsing. */
bool append(const uint8_t* bytes, uint32_t size);
/* Wake the worker without waiting for a boundary; used by GXFlush. */
void wake();
/* The producer buffers GX writes locally; wait() and wake() hand them over first. */
void set_flush_hook(void (*fn)());
/* Every wait kind services through its worker-published barrier before it
 * returns; present drains cannot leave a completed display-list call queued
 * on the worker side. */
void wait(uint32_t kind);
void service();
void stop();

/* Worker-only API used by boundary/parser hooks.  enqueue_list() publishes
 * a completed, cache-flushed display list for ordered replay on core 1.
 * publish_token() invokes the optional token hook on the worker. */
bool enqueue_list(void* displayList, uint32_t bytes);
/* Queue an ordered worker-produced operation for core 1.  arg remains owned
 * by the renderer and must stay valid through the next wait/present drain. */
bool enqueueService(ServiceFn fn, void* arg,
                    ServiceCounterKind kind = SERVICE_COUNTER_CALLBACK);
void publish_token(uint16_t token);
/* Count a GXBeginDisplayList wait for the producer report. */
void record_dl_begin();
/* Include renderer-side service-ordering failures in the prod report. */
void record_service_failure();
void record_flush_hook();
void record_core1_blocked(uint32_t micros);
void record_core1_frontend(uint32_t micros);
void record_core1_peek(uint32_t micros);
void record_core1_present(uint32_t micros);

/* Bind before lists can be emitted.  The callback is invoked only by
 * service(), hence on the integrating core-1 service path. */
void bind_service(ServiceCallback callback);

Counters counters();
void snapshot(Counters* out);
/* Emits one cumulative-delta [prod] line on each nonzero multiple of 30. */
void note_frame(uint32_t frame);

} // namespace gx2prod

#endif
