// Galaxy (Super Mario Galaxy, JAudio2) DSP protocol through the shared HLE.
//
// Drives dsp_hle.cpp + dsp_mixer.cpp (built with PORT_GAME_SMG) with the mail
// words Galaxy's own code sends (games/smg/decomp/src/JSystem/JAudio2:
// dsptask.cpp DSPSendCommands2, dspproc.cpp DsetVARAM/DsetupTable/
// DsyncFrame2ch/DSPReleaseHalt2, JASDSPChannel::updateAll) and plays the
// retail title BGM (AudioRes/Stream/SMG_title_strm.ast, PCM16 stereo) through
// two ordinary PCM16 voices on a three-block ARAM ring that is refilled and
// loop-patched with JASAramStream::updateChannel's own arithmetic.
//
// The disc is read at run time and never copied into the tree:
//   smg_galaxy_protocol_test [DATA/files directory]
// (default: $SMG_DISC_FILES, then the extracted RMGP01 path below). Without
// it the test prints a SKIP line and exits 0.
#include "../dsp_mixer.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

struct DSPTaskInfo;
extern "C" {
void port_audio_dsp_boot(DSPTaskInfo* task);
uint32_t port_audio_dsp_read_mail_from(void);
uint32_t port_audio_dsp_check_mail_from(void);
void port_audio_dsp_mail_to(uint32_t mail);
void port_audio_dsp_assert_int(void);
void port_audio_dsp_subframe_stats(uint32_t* mails, uint32_t* renders, uint32_t* replies);
}

namespace {

const char kDefaultFiles[] =
	"/mnt/fileserver/media/Roms/wii/Super Mario Galaxy (Europe) (RMGP01)-extracted/DATA/files";

// Galaxy's virtual ARAM: HeapMemoryWatcher::createRootHeap passes the MEM2
// arena's low mark to JKRHeap::setAltAramStartAdr, and aralt's ARInit maps
// ARAM offset 0 there (both 0x90xxxxxx on the console). JASDsp::boot sends it
// to the DSP with DsetVARAM. The test's MEM2 window stands at that address.
const uint32_t kVaram = 0x90000000u;
const uint32_t kWindowBytes = 0xE00000u; // createRootHeap reserves 0xE00000
const uint32_t kAramHeapBase = 0x4000u;  // JASKernel::setupAramHeap: ARGetBaseAddress()

// JASDsp::TChannel::setBusConnect's connect_table (index = mix config >> 8).
const uint16_t kConnectTable[12] = { 0x0000, 0x0D00, 0x0D60, 0x0DC0, 0x0E20, 0x0E80,
	                                 0x0EE0, 0x0CA0, 0x0F40, 0x0FA0, 0x0B00, 0x09A0 };

// Plus a few bytes past the window: on a Wii the GDDR3 heap header follows
// it. port_varam_ptr never resolves them; the straddling-frame case below
// fills them so a mixer read past the window becomes audible.
const uint32_t kPastWindowBytes = 16;
std::vector<uint8_t> g_window(kWindowBytes + kPastWindowBytes);
unsigned g_rawAramLookups;
unsigned g_windowMisses;
std::deque<std::function<void()>> g_irqs;
std::vector<std::string> g_log;
std::vector<uint32_t> g_acks;    // 0xF355xxxx command acknowledgements (low 16 bits)
unsigned g_subframeMails;        // 0xF355FF00
unsigned g_bootMails;
int g_failures;

void fail(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "audio-protocol-test: FAIL: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	++g_failures;
}

template<class T> void put(uint8_t* p, int offset, T value) { memcpy(p + offset, &value, sizeof value); }
template<class T> T get(const uint8_t* p, int offset)
{
	T value;
	memcpy(&value, p + offset, sizeof value);
	return value;
}
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

// Main memory the game passes to the DSP as 32-bit words.
uint8_t* low_alloc(size_t size)
{
	void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap(MAP_32BIT)");
		exit(2);
	}
	return (uint8_t*)p;
}
uint32_t addr32(const void* p) { return (uint32_t)(uintptr_t)p; }

void drain_interrupts()
{
	while (!g_irqs.empty()) {
		std::function<void()> fn = g_irqs.front();
		g_irqs.pop_front();
		fn();
	}
}

