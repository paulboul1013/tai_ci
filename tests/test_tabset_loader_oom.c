#define _POSIX_C_SOURCE 200809L
/* TabSet loader-thread allocation failure sweep.
 *
 * Linked with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,
 * --wrap=pthread_create. Only threads created by the project (the TabSet
 * loader) are armed, and only allocations from statically linked code (Tai
 * and QuickJS) pass through the wrappers; shared libraries are not failed.
 * For each point i, the i-th and every later loader allocation fails. Each
 * run must finish loading, keep the previous page and history on failure, and
 * recover on a clean retry in the same tab. */
#include "tai/browser.h"
#include "tai/tabset.h"
#include "tai/url.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s (point %ld)\n", __FILE__,    \
              __LINE__, #condition, current_point);                           \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

static const char *default_css =
    "html {display:block} body {display:block} p {display:block}";
static const char *first_url = "data:text/html,<p>first</p>";
static const char *second_url = "data:text/html,<p>second</p>";

static _Thread_local bool armed_thread;
static atomic_long budget = -1; /* <0 disarmed */
static atomic_bool injected;
static long current_point = -1;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*start)(void *), void *argument);

static bool should_fail(void) {
  if (!armed_thread) return false;
  long left = atomic_load(&budget);
  while (left >= 0) {
    if (left == 0) {
      atomic_store(&injected, true);
      return true;
    }
    if (atomic_compare_exchange_weak(&budget, &left, left - 1)) return false;
  }
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

typedef struct {
  void *(*start)(void *);
  void *argument;
} ThreadStart;

static void *armed_start(void *opaque) {
  ThreadStart start = *(ThreadStart *)opaque;
  free(opaque);
  armed_thread = true;
  return start.start(start.argument);
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*start)(void *), void *argument) {
  ThreadStart *wrapped = __real_malloc(sizeof(*wrapped));
  if (!wrapped) return 12; /* ENOMEM */
  wrapped->start = start;
  wrapped->argument = argument;
  int result = __real_pthread_create(thread, attributes, armed_start, wrapped);
  if (result != 0) free(wrapped);
  return result;
}

static double now_seconds(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}

/* Pumps until the active tab stops loading. A lost completion times out. */
static TaiTabSetView settle(TaiTabSet *tabs) {
  double deadline = now_seconds() + 15.0;
  TaiTabSetView view;
  for (;;) {
    bool changed = false;
    char *error = NULL;
    tai_tabset_pump(tabs, &changed, &error); /* commits may fail under OOM */
    free(error);
    CHECK(tai_tabset_view(tabs, &view));
    if (!view.loading) return view;
    CHECK(now_seconds() < deadline);
    struct timespec pause = {0, 2000000};
    nanosleep(&pause, NULL);
  }
}

static bool page_is(const TaiTabSetView *view, const char *url) {
  return view->page &&
         strcmp(tai_url_string(tai_page_url(view->page)), url) == 0;
}

static void arm(long point) {
  atomic_store(&injected, false);
  atomic_store(&budget, point);
}

static bool disarm(void) {
  atomic_store(&budget, -1);
  return atomic_load(&injected);
}

/* Returns true once point i no longer reaches an injected failure. */
static bool navigation_point(long point) {
  char *error = NULL;
  TaiTabSet *tabs =
      tai_tabset_create_with_home_url(default_css, false, first_url, &error);
  CHECK(tabs && !error);
  CHECK(tai_tabset_start(tabs, first_url, 300, 200, &error));
  TaiTabSetView view = settle(tabs);
  CHECK(page_is(&view, first_url));

  arm(point);
  tai_tabset_navigate_address(tabs, second_url, &error);
  free(error);
  error = NULL;
  view = settle(tabs);
  bool hit = disarm();

  if (page_is(&view, second_url)) {
    CHECK(view.history_count == 2 && view.history_index == 1);
  } else {
    CHECK(page_is(&view, first_url));
    CHECK(view.history_count == 1 && view.history_index == 0);
  }
  CHECK(tai_tabset_navigate_address(tabs, second_url, &error));
  view = settle(tabs);
  CHECK(page_is(&view, second_url));
  tai_tabset_destroy(tabs);
  return !hit;
}

static bool initial_point(long point) {
  char *error = NULL;
  TaiTabSet *tabs =
      tai_tabset_create_with_home_url(default_css, false, first_url, &error);
  CHECK(tabs && !error);
  arm(point);
  bool started = tai_tabset_start(tabs, first_url, 300, 200, &error);
  free(error);
  error = NULL;
  TaiTabSetView view = {0};
  if (started) view = settle(tabs);
  bool hit = disarm();

  if (!started) {
    CHECK(tai_tabset_start(tabs, first_url, 300, 200, &error));
    view = settle(tabs);
  } else if (!view.page) {
    CHECK(tai_tabset_navigate_address(tabs, first_url, &error));
    view = settle(tabs);
  }
  CHECK(page_is(&view, first_url));
  tai_tabset_destroy(tabs);
  return !hit;
}

static long sweep(const char *name, bool (*run)(long point)) {
  for (current_point = 0; current_point < 100000; ++current_point)
    if (run(current_point)) {
      printf("%s: %ld loader allocation failure points\n", name,
             current_point);
      return current_point;
    }
  CHECK(!"sweep never reached a failure-free load");
  return -1;
}

int main(void) {
  /* Each sweep must actually exercise failures before loads succeed. */
  CHECK(sweep("navigation", navigation_point) > 0);
  CHECK(sweep("initial", initial_point) > 0);
  return 0;
}
