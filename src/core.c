#include "tai/core.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

char *tai_strndup(const char *s, size_t n) {
    if (!s || n == SIZE_MAX) return NULL;
    char *copy = malloc(n + 1);
    if (copy) { memcpy(copy, s, n); copy[n] = '\0'; }
    return copy;
}
char *tai_strdup(const char *s) { return s ? tai_strndup(s, strlen(s)) : NULL; }

static size_t map_index(const TaiMap *map, const char *key) {
    for (size_t i = 0; i < map->count; i++)
        if (!strcmp(map->items[i].key, key)) return i;
    return map->count;
}
bool tai_map_set(TaiMap *map, const char *key, const char *value, int priority) {
    if (!map || !key || !value) return false;
    size_t i = map_index(map, key);
    char *v = tai_strdup(value);
    if (!v) return false;
    if (i < map->count) {
        free(map->items[i].value);
        map->items[i].value = v;
        map->items[i].priority = priority;
        return true;
    }
    char *k = tai_strdup(key);
    if (!k) { free(v); return false; }
    if (map->count == map->capacity) {
        size_t cap = map->capacity ? map->capacity * 2 : 8;
        if (cap < map->capacity || cap > SIZE_MAX / sizeof(TaiPair)) {
            free(k); free(v); return false;
        }
        TaiPair *items = realloc(map->items, cap * sizeof(*items));
        if (!items) { free(k); free(v); return false; }
        map->items = items;
        map->capacity = cap;
    }
    map->items[map->count++] = (TaiPair){k, v, priority};
    return true;
}
const char *tai_map_get(const TaiMap *map, const char *key) {
    size_t i = map_index(map, key);
    return i < map->count ? map->items[i].value : NULL;
}
int tai_map_priority(const TaiMap *map, const char *key) {
    size_t i = map_index(map, key);
    return i < map->count ? map->items[i].priority : -1;
}
void tai_map_clear(TaiMap *map) {
    if (!map) return;
    for (size_t i = 0; i < map->count; i++) {
        free(map->items[i].key); free(map->items[i].value);
    }
    free(map->items); *map = (TaiMap){0};
}
bool tai_map_copy(TaiMap *dst, const TaiMap *src) {
    if (dst == src) return true;
    TaiMap copy = {0};
    for (size_t i = 0; i < src->count; i++) {
        const TaiPair *p = &src->items[i];
        if (!tai_map_set(&copy, p->key, p->value, p->priority)) {
            tai_map_clear(&copy); return false;
        }
    }
    tai_map_clear(dst); *dst = copy; return true;
}
char *tai_read_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *data = malloc(cap);
    if (!data) { fclose(f); return NULL; }
    for (;;) {
        size_t n = fread(data + len, 1, cap - len - 1, f);
        len += n;
        if (ferror(f)) { free(data); fclose(f); return NULL; }
        if (feof(f)) break;
        if (len == cap - 1) {
            if (cap > SIZE_MAX / 2) { free(data); fclose(f); return NULL; }
            cap *= 2;
            char *next = realloc(data, cap);
            if (!next) { free(data); fclose(f); return NULL; }
            data = next;
        }
    }
    if (fclose(f)) { free(data); return NULL; }
    data[len] = '\0';
    if (length) *length = len;
    return data;
}
void tai_json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', out); fputc(*p, out); }
        else if (*p < 32) fprintf(out, "\\u%04x", (unsigned int)*p);
        else fputc(*p, out);
    }
    fputc('"', out);
}
