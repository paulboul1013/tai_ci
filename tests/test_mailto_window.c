#define _POSIX_C_SOURCE 200809L
/* Native half of the mailto oracle comparison. Driven by
 * tests/mailto_integration.py, it replays the scenarios of
 * tests/mailto_oracle_probe.py in the real tabbed presentation loop under
 * SDL's dummy driver. External URLs go to OPENER, a test program that logs
 * its argument; the driver adds what it logged and the requests the step
 * sent to each checkpoint.
 *
 * Usage: test_mailto_window PORT CSS OPENER
 * Like the Python probe, the address draft is set on the editor directly;
 * Enter, link clicks and button clicks go through SDL events.
 *
 * Protocol (one line each, answered with "ok"):
 *   MARK                             start a step's request and open log
 *   STATE <scenario> <step> <json>   a checkpoint */
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
static char *css;
static const char *opener;

static void await_ok(void) {
  char reply[16];
  CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
}

static void mark(void) {
  puts("MARK");
  fflush(stdout);
  await_ok();
}

static const char *url_for(const char *path) {
  static char buffer[256];
  CHECK(snprintf(buffer, sizeof(buffer), "%s%s", base, path) > 0);
  return buffer;
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

typedef bool (*Select)(const TaiNode *node, const void *data);

static const TaiNode *find(const TaiNode *node, Select select,
                           const void *data) {
  if (node->kind == TAI_ELEMENT && select(node, data)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    const TaiNode *match = find(node->children[index], select, data);
    if (match) return match;
  }
  return NULL;
}

static bool is_tag(const TaiNode *node, const void *tag) {
  return !strcmp(node->tag, tag);
}

static bool is_link(const TaiNode *node, const void *href) {
  const char *value = tai_map_get(&node->attributes, "href");
  return !strcmp(node->tag, "a") && value && !strcmp(value, href);
}

/* Python strips the joined <h1> text. */
static bool heading(const TaiTabSetView *view, char *out, size_t size) {
  out[0] = '\0';
  if (!view->page) return false;
  const TaiNode *h1 = find(tai_page_root(view->page), is_tag, "h1");
  if (!h1) return false;
  append_text(h1, out, size);
  size_t length = strlen(out);
  while (length && out[length - 1] == ' ') out[--length] = '\0';
  size_t start = strspn(out, " ");
  memmove(out, out + start, strlen(out + start) + 1);
  return true;
}

static bool loaded(TaiTabSet *tabs, const char *expected) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  char text[256];
  return !view.loading && heading(&view, text, sizeof(text)) &&
         !strcmp(text, expected);
}

/* -- SDL input ------------------------------------------------------------ */

static void push(SDL_Event *event) { CHECK(SDL_PushEvent(event)); }

static bool drained(void) {
  SDL_PumpEvents();
  return SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) == 0;
}

static void enter(SDL_WindowID window_id) {
  SDL_Event event = {.key = {.type = SDL_EVENT_KEY_DOWN,
                             .windowID = window_id,
                             .key = SDLK_RETURN,
                             .down = true}};
  push(&event);
}

typedef struct {
  const TaiNode *target;
  double x, y;
  bool found;
} TargetPoint;

static bool find_target_point(const TaiLayoutItem *item, void *opaque) {
  TargetPoint *point = opaque;
  if (point->found || item->width <= 0) return true;
  for (const TaiNode *node = item->node; node; node = node->parent) {
    if (node == point->target) {
      point->x = item->x + item->width / 2.0;
      point->y = item->y + item->height / 2.0;
      point->found = true;
      break;
    }
  }
  return true;
}

/* Clicks the center of the first layout item under the selected element. */
static void click(TaiTabSet *tabs, SDL_WindowID window_id, int width,
                  bool wraps, Select select, const void *data) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view) && view.page);
  TargetPoint point = {.target = find(tai_page_root(view.page), select, data)};
  CHECK(point.target);
  char *error = NULL;
  CHECK(tai_layout_visit(tai_page_layout(view.page), find_target_point,
                         &point, &error));
  CHECK(error == NULL && point.found);
  SDL_Event event = {.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN,
                                .windowID = window_id,
                                .button = SDL_BUTTON_LEFT,
                                .down = true,
                                .x = (float)point.x,
                                .y = (float)(point.y -
                                             tai_page_scroll_y(view.page) +
                                             tabs_chrome_bottom(width,
                                                                wraps))}};
  push(&event);
}

/* Python probe set_draft: typed text, focused and dirty. */
static void set_draft(AddressEditor *editor, const char *text) {
  char *copy = tai_strdup(text);
  CHECK(copy);
  free(editor->text);
  editor->text = copy;
  editor->cursor = strlen(copy);
  editor->dirty = true;
  editor->focused = true;
}

/* -- reporting ------------------------------------------------------------ */

static void report(const char *scenario, const char *step, TaiTabSet *tabs,
                   const AddressEditor *editor) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  char text[256];
  bool has_heading = heading(&view, text, sizeof(text));
  printf("STATE %s %s {\"url\": ", scenario, step);
  tai_json_string(stdout, view.url ? view.url : "");
  fputs(", \"heading\": ", stdout);
  if (has_heading) tai_json_string(stdout, text);
  else fputs("null", stdout);
  fputs(", \"history\": [", stdout);
  for (size_t index = 0; index < view.history_count; index++) {
    char *url = tai_tabset_history_url(tabs, index);
    CHECK(url);
    if (index) fputs(", ", stdout);
    tai_json_string(stdout, url);
    free(url);
  }
  printf("], \"history_index\": %zu, \"address\": ", view.history_index);
  /* The field shows the draft while focused or dirty, else the URL. */
  const char *shown = editor->dirty || editor->focused ? editor->text
                                                       : view.url;
  tai_json_string(stdout, shown ? shown : "");
  printf(", \"focused\": %s, \"dirty\": %s}\n",
         editor->focused ? "true" : "false",
         editor->dirty ? "true" : "false");
  fflush(stdout);
  await_ok();
}

