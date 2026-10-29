#include "tai/visited.h"

#include "tai/core.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Sorted, so contains is a binary search and a copy keeps the order. */
struct TaiVisited {
    char **urls;
    size_t count;
    size_t capacity;
};

static size_t lower_bound(const TaiVisited *visited, const char *url) {
    size_t first = 0;
    size_t count = visited->count;
    while (count) {
        size_t step = count / 2;
        size_t middle = first + step;
        if (strcmp(visited->urls[middle], url) < 0) {
            first = middle + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

TaiVisited *tai_visited_create(void) {
    return calloc(1, sizeof(TaiVisited));
}

void tai_visited_destroy(TaiVisited *visited) {
    if (!visited) return;
    for (size_t index = 0; index < visited->count; index++)
        free(visited->urls[index]);
    free(visited->urls);
    free(visited);
}

bool tai_visited_contains(const TaiVisited *visited, const char *url) {
    if (!visited || !url) return false;
    size_t index = lower_bound(visited, url);
    return index < visited->count && !strcmp(visited->urls[index], url);
}

bool tai_visited_add(TaiVisited *visited, const char *url) {
    if (!visited || !url) return false;
    size_t index = lower_bound(visited, url);
    if (index < visited->count && !strcmp(visited->urls[index], url))
        return true;
    if (visited->count == visited->capacity) {
        size_t maximum = SIZE_MAX / sizeof(*visited->urls);
        if (visited->capacity == maximum) return false;
        size_t next = visited->capacity < maximum / 2
            ? visited->capacity * 2 : maximum;
        if (next < 8) next = 8;
        char **grown = realloc(visited->urls, next * sizeof(*grown));
        if (!grown) return false;
        visited->urls = grown;
        visited->capacity = next;
    }
    char *copy = tai_strdup(url);
    if (!copy) return false;
    memmove(visited->urls + index + 1, visited->urls + index,
            (visited->count - index) * sizeof(*visited->urls));
    visited->urls[index] = copy;
    visited->count++;
    return true;
}

TaiVisited *tai_visited_copy(const TaiVisited *visited) {
    if (!visited) return NULL;
    TaiVisited *copy = tai_visited_create();
    if (!copy) return NULL;
    if (!visited->count) return copy;
    copy->urls = malloc(visited->count * sizeof(*copy->urls));
    if (!copy->urls) {
        tai_visited_destroy(copy);
        return NULL;
    }
    copy->capacity = visited->count;
    for (; copy->count < visited->count; copy->count++) {
        copy->urls[copy->count] = tai_strdup(visited->urls[copy->count]);
        if (!copy->urls[copy->count]) {
            tai_visited_destroy(copy);
            return NULL;
        }
    }
    return copy;
}
