#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <thread>
#include <vector>

using WutTime = int64_t;
constexpr WutTime kTimerHz = 248625000 / 4;

static uint64_t s_now;
static uint32_t s_clockReads;
static std::string s_appRoot;
static std::vector<std::string> s_logs;

extern "C" int64_t wut_OSGetTime(void)
{
	++s_clockReads;
	return static_cast<int64_t>(s_now);
}

extern "C" const char* sms_wiiu_app_root(void)
{
	return s_appRoot.c_str();
}

extern "C" uint32_t sms_wiiu_frame_count(void)
{
	return 77;
}

extern "C" const char* sms_wiiu_stage_path(void)
{
	return "host-stage";
}

extern "C" void port_log(const char* format, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	s_logs.emplace_back(buffer);
}

#include "smg_wiiu_frame_split.inc"

namespace {

void fail(const char* message)
{
	std::fprintf(stderr, "framesplit-host: FAIL: %s\n", message);
	for (const std::string& line : s_logs)
		std::fprintf(stderr, "  %s\n", line.c_str());
	std::exit(1);
}

void check(bool condition, const char* message)
{
	if (!condition)
		fail(message);
}

uint32_t ticks_for_us(uint32_t microseconds)
{
	return static_cast<uint32_t>((static_cast<uint64_t>(microseconds) * static_cast<uint64_t>(kTimerHz) + 999999) / 1000000);
}

void advance_us(uint32_t microseconds)
{
	s_now += ticks_for_us(microseconds);
}

void reset_split()
{
	s_splitMarker = -1;
	s_splitFrameActive = false;
	s_splitStarted = false;
	s_splitGxActive = false;
	s_splitGpuActive = false;
	s_splitAudioActive = false;
	s_splitHavePrevFrameEnd = false;
	s_splitGxTicks = 0;
	s_splitGpuTicks = 0;
	s_splitAudioFrame = 0;
	s_splitAudioTicks = 0;
	s_splitPrevFrameEnd = 0;
	s_splitFields = 0;
	s_splitSamples = 0;
	std::memset(s_splitValues, 0, sizeof(s_splitValues));
	s_logs.clear();
	s_clockReads = 0;
	s_now = 0;
}

void set_marker(bool present)
{
	const std::string path = s_appRoot + "framesplit";
	if (!present) {
		std::remove(path.c_str());
		return;
	}
	std::FILE* marker = std::fopen(path.c_str(), "wb");
	check(marker != nullptr, "create framesplit marker");
	std::fclose(marker);
}

void arm_baseline()
{
	split_frame_begin();
	split_frame_end(1);
}

const std::string& only_report(const char* message)
{
	check(s_logs.size() == 1, message);
	return s_logs.front();
}

void test_buckets()
{
	reset_split();
	set_marker(true);
	arm_baseline();
	split_frame_begin();
	split_gx_begin();
	advance_us(3000);
	split_gx_end();
	split_gpu_begin();
	advance_us(1000);
	split_gpu_end();
	split_gpu_begin();
	advance_us(1000);
	split_gpu_end();
	advance_us(10000);
	split_frame_end(60);
	const std::string& line = only_report("bucket case did not emit exactly one report");
	check(line.find("samples=1 fields=60") != std::string::npos, "bucket sample/field count");
	check(line.find("logic_cpu[p50=10000 p90=10000 max=10000]") != std::string::npos, "10 ms residual logic");
	check(line.find("gx_translate[p50=3000 p90=3000 max=3000]") != std::string::npos, "3 ms GX translation");
	check(line.find("gpu_wait[p50=2000 p90=2000 max=2000]") != std::string::npos, "accumulated 2 ms GPU wait");
	check(line.find("total[p50=15000 p90=15000]") != std::string::npos, "15 ms boundary-to-boundary total");
}

void test_no_draws()
{
	reset_split();
	set_marker(true);
	arm_baseline();
	split_frame_begin();
	advance_us(1000);
	split_frame_end(60);
	const std::string& line = only_report("no-draw case did not emit exactly one report");
	check(line.find("gx_translate[p50=0 p90=0 max=0]") != std::string::npos, "no-draw GX bucket");
}

void test_field_window()
{
	reset_split();
	set_marker(true);
	arm_baseline();
	for (int sample = 0; sample < 20; ++sample) {
		split_frame_begin();
		advance_us(1000);
		split_frame_end(3);
	}
	const std::string& line = only_report("field-window case did not emit exactly one report");
	check(line.find("samples=20 fields=60") != std::string::npos, "three fields count toward window");
}

void test_audio_thread()
{
	reset_split();
	set_marker(true);
	split_audio_begin();
	split_audio_end();
	check(s_clockReads == 0, "audio hook read the clock before main-thread arming");
	arm_baseline();
	split_frame_begin();
	std::thread audio([] {
		split_audio_begin();
		advance_us(4000);
		split_audio_end();
	});
	audio.join();
	advance_us(6000);
	split_frame_end(60);
	const std::string& line = only_report("audio case did not emit exactly one report");
	check(line.find("audio[p50=4000 p90=4000 max=4000]") != std::string::npos, "armed audio-thread bucket");
}

void test_marker_absent()
{
	reset_split();
	set_marker(false);
	split_frame_begin();
	split_gx_begin();
	split_gx_end();
	split_audio_begin();
	split_audio_end();
	check(s_logs.empty(), "marker-absent case emitted a report");
	check(s_clockReads == 0, "marker-absent batch hook read the clock");
}

} // namespace

int main(int argc, char** argv)
{
	check(argc == 2, "expected temporary app-root argument");
	s_appRoot = argv[1];
	if (s_appRoot.empty() || s_appRoot.back() != '/')
		s_appRoot += '/';
	test_buckets();
	test_no_draws();
	test_field_window();
	test_audio_thread();
	test_marker_absent();
	std::puts("framesplit-host: PASS: real profiler split; boundary total, residual logic, accumulated GPU, fields, marker, audio thread");
	return 0;
}
