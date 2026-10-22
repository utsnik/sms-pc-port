#include "dol_data.h"
#include "runtime.h"

#include "port/port.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace {

const size_t kArchiveCapacity = 1u << 20;
const size_t kTableCapacity = 1u << 14;
const uint32_t kMaxRarcNodes = 4096;
const uint32_t kMaxRarcEntries = 65536;
const uint32_t kMaxBcsvRows = 4096;
const uint32_t kMaxBcsvEntrySize = 1024;

struct Object {
    size_t offset;
    size_t size;
};

struct Rarc {
    const uint8_t* data;
    size_t size;
    size_t data_offset;
    size_t node_offset;
    size_t entry_offset;
    size_t string_offset;
    uint32_t node_count;
    uint32_t entry_count;
    uint32_t string_size;
};

bool s_load_called;

bool range_ok(size_t offset, size_t length, size_t limit)
{
    return offset <= limit && length <= limit - offset;
}

uint16_t be16(const uint8_t* data)
{
    return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

uint32_t be32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

int32_t be_s32(const uint8_t* data)
{
    return static_cast<int32_t>(be32(data));
}

bool string_at(const Rarc& rarc, uint32_t offset, const uint8_t** result,
               size_t* length)
{
    if (offset >= rarc.string_size)
        return false;
    const uint8_t* start = rarc.data + rarc.string_offset + offset;
    const size_t remaining = rarc.string_size - offset;
    const void* terminator = memchr(start, '\0', remaining);
    if (!terminator)
        return false;
    *result = start;
    *length = static_cast<const uint8_t*>(terminator) - start;
    return true;
}

bool path_is_error_window(const uint8_t* name, size_t length)
{
    static const char kPath[] = "layoutdata/errormessagewindow.arc";
    const size_t path_length = sizeof(kPath) - 1;
    const uint8_t* relative = static_cast<const uint8_t*>(memchr(name, '/', length));
    if (relative) {
        ++relative;
        length -= static_cast<size_t>(relative - name);
        name = relative;
    }
    if (length != path_length)
        return false;
    for (size_t i = 0; i < length; ++i) {
        char actual = static_cast<char>(name[i]);
        char expected = kPath[i];
        if (actual >= 'A' && actual <= 'Z')
            actual = static_cast<char>(actual - 'A' + 'a');
        if (expected >= 'A' && expected <= 'Z')
            expected = static_cast<char>(expected - 'A' + 'a');
        if (actual != expected)
            return false;
    }
    return true;
}

bool parse_rarc(const uint8_t* data, size_t size, Rarc* result)
{
    if (size < 0x40 || memcmp(data, "RARC", 4) != 0)
        return false;
    const uint32_t archive_size = be32(data + 4);
    if (archive_size < 0x40 || archive_size > size)
        return false;
    const size_t header = 0x20;
    if (!range_ok(header, 0x18, archive_size))
        return false;
    const uint32_t node_count = be32(data + header);
    const uint32_t node_relative = be32(data + header + 4);
    const uint32_t entry_count = be32(data + header + 8);
    const uint32_t entry_relative = be32(data + header + 12);
    const uint32_t string_size = be32(data + header + 16);
    const uint32_t string_relative = be32(data + header + 20);
    const uint32_t data_relative = be32(data + 0x0c);
    if (!node_count || node_count > kMaxRarcNodes || entry_count > kMaxRarcEntries ||
        !string_size)
        return false;
    if (node_relative > archive_size - header || entry_relative > archive_size - header ||
        string_relative > archive_size - header || data_relative > archive_size - header)
        return false;
    const size_t node_offset = header + node_relative;
    const size_t entry_offset = header + entry_relative;
    const size_t string_offset = header + string_relative;
    const size_t data_offset = header + data_relative;
    if (!range_ok(node_offset, static_cast<size_t>(node_count) * 0x10, archive_size) ||
        !range_ok(entry_offset, static_cast<size_t>(entry_count) * 0x14, archive_size) ||
        !range_ok(string_offset, string_size, archive_size) ||
        data_offset > archive_size)
        return false;
    result->data = data;
    result->size = archive_size;
    result->data_offset = data_offset;
    result->node_offset = node_offset;
    result->entry_offset = entry_offset;
    result->string_offset = string_offset;
    result->node_count = node_count;
    result->entry_count = entry_count;
    result->string_size = string_size;
    return true;
}

bool find_rarc_entry(const Rarc& rarc, uint32_t node_index, char* path,
                     size_t path_capacity, uint8_t* visited, Object* result)
{
    if (node_index >= rarc.node_count || visited[node_index])
        return false;
    visited[node_index] = 1;
    const uint8_t* node = rarc.data + rarc.node_offset + node_index * 0x10;
    const uint8_t* node_name;
    size_t node_name_length;
    if (!string_at(rarc, be32(node + 4), &node_name, &node_name_length))
        return false;
    size_t original_length = strlen(path);
    if (node_index == 0 && original_length == 0) {
        if (node_name_length + 2 > path_capacity)
            return false;
        memcpy(path, node_name, node_name_length);
        path[node_name_length] = '/';
        path[node_name_length + 1] = '\0';
        original_length = node_name_length + 1;
    }
    const uint32_t entry_count = be16(node + 10);
    const uint32_t first_entry = be32(node + 12);
    if (first_entry > rarc.entry_count || entry_count > rarc.entry_count - first_entry)
        return false;
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint8_t* entry = rarc.data + rarc.entry_offset +
                               (first_entry + i) * 0x14;
        const uint8_t* name;
        size_t name_length;
        if (!string_at(rarc, be16(entry + 6), &name, &name_length))
            return false;
        const uint16_t flags = be16(entry + 4);
        const uint32_t child_or_offset = be32(entry + 8);
        const uint32_t file_size = be32(entry + 12);
        if (flags & 0x0200) {
            if (name_length == 1 && name[0] == '.')
                continue;
            if (name_length == 2 && name[0] == '.' && name[1] == '.')
                continue;
            if (original_length + name_length + 2 > path_capacity)
                return false;
            memcpy(path + original_length, name, name_length);
            path[original_length + name_length] = '/';
            path[original_length + name_length + 1] = '\0';
            if (find_rarc_entry(rarc, child_or_offset, path, path_capacity, visited, result))
                return true;
            path[original_length] = '\0';
        } else {
            if (original_length + name_length >= path_capacity)
                return false;
            memcpy(path + original_length, name, name_length);
            path[original_length + name_length] = '\0';
            if (path_is_error_window(reinterpret_cast<const uint8_t*>(path),
                                     strlen(path))) {
                if (!range_ok(rarc.data_offset + child_or_offset, file_size,
                              rarc.size))
                    return false;
                result->offset = rarc.data_offset + child_or_offset;
                result->size = file_size;
                return true;
            }
            path[original_length] = '\0';
        }
    }
    return false;
}

