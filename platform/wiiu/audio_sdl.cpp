// SDL2 audio queue for audio.cpp. A file of its own: wut's SDL headers pull in
// coreinit, whose declarations clash with the game's dolphin/ headers.
#include <SDL2/SDL.h>
#include <coreinit/thread.h>

#include <cstdint>
#include <cstring>

extern "C" void port_log(const char* fmt, ...);
extern "C" int sms_wiiu_sd_marker(const char* name);

namespace {
SDL_AudioDeviceID s_dev;
}

static int open_here(int rate)
{
	SDL_SetMainReady();
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		port_log("[audio] SDL audio init failed: %s\n", SDL_GetError());
		return 0;
	}
	SDL_AudioSpec want, have;
	std::memset(&want, 0, sizeof want);
	std::memset(&have, 0, sizeof have);
	want.freq     = rate;
	want.format   = AUDIO_S16SYS; // big-endian on the Wii U, like the DAC buffers
	want.channels = 2;
	want.samples  = 512;
	want.callback = NULL; // queue mode, as libultraship and the DKR port use on Wii U
	s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); // no changes: SDL converts to the device
	if (!s_dev) {
		port_log("[audio] no SDL audio device: %s\n", SDL_GetError());
		return 0;
	}
	SDL_PauseAudioDevice(s_dev, 0);
	const char* driver = SDL_GetCurrentAudioDriver();
	port_log("[audio] SDL output (%s): %d Hz, %u ch, %u-frame device buffer, from %d Hz s16 stereo\n",
	         driver ? driver : "?", have.freq, (unsigned)have.channels, (unsigned)have.samples, rate);
	return 1;
}

/* AX's interrupt service thread and SDL's audio thread (which resamples the
 * 32 kHz DAC stream to the device rate in AX's frame callback) run on the
 * core that initialised AX. Opened from the game's thread they shared core 1
 * with the game: ~5.6 ms per frame in Delfino Plaza (2026-10-21). So the
 * device is opened from a short-lived thread on core 2. SD marker "axcore1"
 * keeps the old placement for A/B runs. */
static bool s_openedOnCore2;
struct OpenJob { int rate; int result; };
static int open_thread(int, const char** argv)
{
	OpenJob* job = reinterpret_cast<OpenJob*>(argv);
	job->result = open_here(job->rate);
	return 0;
}

extern "C" int sms_wiiu_sdl_audio_open(int rate)
{
	if (sms_wiiu_sd_marker("axcore1")) {
		port_log("[audio] AX/SDL on the game's core (SD marker axcore1)\n");
		return open_here(rate);
	}
	static OSThread thread __attribute__((aligned(16)));
	static uint8_t stack[0x8000] __attribute__((aligned(16)));
	OpenJob job = {rate, 0};
	if (!OSCreateThread(&thread, open_thread, 0, reinterpret_cast<char*>(&job), stack + sizeof stack, sizeof stack,
	                    16, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
		port_log("[audio] core-2 open thread failed; opening AX/SDL here\n");
		return open_here(rate);
	}
	OSResumeThread(&thread);
	int ret = 0;
	OSJoinThread(&thread, &ret);
	s_openedOnCore2 = job.result != 0;
	port_log("[audio] AX/SDL opened from core 2: %s\n", job.result ? "ok" : "failed");
	return job.result;
}

extern "C" void sms_wiiu_sdl_audio_queue(const void* data, uint32_t bytes)
{
	if (s_dev)
		SDL_QueueAudio(s_dev, data, bytes);
}

extern "C" uint32_t sms_wiiu_sdl_audio_queued(void) { return s_dev ? SDL_GetQueuedAudioSize(s_dev) : 0; }

static void close_here(void)
{
	SDL_PauseAudioDevice(s_dev, 1);
	SDL_ClearQueuedAudio(s_dev);
	SDL_CloseAudioDevice(s_dev);
	s_dev = 0;
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static int close_thread(int, const char**)
{
	close_here();
	return 0;
}

extern "C" void sms_wiiu_sdl_audio_close(void)
{
	if (!s_dev)
		return;
	/* AX shuts down on the core that initialised it. */
	static OSThread thread __attribute__((aligned(16)));
	static uint8_t stack[0x8000] __attribute__((aligned(16)));
	if (s_openedOnCore2 && OSCreateThread(&thread, close_thread, 0, nullptr, stack + sizeof stack, sizeof stack, 16,
	                                      OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
		OSResumeThread(&thread);
		int ret = 0;
		OSJoinThread(&thread, &ret);
		return;
	}
	close_here();
}
