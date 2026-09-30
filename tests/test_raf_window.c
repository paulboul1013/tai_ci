#define _POSIX_C_SOURCE 200809L
/* requestAnimationFrame in the real tabbed presentation loop under SDL's
 * dummy driver (plan slice 6). Pages are data: URLs whose inline scripts
 * (D6) drive the frames, so no server is needed.
 *
 *   interval    frames are at least 33 ms apart (decision 7), a throwing
 *               callback does not stop its batch (D10) and the loop idles
 *               again once the page stops asking for frames
 *   background  a tab whose page asked for a frame while inactive runs it
 *               only after it becomes active (Python set_needs_animation_frame
 *               ignores inactive tabs; set_active_tab schedules a frame)
 *   navigate    navigating away from an endless animation drops its request
 *   close       closing the window with a frame pending (ASan/LSan) */
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

/* Ten frames of n, after a callback that throws in the first batch. data-t
 * is when the frame ran (performance.now() is monotonic; the loop itself
 * sees the frame only after the repaint). */
#define COUNTER_PAGE                                                          \
  "data:text/html,<p id=a>0</p><script>var n = 0;"                            \
  "requestAnimationFrame(function () { throw Error('frame boom'); });"        \
  "function f() { n = n + 1; a.innerHTML = String(n);"                        \
  " a.setAttribute('data-t', String(performance.now()));"                            \
  " if (n < 10) requestAnimationFrame(f); }"                                  \
  "requestAnimationFrame(f);</script>"
#define ONCE_PAGE                                                             \
  "data:text/html,<p id=a>waiting</p><script>"                                \
  "requestAnimationFrame(function () { a.innerHTML = 'ran'; });</script>"
#define ENDLESS_PAGE                                                          \
  "data:text/html,<p id=a>0</p><script>var n = 0;"                            \
  "function f() { n = n + 1; a.innerHTML = String(n);"                        \
  " requestAnimationFrame(f); } requestAnimationFrame(f);</script>"
#define PLAIN_PAGE "data:text/html,<p id=a>plain</p>"

/* SDL_Quit after each presentation restarts SDL_GetTicks, so time the
 * scenarios with the monotonic clock. */
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
  /* interval: data-t of frames n = 1..10 and the loop count seeing each */
  double changed_at[16];
  Uint64 iterations_at[16];
  size_t changes;
  bool finished;
};

static const char *text_of(const TaiNode *node, const char *id) {
  if (node->kind == TAI_ELEMENT) {
    const char *value = tai_map_get(&node->attributes, "id");
    if (value && !strcmp(value, id))
      return node->child_count && node->children[0]->kind == TAI_TEXT
                 ? node->children[0]->text
                 : "";
  }
  for (size_t index = 0; index < node->child_count; index++) {
    const char *found = text_of(node->children[index], id);
    if (found) return found;
  }
  return NULL;
}

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

static TaiTabSetView view_of(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  return view;
}

/* The #a text of the active committed page, or NULL while loading. */
static const char *active_text(TaiTabSet *tabs) {
  TaiTabSetView view = view_of(tabs);
  if (view.loading || !view.page) return NULL;
  return text_of(tai_page_root(view.page), "a");
}

static void next_step(Driver *driver) {
  driver->step++;
  driver->step_started = now_ms();
}

static bool interval(Driver *driver, TaiTabSet *tabs) {
  const char *text = active_text(tabs);
  switch (driver->step) {
  case 0: {
    size_t n = text ? (size_t)atoi(text) : 0;
    /* One frame per loop iteration at most: n never skips a value. */
    CHECK(n <= driver->changes + 1);
    if (n == driver->changes + 1) {
      const char *stamp = tai_map_get(
          &find_id(tai_page_root(view_of(tabs).page), "a")->attributes,
          "data-t");
      CHECK(stamp);
      driver->changed_at[driver->changes] = strtod(stamp, NULL);
      driver->iterations_at[driver->changes] = driver->iterations;
      driver->changes++;
    }
    if (n == 10) next_step(driver);
    return true;
  }
  case 1: {
    /* The page stopped asking: nothing may run while the loop idles. */
    TaiTabSetView view = view_of(tabs);
    CHECK(!tai_page_needs_animation_frame(view.page));
    if (now_ms() - driver->step_started < 300) return true;
    Uint64 idle = driver->iterations - driver->iterations_at[driver->changes - 1];
    /* 300 ms at the 16 ms idle wait is about 19 iterations. */
    CHECK(idle < 60);
    driver->finished = true;
    return false;
  }
  }
  return false;
}