// DSPSendCommands2 as Galaxy's dsptask.cpp sends it: the word count, the
// interrupt, then the words, where a count of 0 means ONE word (Sunshine's
// dsptask.c sends two).
void send(const uint32_t* words, uint32_t count)
{
	port_audio_dsp_mail_to(count);
	port_audio_dsp_assert_int();
	for (uint32_t i = 0; i < (count ? count : 1); i++)
		port_audio_dsp_mail_to(words[i]);
	drain_interrupts();
}

// ---------------------------------------------------------------------------
// The retail stream (JASAramStream::Header 0x40 bytes, BlockHeader 0x20 bytes).

struct Stream {
	unsigned channels, rate, blockBytes, blockSamples, loopStart, loopEnd;
	std::vector<std::vector<int16_t> > pcm;               // [channel][track sample]
	std::vector<std::vector<std::vector<uint8_t> > > raw; // [block][channel] raw BE bytes
};

bool load_stream(const std::string& path, Stream& s)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::vector<uint8_t> d;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		d.insert(d.end(), buf, buf + n);
	fclose(f);
	if (d.size() < 0x60 || memcmp(&d[0], "STRM", 4) != 0) {
		fail("%s is not an STRM stream", path.c_str());
		exit(1);
	}
	const unsigned format = d[0x09], bits = be16(&d[0x0A]);
	s.channels     = be16(&d[0x0C]);
	s.rate         = be32(&d[0x10]);
	s.loopStart    = be32(&d[0x18]);
	s.loopEnd      = be32(&d[0x1C]);
	s.blockBytes   = be32(&d[0x20]);
	s.blockSamples = s.blockBytes / 2; // JASAramStream::getBlockSamples, format 1
	if (format != 1 || bits != 16 || s.channels != 2 || s.rate != 32000 || s.blockBytes != 10080) {
		fail("title stream header changed: format %u bits %u channels %u rate %u block %u", format, bits,
		     s.channels, s.rate, s.blockBytes);
		exit(1);
	}
	s.pcm.assign(s.channels, std::vector<int16_t>());
	for (size_t off = 0x40; off + 0x20 <= d.size();) {
		if (memcmp(&d[off], "BLCK", 4) != 0) {
			fail("no BLCK at 0x%zx", off);
			exit(1);
		}
		const uint32_t len = be32(&d[off + 4]); // bytes per channel in this block
		std::vector<std::vector<uint8_t> > block(s.channels);
		for (unsigned c = 0; c < s.channels; c++) {
			const uint8_t* p = &d[off + 0x20 + (size_t)c * len];
			block[c].assign(p, p + len);
			for (uint32_t i = 0; i + 1 < len; i += 2)
				s.pcm[c].push_back((int16_t)be16(p + i));
		}
		s.raw.push_back(block);
		off += 0x20 + (size_t)s.channels * len;
	}
	return true;
}

// ---------------------------------------------------------------------------
// TChannel (0x180 bytes) as the Galaxy CPU fills it.

enum {
	kActive = 0x000, kFinished = 0x002, kPitch = 0x004, kReset = 0x008, kPause = 0x00C, kMix = 0x010,
	kAutoMix = 0x058, kPosFrac = 0x060, kBlockSamples = 0x064, kPosition = 0x068, kAramAddr = 0x070,
	kRemaining = 0x074, kSource = 0x100, kLoopFlag = 0x102, kLoopYN1 = 0x104, kLoopYN2 = 0x106,
	kForcedStop = 0x10A, kLoopStart = 0x110, kEnd = 0x114, kBase = 0x118, kSampleCount = 0x11C,
};

uint8_t* g_chbuf;
uint8_t* g_fxbuf;
int16_t* g_dac; // one sDspDacBuffer: outA = g_dac, outB = g_dac + 560
uint8_t* voice(int i) { return g_chbuf + 0x180 * i; }

