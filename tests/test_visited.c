#include "tai/visited.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Linker wrapping fails one allocation without changing the set's API. */
static long fail_after = -1;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);

static bool should_fail(void) {
  if (fail_after < 0) return false;
  if (fail_after == 0) {
    fail_after = -1;
    return true;
  }
  --fail_after;
  return false;
}

void *__wrap_malloc(size_t size) {
  return should_fail() ? NULL : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size) {
  return should_fail() ? NULL : __real_calloc(count, size);
}

void *__wrap_realloc(void *ptr, size_t size) {
  return should_fail() ? NULL : __real_realloc(ptr, size);
}

static const char *const urls[] = {
    "http://127.0.0.1:8000/z.html",
    "http://127.0.0.1:8000/a.html#bottom",
    "http://127.0.0.1:8000/a.html",
    "data:text/html,<p>x</p>",
};
enum { URL_COUNT = sizeof(urls) / sizeof(urls[0]) };

static void exact_strings_are_members_and_duplicates_are_ignored(void) {
  TaiVisited *visited = tai_visited_create();
  assert(visited);
  assert(!tai_visited_contains(visited, urls[0]));
  for (size_t i = 0; i < URL_COUNT; i++)
    assert(tai_visited_add(visited, urls[i]));
  assert(tai_visited_add(visited, urls[1]));
  for (size_t i = 0; i < URL_COUNT; i++)
    assert(tai_visited_contains(visited, urls[i]));
  assert(!tai_visited_contains(visited, "http://127.0.0.1:8000/a.htm"));
  assert(!tai_visited_contains(visited, "http://127.0.0.1:8000/a.html#top"));
  assert(!tai_visited_contains(visited, NULL));
  assert(!tai_visited_add(visited, NULL));
  assert(!tai_visited_add(NULL, urls[0]));
  assert(!tai_visited_contains(NULL, urls[0]));
  tai_visited_destroy(visited);
  tai_visited_destroy(NULL);
}

static void copy_is_independent_of_the_source(void) {
  TaiVisited *visited = tai_visited_create();
  assert(visited);
  TaiVisited *empty = tai_visited_copy(visited);
  assert(empty && !tai_visited_contains(empty, urls[0]));
  tai_visited_destroy(empty);

  assert(tai_visited_add(visited, urls[0]));
  TaiVisited *copy = tai_visited_copy(visited);
  assert(copy);
  assert(tai_visited_add(visited, urls[1]));
  assert(tai_visited_add(copy, urls[2]));
  assert(tai_visited_contains(copy, urls[0]));
  assert(!tai_visited_contains(copy, urls[1]));
  assert(!tai_visited_contains(visited, urls[2]));
  tai_visited_destroy(visited);
  assert(tai_visited_contains(copy, urls[0]));
  tai_visited_destroy(copy);
  assert(!tai_visited_copy(NULL));
}

/* Many adds cross several growth steps of the sorted array. */
static void many_urls_stay_members(void) {
  TaiVisited *visited = tai_visited_create();
  assert(visited);
  char url[64];
  for (int i = 999; i >= 0; i--) {
    snprintf(url, sizeof(url), "http://127.0.0.1/%d", i);
    assert(tai_visited_add(visited, url));
  }
  for (int i = 0; i < 1000; i++) {
    snprintf(url, sizeof(url), "http://127.0.0.1/%d", i);
    assert(tai_visited_contains(visited, url));
  }
  assert(!tai_visited_contains(visited, "http://127.0.0.1/1000"));
  tai_visited_destroy(visited);
}

static void allocation_failures_leave_the_set_unchanged(void) {
  fail_after = 0;
  assert(!tai_visited_create());

  /* Each add fails at every allocation it makes; the set keeps the URLs it
   * had and accepts the same URL once memory is back. */
  bool add_failed = false;
  for (long step = 0;; step++) {
    TaiVisited *visited = tai_visited_create();
    assert(visited);
    for (size_t i = 0; i + 1 < URL_COUNT; i++)
      assert(tai_visited_add(visited, urls[i]));
    fail_after = step;
    bool added = tai_visited_add(visited, urls[URL_COUNT - 1]);
    bool injected = fail_after < 0;
    fail_after = -1;
    for (size_t i = 0; i + 1 < URL_COUNT; i++)
      assert(tai_visited_contains(visited, urls[i]));
    assert(tai_visited_contains(visited, urls[URL_COUNT - 1]) == added);
    if (!added) {
      assert(injected);
      add_failed = true;
      assert(tai_visited_add(visited, urls[URL_COUNT - 1]));
      assert(tai_visited_contains(visited, urls[URL_COUNT - 1]));
    }
    tai_visited_destroy(visited);
    if (!injected) break;
  }
  assert(add_failed);

  /* The adds above fit in the first allocation; a full set must grow. */
  TaiVisited *full = tai_visited_create();
  assert(full);
  char url[64];
  for (int i = 0; i < 8; i++) {
    snprintf(url, sizeof(url), "http://127.0.0.1/%d", i);
    assert(tai_visited_add(full, url));
  }
  fail_after = 0;
  assert(!tai_visited_add(full, "http://127.0.0.1/grow"));
  for (int i = 0; i < 8; i++) {
    snprintf(url, sizeof(url), "http://127.0.0.1/%d", i);
    assert(tai_visited_contains(full, url));
  }
  assert(!tai_visited_contains(full, "http://127.0.0.1/grow"));

  size_t copy_failures = 0;
  for (long step = 0;; step++) {
    fail_after = step;
    TaiVisited *copy = tai_visited_copy(full);
    bool injected = fail_after < 0;
    fail_after = -1;
    if (copy) {
      for (int i = 0; i < 8; i++) {
        snprintf(url, sizeof(url), "http://127.0.0.1/%d", i);
        assert(tai_visited_contains(copy, url));
      }
      tai_visited_destroy(copy);
    } else {
      assert(injected);
      copy_failures++;
    }
    if (!injected) break;
  }
  /* The struct, the array and each of the eight strings. */
  assert(copy_failures == 10);
  tai_visited_destroy(full);
}

int main(void) {
  exact_strings_are_members_and_duplicates_are_ignored();
  copy_is_independent_of_the_source();
  many_urls_stay_members();
  allocation_failures_leave_the_set_unchanged();
  puts("visited tests passed");
  return 0;
}
