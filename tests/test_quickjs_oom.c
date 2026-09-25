/* QuickJS-NG context construction under allocation failure.
 *
 * Fails each allocation made by JS_NewContext() in turn, then frees the
 * runtime. Freed blocks are poisoned and kept until the runtime is gone, so a
 * GC walk over a released object reads the poison pointer and faults even
 * without a sanitizer. Guards patches/quickjs/0001. */
#include "quickjs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d CHECK(%s) point=%ld\n", \
  __FILE__, __LINE__, #c, point); return 1; } } while (0)

typedef struct {
  size_t size;
  size_t pad;
} Header;

typedef struct Parked {
  struct Parked *next;
} Parked;

typedef struct {
  long budget; /* <0 disarmed; allocations left before failures begin */
  bool injected;
  Parked *parked;
} Allocator;

static long point;

static Header *header_of(const void *ptr) {
  return (Header *)((char *)(uintptr_t)ptr - sizeof(Header));
}

static bool should_fail(Allocator *state) {
  if (state->budget < 0) return false;
  if (state->budget == 0) {
    state->injected = true;
    return true;
  }
  state->budget--;
  return false;
}

static void *raw_alloc(size_t size, bool zero) {
  if (size > SIZE_MAX - sizeof(Header)) return NULL;
  Header *header = zero ? calloc(1, sizeof(Header) + size)
                        : malloc(sizeof(Header) + size);
  if (!header) return NULL;
  header->size = size;
  return header + 1;
}

static void *test_malloc(void *opaque, size_t size) {
  return should_fail(opaque) ? NULL : raw_alloc(size, false);
}

static void *test_calloc(void *opaque, size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return NULL;
  return should_fail(opaque) ? NULL : raw_alloc(count * size, true);
}

static void test_free(void *opaque, void *ptr) {
  Allocator *state = opaque;
  if (!ptr) return;
  Header *header = header_of(ptr);
  memset(ptr, 0xa5, header->size);
  if (header->size >= sizeof(Parked)) {
    /* Park the block so later reads see poison instead of reused memory. */
    Parked *parked = (Parked *)(void *)header;
    parked->next = state->parked;
    state->parked = parked;
  } else {
    free(header);
  }
}

static void *test_realloc(void *opaque, void *ptr, size_t size) {
  if (!ptr) return test_malloc(opaque, size);
  if (should_fail(opaque)) return NULL;
  void *next = raw_alloc(size, false);
  if (!next) return NULL;
  size_t old = header_of(ptr)->size;
  memcpy(next, ptr, old < size ? old : size);
  test_free(opaque, ptr);
  return next;
}

static size_t test_usable_size(const void *ptr) {
  return ptr ? header_of(ptr)->size : 0;
}

static void release_parked(Allocator *state) {
  while (state->parked) {
    Parked *next = state->parked->next;
    free(state->parked);
    state->parked = next;
  }
}

int main(void) {
  const JSMallocFunctions functions = {
    .js_calloc = test_calloc,
    .js_malloc = test_malloc,
    .js_free = test_free,
    .js_realloc = test_realloc,
    .js_malloc_usable_size = test_usable_size,
  };
  for (point = 0; point < 100000; ++point) {
    Allocator state = {.budget = -1};
    JSRuntime *runtime = JS_NewRuntime2(&functions, &state);
    CHECK(runtime);
    state.budget = point;
    JSContext *context = JS_NewContext(runtime);
    state.budget = -1;
    if (context) JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    release_parked(&state);
    if (!state.injected) {
      CHECK(context);
      printf("JS_NewContext: %ld allocation failure points\n", point);
      return 0;
    }
    /* Some failures are recoverable; either outcome must free cleanly. */
  }
  CHECK(!"JS_NewContext never completed");
  return 1;
}
