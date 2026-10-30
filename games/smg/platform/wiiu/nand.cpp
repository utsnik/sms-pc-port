#include "nand.h"

#include <revolution/nand.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "runtime.h"

extern "C" void port_log(const char* fmt, ...) __attribute__((weak));
static unsigned s_nlog;
static s32 nlog(const char* fn, const char* path, s32 r)
{
    if (port_log && s_nlog < 120) { ++s_nlog; port_log("[smg-nand] %s %s -> %d", fn, path ? path : "-", (int)r); }
    return r;
}

namespace {

constexpr int kMaxFiles = 32;
constexpr size_t kMaxHostPath = 256;
FILE* s_files[kMaxFiles];
char s_paths[kMaxFiles][NAND_MAX_PATH];
bool s_path_escape_logged;

bool valid_path(const char* path)
{
    if (path == nullptr || path[0] == '\0')
        return false;

    const char* p = path;
    while (*p != '\0') {
        while (*p == '/')
            ++p;
        const char* begin = p;
        while (*p != '\0' && *p != '/')
            ++p;
        const size_t length = static_cast<size_t>(p - begin);
        if (length == 0)
            continue;
        if ((length == 1 && begin[0] == '.') ||
            (length == 2 && begin[0] == '.' && begin[1] == '.') ||
            memchr(begin, '\\', length) != nullptr)
            return false;
    }
    return true;
}

bool within_root(const char* root, const char* path)
{
    const size_t root_length = strlen(root);
    return strncmp(path, root, root_length) == 0 &&
        (path[root_length] == '\0' || path[root_length] == '/');
}

bool path_stays_in_root(const char* path)
{
    // realpath() may use its whole PATH_MAX (4096 on devkitPPC) output buffer;
    // 256-byte buffers overran into the caller's frame on the NAND thread.
    char root[PATH_MAX];
    if (realpath(smg_wiiu_save_root(), root) == nullptr) {
        /* No realpath() on this device (the Wii U SD devoptab): make_path()
         * built the path from the root plus components valid_path() already
         * cleared of ".", ".." and "\\", so a lexical check is exact. */
        return within_root(smg_wiiu_save_root(), path);
    }

    char resolved[PATH_MAX];
    if (realpath(path, resolved) != nullptr)
        return within_root(root, resolved);

    char prefix[kMaxHostPath];
    const size_t length = strlen(path);
    if (length >= sizeof(prefix))
        return false;
    memcpy(prefix, path, length + 1);
    while (realpath(prefix, resolved) == nullptr) {
        char* separator = strrchr(prefix, '/');
        if (separator == nullptr || separator == prefix)
            break;
        *separator = '\0';
    }
    // The remaining check is intentionally a small TOCTOU window; the save SD card is single-user.
    if (realpath(prefix, resolved) == nullptr)
        return within_root(smg_wiiu_save_root(), path);  // nothing resolves: lexical, as above
    return within_root(root, resolved);
}

bool reject_path_escape(const char* path)
{
    if (path_stays_in_root(path))
        return false;
    if (!s_path_escape_logged) {
        fprintf(stderr, "SMG Wii U NAND: refusing path outside save root\n");
        if (port_log) port_log("[smg-nand] refusing path outside save root: %s", path);
        s_path_escape_logged = true;
    }
    return true;
}

bool make_path(char* out, size_t capacity, const char* path)
{
    if (!valid_path(path))
        return false;

    const char* relative = path;
    while (*relative == '/')
        ++relative;

    const bool is_home = path[0] == '/' &&
        (strncmp(path, SMG_WIIU_NAND_HOME, strlen(SMG_WIIU_NAND_HOME)) == 0) &&
        (path[strlen(SMG_WIIU_NAND_HOME)] == '\0' ||
         path[strlen(SMG_WIIU_NAND_HOME)] == '/');
    const char* root = smg_wiiu_save_root();
    if (root == nullptr || root[0] == '\0')
        return false;
    // Establish the canonical root before realpath() validates the mapped path.
    mkdir(root, 0770);

    int written;
    if (path[0] == '/' && !is_home) {
        written = snprintf(out, capacity, "%s/%s", root, relative);
    } else if (is_home) {
        const char* home_relative = path + strlen(SMG_WIIU_NAND_HOME);
        while (*home_relative == '/')
            ++home_relative;
        written = snprintf(out, capacity, "%s%s%s%s", root, SMG_WIIU_NAND_HOME,
                           *home_relative ? "/" : "", home_relative);
    } else {
        written = snprintf(out, capacity, "%s%s/%s", root, SMG_WIIU_NAND_HOME,
                           relative);
    }
    return written > 0 && static_cast<size_t>(written) < capacity &&
        !reject_path_escape(out);
}

void make_parent_dirs(const char* path)
{
    const char* root = smg_wiiu_save_root();
    if (root == nullptr || root[0] == '\0')
        return;

    const size_t root_length = strlen(root);
    mkdir(root, 0770);

    char buffer[kMaxHostPath];
    const size_t length = strlen(path);
    if (length >= sizeof(buffer))
        return;
    memcpy(buffer, path, length + 1);
    for (size_t i = root_length; i < length; ++i) {
        if (buffer[i] == '/') {
            buffer[i] = '\0';
            mkdir(buffer, 0770);
            buffer[i] = '/';
        }
    }
}

int reserve(FILE* file, const char* path)
{
    for (int i = 0; i < kMaxFiles; ++i) {
        if (s_files[i] == nullptr) {
            s_files[i] = file;
            strncpy(s_paths[i], path, sizeof(s_paths[i]) - 1);
            s_paths[i][sizeof(s_paths[i]) - 1] = '\0';
            return i;
        }
    }
    fclose(file);
    return -1;
}

FILE* file_for(const NANDFileInfo* info)
{
    if (info == nullptr || info->fileDescriptor < 0 ||
        info->fileDescriptor >= kMaxFiles)
        return nullptr;
    return s_files[info->fileDescriptor];
}

s32 errno_result(void)
{
    switch (errno) {
    case ENOENT: return NAND_RESULT_NOEXISTS;
    case EEXIST: return NAND_RESULT_EXISTS;
    case EACCES:
    case EPERM: return NAND_RESULT_ACCESS;
    case ENOSPC: return NAND_RESULT_MAXBLOCKS;
    case ENOTEMPTY: return NAND_RESULT_NOTEMPTY;
    default: return NAND_RESULT_UNKNOWN;
    }
}

void finish(NANDCallback callback, s32 result, NANDCommandBlock* block)
{
    if (callback != nullptr)
        callback(result, block);
}

s32 open_file(const char* path, NANDFileInfo* info, u8 mode)
{
    char full[kMaxHostPath];
    if (info == nullptr || !make_path(full, sizeof(full), path))
        return NAND_RESULT_INVALID;

    const char* mode_string;
    switch (mode) {
    case NAND_ACCESS_READ:
        mode_string = "rb";
        break;
    case NAND_ACCESS_WRITE:
    case NAND_ACCESS_RW:
        mode_string = "rb+";
        break;
    default:
        return NAND_RESULT_INVALID;
    }

    FILE* file = fopen(full, mode_string);
    if (file == nullptr && (mode == NAND_ACCESS_WRITE || mode == NAND_ACCESS_RW) &&
        errno == ENOENT) {
        make_parent_dirs(full);
        file = fopen(full, "wb+");
    }
    if (file == nullptr)
        return errno_result();

    const int descriptor = reserve(file, full);
    if (descriptor < 0)
        return NAND_RESULT_MAXFD;
    memset(info, 0, sizeof(*info));
    info->fileDescriptor = descriptor;
    info->origFd = descriptor;
    info->accType = mode;
    strncpy(info->origPath, path, sizeof(info->origPath) - 1);
    info->origPath[sizeof(info->origPath) - 1] = '\0';
    return NAND_RESULT_OK;
}

} // namespace

