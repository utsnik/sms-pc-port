#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <vector>

#include "dol_data.h"

namespace {

std::vector<unsigned char> s_dol;

bool load_file(const char* path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return false;
    const std::streamsize size = input.tellg();
    if (size <= 0)
        return false;
    s_dol.resize(static_cast<size_t>(size));
    input.seekg(0);
    return input.read(reinterpret_cast<char*>(s_dol.data()), size).good();
}

} // namespace

extern "C" {

void port_dvd_init(void) {}

void* sms_wiiu_dvd_read_file(const char* path, size_t* size)
{
    if (strcmp(path, "/sys/main.dol") != 0)
        return nullptr;
    *size = s_dol.size();
    return s_dol.data();
}

void port_log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
}

void port_fatal(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    fputs("dol-data-host: fatal: ", stdout);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    exit(2);
}

} // extern "C"

int main(int argc, char** argv)
{
    if (argc != 2 || !load_file(argv[1])) {
        fputs("dol-data-host: cannot read main.dol\n", stderr);
        return 2;
    }
    smg_wiiu_load_dol_data();
    return 0;
}
