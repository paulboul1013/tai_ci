#define _POSIX_C_SOURCE 200809L
/* Native half of the window-title oracle comparison. Driven by
 * tests/title_integration.py, it replays the scenarios of
 * tests/title_oracle_probe.py in the real multi-window presentation loop
 * under SDL's dummy driver and reports each window's SDL_GetWindowTitle().
 *
 * Usage: test_title_window PORT CSS UNTRUSTED_PORT MARKUP_PATH...
 * Loads call the tab set directly on the SDL owner thread through the
 * observer; switching tabs clicks the tab strip, as a user does.
 *
 * Protocol (one line each, answered with "ok"):
 *   STATE <scenario> <step> <json>   a checkpoint
 *   SEEN <path> | RELEASE <path> */
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

static char base[128];
static char untrusted_base[128];
static char *css;
static char **markup_paths;
static size_t markup_count;

static void await_ok(void) {
  char reply[16];
  CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
}

static void command(const char *verb, const char *argument) {
  printf("%s %s\n", verb, argument);
  fflush(stdout);
  await_ok();
}

static const char *url_for(const char *path) {
  static char buffer[4][256];
  static int next;
  char *url = buffer[next++ % 4];
  CHECK(snprintf(url, sizeof(buffer[0]), "%s%s", base, path) > 0);
  return url;
}

/* -- page inspection ------------------------------------------------------ */

static void append_text(const TaiNode *node, char *out, size_t size) {
  if (node->kind == TAI_TEXT && node->text) {
    size_t used = strlen(out);
    CHECK(snprintf(out + used, size - used, "%s", node->text) >= 0);
  }
  for (size_t index = 0; index < node->child_count; index++)
    append_text(node->children[index], out, size);
}

static const TaiNode *find_tag(const TaiNode *node, const char *tag) {
  if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    const TaiNode *match = find_tag(node->children[index], tag);
    if (match) return match;
  }
  return NULL;
}

static bool heading(const TaiTabSetView *view, char *out, size_t size) {
  out[0] = '\0';
  if (!view->page) return false;
  const TaiNode *h1 = find_tag(tai_page_root(view->page), "h1");
  if (!h1) return false;
  append_text(h1, out, size);
  return true;
}

static TaiTabSetView view_of(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  return view;
}

/* True when the committed page is idle and its first <h1> reads expected. */
static bool loaded(TaiTabSet *tabs, const char *expected) {
  TaiTabSetView view = view_of(tabs);
  char text[256];
  return !view.loading && heading(&view, text, sizeof(text)) &&
         !strcmp(text, expected);
}

/* Peeks at a background tab without a frame in between, so the window never
 * presents the temporary selection. */
static bool tab_loaded(TaiTabSet *tabs, size_t index, const char *expected) {
  size_t active = view_of(tabs).active_index;
  CHECK(tai_tabset_select(tabs, index));
  bool result = loaded(tabs, expected);
  CHECK(tai_tabset_select(tabs, active));
  return result;
}

/* -- SDL input ------------------------------------------------------------ */

static void push(SDL_Event *event) { CHECK(SDL_PushEvent(event)); }

static void ctrl_n(SDL_WindowID window_id) {
  SDL_Event event = {.key = {.type = SDL_EVENT_KEY_DOWN,
                             .windowID = window_id,
                             .key = SDLK_N,
                             .mod = SDL_KMOD_LCTRL,
                             .down = true}};
  push(&event);
}

/* Clicks the tab strip label of tab index, like a user switching tabs. */
static void click_tab(const TaiPresWindowInfo *window, size_t index) {
  TaiTabSetView view = view_of(window->tabs);
  for (int x = 0; x < window->width; x++) {
    size_t hit = 0;
    if (tai_pres_tabs_tab_link_hit(&view, window->width, x, 27.0, &hit) &&
        hit == index) {
      SDL_Event event = {.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN,
                                    .windowID = window->window_id,
                                    .button = SDL_BUTTON_LEFT,
                                    .down = true,
                                    .x = (float)x + 2.0f,
                                    .y = 27.0f}};
      push(&event);
      return;
    }
  }
  CHECK(!"tab label not found");
}