// JASAramStream::channelStart (format 1 -> wave format 3 = PCM16) through
// TChannel::init, setWaveInfo, setBusConnect, setMixerInitVolume, setPitch
// and playStart.
void start_stream_voice(int vi, uint32_t wavePtr, uint32_t bufSamples, unsigned bus, uint16_t pitch)
{
	uint8_t* v = voice(vi);
	memset(v, 0, 0x180);
	put<uint16_t>(v, kBlockSamples, 1);      // COMP_BLOCKSAMPLES[3]
	put<uint16_t>(v, kSource, 0x10);         // COMP_BLOCKBYTES[3]
	put<uint32_t>(v, kSampleCount, bufSamples);
	put<uint16_t>(v, kLoopFlag, 0xFFFF);     // mLoopFlags = -1
	put<uint32_t>(v, kLoopStart, 0);
	put<uint32_t>(v, kEnd, bufSamples);      // mSampleLoopEnd = mBufCount * getBlockSamples()
	put<int16_t>(v, kLoopYN1, 0);
	put<int16_t>(v, kLoopYN2, 0);
	put<uint32_t>(v, kBase, wavePtr);
	put<int32_t>(v, kPosition, 0);           // no skip samples
	put<uint16_t>(v, kMix + 0, kConnectTable[bus]);
	put<uint16_t>(v, kMix + 2, 0x4000);      // Q14 unity, already at target
	put<uint16_t>(v, kMix + 4, 0x4000);
	put<uint16_t>(v, kPitch, pitch);
	put<uint16_t>(v, kReset, 1);
	put<uint16_t>(v, kActive, 1);
}

#if defined(PORT_GAME_SMG)
void start_looping_adpcm4_voice(int vi, uint32_t wavePtr)
{
	uint8_t* v = voice(vi);
	memset(v, 0, 0x180);
	put<uint16_t>(v, kSource, 9);       // AFC ADPCM4: 9-byte, 16-sample frames
	put<uint32_t>(v, kSampleCount, 32);
	put<uint16_t>(v, kLoopFlag, 0xFFFF);
	put<uint32_t>(v, kLoopStart, 0);
	put<uint32_t>(v, kEnd, 32);
	put<uint32_t>(v, kBase, wavePtr);
	put<uint16_t>(v, kMix + 0, kConnectTable[1]);
	put<uint16_t>(v, kMix + 2, 0x4000);
	put<uint16_t>(v, kMix + 4, 0x4000);
	put<uint16_t>(v, kPitch, 0x1000);
	put<uint16_t>(v, kReset, 1);
	put<uint16_t>(v, kActive, 1);
}
#endif

uint16_t create_map(unsigned group)
{
	uint16_t map = 0;
	for (int i = 0; i < 16; i++)
		map = (uint16_t)(map << 1 | (get<uint16_t>(voice(group * 16 + i), kActive) != 0));
	return map;
}

// JASDSPChannel::updateAll: voices are released sixteen at a time, group 3
// last; each release is DSPReleaseHalt2(group).
// The microcode renders the subframe once group 3 is released and reports
// it with one 0xF355FF00 (JASAudioThread counts one per subframe).
unsigned g_misframedReleases;
void release_subframe()
{
	for (unsigned group = 0; group < 4; group++) {
		const unsigned before = g_subframeMails;
		uint32_t mailsBefore, rendersBefore, repliesBefore;
		port_audio_dsp_subframe_stats(&mailsBefore, &rendersBefore, &repliesBefore);
		uint32_t word = group << 16 | create_map(group);
		send(&word, 0);
		if (g_subframeMails - before != (group == 3 ? 1u : 0u))
			++g_misframedReleases;
		uint32_t mailsAfter, rendersAfter, repliesAfter;
		port_audio_dsp_subframe_stats(&mailsAfter, &rendersAfter, &repliesAfter);
		if (group != 3 && (mailsAfter != mailsBefore || rendersAfter != rendersBefore ||
		                   repliesAfter != repliesBefore))
			fail("group %u posted a subframe reply before the render release", group);
		if (group == 3 && (mailsAfter != mailsBefore + 1 ||
		                   rendersAfter != rendersBefore + 1 ||
		                   repliesAfter != repliesBefore + 1))
			fail("subframe mail/render/reply counts diverged at async reply boundary");
	}
}

void sync_frame(uint16_t level)
{
	// Galaxy's DSP_MIXERLEVEL defaults to 0x4000 (dspproc.cpp:35).
	// DsyncFrame2ch: five words, the last two zero.
	uint32_t w[5] = { 0x82000000u | 7u << 16 | level, addr32(g_dac), addr32(g_dac + 560), 0, 0 };
	send(w, 5);
}

bool logged(const char* text)
{
	for (size_t i = 0; i < g_log.size(); i++)
		if (g_log[i].find(text) != std::string::npos)
			return true;
	return false;
}

// Source index played at output sample k for a Q12 ratio: the identity
// resampler outputs window[1], which advances by ratio << 4 in 16.16.
uint64_t source_at(uint64_t k, uint16_t pitch) { return k * ((uint64_t)pitch << 4) >> 16; }