static bool background(Driver *driver, TaiTabSet *tabs) {
  switch (driver->step) {
  case 0:
    if (!active_text(tabs)) return true;
    /* The new tab's page registers a frame at load, but the tab is made
     * inactive before its load can commit (commits happen on this thread). */
    CHECK(tai_tabset_new_tab(tabs, NULL));
    CHECK(tai_tabset_select(tabs, 0));
    next_step(driver);
    return true;
  case 1: {
    TaiTabSetView view = view_of(tabs);
    CHECK(view.active_index == 0 && view.tab_count == 2);
    /* Let many frame intervals pass while the new tab is in the background. */
    if (now_ms() - driver->step_started < 400) return true;
    /* Peek at the new tab; nothing commits or runs frames inside this call,
     * so switching back before returning keeps it in the background. */
    CHECK(tai_tabset_select(tabs, 1));
    view = view_of(tabs);
    if (view.loading || !view.page) {
      CHECK(tai_tabset_select(tabs, 0));
      return true;
    }
    CHECK(!strcmp(text_of(tai_page_root(view.page), "a"), "waiting"));
    CHECK(tai_page_needs_animation_frame(view.page));
    next_step(driver);
    return true;
  }
  case 2: {
    const char *text = active_text(tabs);
    CHECK(text);
    if (strcmp(text, "ran")) return true;
    CHECK(now_ms() - driver->step_started < 200);
    driver->finished = true;
    return false;
  }
  }
  return false;
}

static bool navigate(Driver *driver, TaiTabSet *tabs) {
  const char *text = active_text(tabs);
  switch (driver->step) {
  case 0:
    if (!text || atoi(text) < 3) return true;
    CHECK(tai_tabset_navigate_address(tabs, PLAIN_PAGE, NULL));
    next_step(driver);
    return true;
  case 1:
    if (!text || strcmp(text, "plain")) return true;
    CHECK(!tai_page_needs_animation_frame(view_of(tabs).page));
    next_step(driver);
    return true;
  case 2:
    CHECK(!strcmp(text, "plain"));
    if (now_ms() - driver->step_started < 150) return true;
    driver->finished = true;
    return false;
  }
  return false;
}

static bool close_pending(Driver *driver, TaiTabSet *tabs) {
  const char *text = active_text(tabs);
  if (!text || atoi(text) < 3) return true;
  CHECK(tai_page_needs_animation_frame(view_of(tabs).page));
  driver->finished = true;
  return false; /* closes the window with the next frame requested */
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

static void run(Driver *driver, const char *url, const char *home_url) {
  char *error = NULL;
  TaiBrowserApp *app = tai_browser_app_create_with_home_url(
      css, false, home_url, &error);
  CHECK(app && error == NULL);
  /* SDL_Quit at the end of each presentation resets hints. */
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
  run(&interval_driver, COUNTER_PAGE, PLAIN_PAGE);
  CHECK(interval_driver.changes == 10);
  double shortest = 1e9;
  for (size_t index = 1; index < interval_driver.changes; index++) {
    double gap = interval_driver.changed_at[index] -
                 interval_driver.changed_at[index - 1];
    if (gap < shortest) shortest = gap;
  }
  double total = interval_driver.changed_at[9] - interval_driver.changed_at[0];
  Uint64 loops = interval_driver.iterations_at[9] -
                 interval_driver.iterations_at[0];
  printf("interval: 9 frame gaps in %.1f ms (shortest %.1f ms), "
         "%llu loop iterations\n",
         total, shortest, (unsigned long long)loops);
  /* Frames are due 33 ms after the previous one started; allow the
   * microseconds between the loop's clock read and the callback's. */
  CHECK(shortest >= 32.9);
  CHECK(total >= 9 * 33 - 0.1);
  /* The wait ends at each due frame; a busy loop would spin thousands. */
  CHECK(loops < 9 * 6);

  Driver background_driver = {.name = "background", .scenario = background};
  run(&background_driver, PLAIN_PAGE, ONCE_PAGE);
  puts("background: the inactive tab's frame ran after it became active");

  Driver navigate_driver = {.name = "navigate", .scenario = navigate};
  run(&navigate_driver, ENDLESS_PAGE, PLAIN_PAGE);
  puts("navigate: the replaced page's animation stopped");

  Driver close_driver = {.name = "close", .scenario = close_pending};
  run(&close_driver, ENDLESS_PAGE, PLAIN_PAGE);
  puts("close: window closed with a frame pending");
  puts("RAF window integration passed");
  return EXIT_SUCCESS;
}
