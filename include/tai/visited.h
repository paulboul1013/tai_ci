#ifndef TAI_VISITED_H
#define TAI_VISITED_H

#include <stdbool.h>

/* A set of visited URLs: exact serialized URL strings, in memory only. The
 * set owns its strings. It has no lock: one thread uses a given set, and
 * another thread gets its own copy. */
typedef struct TaiVisited TaiVisited;

TaiVisited *tai_visited_create(void);
void tai_visited_destroy(TaiVisited *visited);

/* Adding a URL that is already present succeeds and changes nothing. On NULL
 * input or allocation failure, returns false and leaves the set unchanged. */
bool tai_visited_add(TaiVisited *visited, const char *url);
bool tai_visited_contains(const TaiVisited *visited, const char *url);

/* An independent owned copy, or NULL on allocation failure. */
TaiVisited *tai_visited_copy(const TaiVisited *visited);

#endif