bool find_error_archive(const uint8_t* dol, size_t dol_size, Object* result)
{
    for (size_t offset = 0; range_ok(offset, 4, dol_size); ++offset) {
        if (memcmp(dol + offset, "RARC", 4) != 0)
            continue;
        Rarc rarc;
        if (!parse_rarc(dol + offset, dol_size - offset, &rarc))
            continue;
        uint8_t visited[kMaxRarcNodes] = {};
        char path[512] = {};
        Object entry;
        if (find_rarc_entry(rarc, 0, path, sizeof(path), visited, &entry)) {
            result->offset = offset;
            result->size = rarc.size;
            return true;
        }
    }
    return false;
}

uint32_t jmap_hash(const char* name)
{
    uint32_t hash = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
        const int32_t signed_byte = *p >= 0x80 ? *p - 0x100 : *p;
        hash = static_cast<uint32_t>(signed_byte + hash * 31);
    }
    return hash;
}

bool field_size_valid(uint8_t type, uint16_t offset, uint32_t entry_size)
{
    if (offset >= entry_size)
        return false;
    switch (type) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 6:
        return offset <= entry_size - 4;
    case 4:
        return offset <= entry_size - 2;
    case 5:
    case 7:
        return true;
    default:
        return false;
    }
}

bool find_table(const uint8_t* dol, size_t dol_size, const char* const* wanted_names,
                uint32_t wanted_count, uint32_t field_count, Object* result)
{
    uint32_t wanted_hashes[3];
    for (uint32_t i = 0; i < wanted_count; ++i)
        wanted_hashes[i] = jmap_hash(wanted_names[i]);
    for (size_t hit = 0; range_ok(hit, 4, dol_size); ++hit) {
        if (be32(dol + hit) != wanted_hashes[0])
            continue;
        for (uint32_t column = 0; column < field_count; ++column) {
            if (hit < 16u + column * 12u)
                continue;
            const size_t position = hit - 16u - column * 12u;
            if (!range_ok(position, 16u + static_cast<size_t>(field_count) * 12u,
                          dol_size))
                continue;
            const int32_t rows = be_s32(dol + position);
            const int32_t fields = be_s32(dol + position + 4);
            const int32_t data_offset = be_s32(dol + position + 8);
            const uint32_t entry_size = be32(dol + position + 12);
            if (rows <= 0 || rows >= static_cast<int32_t>(kMaxBcsvRows) ||
                fields != static_cast<int32_t>(field_count) ||
                data_offset != 16 + fields * 12 || !entry_size ||
                entry_size > kMaxBcsvEntrySize)
                continue;
            if (position > dol_size || data_offset < 0 ||
                static_cast<size_t>(data_offset) > dol_size - position ||
                static_cast<size_t>(rows) >
                    (dol_size - position - static_cast<size_t>(data_offset)) / entry_size)
                continue;
            uint32_t found_hashes = 0;
            bool valid = true;
            uint16_t field_offsets[32];
            uint8_t field_types[32];
            for (uint32_t field = 0; field < field_count; ++field) {
                const uint8_t* descriptor = dol + position + 16 + field * 12;
                const uint32_t hash = be32(descriptor);
                const uint16_t offset = be16(descriptor + 8);
                const uint8_t type = descriptor[11];
                if (!field_size_valid(type, offset, entry_size)) {
                    valid = false;
                    break;
                }
                field_offsets[field] = offset;
                field_types[field] = type;
                for (uint32_t wanted = 0; wanted < wanted_count; ++wanted)
                    if (hash == wanted_hashes[wanted])
                        found_hashes |= 1u << wanted;
            }
            if (!valid || found_hashes != (1u << wanted_count) - 1u)
                continue;
            const size_t string_pool = position + static_cast<size_t>(data_offset) +
                                       static_cast<size_t>(rows) * entry_size;
            if (string_pool > dol_size)
                continue;
            size_t end = string_pool;
            for (int32_t row = 0; row < rows && valid; ++row) {
                const uint8_t* entry = dol + string_pool + static_cast<size_t>(row) * entry_size;
                for (uint32_t field = 0; field < field_count; ++field) {
                    if (field_types[field] != 6)
                        continue;
                    const uint32_t string_offset = be32(entry + field_offsets[field]);
                    if (string_offset >= dol_size - string_pool) {
                        valid = false;
                        break;
                    }
                    const uint8_t* string = dol + string_pool + string_offset;
                    const size_t remaining = dol_size - string_pool - string_offset;
                    const void* terminator = memchr(string, '\0', remaining);
                    if (!terminator) {
                        valid = false;
                        break;
                    }
                    const size_t string_end = static_cast<const uint8_t*>(terminator) - dol + 1;
                    if (string_end > end)
                        end = string_end;
                }
            }
            if (!valid)
                continue;
            const size_t aligned_end = position + ((end - position + 31u) & ~31u);
            if (aligned_end < end || aligned_end > dol_size ||
                position % 32u != 0)
                continue;
            result->offset = position;
            result->size = aligned_end - position;
            return true;
        }
    }
    return false;
}