/* The loop handles one event per iteration; this reports that every pushed
 * event has been handled. */
static bool drained(void) {
  SDL_PumpEvents();
  return SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) == 0;
}

static void navigate_url(TaiTabSet *tabs, const char *url) {
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, url, &error));
  CHECK(error == NULL);
}

static void navigate(TaiTabSet *tabs, const char *path) {
  navigate_url(tabs, url_for(path));
}

/* -- reporting ------------------------------------------------------------ */

static const char *sdl_title(const TaiPresWindowInfo *window) {
  SDL_Window *native = SDL_GetWindowFromID(window->window_id);
  CHECK(native);
  const char *title = SDL_GetWindowTitle(native);
  CHECK(title);
  return title;
}

static char *title_or_name(const TaiTabSetView *view) {
  char *title = view->page ? tai_page_title(view->page) : NULL;
  CHECK(!view->page || title);
  if (title && *title) return title;
  free(title);
  title = tai_strdup(TAI_BROWSER_NAME);
  CHECK(title);
  return title;
}

/* The oracle's checkpoint fields; get_title uses the fallback name. extra is
 * NULL or JSON members appended to the object. */
static void window_json(const TaiPresWindowInfo *window, const char *extra) {
  TaiTabSetView view = view_of(window->tabs);
  char text[256];
  bool has_heading = heading(&view, text, sizeof(text));
  char *title = title_or_name(&view);
  fputs("{\"get_title\": ", stdout);
  tai_json_string(stdout, title);
  fputs(", \"sdl_title\": ", stdout);
  tai_json_string(stdout, sdl_title(window));
  fputs(", \"heading\": ", stdout);
  if (has_heading) tai_json_string(stdout, text);
  else fputs("null", stdout);
  fputs(", \"url\": ", stdout);
  tai_json_string(stdout, view.url ? view.url : "");
  if (extra) fputs(extra, stdout);
  putchar('}');
  free(title);
}

static void state_begin(const char *scenario, const char *step) {
  printf("STATE %s %s ", scenario, step);
}

static void state_end(void) {
  putchar('\n');
  fflush(stdout);
  await_ok();
}

static void report(const char *scenario, const char *step,
                   const TaiPresWindowInfo *window) {
  state_begin(scenario, step);
  window_json(window, NULL);
  state_end();
}

static void report_windows(const char *scenario, const char *step,
                           const TaiPresWindowInfo *windows, size_t count) {
  state_begin(scenario, step);
  putchar('[');
  for (size_t index = 0; index < count; index++) {
    if (index) fputs(", ", stdout);
    window_json(&windows[index], NULL);
  }
  putchar(']');
  state_end();
}

/* -- scenario driver ------------------------------------------------------ */

typedef struct Driver Driver;
typedef bool (*Scenario)(Driver *driver, const TaiPresWindowInfo *windows,
                         size_t count);

struct Driver {
  const char *name;
  Scenario scenario;
  int step;
  size_t index;
  struct timespec deadline;
  bool finished;
};

static void next_step(Driver *driver) {
  driver->step++;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &driver->deadline) == 0);
  driver->deadline.tv_sec += 10;
}

static bool frame(void *opaque, const TaiPresWindowInfo *windows,
                  size_t count) {
  Driver *driver = opaque;
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  if (now.tv_sec > driver->deadline.tv_sec ||
      (now.tv_sec == driver->deadline.tv_sec &&
       now.tv_nsec > driver->deadline.tv_nsec)) {
    fprintf(stderr, "%s timed out in step %d\n", driver->name, driver->step);
    exit(EXIT_FAILURE);
  }
  return driver->scenario(driver, windows, count);
}

#define ADVANCE() do { next_step(driver); return true; } while (0)
#define FINISH() do { driver->finished = true; return false; } while (0)