/* -- scenario driver ------------------------------------------------------ */

typedef struct Driver Driver;
typedef bool (*Scenario)(Driver *driver, TaiTabSet *tabs,
                         SDL_WindowID window_id, int width, bool wraps,
                         AddressEditor *editor);

struct Driver {
  const char *name;
  Scenario scenario;
  int step;
  struct timespec deadline;
  bool finished;
};

static void next_step(Driver *driver) {
  driver->step++;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &driver->deadline) == 0);
  driver->deadline.tv_sec += 10;
}

static bool frame(void *opaque, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  Driver *driver = opaque;
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  if (now.tv_sec > driver->deadline.tv_sec ||
      (now.tv_sec == driver->deadline.tv_sec &&
       now.tv_nsec > driver->deadline.tv_nsec)) {
    fprintf(stderr, "%s timed out in step %d\n", driver->name, driver->step);
    exit(EXIT_FAILURE);
  }
  return driver->scenario(driver, tabs, window_id, width, wraps, editor);
}

#define ADVANCE() do { next_step(driver); return true; } while (0)
#define FINISH() do { driver->finished = true; return false; } while (0)

static const char *const addresses[][2] = {
  {"query", "mailto:someone@example.com?subject=Hi"},
  {"padded_fragment", "  mailto:pad@example.com#part  "},
  {"bare", "mailto:"},
};
enum { ADDRESS_COUNT = sizeof(addresses) / sizeof(addresses[0]) };

/* Each step submits one address, and the next reports it. */
static bool address(Driver *driver, TaiTabSet *tabs, SDL_WindowID window_id,
                    int width, bool wraps, AddressEditor *editor) {
  (void)width;
  (void)wraps;
  int step = driver->step;
  if (step == 0 && !loaded(tabs, "page-a")) return true;
  if (step > 0) {
    if (!drained()) return true;
    report(driver->name, addresses[step - 1][0], tabs, editor);
  }
  if (step == ADDRESS_COUNT) FINISH();
  mark();
  set_draft(editor, addresses[step][1]);
  enter(window_id);
  ADVANCE();
}

static const char *const links_hrefs[][2] = {
  {"query", "mailto:someone@example.com?subject=Hello%20there"},
  {"fragment", "mailto:frag@example.com#part"},
};
enum { LINK_COUNT = sizeof(links_hrefs) / sizeof(links_hrefs[0]) };

static bool links(Driver *driver, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  int step = driver->step;
  if (step == 0 && !loaded(tabs, "links")) return true;
  if (step > 0) {
    if (!drained()) return true;
    report(driver->name, links_hrefs[step - 1][0], tabs, editor);
  }
  if (step == LINK_COUNT) FINISH();
  mark();
  set_draft(editor, "typed draft");
  click(tabs, window_id, width, wraps, is_link, links_hrefs[step][1]);
  ADVANCE();
}

static bool cancelled(Driver *driver, TaiTabSet *tabs, SDL_WindowID window_id,
                      int width, bool wraps, AddressEditor *editor) {
  switch (driver->step) {
  case 0:
    if (!loaded(tabs, "cancel")) return true;
    mark();
    click(tabs, window_id, width, wraps, is_link, "mailto:cancel@example.com");
    ADVANCE();
  case 1:
    if (!drained()) return true;
    report(driver->name, "clicked", tabs, editor);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static bool form(Driver *driver, TaiTabSet *tabs, SDL_WindowID window_id,
                 int width, bool wraps, AddressEditor *editor) {
  switch (driver->step) {
  case 0:
    if (!loaded(tabs, "form")) return true;
    mark();
    click(tabs, window_id, width, wraps, is_tag, "button");
    ADVANCE();
  case 1:
    if (!drained() || !loaded(tabs, "External mail link")) return true;
    report(driver->name, "submitted", tabs, editor);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static void run(const char *name, Scenario scenario, const char *path) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(css, false, url_for("/a"),
                                                    &error);
  CHECK(tabs && error == NULL);
  CHECK(tai_tabset_set_external_opener_for_test(tabs, opener));
  /* SDL_Quit at the end of each presentation resets hints. */
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  Driver driver = {.name = name, .scenario = scenario, .step = -1};
  next_step(&driver);
  TaiPresTabsObserver observer = {.frame = frame, .opaque = &driver};
  bool presented = tai_pres_present_tabs_observed(tabs, url_for(path), 800,
                                                  600, &observer, &error);
  if (!presented)
    fprintf(stderr, "%s: presentation failed: %s\n", name,
            error ? error : "unknown");
  CHECK(presented && error == NULL && driver.finished);
  tai_tabset_destroy(tabs);
}

int main(int argc, char **argv) {
  CHECK(argc == 4);
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  size_t length = 0;
  css = tai_read_file(argv[2], &length);
  CHECK(css);
  opener = argv[3];
  run("address", address, "/a");
  run("links", links, "/links");
  run("cancelled", cancelled, "/cancel");
  run("form", form, "/form");
  free(css);
  puts("DONE");
  fflush(stdout);
  return EXIT_SUCCESS;
}
