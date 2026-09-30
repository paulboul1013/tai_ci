#define _POSIX_C_SOURCE 200809L
/* Timers (D5) in the real tabbed presentation loop under SDL's dummy driver.
 * Pages are data: URLs whose inline scripts (D6) arm the timers, so no
 * server is needed.
 *
 *   interval    setInterval ticks follow the ideal timeline (never early),
 *               a 0 ms setTimeout runs at once, and the loop waits for the
 *               next timer instead of spinning
 *   background  a background tab's timers run (Python gives every tab its
 *               own task runner)
 *   close       closing the window with an endless interval and an
 *               asynchronous request pending (ASan/LSan) */
#include "tai/tabset.h"
#include "../src/presentation_internal.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,      \
              #condition);                                                     \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

static const char *css =
    "html {display:block} body {display:block} p {display:block}";

/* Five 100 ms ticks; data-t0 is when the interval was armed and data-tN
 * when tick N ran (performance.now() is monotonic). */
#define INTERVAL_PAGE                                                         \
  "data:text/html,<p id=a>0</p><p id=z>no</p><script>var n = 0;"              \
  "setTimeout(function () { z.innerHTML = 'yes';"                             \
  " z.setAttribute('data-t', String(performance.now())); }, 0);"              \
  "a.setAttribute('data-t0', String(performance.now()));"                     \
  "var id = setInterval(function () { n = n + 1; a.innerHTML = String(n);"    \
  " a.setAttribute('data-t' + n, String(performance.now()));"                 \
  " if (n == 5) clearInterval(id); }, 100);</script>"
#define LATER_PAGE                                                            \
  "data:text/html,<p id=a>waiting</p><script>"                                \
  "setTimeout(function () { a.innerHTML = 'ran'; }, 200);</script>"
#define ENDLESS_PAGE                                                          \
  "data:text/html,<p id=a>0</p><script>var n = 0;"                            \
  "setInterval(function () { n = n + 1; a.innerHTML = String(n); }, 5);"      \
  "var x = new XMLHttpRequest(); x.open('GET', 'data:text/plain,hi', true);"  \
  "x.send();</script>"
#define PLAIN_PAGE "data:text/html,<p id=a>plain</p>"

static Uint64 now_ms(void) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (Uint64)now.tv_sec * 1000u + (Uint64)now.tv_nsec / 1000000u;
}

typedef struct Driver Driver;
typedef bool (*Scenario)(Driver *driver, TaiTabSet *tabs);
struct Driver {
  const char *name;
  Scenario scenario;
  int step;
  Uint64 step_started;
  Uint64 iterations; /* observer calls, one per loop iteration */
  Uint64 iterations_first, iterations_last; /* at ticks 1 and 5 */
  double stamps[6];  /* interval: data-t0..data-t5 */
  double zero_at;    /* interval: when the 0 ms timeout ran */
  bool finished;
};

static const TaiNode *find_id(const TaiNode *node, const char *id) {
  const char *value =
      node->kind == TAI_ELEMENT ? tai_map_get(&node->attributes, "id") : NULL;
  if (value && !strcmp(value, id)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    const TaiNode *found = find_id(node->children[index], id);
    if (found) return found;
  }
  return NULL;
}

static const char *text_in(const TaiPage *page, const char *id) {
  const TaiNode *node = find_id(tai_page_root(page), id);
  return node && node->child_count && node->children[0]->kind == TAI_TEXT
             ? node->children[0]->text
             : "";
}

static TaiTabSetView view_of(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  return view;
}

static const TaiPage *active_page(TaiTabSet *tabs) {
  TaiTabSetView view = view_of(tabs);
  return view.loading ? NULL : view.page;
}

static void next_step(Driver *driver) {
  driver->step++;
  driver->step_started = now_ms();
}

static double stamp(const TaiPage *page, const char *id, const char *name) {
  const char *value = tai_map_get(&find_id(tai_page_root(page), id)->attributes,
                                  name);
  CHECK(value);
  return strtod(value, NULL);
}

