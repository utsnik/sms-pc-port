#include "port_os.h"
#include "runtime.h"

#include <dolphin/dvd.h>
#include <dolphin/os/OSCache.h>

#include <cctype>
#include <cstdio>
#include <cstring>

extern "C" void port_watch_check(const void* dst, uint32_t len, const char* what, const char* detail, uint32_t extra);
extern "C" void port_watch_verify(const char* where, const char* name);

namespace {

const uint32_t kMaxEntries = 512;
const uint32_t kMaxPath = 512;
const uint32_t kMaxFstBytes = 64u * 1024u;
const uint32_t kMaxFiles = 96;
const uint32_t kMaxPending = 96;

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
		append_text(source, sizeof(source), "/");
		append_text(source, sizeof(source), input);
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
		if (!std::strcmp(part, "."))
			continue;
		if (!std::strcmp(part, "..")) {
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
	/* FST paths are "/data/x.szs"; the extracted tree keeps them under files/. */
	int n = std::snprintf(out, cap, "%sfiles%s", sms_wiiu_app_root(),
	                      s_entries[entry].path);
	if (n < 0 || static_cast<uint32_t>(n) >= cap)
		return false;
	const char* root = sms_wiiu_app_root();
	uint32_t root_len = static_cast<uint32_t>(std::strlen(root));
	return std::strncmp(out, root, root_len) == 0 &&
	       std::strncmp(out + root_len, "files/", 6) == 0;
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
	if (!files_path(state->entry, path, sizeof(path))) {
		if (s_failures++ < 16)
			port_log("[dvd] read rejected: bad path for entry %u (%s)\n", state->entry, entry.path);
		return DVD_RESULT_FATAL_ERROR;
	}
	FILE* file = std::fopen(path, "rb");
	if (!file) {
		if (s_failures++ < 16)
			port_log("[dvd] read failed: cannot open %s\n", path);
		return DVD_RESULT_FATAL_ERROR;
	}
	if (std::fseek(file, offset, SEEK_SET) != 0) {
		std::fclose(file);
		return DVD_RESULT_FATAL_ERROR;
	}
	port_watch_check(address, static_cast<uint32_t>(length), "dvd", entry.path, static_cast<uint32_t>(offset));
	size_t got = std::fread(address, 1, want, file);
	port_watch_verify("dvd-read", entry.path);
	/* A GameCube DVD read is a DMA; callers may invalidate the buffer after it,
	 * which on the Wii U would drop this CPU copy. Write it back to memory. */
	if (got)
		DCFlushRange(address, got);
	std::fclose(file);
	if (got != want) {
		if (s_failures++ < 16)
			port_log("[dvd] short read %s: %u of %u\n", path, static_cast<unsigned>(got), static_cast<unsigned>(want));
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
	if (s_initialized)
		return;
	char fst_path[192];
	std::snprintf(fst_path, sizeof(fst_path), "%ssys/fst.bin", sms_wiiu_app_root());
	FILE* fst = std::fopen(fst_path, "rb");
	if (!fst || std::fread(s_fst_raw, 1, sizeof(s_fst_raw), fst) == 0) {
		if (fst)
			std::fclose(fst);
		port_log("[dvd] cannot open %s\n", fst_path);
		return;
	}
	long fst_size = std::ftell(fst);
	std::fclose(fst);
	if (fst_size < 12 || fst_size > static_cast<long>(sizeof(s_fst_raw))) {
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
	std::strncpy(s_entries[0].path, "/", kMaxPath - 1);
	for (uint32_t i = 0; i < s_entry_count; ++i) {
		const uint8_t* raw = s_fst_raw + i * 12;
		u32 name_offset = be32(raw) & 0x00ffffffu;
		if (i && name_offset >= string_bytes) {
			port_log("[dvd] FST name offset out of range at %u\n", i);
			return;
		}
		const char* name = i ? reinterpret_cast<const char*>(strings + name_offset) : "";
		if (i && !std::memchr(name, '\0', string_bytes - name_offset)) {
			port_log("[dvd] unterminated FST name at %u\n", i);
			return;
		}
		s_entries[i].directory = (raw[0] & 1) != 0;
		if (s_entries[i].directory) {
			s_entries[i].parent = be32(raw + 4);
			s_entries[i].next = be32(raw + 8);
			++s_directory_count;
		} else {
			s_entries[i].offset = be32(raw + 4);
			s_entries[i].size = be32(raw + 8);
			++s_file_count;
		}
		if (!i)
			continue;
		u32 parent = s_entries[i].directory ? s_entries[i].parent : parent_for(i);
		if (parent >= i || !s_entries[parent].path[0] ||
		    std::strlen(s_entries[parent].path) + std::strlen(name) + 2 >= kMaxPath) {
			port_log("[dvd] FST parent/path error at %u\n", i);
			return;
		}
		std::snprintf(s_entries[i].path, kMaxPath, "%s%s%s",
		              s_entries[parent].path,
		              std::strcmp(s_entries[parent].path, "/") ? "/" : "", name);
	}
	char boot_path[192];
	std::snprintf(boot_path, sizeof(boot_path), "%ssys/boot.bin", sms_wiiu_app_root());
	FILE* boot = std::fopen(boot_path, "rb");
	std::memset(&s_disk_id, 0, sizeof(s_disk_id));
	if (boot) {
		std::fread(&s_disk_id, 1, sizeof(s_disk_id), boot);
		std::fclose(boot);
	}
	s_current_dir[0] = '/';
	s_current_dir[1] = '\0';
	s_initialized = true;
	port_log("[dvd] %.4s: FST: %u entries, %u files, %u directories\n",
	         s_disk_id.gameName, s_entry_count, s_file_count, s_directory_count);
}

void DVDInit(void) { port_dvd_init(); }

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
	std::memset(file, 0, sizeof(*file));
	FileState* state = state_for(file, true);
	if (!state)
		return FALSE;
	state->entry = static_cast<u32>(entry);
	file->startAddr = s_entries[entry].offset;
	file->length = s_entries[entry].size;
	file->cb.state = DVD_STATE_END;
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
	FileState* state = state_for(file, false);
	if (!state)
		return FALSE;
	state->active = false;
	file->cb.state = DVD_STATE_END;
	return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* file, void* address, s32 length, s32 offset, s32)
{
	FileState* state = state_for(file, false);
	if (!state)
		return DVD_RESULT_FATAL_ERROR;
	s_busy = true;
	file->cb.state = DVD_STATE_BUSY;
	s32 result = read_file(state, address, length, offset);
	file->cb.state = result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
	file->cb.transferredSize = result > 0 ? static_cast<u32>(result) : 0;
	s_busy = false;
	return result;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* file, void* address, s32 length, s32 offset,
                      DVDCallback callback, s32)
{
	FileState* state = state_for(file, false);
	if (!state)
		return FALSE;
	file->cb.state = DVD_STATE_BUSY;
	s32 result = read_file(state, address, length, offset);
	file->cb.state = result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
	file->cb.transferredSize = result > 0 ? static_cast<u32>(result) : 0;
	queue_callback(file, callback, result);
	return TRUE;
}

void sms_wiiu_dvd_pump(void)
{
	for (unsigned i = 0; i < kMaxPending; ++i) {
		if (!s_pending[i].active)
			continue;
		Pending pending = s_pending[i];
		s_pending[i].active = false;
		if (pending.file)
			pending.file->cb.state = pending.result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
		if (pending.callback)
			pending.callback(pending.result, pending.file);
	}
	s_busy = false;
}

int port_dvd_busy(void) { return s_busy ? 1 : 0; }
const char* port_dvd_stage_path(void) { return s_last_stage_path; }

void* sms_wiiu_dvd_read_file(const char* path, size_t* size)
{
	if (size)
		*size = 0;
	int entry = find_entry(path);
	if (entry < 0 || s_entries[entry].directory)
		return nullptr;
	void* data = port_low_alloc(s_entries[entry].size);
	if (!data)
		return nullptr;
	DVDFileInfo file;
	if (!DVDFastOpen(entry, &file) ||
	    DVDReadPrio(&file, data, static_cast<s32>(s_entries[entry].size), 0, 2) < 0)
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