// port_dspmix_render applies the Galaxy master level after bus mixing.
int master_scaled(int sample, uint16_t master)
{
	const int32_t scaled = (int32_t)(((int64_t)sample * master) >> 15);
	return scaled < -32768 ? -32768 : scaled > 32767 ? 32767 : scaled;
}

} // namespace

// ---------------------------------------------------------------------------
// Port services dsp_hle.cpp links against.

void port_irq_defer(std::function<void()> fn) { g_irqs.push_back(fn); }
void port_irq_run_now(std::function<void()> fn) { fn(); }

extern "C" void port_log(const char* fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	g_log.push_back(line);
	if (getenv("SMG_AUDIO_TEST_VERBOSE"))
		fputs(line, stderr);
}

// Sunshine-style ARAM offsets: Galaxy has no ARAM of its own, so a raw
// offset reaching here is unresolved.
extern "C" uint8_t* port_aram_ptr(uint32_t addr)
{
	(void)addr;
	++g_rawAramLookups;
	return NULL;
}

// The Wii's MEM2 window, at the address DsetVARAM announced.
extern "C" const uint8_t* port_varam_ptr(uint32_t addr)
{
	if (addr < kVaram || addr - kVaram >= kWindowBytes) {
		++g_windowMisses;
		return NULL;
	}
	return &g_window[addr - kVaram];
}

// osdsp_task.cpp's __DSPHandler with the audio task's callbacks: init_cb
// (DspHandShake) reads one more mail; req_cb (JASAudioThread::DSPCallback)
// reads 0xF355xxxx: FF00 = subframe rendered, else DspFinishWork(low 16).
extern "C" void __DSPHandler(int interrupt, void* context)
{
	(void)interrupt;
	(void)context;
	if (!port_audio_dsp_check_mail_from()) {
		fail("DSP interrupt without a mail");
		return;
	}
	const uint32_t mail = port_audio_dsp_read_mail_from();
	if (mail == 0xDCD10000u) {
		port_audio_dsp_read_mail_from();
		++g_bootMails;
	} else if (mail == 0xDCD10004u) {
		const uint32_t second = port_audio_dsp_read_mail_from();
		if (second >> 16 != 0xF355)
			fail("request mail 0x%08x is not 0xF355xxxx", second);
		else if ((second & 0xFF00) == 0xFF00)
			++g_subframeMails;
		else
			g_acks.push_back(second & 0xFFFF);
	} else {
		fail("unexpected task mail 0x%08x", mail);
	}
}

