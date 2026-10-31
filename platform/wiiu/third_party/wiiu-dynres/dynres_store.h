/* wiiu-dynres memory file: one "stage lines" pair per line.  A missing or
 * malformed file (or line) means "no memory"; saving writes <path>.tmp and
 * renames it, so a power-off mid-write keeps the old file. */
#ifndef WIIU_DYNRES_STORE_H
#define WIIU_DYNRES_STORE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DYNRES_NAME_MAX 32   /* including the terminator: 31 characters */
#define DYNRES_STORE_MAX 256
#define DYNRES_LINES_MAX 4096

typedef struct { char name[DYNRES_NAME_MAX]; uint16_t lines; } dynres_entry;
typedef struct { dynres_entry e[DYNRES_STORE_MAX]; uint16_t n; } dynres_store;

void dynres_store_clear(dynres_store* s);
int dynres_store_parse(dynres_store* s, const char* text, size_t len);
int dynres_store_load(dynres_store* s, const char* path);
int dynres_store_get(const dynres_store* s, const char* name);
int dynres_store_set(dynres_store* s, const char* name, int lines);
int dynres_store_save(const dynres_store* s, const char* path);

#ifdef __cplusplus
}
#endif
#endif
