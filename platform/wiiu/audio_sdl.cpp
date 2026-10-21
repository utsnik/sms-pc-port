// Wii U audio output for audio.cpp. The default path feeds two mono AX
// voices directly; the SDL path remains available for A/B comparisons.
// This is a file of its own because wut's SDL headers pull in coreinit, whose
// declarations clash with the game's dolphin/ headers.
#include <SDL2/SDL.h>
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <sndcore2/core.h>
#include <sndcore2/voice.h>
#include <sndcore2/drcvs.h>
#include <sndcore2/device.h>

#include <cstdint>
#include <cstring>

extern "C" void port_log(const char* fmt, ...);
extern "C" int sms_wiiu_sd_marker(const char* name);
/* The build maps OSCreateThread & co. to the GameCube adapters for every port
 * file (SMS_WIIU_OS_ADAPTER_DEFS). Keep the SDL open/close threads bound to
 * coreinit; AX's thread follows the core that initializes it. */
extern "C" {
BOOL cafe_OSCreateThread(OSThread*, OSThreadEntryPointFn, int32_t, char*, void*, uint32_t, int32_t,
                         OSThreadAttributes) __asm__("OSCreateThread");
int32_t cafe_OSResumeThread(OSThread*) __asm__("OSResumeThread");
/* sndcore2 exports missing from wut's headers. Mixer select 0 = DSP, 1 = PPC,
 * 2 = auto; it applies to voices acquired afterwards. Loads are percent. */
uint32_t AXGetPpcLoad(void);
uint32_t AXGetDspLoad(void);
BOOL cafe_OSJoinThread(OSThread*, int*) __asm__("OSJoinThread");
}