int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0); // keep PASS/FAIL lines in order
	std::string files = argc > 1 ? argv[1] : getenv("SMG_DISC_FILES") ? getenv("SMG_DISC_FILES") : kDefaultFiles;
	const std::string path = files + "/AudioRes/Stream/SMG_title_strm.ast";
	Stream st;
	if (!load_stream(path, st)) {
		printf("audio-protocol-test: SKIP: %s not readable (pass the extracted DATA/files directory)\n",
		       path.c_str());
		return 0;
	}
	const unsigned bs = st.blockSamples, bb = st.blockBytes, nslots = 3, ring = nslots * bs;
	const unsigned loopStartBlock = st.loopStart / bs, loopEndBlock = (st.loopEnd - 1) / bs;
	printf("audio-protocol-test: %s: PCM16 x%u %u Hz, %u-byte blocks, loop [%u, %u), %zu blocks\n",
	       path.c_str(), st.channels, st.rate, bb, st.loopStart, st.loopEnd, st.raw.size());

	// The audio task's buffers, at 32-bit main-memory addresses.
	uint8_t* low = low_alloc(0x10000);
	g_chbuf = low;                              // CH_BUF: TChannel[64]
	g_fxbuf = low + 0x6000;                     // FX_BUF: FxBuf[4] (lines off)
	uint32_t* resFilter = (uint32_t*)(low + 0x6100);   // DSPRES_FILTER: 320 words
	uint32_t* adpcmFilter = (uint32_t*)(low + 0x6600); // DSPADPCM_FILTER: 64 bytes
	g_dac = (int16_t*)(low + 0x6700);           // sDspDacBuffer[n]: 2 x 560 s16
	// Identity resampling taps (window[1] x 32767), so output is checkable
	// sample for sample; the rest of the table (patterns) stays zero.
	for (int p = 0; p < 64; p++) {
		resFilter[p * 2]     = 32767;
		resFilter[p * 2 + 1] = 0;
	}

	// JASAudioThread::run: DspBoot, then DsetVARAM, then initBuffer's DsetupTable.
	port_audio_dsp_boot(NULL);
	drain_interrupts();
	if (g_bootMails != 1)
		fail("boot: %u task-start mails (want 1)", g_bootMails);
	const uint32_t varam[2] = { 0x8E000000u, kVaram };
	send(varam, 2);
	const uint32_t setup[5] = { 0x81000000u | 0x40, addr32(g_chbuf), addr32(resFilter), addr32(adpcmFilter),
		                        addr32(g_fxbuf) };
	send(setup, 5);
	if (g_acks.size() != 2 || g_acks[0] != 0x8E00 || g_acks[1] != 0x8100)
		fail("acknowledgements %zu: want 0xF3558E00 (DsetVARAM) then 0xF3558100 (DsetupTable)", g_acks.size());
	if (logged("unknown command 0x8e"))
		fail("0x8E DsetVARAM fell into the unknown-command branch");

	// The ARAM rings, as JASAramStream lays them out: channel i at
	// _148 + sBlockSize * _160 * i, block slots of sBlockSize bytes.
	uint32_t ringBase[2];
	for (unsigned c = 0; c < 2; c++)
		ringBase[c] = kAramHeapBase + c * nslots * bb;
	unsigned nextBlock = 0; // JASAramStream::load's mBlock
	unsigned refills = 0;
	auto load_slot = [&](unsigned slot) {
		const unsigned b = nextBlock;
		for (unsigned c = 0; c < 2; c++) {
			const std::vector<uint8_t>& src = st.raw[b][c];
			memcpy(&g_window[ringBase[c] + slot * bb], &src[0], src.size()); // JKRAram::mainRamToAram
		}
		++refills;
		nextBlock = b + 1;
		if (nextBlock > loopEndBlock)
			nextBlock = loopStartBlock;
	};
	for (unsigned s = 0; s < nslots; s++)
		load_slot(s);

	// 32000 Hz at the 32028.5 Hz DAC: setInitPitch(_164 / getDacRate()).
	const uint16_t pitch = (uint16_t)(4096.0 * 32000.0 / 32028.5);
	start_stream_voice(0, ringBase[0], ring, 1, pitch); // bus 1 -> outA
	start_stream_voice(1, ringBase[1], ring, 2, pitch); // bus 2 -> outB

	// JASAramStream::updateChannel state (CB_START values).
	uint32_t B0 = 0, B4 = ring, B8 = 0, BC = loopEndBlock, loops = 0;
	bool C0 = false;

	// The expected sequence: the track, spliced at its loop.
	auto track_at = [&](unsigned c, uint64_t i) -> int {
		if (i < st.loopEnd)
			return st.pcm[c][i];
		return st.pcm[c][st.loopStart + (i - st.loopEnd) % (st.loopEnd - st.loopStart)];
	};

	const uint64_t totalOut = (uint64_t)st.loopEnd + ring + 8000; // past the loop and one more ring
	uint64_t out = 0, nonzero = 0, mismatches = 0, frames = 0;
	double energy = 0;
	unsigned firstFrameNonzero = 0;
	bool b8Checked = false;
	unsigned cursorOutside = 0, drifted = 0;
	while (out < totalOut) {
		const unsigned before = g_subframeMails;
		sync_frame(0x4000);
		for (int sub = 0; sub < 7; sub++) {
			release_subframe();
			uint8_t* v0 = voice(0);
			uint8_t* v1 = voice(1);
			if (get<uint16_t>(v0, kReset) != 0)
				fail("the DSP did not clear the reset flag");
			// CB_PLAY for the update channel (voice 0), as JASAramStream.cpp:409-529.
			const uint32_t sp28 = get<uint32_t>(v0, kRemaining) + get<uint16_t>(v0, kBlockSamples);
			if (sp28 <= B4) {
				B8 += B4 - sp28;
			} else if (!C0) {
				B8 += B4;
				B8 += ring - sp28;
			} else {
				B8 += B4;
				B8 += ring - sp28 - get<uint32_t>(v0, kLoopStart);
				B8 -= st.loopEnd;
				B8 += st.loopStart;
				put<uint32_t>(v0, kLoopStart, 0);
				put<uint32_t>(v1, kLoopStart, 0);
				++loops;
				C0 = false;
			}
			if (B8 > st.loopEnd)
				fail("stream position %u passed the loop end (JASAramStream's fatal error)", B8);
			if (B8 + 400 >= st.loopEnd && !C0) {
				uint32_t slot = BC + 1;
				if (slot >= nslots)
					slot = 0;
				const uint32_t ls = st.loopStart % bs + slot * bs;
				const uint32_t rem = get<uint32_t>(v0, kRemaining) - (ring - BC * bs - st.loopEnd % bs);
				put<uint32_t>(v0, kLoopStart, ls);
				put<uint32_t>(v1, kLoopStart, ls);
				put<uint32_t>(v0, kRemaining, rem);
				put<uint32_t>(v1, kRemaining, rem);
				BC += loopEndBlock - loopStartBlock + 1;
				C0 = true;
			}
			uint32_t pos = get<uint32_t>(v0, kAramAddr) - ringBase[0];
			if (pos != 0)
				pos--;
			const uint32_t slotNow = pos / bb;
			if (slotNow >= nslots && !cursorOutside++)
				fail("ARAM cursor 0x%08x outside the ring", get<uint32_t>(v0, kAramAddr));
			if (slotNow != B0 && slotNow < nslots) {
				const bool wrapped = slotNow < B0;
				while (slotNow != B0) {
					load_slot(B0); // loadToAramTask
					B0 = (B0 + 1) % nslots;
				}
				if (wrapped)
					BC -= nslots;
			}
			B4 = get<uint32_t>(v0, kRemaining) + get<uint16_t>(v0, kBlockSamples);
			if (get<uint32_t>(v1, kAramAddr) - ringBase[1] != get<uint32_t>(v0, kAramAddr) - ringBase[0] &&
			    !drifted++)
				fail("the two stream channels drifted apart");
		}
		if (g_subframeMails - before != 7) {
			fail("frame %llu: %u subframe mails for 7 subframes", (unsigned long long)frames,
			     g_subframeMails - before);
			break;
		}
		// readDspBuffer: outB-half then outA-half per stereo pair; check both.
		for (int k = 0; k < 560 && out < totalOut; k++, out++) {
			const uint64_t si = source_at(out, pitch);
			for (unsigned c = 0; c < 2; c++) {
				const int got = g_dac[c * 560 + k], want = master_scaled(track_at(c, si), 0x4000);
				if (got < want - 2 || got > want + 2) {
					if (mismatches < 5)
						fail("out[%u][%llu] = %d, source[%llu] = %d", c, (unsigned long long)out, got,
						     (unsigned long long)si, want);
					++mismatches;
				}
				nonzero += got != 0;
				energy += (double)got * got;
				if (frames == 0)
					firstFrameNonzero += got != 0;
			}
		}
		++frames;
		if (frames == 1) {
			// Fetch cursor = source samples consumed + the 3-sample window.
			const uint32_t fetched = (uint32_t)source_at(560, pitch) + 3;
			const uint32_t addr = get<uint32_t>(voice(0), kAramAddr);
			if (addr != ringBase[0] + fetched * 2)
				fail("ARAM cursor (0x070) 0x%x after one frame, want base + 2 x %u", addr, fetched);
			if (get<uint32_t>(voice(0), kRemaining) != ring - fetched)
				fail("remaining length (0x074) %u after one frame, want %u", get<uint32_t>(voice(0), kRemaining),
				     ring - fetched);
			if (firstFrameNonzero < 1000)
				fail("first BLCK block: only %u of 1120 output samples are non-zero", firstFrameNonzero);
			else
				printf("audio-protocol-test: first BLCK block plays through VARAM 0x%08x + 0x%x: %u/1120 "
				       "samples non-zero, tracking the source\n",
				       kVaram, ringBase[0], firstFrameNonzero);
		}
		if (!b8Checked && out >= 100000) {
			// The stream manager's own position equals the DSP's fetch count
			// (+ _64 = 1: it seeds _0B4 with the ring length).
			b8Checked = true;
			const uint32_t fetched = (uint32_t)source_at(out, pitch) + 3;
			if (B8 + 1 != fetched)
				fail("JASAramStream position %u, DSP fetched %u", B8, fetched);
		}
	}
	if (loops != 1)
		fail("the stream looped %u times (want 1)", loops);
	if (mismatches)
		fail("%llu output samples differ from the spliced track", (unsigned long long)mismatches);
	const double rms = out ? sqrt(energy / (2.0 * out)) : 0;
	printf("audio-protocol-test: streamed %llu samples x2 (%llu frames) through a %u-block ring: %u ring "
	       "refills, loop %u -> %u spliced, rms %.0f, mismatches %llu\n",
	       (unsigned long long)out, (unsigned long long)frames, nslots, refills - nslots, st.loopEnd,
	       st.loopStart, rms,
	       (unsigned long long)mismatches);

	// endPosition patching (JASAramStream.cpp:497-517: _114 and _74 move by a
	// block when mBufCount changes): a 4-slot ring played as 3 blocks, then
	// grown to 4 while the first block plays.
	put<uint16_t>(voice(0), kActive, 0);
	put<uint16_t>(voice(1), kActive, 0);
	const uint32_t ring4 = kAramHeapBase + 2 * nslots * bb;
	for (unsigned s = 0; s < 4; s++)
		memcpy(&g_window[ring4 + s * bb], &st.raw[s][0][0], bb);
	start_stream_voice(2, ring4, 3 * bs, 1, 0x1000);
	uint64_t out2 = 0, bad2 = 0;
	bool grown = false;
	uint32_t remainingAfterWrap = 0;
	while (out2 < 4 * bs + 2000) {
		sync_frame(0x4000);
		for (int sub = 0; sub < 7; sub++) {
			release_subframe();
			uint8_t* v = voice(2);
			if (!grown && out2 + (sub + 1) * 80 >= 1000) {
				put<uint32_t>(v, kEnd, get<uint32_t>(v, kEnd) + bs);
				put<uint32_t>(v, kRemaining, get<uint32_t>(v, kRemaining) + bs);
				grown = true;
			}
			if (!remainingAfterWrap && get<uint32_t>(v, kAramAddr) < ring4 + bb && out2 > bs)
				remainingAfterWrap = get<uint32_t>(v, kRemaining) +
				                     (get<uint32_t>(v, kAramAddr) - ring4) / 2; // reload length
		}
		for (int k = 0; k < 560; k++, out2++) {
			const int got = g_dac[k];
			const int want = master_scaled(st.pcm[0][out2 % (4 * bs)], 0x4000);
			if (got < want - 2 || got > want + 2)
				++bad2;
		}
	}
	if (bad2)
		fail("grown ring: %llu samples differ from blocks 0-3", (unsigned long long)bad2);
	if (remainingAfterWrap != 4 * bs)
		fail("grown ring: reload length %u after the wrap, want %u", remainingAfterWrap, 4 * bs);
	else
		printf("audio-protocol-test: endPosition patch: ring grew to 4 blocks mid-play and reloaded %u samples\n",
		       remainingAfterWrap);

	// TChannel::setWaveInfo's skip: a PCM voice starts at 0x068.
	put<uint16_t>(voice(2), kActive, 0);
	start_stream_voice(3, ring4, 4 * bs, 1, 0x1000);
	put<uint16_t>(voice(3), kLoopFlag, 0);
	put<int32_t>(voice(3), kPosition, 1000);
	sync_frame(0x4000);
	for (int sub = 0; sub < 7; sub++)
		release_subframe();
	unsigned badSkip = 0;
	for (int k = 0; k < 560; k++) {
		const int want = master_scaled(st.pcm[0][1000 + k], 0x4000);
		badSkip += g_dac[k] < want - 2 || g_dac[k] > want + 2;
	}
	if (badSkip)
		fail("skip start: %u of 560 samples do not start at source[1000]", badSkip);
	else
		printf("audio-protocol-test: PCM start offset 0x068 = 1000 honoured\n");
	put<uint16_t>(voice(3), kActive, 0);

