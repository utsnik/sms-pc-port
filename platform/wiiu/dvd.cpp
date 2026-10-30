#include "port_os.h"
#include "runtime.h"

/* The game's OSMutex/OSThread layouts differ from coreinit's. */
#define OSMutex CafeOSMutex
#define OSMutexLink CafeOSMutexLink
#define OSThread CafeOSThread
#define OSThreadQueue CafeOSThreadQueue
#define OSThreadLink CafeOSThreadLink
#define OSPanic cafe_OSPanic
#undef OSInitThreadQueue
#undef OSTryLockMutex
#undef OSInitMutex
#undef OSLockMutex
#undef OSUnlockMutex
#define OSInitMutex cafe_OSInitMutex
#define OSLockMutex cafe_OSLockMutex
#define OSUnlockMutex cafe_OSUnlockMutex
#define OSInitThreadQueue cafe_OSInitThreadQueue
#define OSTryLockMutex cafe_OSTryLockMutex
#include <coreinit/mutex.h>
#undef OSInitMutex
#undef OSLockMutex
#undef OSUnlockMutex
#undef OSInitThreadQueue
#undef OSTryLockMutex
#undef OSMutex
#undef OSMutexLink
#undef OSThread
#undef OSThreadQueue
#undef OSThreadLink
#undef OSPanic
extern "C" {
void cafe_OSInitMutex(CafeOSMutex*) __asm__("OSInitMutex");
void cafe_OSLockMutex(CafeOSMutex*) __asm__("OSLockMutex");
void cafe_OSUnlockMutex(CafeOSMutex*) __asm__("OSUnlockMutex");
}

#include <dolphin/dvd.h>
#include <dolphin/os/OSCache.h>
#if defined(PORT_GAME_SMG)
#include <dolphin/os.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

#if defined(PORT_GAME_SMG)
const uint32_t kMaxEntries = 2465;
#else
const uint32_t kMaxEntries = 512;
#endif
const uint32_t kMaxPath = 512;
#if defined(PORT_GAME_SMG)
const uint32_t kMaxFstBytes = 81780u;
#else
const uint32_t kMaxFstBytes = 64u * 1024u;
#endif
const uint32_t kMaxFiles = 96;
const uint32_t kMaxPending = 96;
const uint32_t kCacheSlots = 6;
const uint32_t kPackCacheSlot = 0;
const size_t kReadAheadSize = 128u * 1024u;
const size_t kLargeReadSize = 64u * 1024u;

struct CachedFile {
	FILE* file;
	u32 entry;
	uint64_t used;
	uint32_t window_offset;
	size_t window_length;
	uint32_t position;
};

struct FstEntry {
	bool directory;
	u32 parent;
	u32 next;
	u32 offset;
	u32 size;
	char path[kMaxPath];
};

struct FileState {
	DVDFileInfo* file;
	u32 entry;
	bool active;
};

struct PackIndexEntry {
	uint32_t offset;
	uint32_t size;
	bool valid;
};

struct Pending {
	DVDFileInfo* file;
	DVDCallback callback;
	s32 result;
	bool active;
};

alignas(32) uint8_t s_fst_raw[kMaxFstBytes];
FstEntry s_entries[kMaxEntries];
FileState s_files[kMaxFiles];
Pending s_pending[kMaxPending];
DVDDiskID s_disk_id;
DVDDirEntry s_dir_entry;
char s_dir_name[kMaxPath];
char s_current_dir[kMaxPath] = "/";
char s_last_stage_path[kMaxPath] = "unknown";
uint32_t s_entry_count;
uint32_t s_file_count;
uint32_t s_directory_count;
bool s_initialized;
bool s_busy;
bool s_mutex_initialized;
CafeOSMutex s_read_mutex;
CachedFile s_cache[kCacheSlots];
alignas(64) static uint8_t s_read_ahead[kCacheSlots][kReadAheadSize];
PackIndexEntry s_pack_entries[kMaxEntries];
FILE* s_pack_file;
uint32_t s_pack_file_size;
uint64_t s_cache_clock;
uint64_t s_stat_reads;
uint64_t s_stat_bytes;
uint64_t s_stat_fill_bytes;
uint64_t s_stat_opens;
uint64_t s_stat_pack_reads;
uint64_t s_stat_read_ticks;
OSTime s_stat_log_time;

void reset_cached_file(unsigned slot)
{
	s_cache[slot].window_length = 0;
	s_cache[slot].position = 0;
}

void close_cached_file(unsigned slot)
{
	if (s_cache[slot].file &&
	    !(slot == kPackCacheSlot && s_cache[slot].file == s_pack_file)) {
		std::fclose(s_cache[slot].file);
	}
	s_cache[slot].file = nullptr;
	reset_cached_file(slot);
}

void log_read_stats()
{
	const OSTime now = OSGetTime();
	if (!s_stat_log_time)
		s_stat_log_time = now;
	if (now - s_stat_log_time >= OSSecondsToTicks(5)) {
		port_log("[dvd] stats reads=%llu pack_reads=%llu bytes=%llu fill_bytes=%llu opens=%llu read_ms=%llu\n",
		         static_cast<unsigned long long>(s_stat_reads),
		         static_cast<unsigned long long>(s_stat_pack_reads),
		         static_cast<unsigned long long>(s_stat_bytes),
		         static_cast<unsigned long long>(s_stat_fill_bytes),
		         static_cast<unsigned long long>(s_stat_opens),
		         static_cast<unsigned long long>(OSTicksToMilliseconds(s_stat_read_ticks)));
		s_stat_log_time = now;
	}
}