static bool interval(Driver *driver, TaiTabSet *tabs) {
  const TaiPage *page = active_page(tabs);
  if (!page) return true;
  int n = atoi(text_in(page, "a"));
  if (n >= 1 && !driver->iterations_first)
    driver->iterations_first = driver->iterations;
  if (n < 5) return true;
  driver->iterations_last = driver->iterations;
  CHECK(!strcmp(text_in(page, "z"), "yes"));
  driver->zero_at = stamp(page, "z", "data-t");
  char name[16];
  for (int tick = 0; tick <= 5; tick++) {
    snprintf(name, sizeof(name), "data-t%d", tick);
    driver->stamps[tick] = stamp(page, "a", name);
  }
  driver->finished = true;
  return false;
}

static bool background(Driver *driver, TaiTabSet *tabs) {
  switch (driver->step) {
  case 0:
    if (!active_page(tabs)) return true;
    /* The 200 ms timer was armed at load; move to another tab at once. */
    CHECK(!strcmp(text_in(active_page(tabs), "a"), "waiting"));
    CHECK(tai_tabset_new_tab(tabs, NULL));
    next_step(driver);
    return true;
  case 1: {
    if (now_ms() - driver->step_started < 400) return true;
    TaiTabSetView view = view_of(tabs);
    CHECK(view.active_index == 1 && view.tab_count == 2);
    CHECK(tai_tabset_select(tabs, 0));
    CHECK(!strcmp(text_in(active_page(tabs), "a"), "ran"));
    driver->finished = true;
    return false;
  }
  }
  return false;
}

static bool close_pending(Driver *driver, TaiTabSet *tabs) {
  const TaiPage *page = active_page(tabs);
  if (!page || atoi(text_in(page, "a")) < 5) return true;
  CHECK(tai_page_next_task(page) < 1e300);
  driver->finished = true;
  return false; /* closes the window with the interval still armed */
}

static bool frame(void *opaque, const TaiPresWindowInfo *windows,
                  size_t count) {
  Driver *driver = opaque;
  CHECK(count == 1);
  driver->iterations++;
  if (now_ms() - driver->step_started > 10000) {
    fprintf(stderr, "%s timed out in step %d\n", driver->name, driver->step);
    exit(EXIT_FAILURE);
  }
  return driver->scenario(driver, windows[0].tabs);
}

static void run(Driver *driver, const char *url) {
  char *error = NULL;
  TaiBrowserApp *app = tai_browser_app_create_with_home_url(
      css, false, PLAIN_PAGE, &error);
  CHECK(app && error == NULL);
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  driver->step_started = now_ms();
  TaiPresBrowserObserver observer = {.frame = frame, .opaque = driver};
  bool presented = tai_pres_present_browser_observed(app, url, 400, 300,
                                                     &observer, &error);
  if (!presented)
    fprintf(stderr, "%s: presentation failed: %s\n", driver->name,
            error ? error : "unknown");
  CHECK(presented && error == NULL && driver->finished);
  tai_browser_app_destroy(app);
}

int main(void) {
  Driver interval_driver = {.name = "interval", .scenario = interval};
  run(&interval_driver, INTERVAL_PAGE);
  const double *t = interval_driver.stamps;
  double earliest = 1e9, latest = 0.0;
  for (int tick = 1; tick <= 5; tick++) {
    double late = t[tick] - (t[0] + 100.0 * tick);
    if (late < earliest) earliest = late;
    if (late > latest) latest = late;
  }
  Uint64 loops = interval_driver.iterations_last -
                 interval_driver.iterations_first;
  printf("interval: ticks %.1f..%.1f ms after the ideal timeline, 0 ms "
         "timeout after %.1f ms, %llu loop iterations over 4 ticks\n",
         earliest, latest, interval_driver.zero_at - t[0],
         (unsigned long long)loops);
  /* data-t0 is read just after setInterval armed, so a tick may appear a
   * few microseconds early against it. */
  CHECK(earliest >= -0.5 && latest < 60.0);
  CHECK(interval_driver.zero_at - t[0] < 100.0);
  /* About 400 ms: the 16 ms idle cap allows ~25 turns, a spinning loop
   * thousands. */
  CHECK(loops < 80);

  Driver background_driver = {.name = "background", .scenario = background};
  run(&background_driver, LATER_PAGE);
  printf("background: a background tab's 200 ms timer ran\n");

  Driver close_driver = {.name = "close", .scenario = close_pending};
  run(&close_driver, ENDLESS_PAGE);
  printf("close: closed with an interval armed\n");
  return EXIT_SUCCESS;
}