#if defined(PORT_GAME_SMG)
	// ADPCM4 is the common Galaxy SFX/sequenced-music format. Exercise a
	// genuine looping two-frame voice so D4's cursor/remaining writeback is not
	// covered only by the PCM16 title stream.
	const uint32_t adpcmBase = kAramHeapBase + 0x300000;
	for (unsigned frame = 0; frame < 2; frame++) {
		g_window[adpcmBase + frame * 9] = 0x80; // scale 8, predictor 0
		for (unsigned i = 0; i < 8; i++)
			g_window[adpcmBase + frame * 9 + 1 + i] = 0x88;
	}
	start_looping_adpcm4_voice(4, adpcmBase);
	sync_frame(0x4000);
	for (int sub = 0; sub < 7; sub++)
		release_subframe();
	unsigned adpcmNonzero = 0;
	for (int k = 0; k < 560; k++)
		adpcmNonzero += g_dac[k] != 0;
	const uint32_t adpcmCursor = get<uint32_t>(voice(4), kAramAddr);
	if (adpcmNonzero < 400)
		fail("looping ADPCM4 produced only %u non-zero samples", adpcmNonzero);
	if (adpcmCursor < adpcmBase || adpcmCursor > adpcmBase + 18)
		fail("looping ADPCM4 cursor 0x%08x escaped its two-frame wave", adpcmCursor);
	else
		printf("audio-protocol-test: looping ADPCM4: %u/560 samples non-zero, cursor 0x%08x\n",
		       adpcmNonzero, adpcmCursor);
	put<uint16_t>(voice(4), kActive, 0);

	// Ruling R18: an ADPCM4 frame that starts 5 bytes below the window end
	// would read 4 bytes of what follows the window (the GDDR3 heap header).
	// The mixer must treat it as unresolved: silence, and a window miss.
	const uint32_t straddleBase = kWindowBytes - 5;
	g_window[straddleBase] = 0x80;
	for (uint32_t i = straddleBase + 1; i < kWindowBytes + kPastWindowBytes; i++)
		g_window[i] = 0x88;
	const unsigned missesBefore = g_windowMisses;
	start_looping_adpcm4_voice(4, straddleBase);
	sync_frame(0x4000);
	release_subframe();
	unsigned straddleNonzero = 0;
	for (int k = 0; k < 80; k++)
		straddleNonzero += g_dac[k] != 0;
	if (straddleNonzero || g_windowMisses == missesBefore)
		fail("ADPCM4 frame straddling the VARAM window end read past it (%u non-zero samples, %u window misses)",
		     straddleNonzero, g_windowMisses - missesBefore);
	else
		printf("audio-protocol-test: ADPCM4 frame straddling the window end is unresolved (silent)\n");
	put<uint16_t>(voice(4), kActive, 0);