/* Questions 1-5. The heading of markup page /t/<name> is <name>. */
static bool markup(Driver *driver, const TaiPresWindowInfo *w, size_t count) {
  (void)count;
  if (driver->step == 0) {
    if (!loaded(w[0].tabs, "home")) return true;
  } else {
    const char *path = markup_paths[driver->index];
    if (!loaded(w[0].tabs, path + strlen("/t/"))) return true;
    report(driver->name, path, &w[0]);
    driver->index++;
  }
  if (driver->index == markup_count) FINISH();
  navigate(w[0].tabs, markup_paths[driver->index]);
  ADVANCE();
}

static bool error_pages(Driver *driver, const TaiPresWindowInfo *w,
                        size_t count) {
  (void)count;
  char url[256];
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    navigate(w[0].tabs, "/fail");
    ADVANCE();
  case 1:
    if (!loaded(w[0].tabs, "Network Error")) return true;
    report(driver->name, "network_error", &w[0]);
    navigate(w[0].tabs, "/a");
    ADVANCE();
  case 2:
    if (!loaded(w[0].tabs, "page-a")) return true;
    CHECK(snprintf(url, sizeof(url), "%s/secure-home", untrusted_base) > 0);
    navigate_url(w[0].tabs, url);
    ADVANCE();
  case 3:
    if (!loaded(w[0].tabs, "Certificate Error")) return true;
    report(driver->name, "certificate_error", &w[0]);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static bool bookmarks(Driver *driver, const TaiPresWindowInfo *w,
                      size_t count) {
  (void)count;
  char *error = NULL;
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    CHECK(tai_tabset_open_bookmarks(w[0].tabs, &error) && !error);
    ADVANCE();
  case 1:
    if (!loaded(w[0].tabs, "Bookmarks")) return true;
    report(driver->name, "about_bookmarks", &w[0]);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static bool pending(Driver *driver, const TaiPresWindowInfo *w,
                    size_t count) {
  (void)count;
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    navigate(w[0].tabs, "/delay");
    command("SEEN", "/delay");
    ADVANCE();
  case 1:
    /* This frame presented the window with the navigation pending. */
    CHECK(view_of(w[0].tabs).loading);
    report(driver->name, "pending", &w[0]);
    command("RELEASE", "/delay");
    ADVANCE();
  case 2:
    if (!loaded(w[0].tabs, "delay")) return true;
    report(driver->name, "released", &w[0]);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

/* Reports the window with the background first tab's title added. */
static void inactive_loaded(const char *scenario,
                            const TaiPresWindowInfo *window) {
  size_t active = view_of(window->tabs).active_index;
  CHECK(tai_tabset_select(window->tabs, 0));
  TaiTabSetView first = view_of(window->tabs);
  char *title = title_or_name(&first);
  CHECK(tai_tabset_select(window->tabs, active));
  char extra[256];
  CHECK(snprintf(extra, sizeof(extra), ", \"inactive_committed_title\": \"%s\"",
                 title) > 0);
  free(title);
  state_begin(scenario, "inactive_loaded");
  window_json(window, extra);
  state_end();
}

static bool tabs(Driver *driver, const TaiPresWindowInfo *w, size_t count) {
  (void)count;
  char *error = NULL;
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    CHECK(tai_tabset_new_tab(w[0].tabs, &error) && !error);
    ADVANCE();
  case 1:
    if (!loaded(w[0].tabs, "home")) return true;
    navigate(w[0].tabs, "/b");
    ADVANCE();
  case 2:
    if (!loaded(w[0].tabs, "page-b")) return true;
    report(driver->name, "second_active", &w[0]);
    click_tab(&w[0], 0);
    ADVANCE();
  case 3:
    if (!drained() || !loaded(w[0].tabs, "page-a")) return true;
    report(driver->name, "first_active", &w[0]);
    /* Start a load in the first tab, then leave it in the background. */
    navigate(w[0].tabs, "/delay");
    command("SEEN", "/delay");
    click_tab(&w[0], 1);
    ADVANCE();
  case 4:
    if (!drained() || !loaded(w[0].tabs, "page-b")) return true;
    report(driver->name, "second_again", &w[0]);
    command("RELEASE", "/delay");
    ADVANCE();
  case 5:
    if (!tab_loaded(w[0].tabs, 0, "delay")) return true;
    inactive_loaded(driver->name, &w[0]);
    click_tab(&w[0], 0);
    ADVANCE();
  case 6:
    if (!drained() || !loaded(w[0].tabs, "delay")) return true;
    report(driver->name, "switched_to_loaded", &w[0]);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static bool windows(Driver *driver, const TaiPresWindowInfo *w,
                    size_t count) {
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    ctrl_n(w[0].window_id);
    ADVANCE();
  case 1:
    if (count != 2 || !loaded(w[1].tabs, "home")) return true;
    navigate(w[1].tabs, "/b");
    ADVANCE();
  case 2:
    if (!loaded(w[1].tabs, "page-b")) return true;
    report_windows(driver->name, "opened", w, count);
    navigate(w[1].tabs, "/c");
    ADVANCE();
  case 3:
    if (!loaded(w[1].tabs, "page-c")) return true;
    report_windows(driver->name, "second_navigated", w, count);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static bool fresh_window(Driver *driver, const TaiPresWindowInfo *w,
                         size_t count) {
  (void)count;
  switch (driver->step) {
  case 0:
    command("SEEN", "/delay");
    /* The first frame has presented the window before any commit. */
    CHECK(view_of(w[0].tabs).page == NULL);
    state_begin(driver->name, "presented_before_commit");
    printf("{\"sdl_title\": ");
    tai_json_string(stdout, sdl_title(&w[0]));
    printf(", \"committed\": 0}");
    state_end();
    command("RELEASE", "/delay");
    ADVANCE();
  case 1:
    if (!loaded(w[0].tabs, "delay")) return true;
    report(driver->name, "committed", &w[0]);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

/* Question 12: /retitle.js rewrites the <title> text through innerHTML. */
static bool dom_change(Driver *driver, const TaiPresWindowInfo *w,
                       size_t count) {
  (void)count;
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    navigate(w[0].tabs, "/retitle");
    ADVANCE();
  case 1:
    if (!loaded(w[0].tabs, "retitle")) return true;
    report(driver->name, "after_script", &w[0]);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static void run(const char *name, Scenario scenario, const char *path) {
  char *error = NULL;
  TaiBrowserApp *app = tai_browser_app_create_with_home_url(
      css, false, url_for("/home"), &error);
  CHECK(app && error == NULL);
  /* SDL_Quit at the end of each presentation resets hints. */
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  Driver driver = {.name = name, .scenario = scenario, .step = -1};
  next_step(&driver);
  TaiPresBrowserObserver observer = {.frame = frame, .opaque = &driver};
  bool presented = tai_pres_present_browser_observed(app, url_for(path), 800,
                                                     600, &observer, &error);
  if (!presented)
    fprintf(stderr, "%s: presentation failed: %s\n", name,
            error ? error : "unknown");
  CHECK(presented && error == NULL && driver.finished);
  tai_browser_app_destroy(app);
}

int main(int argc, char **argv) {
  CHECK(argc >= 4);
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  CHECK(snprintf(untrusted_base, sizeof(untrusted_base),
                 "https://127.0.0.1:%s", argv[3]) > 0);
  size_t length = 0;
  css = tai_read_file(argv[2], &length);
  CHECK(css);
  markup_paths = argv + 4;
  markup_count = (size_t)(argc - 4);
  run("markup", markup, "/home");
  run("error_pages", error_pages, "/a");
  run("bookmarks", bookmarks, "/a");
  run("pending", pending, "/a");
  run("tabs", tabs, "/a");
  run("windows", windows, "/a");
  run("fresh_window", fresh_window, "/delay");
  run("dom_change", dom_change, "/a");
  free(css);
  puts("DONE");
  fflush(stdout);
  return EXIT_SUCCESS;
}
