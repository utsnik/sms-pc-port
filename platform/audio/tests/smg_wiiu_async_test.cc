#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

typedef int BOOL;

struct DSPTaskInfo;
extern "C" {
void port_audio_dsp_boot(DSPTaskInfo*);
void port_audio_dsp_mail_to(uint32_t);
void port_audio_dsp_assert_int(void);
void port_audio_dsp_subframe_stats_ex(uint32_t*, uint32_t*, uint32_t*, uint32_t*);
void port_audio_dsp_shutdown(void);
void port_audio_dsp_async_alloc_stats(uint32_t*, uint32_t*, uint32_t*, uint32_t*);
uint32_t port_audio_dsp_check_mail_from(void);
uint32_t port_audio_dsp_read_mail_from(void);
}

namespace {
std::mutex g_mutex;
std::condition_variable g_ready;
unsigned g_boot;
unsigned g_replies;
unsigned g_order_failures;

void send(const uint32_t* words, uint32_t count)
{
    port_audio_dsp_mail_to(count);
    port_audio_dsp_assert_int();
    for (uint32_t i = 0; i < (count ? count : 1); ++i)
        port_audio_dsp_mail_to(words[i]);
}

void wait_for(unsigned boot, unsigned replies)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    g_ready.wait_for(lock, std::chrono::seconds(2), [=] {
        return g_boot >= boot && g_replies >= replies;
    });
}
}

void port_irq_defer(std::function<void()> fn) { if (fn) fn(); }
extern "C" BOOL OSDisableInterrupts(void) { return 0; }
extern "C" BOOL OSRestoreInterrupts(BOOL) { return 0; }
extern "C" void port_log(const char*, ...) {}
extern "C" const uint8_t* port_varam_ptr(uint32_t) { return nullptr; }
extern "C" void* port_host_alloc(size_t size, int) { return std::malloc(size); }
extern "C" void port_host_free(void* pointer) { std::free(pointer); }

extern "C" void __DSPHandler(int, void*)
{
    while (port_audio_dsp_check_mail_from()) {
        const uint32_t first = port_audio_dsp_read_mail_from();
        std::lock_guard<std::mutex> lock(g_mutex);
        if (first == 0xDCD10000u) {
            (void)port_audio_dsp_read_mail_from();
            ++g_boot;
        } else if (first == 0xDCD10004u) {
            const uint32_t second = port_audio_dsp_read_mail_from();
            if (second == 0xF355FF00u) {
                uint32_t releases, renders, replies, idle;
                port_audio_dsp_subframe_stats_ex(&releases, &renders, &replies, &idle);
                if (renders <= replies)
                    ++g_order_failures;
                ++g_replies;
            }
        }
        g_ready.notify_all();
    }
}

int port_audio_dsp_async_test_run(void)
{
    void* memory = mmap(nullptr, 0x4000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (memory == MAP_FAILED)
        return 0;
    int16_t* out = static_cast<int16_t*>(memory);
    port_audio_dsp_boot(nullptr);
    wait_for(1, 0);
    if (g_boot != 1)
        return 0;

    uint32_t frame[5] = {0x82010000u | 0x8000u,
                         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(out)),
                         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(out + 560)), 0, 0};
    const unsigned frame_count = 2000;
    for (unsigned frame_index = 0; frame_index < frame_count; ++frame_index) {
        send(frame, 5);
        for (unsigned group = 0; group < 4; ++group) {
            const uint32_t release = group << 16;
            send(&release, 0);
            if (group == 3)
                wait_for(1, frame_index + 1);
        }
    }

    uint32_t releases, renders, replies, idle;
    port_audio_dsp_subframe_stats_ex(&releases, &renders, &replies, &idle);
    const unsigned replies_before_idle = g_replies;
    const uint32_t idle_release = 3u << 16;
    send(&idle_release, 0);
    for (unsigned i = 0; i < 200; ++i) {
        port_audio_dsp_subframe_stats_ex(&releases, &renders, &replies, &idle);
        if (idle == 1)
            break;
        usleep(10000);
    }
    port_audio_dsp_subframe_stats_ex(&releases, &renders, &replies, &idle);
    const bool ok = releases == frame_count + 1 && renders == frame_count &&
                    replies == frame_count && idle == 1 &&
                    releases - idle == renders && renders == replies &&
                    g_order_failures == 0 && g_replies == replies_before_idle;
    if (!ok)
        fprintf(stderr, "async-reply-test: counts releases=%u renders=%u replies=%u idle=%u boot=%u order=%u\n",
                releases, renders, replies, idle, g_boot, g_order_failures);
    port_audio_dsp_shutdown();
    uint32_t command_allocs, command_frees, reply_allocs, reply_frees;
    port_audio_dsp_async_alloc_stats(&command_allocs, &command_frees,
                                     &reply_allocs, &reply_frees);
    munmap(memory, 0x4000);
    if (command_allocs != command_frees || reply_allocs != reply_frees) {
        fprintf(stderr, "async-reply-test: counts command alloc/free=%u/%u reply alloc/free=%u/%u\n",
                command_allocs, command_frees, reply_allocs, reply_frees);
        return 0;
    }
    return ok ? 1 : 0;
}

int main()
{
    if (!port_audio_dsp_async_test_run()) {
        fprintf(stderr, "async-reply-test: FAIL: async DSP reply contract failed\n");
        return 1;
    }
    puts("async-reply-test: PASS: async releases/render/replies are ordered");
    return 0;
}