#endif

	// The decoder test hook, through the same VARAM resolver.
	int16_t decoded[64];
	port_dspmix_decode_wave(3, ring4, 64, decoded);
	if (memcmp(decoded, &st.pcm[0][0], sizeof decoded) != 0)
		fail("port_dspmix_decode_wave(PCM16) does not return the ring's samples");

	// An FX return through Galaxy's SEND_TABLE[2] (0x0DC8) reaches bus 3,
	// which FX line 0 writes back to its delay buffer.
	int16_t* delay0 = (int16_t*)(low + 0x7000);
	int16_t* delay1 = (int16_t*)(low + 0x7100);
	for (int k = 0; k < 80; k++) {
		delay0[k] = 0;
		delay1[k] = 1000;
	}
	uint8_t* line0 = g_fxbuf;
	uint8_t* line1 = g_fxbuf + 0x20;
	put<uint16_t>(line0, 0x00, 2); // filter the feedback (zero taps), no return
	put<uint16_t>(line0, 0x02, 1);
	put<uint32_t>(line0, 0x04, addr32(delay0));
	put<uint16_t>(line1, 0x00, 2);
	put<uint16_t>(line1, 0x02, 1);
	put<uint32_t>(line1, 0x04, addr32(delay1));
	put<uint16_t>(line1, 0x08, 0x0DC8); // JASDsp::SEND_TABLE[2]
	put<int16_t>(line1, 0x0A, 0x7FFF);
	sync_frame(0x4000);
	release_subframe();
	unsigned badFx = 0;
	for (int k = 8; k < 80; k++)
		badFx += delay0[k] < 998 || delay0[k] > 1000;
	if (badFx)
		fail("FX return to 0x0DC8: %u of 72 bus-3 samples missing (delay0[8] = %d)", badFx, delay0[8]);
	else
		printf("audio-protocol-test: FX return through SEND_TABLE[2] 0x0DC8 reaches bus 3\n");
	memset(g_fxbuf, 0, 0x80);

	if (g_misframedReleases)
		fail("%u release-halt mails did not frame as Galaxy's (1 word; render + 0xF355FF00 after group 3 only)",
		     g_misframedReleases);
	if (g_rawAramLookups)
		fail("%u ARAM lookups bypassed the VARAM base", g_rawAramLookups);
	if (g_failures || nonzero == 0) {
		printf("audio-protocol-test: FAIL: %d check(s) failed (non-zero samples %llu, window misses %u)\n",
		       g_failures, (unsigned long long)nonzero, g_windowMisses);
		return 1;
	}
	printf("audio-protocol-test: PASS: non-silent output (%llu of %llu samples non-zero, rms %.0f)\n",
	       (unsigned long long)nonzero, (unsigned long long)out * 2, rms);
	return 0;
}