uint32_t be32(const uint8_t* p)
{
	return (static_cast<uint32_t>(p[0]) << 24) |
	       (static_cast<uint32_t>(p[1]) << 16) |
	       (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool equal_ci(const char* a, const char* b)
{
	while (*a && *b) {
		if (std::tolower(static_cast<unsigned char>(*a)) !=
		    std::tolower(static_cast<unsigned char>(*b)))
			return false;
		++a;
		++b;
	}
	return *a == *b;
}

bool append_text(char* out, uint32_t cap, const char* text)
{
	uint32_t used = static_cast<uint32_t>(std::strlen(out));
	uint32_t n = static_cast<uint32_t>(std::strlen(text));
	if (used + n + 1 >= cap)
		return false;
	std::memcpy(out + used, text, n + 1);
	return true;
}

#if defined(PORT_GAME_SMG)
bool valid_fst_name(const char* name)
{
	if (!name || !*name || !std::strcmp(name, ".") || !std::strcmp(name, ".."))
		return false;
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
		if (*p < 0x20 || *p == '/' || *p == '\\' || *p == ':')
			return false;
	return true;
}
#endif

#if defined(PORT_GAME_SMG)
bool valid_entry_path(const char* path)
{
	if (!path || path[0] != '/')
		return false;
	const char* part = path + 1;
	while (*part) {
		char name[256];
		uint32_t length = 0;
		while (part[length] && part[length] != '/') {
			if (length + 1 >= sizeof(name))
				return false;
			name[length] = part[length];
			++length;
		}
		name[length] = '\0';
		if (!valid_fst_name(name))
			return false;
		part += length;
		if (*part == '/')
			++part;
	}
	return true;
}
#endif

bool normalize_path(const char* input, char* out, uint32_t cap)
{
	if (!input || !*input)
		return false;
	char source[1024];
	source[0] = '\0';
	if (*input == '/') {
		std::strncpy(source, input, sizeof(source) - 1);
	} else {
		std::strncpy(source, s_current_dir, sizeof(source) - 1);
		#if defined(PORT_GAME_SMG)
		if (!append_text(source, sizeof(source), "/") ||
		    !append_text(source, sizeof(source), input))
			return false;
		#else
		append_text(source, sizeof(source), "/");
		append_text(source, sizeof(source), input);
		#endif
	}
	source[sizeof(source) - 1] = '\0';

	out[0] = '/';
	out[1] = '\0';
	const char* p = source;
	while (*p) {
		while (*p == '/')
			++p;
		if (!*p)
			break;
		char part[256];
		uint32_t n = 0;
		while (*p && *p != '/') {
			if (n + 1 >= sizeof(part))
				return false;
			part[n++] = *p++;
		}
		part[n] = '\0';
		#if defined(PORT_GAME_SMG)
		if (!valid_fst_name(part) && std::strcmp(part, ".") &&
		    std::strcmp(part, ".."))
			return false;
		#endif
		if (!std::strcmp(part, "."))
			continue;
		if (!std::strcmp(part, "..")) {
			#if defined(PORT_GAME_SMG)
			if (std::strlen(out) <= 1)
				return false;
			#endif
			char* slash = std::strrchr(out + 1, '/');
			if (slash)
				*slash = '\0';
			else
				out[1] = '\0';
			continue;
		}
		if (std::strlen(out) > 1 && !append_text(out, cap, "/"))
			return false;
		if (!append_text(out, cap, part))
			return false;
	}
	return true;
}

int find_entry(const char* path)
{
	char normalized[kMaxPath];
	if (!normalize_path(path, normalized, sizeof(normalized)))
		return -1;
	for (uint32_t i = 0; i < s_entry_count; ++i)
		if (equal_ci(normalized, s_entries[i].path))
			return static_cast<int>(i);
	return -1;
}

bool load_pack()
{
	char pack_path[192];
	std::snprintf(pack_path, sizeof(pack_path), "%spack.bin", sms_wiiu_app_root());
	FILE* pack = std::fopen(pack_path, "rb");
	if (!pack)
		return false;
	auto failed = [&]() {
		std::fclose(pack);
		std::memset(s_pack_entries, 0, sizeof(s_pack_entries));
		s_pack_file_size = 0;
		return false;
	};
	if (std::setvbuf(pack, nullptr, _IONBF, 0) != 0)
		return failed();
	if (std::fseek(pack, 0, SEEK_END) != 0)
		return failed();
	long pack_size = std::ftell(pack);
	if (pack_size < 32 || static_cast<unsigned long>(pack_size) > 0xfffffffful)
		return failed();
	s_pack_file_size = static_cast<uint32_t>(pack_size);
	if (std::fseek(pack, 0, SEEK_SET) != 0)
		return failed();

	uint8_t header[32];
	if (std::fread(header, 1, sizeof(header), pack) != sizeof(header) ||
	    std::memcmp(header, "SMGP", 4) != 0 || be32(header + 4) != 1 ||
	    be32(header + 8) > kMaxEntries || be32(header + 16) < 32 ||
	    (be32(header + 16) & 63u) != 0)
		return failed();
	const uint32_t count = be32(header + 8);
	const uint32_t index_size = be32(header + 12);
	const uint32_t data_offset = be32(header + 16);
	if (index_size > s_pack_file_size - 32 ||
	    data_offset < 32 + index_size || data_offset > s_pack_file_size ||
	    std::fseek(pack, 32, SEEK_SET) != 0)
		return failed();

	std::memset(s_pack_entries, 0, sizeof(s_pack_entries));
	const uint32_t index_end = 32 + index_size;
	uint32_t index_position = 32;
	uint32_t mapped = 0;
	for (uint32_t i = 0; i < count; ++i) {
		uint8_t record_header[4];
		if (index_position + sizeof(record_header) > index_end ||
		    std::fread(record_header, 1, sizeof(record_header), pack) !=
		        sizeof(record_header))
			return failed();
		index_position += sizeof(record_header);
		const uint32_t path_length = (static_cast<uint32_t>(record_header[0]) << 8) |
		                             record_header[1];
		if (!path_length || path_length >= kMaxPath ||
		    path_length + 8 > index_end - index_position)
			return failed();
		char path[kMaxPath];
		if (std::fread(path, 1, path_length, pack) != path_length)
			return failed();
		path[path_length] = '\0';
		index_position += path_length;
		uint8_t location[8];
		if (std::fread(location, 1, sizeof(location), pack) != sizeof(location))
			return failed();
		index_position += sizeof(location);
		const uint32_t offset = be32(location);
		const uint32_t size = be32(location + 4);
		if ((offset & 63u) != 0 || offset < data_offset ||
		    offset > s_pack_file_size || size > s_pack_file_size - offset)
			return failed();
		const int entry = find_entry(path);
		if (entry >= 0 && !s_entries[entry].directory) {
			if (s_pack_entries[entry].valid || s_entries[entry].size != size)
				return failed();
			s_pack_entries[entry].offset = offset;
			s_pack_entries[entry].size = size;
			s_pack_entries[entry].valid = true;
			++mapped;
		}
	}
	if (index_position != index_end)
		return failed();

	s_pack_file = pack;
	s_cache[kPackCacheSlot].file = pack;
	s_cache[kPackCacheSlot].entry = 0xffffffffu;
	reset_cached_file(kPackCacheSlot);
	++s_stat_opens;
	port_log("[dvd] pack: %u entries, %u mapped, %u bytes\n", count,
	         mapped,
	         s_pack_file_size);
	return true;
}

u32 parent_for(u32 index)
{
	u32 parent = 0;
	for (u32 i = 1; i < index; ++i)
		if (s_entries[i].directory && index < s_entries[i].next)
			parent = i;
	return parent;
}

const char* entry_basename(const char* path)
{
	const char* slash = std::strrchr(path, '/');
	return slash ? slash + 1 : path;
}

FileState* state_for(DVDFileInfo* file, bool create)
{
	for (unsigned i = 0; i < kMaxFiles; ++i)
		if (s_files[i].active && s_files[i].file == file)
			return &s_files[i];
	if (!create)
		return nullptr;
	for (unsigned i = 0; i < kMaxFiles; ++i) {
		if (s_files[i].active)
			continue;
		s_files[i].file = file;
		s_files[i].active = true;
		return &s_files[i];
	}
	return nullptr;
}

bool files_path(u32 entry, char* out, uint32_t cap)
{
	if (entry >= s_entry_count || s_entries[entry].directory)
		return false;
#if defined(PORT_GAME_SMG)
	const char* root = sms_wiiu_app_root();
	if (!root || !*root || !valid_entry_path(s_entries[entry].path))
		return false;
	const uint32_t root_len = static_cast<uint32_t>(std::strlen(root));
	if (!root_len || root[root_len - 1] != '/')
		return false;
	/* FST paths are "/data/x.szs"; the extracted tree keeps them under files/. */
	int n = std::snprintf(out, cap, "%sfiles%s", root,
	                      s_entries[entry].path);
	if (n < 0 || static_cast<uint32_t>(n) >= cap)
		return false;
	return std::strncmp(out, root, root_len) == 0 &&
	       std::strncmp(out + root_len, "files/", 6) == 0;
#else
	/* FST paths are "/data/x.szs"; the extracted tree keeps them under files/. */
	int n = std::snprintf(out, cap, "%sfiles%s", sms_wiiu_app_root(),
	                      s_entries[entry].path);
	if (n < 0 || static_cast<uint32_t>(n) >= cap)
		return false;
	const char* root = sms_wiiu_app_root();
	uint32_t root_len = static_cast<uint32_t>(std::strlen(root));
	return std::strncmp(out, root, root_len) == 0 &&
	       std::strncmp(out + root_len, "files/", 6) == 0;
#endif
}

s32 read_file(FileState* state, void* address, s32 length, s32 offset)
{
	if (!state || !address || length < 0 || offset < 0)
		return DVD_RESULT_FATAL_ERROR;
	FstEntry& entry = s_entries[state->entry];
	/* A GameCube DVD read past the end of a file returns the disc bytes that
	 * follow, and JKR's decompressor reads fixed 0xFE0 chunks that overrun the
	 * last one. Read what the file has and zero-fill the rest. */
	if (static_cast<uint32_t>(offset) > entry.size)
		return DVD_RESULT_FATAL_ERROR;
	const uint32_t remaining = entry.size - static_cast<uint32_t>(offset);
	const size_t want = static_cast<uint32_t>(length) < remaining ? static_cast<size_t>(length) : remaining;
	char path[kMaxPath + 128];
	static uint32_t s_failures;
	const bool packed = s_pack_file && s_pack_entries[state->entry].valid;
	if (!packed && !files_path(state->entry, path, sizeof(path))) {
		if (s_failures++ < 16)
			port_log("[dvd] read rejected: bad path for entry %u (%s)\n", state->entry, entry.path);
		return DVD_RESULT_FATAL_ERROR;
	}
	const char* log_path = packed ? entry.path : path;
	const unsigned first_file_slot = s_pack_file ? kPackCacheSlot + 1 : kPackCacheSlot;
	unsigned slot = packed ? kPackCacheSlot : first_file_slot;
	if (!packed) {
		for (; slot < kCacheSlots; ++slot)
			if (s_cache[slot].file && s_cache[slot].entry == state->entry)
				break;
	}
	if (!packed && slot == kCacheSlots) {
		slot = first_file_slot;
		for (unsigned i = first_file_slot; i < kCacheSlots; ++i)
			if (!s_cache[i].file || s_cache[i].used < s_cache[slot].used)
				slot = i;
		close_cached_file(slot);
	}
	const OSTime read_start = OSGetTime();
	FILE* file = s_cache[slot].file;
	if (!file) {
		file = packed ? s_pack_file : std::fopen(path, "rb");
		if (!file) {
			if (s_failures++ < 16)
				port_log("[dvd] read failed: cannot open %s\n", log_path);
			s_stat_read_ticks += OSGetTime() - read_start;
			log_read_stats();
			return DVD_RESULT_FATAL_ERROR;
		}
		if (!packed && std::setvbuf(file, nullptr, _IONBF, 0) != 0) {
			std::fclose(file);
			s_stat_read_ticks += OSGetTime() - read_start;
			log_read_stats();
			return DVD_RESULT_FATAL_ERROR;
		}
		s_cache[slot].file = file;
		s_cache[slot].entry = state->entry;
		reset_cached_file(slot);
		if (!packed)
			++s_stat_opens;
	}
	if (packed)
		++s_stat_pack_reads;
	s_cache[slot].used = ++s_cache_clock;
	size_t got = 0;
	size_t fill = 0;
	if (want != 0) {
		const uint32_t file_offset = static_cast<uint32_t>(offset);
		if (packed && s_pack_entries[state->entry].offset > 0xffffffffu - file_offset) {
			reset_cached_file(slot);
			s_stat_read_ticks += OSGetTime() - read_start;
			log_read_stats();
			return DVD_RESULT_FATAL_ERROR;
		}
		const uint32_t request_offset = packed ?
			s_pack_entries[state->entry].offset + file_offset : file_offset;
		const bool in_window = request_offset >= s_cache[slot].window_offset &&
		                       request_offset <= s_cache[slot].window_offset + s_cache[slot].window_length &&
		                       want <= s_cache[slot].window_length -
		                                  (request_offset - s_cache[slot].window_offset);
		if (in_window) {
			std::memcpy(address, s_read_ahead[slot] +
			                         (request_offset - s_cache[slot].window_offset), want);
			got = want;
		} else if (want >= kLargeReadSize) {
			if (s_cache[slot].position != request_offset) {
				if (std::fseek(file, static_cast<long>(request_offset), SEEK_SET) != 0) {
					if (packed)
						reset_cached_file(slot);
					else
						close_cached_file(slot);
					s_stat_read_ticks += OSGetTime() - read_start;
					log_read_stats();
					return DVD_RESULT_FATAL_ERROR;
				}
				s_cache[slot].position = request_offset;
			}
			got = std::fread(address, 1, want, file);
			fill = got;
			s_cache[slot].position += static_cast<uint32_t>(got);
			s_cache[slot].window_length = 0;
		} else {
			if (s_cache[slot].position != request_offset) {
				if (std::fseek(file, static_cast<long>(request_offset), SEEK_SET) != 0) {
					if (packed)
						reset_cached_file(slot);
					else
						close_cached_file(slot);
					s_stat_read_ticks += OSGetTime() - read_start;
					log_read_stats();
					return DVD_RESULT_FATAL_ERROR;
				}
				s_cache[slot].position = request_offset;
			}
			fill = std::fread(s_read_ahead[slot], 1, kReadAheadSize, file);
			s_cache[slot].window_offset = request_offset;
			s_cache[slot].window_length = fill;
			s_cache[slot].position += static_cast<uint32_t>(fill);
			got = fill < want ? fill : want;
			if (got)
				std::memcpy(address, s_read_ahead[slot], got);
		}
	}
	s_stat_read_ticks += OSGetTime() - read_start;
	s_stat_bytes += got;
	s_stat_fill_bytes += fill;
	++s_stat_reads;
	/* A GameCube DVD read is a DMA; callers may invalidate the buffer after it,
	 * which on the Wii U would drop this CPU copy. Write it back to memory. */
	if (got)
		DCFlushRange(address, got);
	if (got != want) {
		if (packed)
			reset_cached_file(slot);
		else
			close_cached_file(slot);
		if (s_failures++ < 16)
			port_log("[dvd] short read %s: %u of %u\n", log_path, static_cast<unsigned>(got), static_cast<unsigned>(want));
		log_read_stats();
		return DVD_RESULT_FATAL_ERROR;
	}
	if (want < static_cast<size_t>(length))
		std::memset(static_cast<unsigned char*>(address) + want, 0, static_cast<size_t>(length) - want);
	if (std::strstr(entry.path, "/data/") != nullptr) {
		std::strncpy(s_last_stage_path, entry.path, sizeof(s_last_stage_path) - 1);
		s_last_stage_path[sizeof(s_last_stage_path) - 1] = '\0';
	}
	/* Keep UDP low-rate: the first reads in full, then every 256th. */
	static uint32_t s_reads;
	if (s_reads < 64 || (s_reads & 255u) == 0)
		port_log("[dvd] read #%u path=%s entry=%u offset=0x%x bytes=%d state=complete\n",
		         s_reads, entry.path, state->entry, offset, length);
	++s_reads;
	log_read_stats();
	return length;
}

void queue_callback(DVDFileInfo* file, DVDCallback callback, s32 result)
{
	for (unsigned i = 0; i < kMaxPending; ++i) {
		if (s_pending[i].active)
			continue;
		s_pending[i].file = file;
		s_pending[i].callback = callback;
		s_pending[i].result = result;
		s_pending[i].active = true;
		s_busy = true;
		return;
	}
	port_log("[dvd] async completion queue exhausted\n");
}

} // namespace

extern "C" {

void port_dvd_init(void)
{
	if (!s_mutex_initialized) {
		cafe_OSInitMutex(&s_read_mutex);
		s_mutex_initialized = true;
	}
	if (s_initialized)
		return;
#if defined(PORT_GAME_SMG)
	s_entry_count = 0;
	s_file_count = 0;
	s_directory_count = 0;
#endif
	char fst_path[192];
	std::snprintf(fst_path, sizeof(fst_path), "%ssys/fst.bin", sms_wiiu_app_root());
	FILE* fst = std::fopen(fst_path, "rb");
#if defined(PORT_GAME_SMG)
	if (!fst || std::fseek(fst, 0, SEEK_END) != 0) {
		if (fst)
			std::fclose(fst);
		port_log("[dvd] cannot open %s\n", fst_path);
		return;
	}
	long fst_size = std::ftell(fst);
	if (fst_size < 0 || fst_size > static_cast<long>(sizeof(s_fst_raw))) {
		std::fclose(fst);
		port_log("[dvd] invalid FST size=%ld\n", fst_size);
		return;
	}
	std::rewind(fst);
	if (fst_size == 0 ||
	    std::fread(s_fst_raw, 1, static_cast<size_t>(fst_size), fst) !=
	        static_cast<size_t>(fst_size)) {
		std::fclose(fst);
		port_log("[dvd] cannot read %s\n", fst_path);
		return;
	}
	std::fclose(fst);
#else
	if (!fst || std::fread(s_fst_raw, 1, sizeof(s_fst_raw), fst) == 0) {
		if (fst)
			std::fclose(fst);
		port_log("[dvd] cannot open %s\n", fst_path);
		return;
	}
	long fst_size = std::ftell(fst);
	std::fclose(fst);
#endif
#if defined(PORT_GAME_SMG)
	if (fst_size < 12) {
#else
	if (fst_size < 12 || fst_size > static_cast<long>(sizeof(s_fst_raw))) {
#endif
		port_log("[dvd] invalid FST size=%ld\n", fst_size);
		return;
	}
	s_entry_count = be32(s_fst_raw + 8);
	if (!s_entry_count || s_entry_count > kMaxEntries ||
	    s_entry_count * 12u > static_cast<uint32_t>(fst_size)) {
		port_log("[dvd] invalid FST entry count=%u\n", s_entry_count);
		return;
	}
	const uint8_t* strings = s_fst_raw + s_entry_count * 12;
	uint32_t string_bytes = static_cast<uint32_t>(fst_size) - s_entry_count * 12;
	std::memset(s_entries, 0, sizeof(s_entries));
#if !defined(PORT_GAME_SMG)
	std::strncpy(s_entries[0].path, "/", kMaxPath - 1);
#endif
	for (uint32_t i = 0; i < s_entry_count; ++i) {
		const uint8_t* raw = s_fst_raw + i * 12;
#if defined(PORT_GAME_SMG)
		u32 type_and_name = be32(raw);
		u32 name_offset = type_and_name & 0x00ffffffu;
#else
		u32 name_offset = be32(raw) & 0x00ffffffu;
#endif
		if (i && name_offset >= string_bytes) {
			port_log("[dvd] FST name offset out of range at %u\n", i);
			return;
		}
		const char* name = i ? reinterpret_cast<const char*>(strings + name_offset) : "";
		if (i && !std::memchr(name, '\0', string_bytes - name_offset)) {
			port_log("[dvd] unterminated FST name at %u\n", i);
			return;
		}
#if defined(PORT_GAME_SMG)
		s_entries[i].directory = (type_and_name & 0x01000000u) != 0;
#else
		s_entries[i].directory = (raw[0] & 1) != 0;
#endif
		if (s_entries[i].directory) {
			s_entries[i].parent = be32(raw + 4);
			s_entries[i].next = be32(raw + 8);
#if defined(PORT_GAME_SMG)
			if (s_entries[i].next <= i || s_entries[i].next > s_entry_count ||
			    (i == 0 && (s_entries[i].parent != 0 ||
			                s_entries[i].next != s_entry_count))) {
				port_log("[dvd] invalid FST directory range at %u\n", i);
				return;
			}
#endif
			++s_directory_count;
		} else {
#if defined(PORT_GAME_SMG)
			if (!i) {
				port_log("[dvd] FST root is not a directory\n");
				return;
			}
#endif
			s_entries[i].offset = be32(raw + 4);
			s_entries[i].size = be32(raw + 8);
			++s_file_count;
		}
#if defined(PORT_GAME_SMG)
		if (!i) {
			std::strcpy(s_entries[i].path, "/");
			continue;
		}
#else
		if (!i)
			continue;
#endif
#if defined(PORT_GAME_SMG)
		if (!valid_fst_name(name)) {
			port_log("[dvd] malformed FST name at %u\n", i);
			return;
		}
#endif
		u32 parent = s_entries[i].directory ? s_entries[i].parent : parent_for(i);
#if defined(PORT_GAME_SMG)
		if (parent >= i || !s_entries[parent].directory ||
		    s_entries[parent].next <= i || !s_entries[parent].path[0] ||
#else
		if (parent >= i || !s_entries[parent].path[0] ||
#endif
		    std::strlen(s_entries[parent].path) + std::strlen(name) + 2 >= kMaxPath) {
			port_log("[dvd] FST parent/path error at %u\n", i);
			return;
		}
		std::snprintf(s_entries[i].path, kMaxPath, "%s%s%s",
		              s_entries[parent].path,
		              std::strcmp(s_entries[parent].path, "/") ? "/" : "", name);
	}
	load_pack();
	char boot_path[192];
	std::snprintf(boot_path, sizeof(boot_path), "%ssys/boot.bin", sms_wiiu_app_root());
	FILE* boot = std::fopen(boot_path, "rb");
	std::memset(&s_disk_id, 0, sizeof(s_disk_id));
#if defined(PORT_GAME_SMG)
	if (!boot || std::fread(&s_disk_id, 1, sizeof(s_disk_id), boot) != sizeof(s_disk_id)) {
		if (boot)
			std::fclose(boot);
		port_log("[dvd] cannot read %s\n", boot_path);
		return;
	}
	std::fclose(boot);
	/* The Wii IPL leaves the disc ID at physical 0 (game code "RMGx");
	 * Galaxy reads it there to pick its language table. */
	if (void* low = OSPhysicalToCached(0))
		std::memcpy(low, &s_disk_id, sizeof(s_disk_id));
#else
	if (boot) {
		std::fread(&s_disk_id, 1, sizeof(s_disk_id), boot);
		std::fclose(boot);
	}
#endif
	s_current_dir[0] = '/';
	s_current_dir[1] = '\0';
	s_initialized = true;
	port_log("[dvd] %.4s: FST: %u entries, %u files, %u directories\n",
	         s_disk_id.gameName, s_entry_count, s_file_count, s_directory_count);
}

void DVDInit(void) { port_dvd_init(); }

void sms_wiiu_dvd_shutdown(void)
{
	if (!s_mutex_initialized)
		return;
	cafe_OSLockMutex(&s_read_mutex);
	for (unsigned i = 0; i < kCacheSlots; ++i)
		close_cached_file(i);
	if (s_pack_file) {
		std::fclose(s_pack_file);
		s_pack_file = nullptr;
	}
	cafe_OSUnlockMutex(&s_read_mutex);
}

s32 DVDConvertPathToEntrynum(char* path)
{
	if (!s_initialized)
		port_dvd_init();
	return find_entry(path);
}

BOOL DVDFastOpen(s32 entry, DVDFileInfo* file)
{
	if (!file || entry < 0 || static_cast<uint32_t>(entry) >= s_entry_count ||
	    s_entries[entry].directory)
		return FALSE;
	cafe_OSLockMutex(&s_read_mutex);
	std::memset(file, 0, sizeof(*file));
	FileState* state = state_for(file, true);
	if (!state) {
		cafe_OSUnlockMutex(&s_read_mutex);
		return FALSE;
	}
	state->entry = static_cast<u32>(entry);
	file->startAddr = s_entries[entry].offset;
	file->length = s_entries[entry].size;
	file->cb.state = DVD_STATE_END;
	cafe_OSUnlockMutex(&s_read_mutex);
	return TRUE;
}

BOOL DVDOpen(char* name, DVDFileInfo* file)
{
	s32 entry = DVDConvertPathToEntrynum(name);
	if (entry < 0 || !DVDFastOpen(entry, file)) {
		port_log("[dvd] path not found: %s\n", name ? name : "(null)");
		return FALSE;
	}
	port_log("[dvd] open path=%s entry=%d offset=0x%x bytes=%u\n", name, entry,
	         file->startAddr, file->length);
	return TRUE;
}

BOOL DVDClose(DVDFileInfo* file)
{
	cafe_OSLockMutex(&s_read_mutex);
	FileState* state = state_for(file, false);
	if (!state) {
		cafe_OSUnlockMutex(&s_read_mutex);
		return FALSE;
	}
	state->active = false;
	file->cb.state = DVD_STATE_END;
	cafe_OSUnlockMutex(&s_read_mutex);
	return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* file, void* address, s32 length, s32 offset, s32)
{
	cafe_OSLockMutex(&s_read_mutex);
	FileState* state = state_for(file, false);
	if (!state) {
		cafe_OSUnlockMutex(&s_read_mutex);
		return DVD_RESULT_FATAL_ERROR;
	}
	s_busy = true;
	file->cb.state = DVD_STATE_BUSY;
	s32 result = read_file(state, address, length, offset);
	file->cb.state = result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
	file->cb.transferredSize = result > 0 ? static_cast<u32>(result) : 0;
	s_busy = false;
	cafe_OSUnlockMutex(&s_read_mutex);
	return result;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* file, void* address, s32 length, s32 offset,
                      DVDCallback callback, s32)
{
	cafe_OSLockMutex(&s_read_mutex);
	FileState* state = state_for(file, false);
	if (!state) {
		cafe_OSUnlockMutex(&s_read_mutex);
		return FALSE;
	}
	file->cb.state = DVD_STATE_BUSY;
	s32 result = read_file(state, address, length, offset);
	file->cb.state = result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
	file->cb.transferredSize = result > 0 ? static_cast<u32>(result) : 0;
	queue_callback(file, callback, result);
	cafe_OSUnlockMutex(&s_read_mutex);
	return TRUE;
}

void sms_wiiu_dvd_pump(void)
{
	for (unsigned i = 0; i < kMaxPending; ++i) {
		cafe_OSLockMutex(&s_read_mutex);
		if (!s_pending[i].active) {
			cafe_OSUnlockMutex(&s_read_mutex);
			continue;
		}
		Pending pending = s_pending[i];
		s_pending[i].active = false;
		cafe_OSUnlockMutex(&s_read_mutex);
		if (pending.file)
			pending.file->cb.state = pending.result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
		if (pending.callback)
			pending.callback(pending.result, pending.file);
	}
	cafe_OSLockMutex(&s_read_mutex);
	s_busy = false;
	cafe_OSUnlockMutex(&s_read_mutex);
}

int port_dvd_busy(void)
{
	cafe_OSLockMutex(&s_read_mutex);
	const int busy = s_busy ? 1 : 0;
	cafe_OSUnlockMutex(&s_read_mutex);
	return busy;
}
const char* port_dvd_stage_path(void) { return s_last_stage_path; }

void* sms_wiiu_dvd_read_file(const char* path, size_t* size)
{
	if (size)
		*size = 0;
#if defined(PORT_GAME_SMG)
	/* sys/ (main.dol, boot.bin, ...) is outside the disc FST, so the FST
	 * lookup below can never find "/sys/main.dol"; read it from the app
	 * root's sys/ directory, as port_dvd_init does for sys/fst.bin. */
	if (path && std::strncmp(path, "/sys/", 5) == 0) {
		if (!valid_fst_name(path + 5))
			return nullptr;
		char sys_path[kMaxPath + 128];
		int n = std::snprintf(sys_path, sizeof(sys_path), "%ssys/%s",
		                      sms_wiiu_app_root(), path + 5);
		if (n < 0 || static_cast<uint32_t>(n) >= sizeof(sys_path))
			return nullptr;
		FILE* file = std::fopen(sys_path, "rb");
		if (!file)
			return nullptr;
		long length = -1;
		if (std::fseek(file, 0, SEEK_END) == 0)
			length = std::ftell(file);
		void* data = length > 0 ? port_low_alloc(static_cast<size_t>(length)) : nullptr;
		const bool ok = data && std::fseek(file, 0, SEEK_SET) == 0 &&
		                std::fread(data, 1, static_cast<size_t>(length), file) ==
		                    static_cast<size_t>(length);
		std::fclose(file);
		if (!ok)
			return nullptr;
		port_log("[dvd] read %s bytes=%ld (sys/, outside the FST)\n", sys_path, length);
		if (size)
			*size = static_cast<size_t>(length);
		return data;
	}
#endif
	int entry = find_entry(path);
	if (entry < 0 || s_entries[entry].directory)
		return nullptr;
	void* data = port_low_alloc(s_entries[entry].size);
	if (!data)
		return nullptr;
	DVDFileInfo file;
	if (!DVDFastOpen(entry, &file))
		return nullptr;
	s32 result = DVDReadPrio(&file, data, static_cast<s32>(s_entries[entry].size), 0, 2);
	DVDClose(&file);
	if (result < 0)
		return nullptr;
	if (size)
		*size = s_entries[entry].size;
	return data;
}

BOOL DVDOpenDir(char* name, DVDDir* dir)
{
	if (!dir)
		return FALSE;
	s32 entry = DVDConvertPathToEntrynum(name);
	if (entry < 0 || !s_entries[entry].directory)
		return FALSE;
	dir->entryNum = static_cast<u32>(entry);
	dir->location = static_cast<u32>(entry + 1);
	dir->next = s_entries[entry].next;
	return TRUE;
}

BOOL DVDReadDir(DVDDir* dir, DVDDirEntry* out)
{
	if (!dir || !out)
		return FALSE;
	while (dir->location < dir->next) {
		u32 entry = dir->location++;
		if (parent_for(entry) != dir->entryNum)
			continue;
		std::strncpy(s_dir_name, entry_basename(s_entries[entry].path), sizeof(s_dir_name) - 1);
		s_dir_name[sizeof(s_dir_name) - 1] = '\0';
		s_dir_entry.entryNum = entry;
		s_dir_entry.isDir = s_entries[entry].directory ? TRUE : FALSE;
		s_dir_entry.name = s_dir_name;
		*out = s_dir_entry;
		return TRUE;
	}
	return FALSE;
}

BOOL DVDCloseDir(DVDDir*) { return TRUE; }

BOOL DVDGetCurrentDir(char* path, u32 maxlen)
{
	if (!path || !maxlen || std::strlen(s_current_dir) + 1 > maxlen)
		return FALSE;
	std::strcpy(path, s_current_dir);
	return TRUE;
}

BOOL DVDChangeDir(char* name)
{
	char normalized[kMaxPath];
	if (!normalize_path(name, normalized, sizeof(normalized)))
		return FALSE;
	int entry = find_entry(normalized);
	if (entry < 0 || !s_entries[entry].directory)
		return FALSE;
	std::strncpy(s_current_dir, normalized, sizeof(s_current_dir) - 1);
	s_current_dir[sizeof(s_current_dir) - 1] = '\0';
	return TRUE;
}

s32 DVDGetFileInfoStatus(DVDFileInfo* file)
{
	return file ? file->cb.state : DVD_STATE_FATAL_ERROR;
}
s32 DVDGetCommandBlockStatus(DVDCommandBlock* block)
{
	return block ? block->state : DVD_STATE_FATAL_ERROR;
}
s32 DVDGetTransferredSize(DVDFileInfo* file)
{
	return file ? static_cast<s32>(file->cb.transferredSize) : 0;
}
s32 DVDGetDriveStatus(void) { return s_busy ? DVD_STATE_BUSY : DVD_STATE_END; }
BOOL DVDCheckDisk(void) { return s_initialized ? TRUE : FALSE; }
DVDDiskID* DVDGetCurrentDiskID(void) { return &s_disk_id; }

s32 DVDCancel(DVDCommandBlock* block)
{
	if (!block)
		return DVD_RESULT_FATAL_ERROR;
	for (unsigned i = 0; i < kMaxPending; ++i) {
		if (s_pending[i].active && s_pending[i].file &&
		    &s_pending[i].file->cb == block) {
			s_pending[i].result = DVD_RESULT_CANCELED;
			break;
		}
	}
	block->state = DVD_STATE_CANCELED;
	return DVD_RESULT_CANCELED;
}
BOOL DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback)
{
	s32 result = DVDCancel(block);
	if (callback)
		callback(result, block);
	return TRUE;
}
s32 DVDCancelAll(void)
{
	for (unsigned i = 0; i < kMaxPending; ++i)
		if (s_pending[i].active)
			s_pending[i].result = DVD_RESULT_CANCELED;
	return 1;
}
BOOL DVDCancelAllAsync(DVDCBCallback callback)
{
	DVDCancelAll();
	if (callback)
		callback(DVD_RESULT_CANCELED, nullptr);
	return TRUE;
}

BOOL DVDSeekAsyncPrio(DVDFileInfo* file, s32 offset, DVDCallback callback, s32)
{
	FileState* state = state_for(file, false);
	if (!state || offset < 0 || static_cast<u32>(offset) > s_entries[state->entry].size)
		return FALSE;
	file->cb.state = DVD_STATE_END;
	queue_callback(file, callback, 0);
	return TRUE;
}
s32 DVDSeekPrio(DVDFileInfo* file, s32 offset, s32)
{
	FileState* state = state_for(file, false);
	return state && offset >= 0 && static_cast<u32>(offset) <= s_entries[state->entry].size
	           ? offset
	           : DVD_RESULT_FATAL_ERROR;
}

BOOL DVDReadAbsAsyncPrio(DVDCommandBlock* block, void* address, s32 length,
                         s32 offset, DVDCBCallback callback, s32)
{
	if (!block || !address || length < 0)
		return FALSE;
	block->addr = address;
	block->length = static_cast<u32>(length);
	block->offset = static_cast<u32>(offset);
	block->state = DVD_STATE_END;
	if (callback)
		callback(DVD_RESULT_FATAL_ERROR, block);
	return FALSE;
}
BOOL DVDReadAbsAsyncForBS(DVDCommandBlock* block, void* address, s32 length,
                          s32 offset, DVDCBCallback callback)
{
	return DVDReadAbsAsyncPrio(block, address, length, offset, callback, 2);
}
BOOL DVDReadDiskID(DVDCommandBlock* block, DVDDiskID* disk, DVDCBCallback callback)
{
	if (!disk)
		return FALSE;
	std::memcpy(disk, &s_disk_id, sizeof(*disk));
	if (callback)
		callback(0, block);
	return TRUE;
}
BOOL DVDPrepareStreamAsync(DVDFileInfo*, u32, u32, DVDCallback callback)
{
	if (callback)
		callback(0, nullptr);
	return TRUE;
}
s32 DVDPrepareStream(DVDFileInfo*, u32, u32) { return 0; }

void DVDReset(void) {}
BOOL DVDResetRequired(void) { return FALSE; }
BOOL DVDSetAutoInvalidation(BOOL value) { return value; }
void DVDPause(void) {}
void DVDResume(void) {}
s32 DVDGetStreamErrorStatus(DVDCommandBlock*) { return 0; }
BOOL DVDGetStreamErrorStatusAsync(DVDCommandBlock* block, DVDCBCallback callback)
{
	if (callback)
		callback(0, block);
	return TRUE;
}
s32 DVDGetStreamPlayAddr(DVDCommandBlock*) { return 0; }
s32 DVDGetStreamStartAddr(DVDCommandBlock*) { return 0; }
s32 DVDGetStreamLength(DVDCommandBlock*) { return 0; }
s32 DVDCancelStream(DVDCommandBlock* block) { return DVDCancel(block); }
BOOL DVDCancelStreamAsync(DVDCommandBlock* block, DVDCBCallback callback)
{
	return DVDCancelAsync(block, callback);
}

void* DVDGetFSTLocation(void) { return s_fst_raw; }

} // extern "C"
