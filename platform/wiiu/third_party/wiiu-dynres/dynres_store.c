#include "dynres_store.h"
#include <stdio.h>
#include <string.h>

void dynres_store_clear(dynres_store* s) { s->n = 0; }

static int valid_name(const char* name, size_t len) {
    if (len == 0 || len >= DYNRES_NAME_MAX) return 0;
    for (size_t i = 0; i < len; ++i)
        if (name[i] <= ' ' || name[i] == 0x7f) return 0;
    return 1;
}

static int find(const dynres_store* s, const char* name) {
    for (uint16_t i = 0; i < s->n; ++i)
        if (strcmp(s->e[i].name, name) == 0) return i;
    return -1;
}

int dynres_store_get(const dynres_store* s, const char* name) {
    const int i = find(s, name);
    return i < 0 ? 0 : s->e[i].lines;
}

int dynres_store_set(dynres_store* s, const char* name, int lines) {
    if (!name || !valid_name(name, strlen(name)) || lines <= 0 || lines > DYNRES_LINES_MAX) return -1;
    const int i = find(s, name);
    if (i >= 0) {
        if (s->e[i].lines == lines) return 0;
        s->e[i].lines = (uint16_t)lines;
        return 1;
    }
    if (s->n >= DYNRES_STORE_MAX) return -1;
    strcpy(s->e[s->n].name, name);
    s->e[s->n].lines = (uint16_t)lines;
    ++s->n;
    return 1;
}

int dynres_store_parse(dynres_store* s, const char* text, size_t len) {
    dynres_store_clear(s);
    size_t p = 0;
    while (p < len) {
        size_t end = p;
        while (end < len && text[end] != '\n') ++end;
        size_t a = p, b = end;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) --b;
        size_t n0 = a;
        while (n0 < b && text[n0] != ' ' && text[n0] != '\t') ++n0;
        size_t v = n0;
        while (v < b && (text[v] == ' ' || text[v] == '\t')) ++v;
        int lines = 0, ok = v < b;
        for (size_t k = v; k < b && ok; ++k) {
            if (text[k] < '0' || text[k] > '9') ok = 0;
            else if ((lines = lines * 10 + (text[k] - '0')) > DYNRES_LINES_MAX) ok = 0;
        }
        if (ok && valid_name(text + a, n0 - a)) {
            char name[DYNRES_NAME_MAX];
            memcpy(name, text + a, n0 - a);
            name[n0 - a] = 0;
            dynres_store_set(s, name, lines);
        }
        p = end + 1;
    }
    return s->n;
}

int dynres_store_load(dynres_store* s, const char* path) {
    static char buf[DYNRES_STORE_MAX * (DYNRES_NAME_MAX + 8)];
    dynres_store_clear(s);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    const size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return dynres_store_parse(s, buf, len);
}

int dynres_store_save(const dynres_store* s, const char* path) {
    char tmp[512];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = 1;
    for (uint16_t i = 0; i < s->n && ok; ++i)
        ok = fprintf(f, "%s %u\n", s->e[i].name, (unsigned)s->e[i].lines) > 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) {
        /* Some devoptabs refuse to rename over an existing file. */
        remove(path);
        if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    }
    return 0;
}
