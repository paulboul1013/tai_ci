#ifndef TAI_CORE_H
#define TAI_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
/* Owned key/value strings. A map owns each entry; get returns a borrowed value. */
typedef struct { char *key; char *value; int priority; } TaiPair;
typedef struct { TaiPair *items; size_t count; size_t capacity; } TaiMap;
char *tai_strndup(const char *s, size_t n);
char *tai_strdup(const char *s);
bool tai_map_set(TaiMap *map, const char *key, const char *value, int priority);
const char *tai_map_get(const TaiMap *map, const char *key);
int tai_map_priority(const TaiMap *map, const char *key);
bool tai_map_copy(TaiMap *dst, const TaiMap *src);
void tai_map_clear(TaiMap *map);
char *tai_read_file(const char *path, size_t *length);
void tai_json_string(FILE *out, const char *s);
#endif