namespace {

enum Output {
	OUTPUT_NONE,
	OUTPUT_AX,
	OUTPUT_SDL
};

SDL_AudioDeviceID s_dev;
Output s_output = OUTPUT_NONE;
bool s_outputLogged;

const uint32_t kAxRate        = 48000;
const uint32_t kRingFrames    = 8192; // 256 ms at the 32 kHz source rate
const uint32_t kVoicePriority = 31;

int16_t s_ring[2][kRingFrames] __attribute__((aligned(0x100)));
AXVoice* s_voice[2];
uint32_t s_writeFrames;
uint32_t s_playFrames;
uint32_t s_lastOffset;
bool s_playing;
/* AX cost experiments (SD markers, 2026-10-21): the SYS AX IST takes ~13% of
 * core 1 and noaudio saves ~3.7 ms of plaza p90. axtvonly: no GamePad mix;
 * axvsoff: GamePad virtual surround off; ax32k: 32 kHz renderer; axpipe4:
 * four-stage pipeline; axidle: voices configured but never started. */
bool s_axTvOnly, s_axIdle;

static void clear_ring()
{
	std::memset(s_ring, 0, sizeof s_ring);
	DCFlushRange(s_ring, sizeof s_ring);
}

static uint32_t current_offset()
{
	AXVoiceOffsets offsets;
	AXGetVoiceOffsets(s_voice[0], &offsets);
	uint32_t offset = offsets.currentOffset;
	return offset < kRingFrames ? offset : offset % kRingFrames;
}

/* Advance the monotonic play count from AX's looping sample offset. The AX
 * voices run in lockstep because they share the same source rate and start
 * offset; voice zero is enough to clock the stereo ring. */
static uint32_t update_played()
{
	if (!s_voice[0] || !s_playing)
		return s_writeFrames - s_playFrames;

	uint32_t offset = current_offset();
	uint32_t delta  = (offset + kRingFrames - s_lastOffset) % kRingFrames;
	s_playFrames += delta;
	s_lastOffset = offset;
	if (s_playFrames < s_writeFrames)
		return s_writeFrames - s_playFrames;

	/* AX loops by design. Once it catches the producer, stop both voices and
	 * clear the whole ring before accepting more data; otherwise the loop
	 * would replay the previous block during an underrun. */
	s_writeFrames = s_playFrames;
	AXSetVoiceState(s_voice[0], AX_VOICE_STATE_STOPPED);
	AXSetVoiceState(s_voice[1], AX_VOICE_STATE_STOPPED);
	s_playing = false;
	clear_ring();
	return 0;
}

static void set_voice_mix(AXVoice* voice, uint32_t bus)
{
	AXVoiceDeviceMixData mix;
	std::memset(&mix, 0, sizeof mix);
	mix.bus[bus].volume = 0x8000;
	AXSetVoiceDeviceMix(voice, AX_DEVICE_TYPE_TV, 0, &mix);
	if (!s_axTvOnly)
		AXSetVoiceDeviceMix(voice, AX_DEVICE_TYPE_DRC, 0, &mix); // GamePad
}

static bool configure_voice(AXVoice* voice, int16_t* data, uint32_t sourceRate, uint32_t bus)
{
	if (!voice)
		return false;
	if (AXVoiceBegin(voice) < 0)
		return false;

	AXSetVoiceType(voice, AX_VOICE_TYPE_UNKNOWN);
	AXVoiceOffsets offsets;
	std::memset(&offsets, 0, sizeof offsets);
	offsets.dataType       = AX_VOICE_FORMAT_LPCM16;
	offsets.loopingEnabled = AX_VOICE_LOOP_ENABLED;
	offsets.loopOffset     = 0;
	offsets.endOffset      = kRingFrames - 1;
	offsets.currentOffset  = 0;
	offsets.data           = data;
	AXSetVoiceOffsets(voice, &offsets);

	/* AX renders at 48 kHz. The 16.16 source ratio describes the 32 kHz
	 * source, so conversion is performed by the DSP rather than SDL. */
	AXVoiceSrc src;
	std::memset(&src, 0, sizeof src);
	src.ratio = (sourceRate << 16) / (sms_wiiu_sd_marker("ax32k") ? 32000u : kAxRate);
	AXSetVoiceSrc(voice, &src);
	AXSetVoiceSrcType(voice, AX_VOICE_SRC_TYPE_LINEAR);
	set_voice_mix(voice, bus);
	AXSetVoiceState(voice, AX_VOICE_STATE_STOPPED);
	AXVoiceEnd(voice);
	return true;
}

static int open_ax(int rate)
{
	AXInitParams params;
	std::memset(&params, 0, sizeof params);
	s_axTvOnly = sms_wiiu_sd_marker("axtvonly") != 0;
	s_axIdle   = sms_wiiu_sd_marker("axidle") != 0;
	const bool ax32k = sms_wiiu_sd_marker("ax32k") != 0, pipe4 = sms_wiiu_sd_marker("axpipe4") != 0;
	params.renderer = ax32k ? AX_INIT_RENDERER_32KHZ : AX_INIT_RENDERER_48KHZ;
	params.pipeline  = pipe4 ? AX_INIT_PIPELINE_FOUR_STAGE : AX_INIT_PIPELINE_SINGLE;
	AXInitWithParams(&params);
	{
		AXDRCVSMode vs = 0xffffffffu;
		AXGetDRCVSMode(&vs);
		// GamePad virtual surround runs on core 1 in the AX IST: off by
		// default (~1.5 ms of plaza p90, 2026-10-21); SD marker axvson keeps it.
		if (!sms_wiiu_sd_marker("axvson"))
			AXSetDRCVSMode(0);
		if (sms_wiiu_sd_marker("axnocomp")) {
			AXSetDeviceCompressor(AX_DEVICE_TYPE_TV, 0);
			AXSetDeviceCompressor(AX_DEVICE_TYPE_DRC, 0);
			port_log("[audio] SD marker axnocomp: TV and DRC compressors off\n");
		}
		port_log("[audio] AX experiment: tvonly=%d idle=%d 32k=%d pipe4=%d drcvs=%u%s\n", (int)s_axTvOnly,
		         (int)s_axIdle, (int)ax32k, (int)pipe4, (unsigned)vs, sms_wiiu_sd_marker("axvson") ? "" : "->0");
	}
	if (!AXIsInit()) {
		port_log("[audio] AX voice init failed\n");
		return 0;
	}

	clear_ring();
	if (sms_wiiu_sd_marker("axdsp")) {
		AXSetDefaultMixerSelect(0);
		port_log("[audio] SD marker axdsp: default mixer select = DSP\n");
	}
	s_voice[0] = AXAcquireVoice(kVoicePriority, nullptr, nullptr);
	s_voice[1] = AXAcquireVoice(kVoicePriority, nullptr, nullptr);
	if (!configure_voice(s_voice[0], s_ring[0], (uint32_t)rate, 0) ||
	    !configure_voice(s_voice[1], s_ring[1], (uint32_t)rate, 1)) {
		if (s_voice[0])
			AXFreeVoice(s_voice[0]);
		if (s_voice[1])
			AXFreeVoice(s_voice[1]);
		s_voice[0] = s_voice[1] = nullptr;
		AXQuit();
		port_log("[audio] AX voice acquire/configure failed\n");
		return 0;
	}

	s_writeFrames = 0;
	s_playFrames  = 0;
	s_lastOffset  = 0;
	s_playing     = false;
	s_output      = OUTPUT_AX;
	port_log("[audio] AX voice renderer=%u/%u max voices=%d\n", (unsigned)s_voice[0]->renderer,
	         (unsigned)s_voice[1]->renderer, (int)AXGetMaxVoices());
	if (!s_outputLogged) {
		port_log("[audio] AX voice output: TV and GamePad, 2 x mono %u Hz s16 voices (DSP -> %u Hz)\n",
		         (unsigned)rate, (unsigned)kAxRate);
		s_outputLogged = true;
	}
	return 1;
}

static int open_sdl_here(int rate)
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
	s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); // SDL converts to the device
	if (!s_dev) {
		port_log("[audio] no SDL audio device: %s\n", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return 0;
	}
	SDL_PauseAudioDevice(s_dev, 0);
	const char* driver = SDL_GetCurrentAudioDriver();
	if (!s_outputLogged) {
		port_log("[audio] SDL output (%s): %d Hz, %u ch, %u-frame device buffer, from %d Hz s16 stereo\n",
		         driver ? driver : "?", have.freq, (unsigned)have.channels, (unsigned)have.samples, rate);
		s_outputLogged = true;
	}
	s_output = OUTPUT_SDL;
	return 1;
}

