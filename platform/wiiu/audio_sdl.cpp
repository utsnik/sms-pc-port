// SDL2 audio queue for audio.cpp. A file of its own: wut's SDL headers pull in
// coreinit, whose declarations clash with the game's dolphin/ headers.
#include <SDL2/SDL.h>

#include <cstdint>
#include <cstring>

extern "C" void port_log(const char* fmt, ...);

namespace {
SDL_AudioDeviceID s_dev;
}

extern "C" int sms_wiiu_sdl_audio_open(int rate)
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

extern "C" void sms_wiiu_sdl_audio_queue(const void* data, uint32_t bytes)
{
	if (s_dev)
		SDL_QueueAudio(s_dev, data, bytes);
}

extern "C" uint32_t sms_wiiu_sdl_audio_queued(void) { return s_dev ? SDL_GetQueuedAudioSize(s_dev) : 0; }