extern "C" {

s32 NANDInit(void) { return NAND_RESULT_OK; }
BOOL nandIsInitialized(void) { return TRUE; }

s32 NANDGetHomeDir(char path[NAND_MAX_PATH])
{
    if (path == nullptr)
        return NAND_RESULT_INVALID;
    strncpy(path, SMG_WIIU_NAND_HOME, NAND_MAX_PATH - 1);
    path[NAND_MAX_PATH - 1] = '\0';
    return NAND_RESULT_OK;
}

static s32 NANDCreate_impl(const char* path, u8, u8)
{
    char full[kMaxHostPath];
    if (!make_path(full, sizeof(full), path))
        return NAND_RESULT_INVALID;
    make_parent_dirs(full);
    FILE* existing = fopen(full, "rb");
    if (existing != nullptr) {
        fclose(existing);
        return NAND_RESULT_EXISTS;
    }
    FILE* file = fopen(full, "wb");
    if (file == nullptr)
        return errno_result();
    fclose(file);
    return NAND_RESULT_OK;
}

s32 NANDPrivateCreate(const char* path, u8 perm, u8 attr)
{ return NANDCreate(path, perm, attr); }
static s32 NANDOpen_impl(const char* path, NANDFileInfo* info, u8 mode)
{ return open_file(path, info, mode); }
s32 NANDPrivateOpen(const char* path, NANDFileInfo* info, u8 mode)
{ return open_file(path, info, mode); }

static s32 NANDClose_impl(NANDFileInfo* info)
{
    FILE* file = file_for(info);
    if (file == nullptr)
        return NAND_RESULT_INVALID;
    const int descriptor = info->fileDescriptor;
    if (fflush(file) != 0) {
        fclose(file);
        s_files[descriptor] = nullptr;
        return NAND_RESULT_UNKNOWN;
    }
    const int result = fclose(file);
    s_files[descriptor] = nullptr;
    s_paths[descriptor][0] = '\0';
    info->fileDescriptor = -1;
    return result == 0 ? NAND_RESULT_OK : NAND_RESULT_UNKNOWN;
}

static s32 NANDRead_impl(NANDFileInfo* info, void* buffer, u32 length)
{
    FILE* file = file_for(info);
    if (file == nullptr || (buffer == nullptr && length != 0))
        return NAND_RESULT_INVALID;
    const size_t count = fread(buffer, 1, length, file);
    if (count == 0 && ferror(file))
        return NAND_RESULT_UNKNOWN;
    return static_cast<s32>(count);
}

static s32 NANDWrite_impl(NANDFileInfo* info, const void* buffer, u32 length)
{
    FILE* file = file_for(info);
    if (file == nullptr || (buffer == nullptr && length != 0))
        return NAND_RESULT_INVALID;
    const size_t count = fwrite(buffer, 1, length, file);
    return count == length ? static_cast<s32>(count) : NAND_RESULT_UNKNOWN;
}

s32 NANDSeek(NANDFileInfo* info, s32 offset, s32 whence)
{
    FILE* file = file_for(info);
    if (file == nullptr || fseek(file, offset, whence) != 0)
        return NAND_RESULT_UNKNOWN;
    const long position = ftell(file);
    return position < 0 ? NAND_RESULT_UNKNOWN : static_cast<s32>(position);
}

static s32 NANDGetLength_impl(NANDFileInfo* info, u32* length)
{
    FILE* file = file_for(info);
    if (file == nullptr || length == nullptr)
        return NAND_RESULT_INVALID;
    const long saved = ftell(file);
    if (saved < 0 || fseek(file, 0, SEEK_END) != 0)
        return NAND_RESULT_UNKNOWN;
    const long end = ftell(file);
    if (fseek(file, saved, SEEK_SET) != 0 || end < 0)
        return NAND_RESULT_UNKNOWN;
    *length = static_cast<u32>(end);
    return NAND_RESULT_OK;
}

static s32 NANDDelete_impl(const char* path)
{
    char full[kMaxHostPath];
    if (!make_path(full, sizeof(full), path))
        return NAND_RESULT_INVALID;
    return remove(full) == 0 ? NAND_RESULT_OK : errno_result();
}

static s32 NANDMove_impl(const char* path, const char* destination)
{
    char source[kMaxHostPath];
    char directory[kMaxHostPath];
    char target[kMaxHostPath];
    if (!make_path(source, sizeof(source), path) ||
        !make_path(directory, sizeof(directory), destination))
        return NAND_RESULT_INVALID;
    const char* name = strrchr(source, '/');
    if (name == nullptr || snprintf(target, sizeof(target), "%s/%s", directory, name + 1) >=
        static_cast<int>(sizeof(target)))
        return NAND_RESULT_INVALID;
    make_parent_dirs(target);
    return rename(source, target) == 0 ? NAND_RESULT_OK : errno_result();
}

static s32 NANDCheck_impl(u32, u32, u32* answer)
{
    if (answer != nullptr)
        *answer = 0;
    return NAND_RESULT_OK;
}

static s32 async_open(const char* path, NANDFileInfo* info, u8 mode,
                      NANDCallback callback, NANDCommandBlock* block)
{
    const s32 result = open_file(path, info, mode);
    finish(callback, result, block);
    return result;
}

s32 NANDOpenAsync(const char* path, NANDFileInfo* info, u8 mode,
                  NANDCallback callback, NANDCommandBlock* block)
{ return async_open(path, info, mode, callback, block); }
s32 NANDPrivateOpenAsync(const char* path, NANDFileInfo* info, const u8 mode,
                         NANDCallback callback, NANDCommandBlock* block)
{ return async_open(path, info, mode, callback, block); }
s32 NANDCloseAsync(NANDFileInfo* info, NANDCallback callback, NANDCommandBlock* block)
{ const s32 result = NANDClose(info); finish(callback, result, block); return result; }
s32 NANDReadAsync(NANDFileInfo* info, void* buffer, u32 length,
                  NANDCallback callback, NANDCommandBlock* block)
{ const s32 result = NANDRead(info, buffer, length); finish(callback, result, block); return result; }
s32 NANDWriteAsync(NANDFileInfo* info, const void* buffer, u32 length,
                   NANDCallback callback, NANDCommandBlock* block)
{ const s32 result = NANDWrite(info, buffer, length); finish(callback, result, block); return result; }
s32 NANDSeekAsync(NANDFileInfo* info, s32 offset, s32 whence,
                  NANDCallback callback, NANDCommandBlock* block)
{ const s32 result = NANDSeek(info, offset, whence); finish(callback, result, block); return result; }

s32 NANDPrivateDelete(const char* path) { return NANDDelete(path); }
// Status is a smoke-test stub for now; it intentionally does not validate the path.
s32 NANDPrivateGetStatus(const char*, NANDStatus* status)
{ if (status != nullptr) memset(status, 0, sizeof(*status)); return NAND_RESULT_OK; }
s32 NANDGetStatus(const char* path, NANDStatus* status)
{ return NANDPrivateGetStatus(path, status); }
s32 NANDSecretGetUsage(const char*, u32* blocks, u32* inodes)
{ if (blocks != nullptr) *blocks = 0; if (inodes != nullptr) *inodes = 0; return NAND_RESULT_OK; }
s32 NANDPrivateGetStatusAsync(const char* path, NANDStatus* status,
                              NANDCallback callback, NANDCommandBlock* block)
{ const s32 result = NANDPrivateGetStatus(path, status); finish(callback, result, block); return result; }
s32 NANDPrivateDeleteAsync(const char* path, NANDCallback callback, NANDCommandBlock* block)
{ const s32 result = NANDPrivateDelete(path); finish(callback, result, block); return result; }
s32 NANDPrivateGetTypeAsync(const char*, u8* type, NANDCallback callback, NANDCommandBlock* block)
{ if (type != nullptr) *type = 0; finish(callback, NAND_RESULT_OK, block); return NAND_RESULT_OK; }
s32 NANDPrivateCreateDirAsync(const char* path, u8 perm, u8 attr,
                              NANDCallback callback, NANDCommandBlock* block)
{ const s32 result = NANDCreate(path, perm, attr); finish(callback, result, block); return result; }
s32 NANDPrivateCreateAsync(const char* path, u8 perm, u8 attr,
                           NANDCallback callback, NANDCommandBlock* block)
{ const s32 result = NANDCreate(path, perm, attr); finish(callback, result, block); return result; }

s32 NANDCreate(const char* path, u8 a, u8 b)
{ return nlog("NANDCreate", path, NANDCreate_impl(path, a, b)); }
s32 NANDOpen(const char* path, NANDFileInfo* info, u8 mode)
{ return nlog("NANDOpen", path, NANDOpen_impl(path, info, mode)); }
s32 NANDClose(NANDFileInfo* info)
{ return nlog("NANDClose", nullptr, NANDClose_impl(info)); }
s32 NANDRead(NANDFileInfo* info, void* buffer, u32 length)
{ return nlog("NANDRead", nullptr, NANDRead_impl(info, buffer, length)); }
s32 NANDWrite(NANDFileInfo* info, const void* buffer, u32 length)
{ return nlog("NANDWrite", nullptr, NANDWrite_impl(info, buffer, length)); }
s32 NANDGetLength(NANDFileInfo* info, u32* length)
{ return nlog("NANDGetLength", nullptr, NANDGetLength_impl(info, length)); }
s32 NANDDelete(const char* path)
{ return nlog("NANDDelete", path, NANDDelete_impl(path)); }
s32 NANDMove(const char* path, const char* destination)
{ return nlog("NANDMove", path, NANDMove_impl(path, destination)); }
s32 NANDCheck(u32 a, u32 b, u32* answer)
{ return nlog("NANDCheck", nullptr, NANDCheck_impl(a, b, answer)); }
s32 nandConvertErrorCode(const ISFSError) { return NAND_RESULT_UNKNOWN; }
BOOL nandIsPrivatePath(const char*) { return FALSE; }
void nandGenerateAbsPath(char* out, const char* path)
{ if (out != nullptr) snprintf(out, NAND_MAX_PATH, "%s", path != nullptr ? path : ""); }
void nandGetParentDirectory(char* out, const char* path)
{ if (out != nullptr) snprintf(out, NAND_MAX_PATH, "%s", path != nullptr ? path : ""); }
void nandGetRelativeName(char* out, const char* path)
{ if (out != nullptr) snprintf(out, NAND_MAX_PATH, "%s", path != nullptr ? path : ""); }
const char* nandGetHomeDir(void) { return SMG_WIIU_NAND_HOME; }
void nandCallback(ISFSError, void*) {}

void NANDInitBanner(NANDBanner* banner, u32 flag, const u16* title, const u16* comment)
{
    if (banner == nullptr)
        return;
    memset(banner, 0, sizeof(*banner));
    banner->signature = NAND_BANNER_SIGNATURE;
    banner->flag = flag;
    for (int i = 0; title != nullptr && i < NAND_BANNER_COMMENT_SIZE && title[i]; ++i)
        banner->comment[0][i] = title[i];
    for (int i = 0; comment != nullptr && i < NAND_BANNER_COMMENT_SIZE && comment[i]; ++i)
        banner->comment[1][i] = comment[i];
}

} // extern "C"