/* AX's interrupt service thread and SDL's audio thread run on the core that
 * initialized AX. Keep the old core-2 placement only for the SDL A/B path. */
static bool s_openedOnCore2;
struct OpenJob { int rate; int result; };
static int open_thread(int, const char** argv)
{
	OpenJob* job = reinterpret_cast<OpenJob*>(argv);
	job->result = open_sdl_here(job->rate);
	return 0;
}

/* SD marker axcore2: AXInit (and AXQuit) from a core-2 thread, so the
 * {SYS AX IST} it creates may run on core 2 instead of the game's core 1. */
static bool s_axOnCore2;
static int open_ax_thread(int, const char** argv)
{
	OpenJob* job = reinterpret_cast<OpenJob*>(argv);
	job->result = open_ax(job->rate);
	return 0;
}
static int close_ax_thread(int, const char**);

extern "C" int sms_wiiu_sdl_audio_open(int rate)
{
	if (!sms_wiiu_sd_marker("sdlaudio")) {
		if (sms_wiiu_sd_marker("axcore2")) {
			static OSThread thread __attribute__((aligned(16)));
			static uint8_t stack[0x8000] __attribute__((aligned(16)));
			OpenJob job = {rate, 0};
			if (cafe_OSCreateThread(&thread, open_ax_thread, 0, reinterpret_cast<char*>(&job), stack + sizeof stack,
			                        sizeof stack, 16, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
				cafe_OSResumeThread(&thread);
				int ret = 0;
				cafe_OSJoinThread(&thread, &ret);
				s_axOnCore2 = job.result != 0;
				port_log("[audio] SD marker axcore2: AX initialised on core 2 (%s)\n", job.result ? "ok" : "failed");
				return job.result;
			}
			port_log("[audio] axcore2: core-2 thread failed; initialising AX here\n");
		}
		return open_ax(rate);
	}

	port_log("[audio] SD marker sdlaudio: selecting SDL output\n");
	if (sms_wiiu_sd_marker("axcore1")) {
		port_log("[audio] SDL on the game's core (SD marker axcore1)\n");
		return open_sdl_here(rate);
	}
	static OSThread thread __attribute__((aligned(16)));
	static uint8_t stack[0x8000] __attribute__((aligned(16)));
	OpenJob job = {rate, 0};
	if (!cafe_OSCreateThread(&thread, open_thread, 0, reinterpret_cast<char*>(&job), stack + sizeof stack, sizeof stack,
	                         16, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
		port_log("[audio] core-2 SDL open thread failed; opening SDL here\n");
		return open_sdl_here(rate);
	}
	cafe_OSResumeThread(&thread);
	int ret = 0;
	cafe_OSJoinThread(&thread, &ret);
	s_openedOnCore2 = job.result != 0;
	return job.result;
}

static void queue_ax(const void* data, uint32_t bytes)
{
	if (!s_voice[0] || !data)
		return;
	uint32_t frames = bytes / 4;
	if (!frames)
		return;
	uint32_t queued = update_played();
	uint32_t room   = kRingFrames - queued;
	if (frames > room)
		frames = room;
	if (!frames)
		return;

	const int16_t* src = static_cast<const int16_t*>(data);
	uint32_t index = s_writeFrames % kRingFrames;
	uint32_t first = kRingFrames - index;
	if (first > frames)
		first = frames;
	for (uint32_t i = 0; i < first; i++) {
		s_ring[0][index + i] = src[i * 2];
		s_ring[1][index + i] = src[i * 2 + 1];
	}
	if (first < frames) {
		for (uint32_t i = first; i < frames; i++) {
			s_ring[0][i - first] = src[i * 2];
			s_ring[1][i - first] = src[i * 2 + 1];
		}
	}
	DCFlushRange(&s_ring[0][index], first * sizeof(int16_t));
	DCFlushRange(&s_ring[1][index], first * sizeof(int16_t));
	if (first < frames) {
		DCFlushRange(s_ring[0], (frames - first) * sizeof(int16_t));
		DCFlushRange(s_ring[1], (frames - first) * sizeof(int16_t));
	}
	s_writeFrames += frames;
	if (!s_playing && !s_axIdle) {
		/* An underrun stopped AX at the producer position, after clearing the
		 * ring. Both voices therefore restart on the same sample. */
		uint32_t offset = s_writeFrames % kRingFrames;
		AXSetVoiceCurrentOffset(s_voice[0], offset);
		AXSetVoiceCurrentOffset(s_voice[1], offset);
		s_lastOffset = offset;
		AXSetVoiceState(s_voice[0], AX_VOICE_STATE_PLAYING);
		AXSetVoiceState(s_voice[1], AX_VOICE_STATE_PLAYING);
		s_playing = true;
	}
}

extern "C" void sms_wiiu_ax_load(uint32_t* ppc, uint32_t* dsp)
{
	*ppc = *dsp = 0;
	if (s_output != OUTPUT_AX)
		return;
	*ppc = AXGetPpcLoad();
	*dsp = AXGetDspLoad();
}

extern "C" void sms_wiiu_sdl_audio_queue(const void* data, uint32_t bytes)
{
	if (s_output == OUTPUT_AX)
		queue_ax(data, bytes);
	else if (s_output == OUTPUT_SDL && s_dev)
		SDL_QueueAudio(s_dev, data, bytes);
}

static uint32_t queued_ax()
{
	if (!s_voice[0])
		return 0;
	uint32_t frames = update_played();
	return frames * 4;
}

extern "C" uint32_t sms_wiiu_sdl_audio_queued()
{
	if (s_output == OUTPUT_AX)
		return queued_ax();
	if (s_output == OUTPUT_SDL && s_dev)
		return SDL_GetQueuedAudioSize(s_dev);
	return 0;
}

static void close_ax()
{
	if (!s_voice[0] && !s_voice[1])
		return;
	if (s_voice[0])
		AXSetVoiceState(s_voice[0], AX_VOICE_STATE_STOPPED);
	if (s_voice[1])
		AXSetVoiceState(s_voice[1], AX_VOICE_STATE_STOPPED);
	if (s_voice[0])
		AXFreeVoice(s_voice[0]);
	if (s_voice[1])
		AXFreeVoice(s_voice[1]);
	s_voice[0] = s_voice[1] = nullptr;
	s_playing = false;
	AXQuit();
}

static void close_sdl_here()
{
	SDL_PauseAudioDevice(s_dev, 1);
	SDL_ClearQueuedAudio(s_dev);
	SDL_CloseAudioDevice(s_dev);
	s_dev = 0;
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static int close_thread(int, const char**)
{
	close_sdl_here();
	return 0;
}

extern "C" void sms_wiiu_sdl_audio_close(void)
{
	if (s_output == OUTPUT_AX) {
		static OSThread thread __attribute__((aligned(16)));
		static uint8_t stack[0x8000] __attribute__((aligned(16)));
		if (s_axOnCore2 && cafe_OSCreateThread(&thread, close_ax_thread, 0, nullptr, stack + sizeof stack, sizeof stack,
		                                       16, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
			cafe_OSResumeThread(&thread);
			int ret = 0;
			cafe_OSJoinThread(&thread, &ret);
		} else {
			close_ax();
		}
		s_axOnCore2 = false;
		s_output = OUTPUT_NONE;
		return;
	}
	if (s_output != OUTPUT_SDL || !s_dev)
		return;
	/* SDL initializes AX on this thread, so only its path needs the matching
	 * core-2 close thread. */
	static OSThread thread __attribute__((aligned(16)));
	static uint8_t stack[0x8000] __attribute__((aligned(16)));
	if (s_openedOnCore2 && cafe_OSCreateThread(&thread, close_thread, 0, nullptr, stack + sizeof stack, sizeof stack, 16,
	                                           OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
		cafe_OSResumeThread(&thread);
		int ret = 0;
		cafe_OSJoinThread(&thread, &ret);
	} else {
		close_sdl_here();
	}
	s_openedOnCore2 = false;
	s_output = OUTPUT_NONE;
}

static int close_ax_thread(int, const char**)
{
	close_ax();
	return 0;
}

} // namespace