void require_object(bool found, const char* name)
{
    if (!found)
        port_fatal("SMG Wii U: native-BE %s is missing from sys/main.dol", name);
}

void copy_object(const uint8_t* dol, const Object& object, unsigned char* target,
                 size_t capacity, const char* name)
{
    if (!object.size || object.size > capacity)
        port_fatal("SMG Wii U: native-BE %s size=%u exceeds target capacity=%u",
                   name, static_cast<unsigned>(object.size),
                   static_cast<unsigned>(capacity));
    memcpy(target, dol + object.offset, object.size);
}

} // namespace

extern "C" {

/* Native-BE writable target storage. The archive mount and JMap readers mutate
 * these buffers, so they must not be const or point into the DVD read buffer. */
alignas(32) unsigned char cErrorArchive[kArchiveCapacity];
alignas(32) unsigned char StoryEventBCSV[kTableCapacity];
alignas(32) unsigned char GalaxyIDBCSV[kTableCapacity];

void smg_wiiu_load_dol_data(void)
{
    if (s_load_called)
        return;
    s_load_called = true;

    port_dvd_init();
    size_t dol_size = 0;
    const uint8_t* dol = static_cast<const uint8_t*>(
        sms_wiiu_dvd_read_file("/sys/main.dol", &dol_size));
    if (!dol || !dol_size)
        port_fatal("SMG Wii U: cannot load native-BE sys/main.dol through DVD");

    static const char* kStoryNames[] = {"name", "progress"};
    static const char* kGalaxyNames[] = {"name", "MapPaneName", "PowerStarNum"};
    Object archive = {};
    Object story = {};
    Object galaxy = {};
    require_object(find_error_archive(dol, dol_size, &archive), "ErrorMessageArchive");
    require_object(find_table(dol, dol_size, kStoryNames, 2, 2, &story), "StoryEvent");
    require_object(find_table(dol, dol_size, kGalaxyNames, 3, 7, &galaxy), "GalaxyID");
    copy_object(dol, archive, cErrorArchive, sizeof(cErrorArchive), "ErrorMessageArchive");
    copy_object(dol, story, StoryEventBCSV, sizeof(StoryEventBCSV), "StoryEvent");
    copy_object(dol, galaxy, GalaxyIDBCSV, sizeof(GalaxyIDBCSV), "GalaxyID");
    port_log("SMG Wii U: native-BE main.dol ErrorMessageArchive offset=0x%x size=%u "
             "StoryEvent offset=0x%x size=%u GalaxyID offset=0x%x size=%u",
             static_cast<unsigned>(archive.offset), static_cast<unsigned>(archive.size),
             static_cast<unsigned>(story.offset), static_cast<unsigned>(story.size),
             static_cast<unsigned>(galaxy.offset), static_cast<unsigned>(galaxy.size));
}

} // extern "C"
