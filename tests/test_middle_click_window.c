#define _POSIX_C_SOURCE 200809L
/* Native half of the middle-click oracle comparison. Driven by
 * tests/middle_click_integration.py, it replays the scenarios of
 * tests/middle_click_oracle_probe.py in the real tabbed presentation loop
 * under SDL's dummy driver. External URLs go to OPENER, a test program that
 * logs its argument; the driver adds what it logged and the requests the
 * step sent to each checkpoint.
 *
 * Usage: test_middle_click_window PORT CSS OPENER
 * Like the Python probe, the address draft is set on the editor directly;
 * every click goes through SDL events.
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
static char absolute_href[160];
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

static bool is_plain_text(const TaiNode *node, const void *unused) {
  (void)unused;
  const char *id = tai_map_get(&node->attributes, "id");
  return !strcmp(node->tag, "p") && id && !strcmp(id, "part");
}

/* Python strips the joined <h1> text. */
static bool heading(TaiPage *page, char *out, size_t size) {
  out[0] = '\0';
  if (!page) return false;
  const TaiNode *h1 = find(tai_page_root(page), is_tag, "h1");
  if (!h1) return false;
  append_text(h1, out, size);
  size_t length = strlen(out);
  while (length && out[length - 1] == ' ') out[--length] = '\0';
  size_t start = strspn(out, " ");
  memmove(out, out + start, strlen(out + start) + 1);
  return true;
}

static bool loaded(TaiTabSet *tabs, size_t tab_count, const char *expected) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  char text[256];
  return view.tab_count == tab_count && !view.loading &&
         heading(view.page, text, sizeof(text)) && !strcmp(text, expected);
}

/* -- SDL input ------------------------------------------------------------ */

static bool drained(void) {
  SDL_PumpEvents();
  return SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) == 0;
}

static void press(SDL_WindowID window_id, Uint8 button, double x, double y) {
  SDL_Event event = {.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN,
                                .windowID = window_id,
                                .button = button,
                                .down = true,
                                .x = (float)x,
                                .y = (float)y}};
  CHECK(SDL_PushEvent(&event));
}

typedef struct {
  const TaiNode *target;
  double x, y;
  bool found;
} TargetPoint;

/* Keeps the last match: the innermost box, as in the Python probe. */
static bool find_target_point(const TaiLayoutItem *item, void *opaque) {
  TargetPoint *point = opaque;
  if (item->width <= 0) return true;
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

/* Presses button over the center of the selected element's innermost box. */
static void click(TaiTabSet *tabs, SDL_WindowID window_id, int width,
                  bool wraps, Uint8 button, Select select, const void *data) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view) && view.page);
  TargetPoint point = {.target = find(tai_page_root(view.page), select, data)};
  CHECK(point.target);
  char *error = NULL;
  CHECK(tai_layout_visit(tai_page_layout(view.page), find_target_point,
                         &point, &error));
  CHECK(error == NULL && point.found);
  press(window_id, button, point.x,
        point.y - tai_page_scroll_y(view.page) +
            tabs_chrome_bottom(width, wraps));
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

/* Selects each tab in turn to read it, then restores the active tab before
 * the presentation loop runs again. */
static void report_tab(TaiTabSet *tabs, size_t index) {
  CHECK(tai_tabset_select(tabs, index));
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  char text[256];
  bool has_heading = heading(view.page, text, sizeof(text));
  fputs("{\"url\": ", stdout);
  tai_json_string(stdout, view.url ? view.url : "");
  fputs(", \"heading\": ", stdout);
  if (has_heading) tai_json_string(stdout, text);
  else fputs("null", stdout);
  fputs(", \"history\": [", stdout);
  for (size_t entry = 0; entry < view.history_count; entry++) {
    char *url = tai_tabset_history_url(tabs, entry);
    CHECK(url);
    if (entry) fputs(", ", stdout);
    tai_json_string(stdout, url);
    free(url);
  }
  printf("], \"history_index\": %zu}", view.history_index);
}

static void report(const char *scenario, const char *step, TaiTabSet *tabs,
                   const AddressEditor *editor) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  size_t active = view.active_index, count = view.tab_count;
  bool input_focused = view.page && tai_page_text_input_active(view.page);
  /* The field shows the draft while focused or dirty, else the URL. */
  char *shown = tai_strdup(editor->dirty || editor->focused
                               ? editor->text
                               : (view.url ? view.url : ""));
  CHECK(shown);
  printf("STATE %s %s {\"tab_count\": %zu, \"active\": %zu, \"tabs\": [",
         scenario, step, count, active);
  for (size_t index = 0; index < count; index++) {
    if (index) fputs(", ", stdout);
    report_tab(tabs, index);
  }
  CHECK(tai_tabset_select(tabs, active));
  fputs("], \"address\": ", stdout);
  tai_json_string(stdout, shown);
  free(shown);
  printf(", \"focused\": %s, \"dirty\": %s, \"input_focused\": %s}\n",
         editor->focused ? "true" : "false",
         editor->dirty ? "true" : "false", input_focused ? "true" : "false");
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
  /* For scenarios that open one link: its href and the new tab's heading. */
  const char *href;
  const char *heading;
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

/* Middle-clicks driver->href on the first page, then reports once the new
 * tab shows driver->heading. */
static bool opens(Driver *driver, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  switch (driver->step) {
  case 0: {
    TaiTabSetView view = {0};
    CHECK(tai_tabset_view(tabs, &view));
    char text[256];
    if (view.loading || !heading(view.page, text, sizeof(text))) return true;
    mark();
    click(tabs, window_id, width, wraps, SDL_BUTTON_MIDDLE, is_link,
          driver->href);
    ADVANCE();
  }
  case 1:
    if (!drained() || !loaded(tabs, 2, driver->heading)) return true;
    report(driver->name, "opened", tabs, editor);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static bool mailto(Driver *driver, TaiTabSet *tabs, SDL_WindowID window_id,
                   int width, bool wraps, AddressEditor *editor) {
  switch (driver->step) {
  case 0:
    if (!loaded(tabs, 1, "links")) return true;
    mark();
    click(tabs, window_id, width, wraps, SDL_BUTTON_MIDDLE, is_link,
          "mailto:frag@example.com#part");
    ADVANCE();
  case 1:
    if (!drained()) return true;
    report(driver->name, "clicked", tabs, editor);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static bool draft(Driver *driver, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  switch (driver->step) {
  case 0:
    if (!loaded(tabs, 1, "links")) return true;
    mark();
    set_draft(editor, "typed draft");
    click(tabs, window_id, width, wraps, SDL_BUTTON_MIDDLE, is_plain_text,
          NULL);
    ADVANCE();
  case 1:
    if (!drained()) return true;
    report(driver->name, "text", tabs, editor);
    mark();
    set_draft(editor, "typed draft");
    click(tabs, window_id, width, wraps, SDL_BUTTON_MIDDLE, is_link,
          absolute_href);
    ADVANCE();
  case 2:
    if (!drained() || !loaded(tabs, 2, "page-b")) return true;
    report(driver->name, "link", tabs, editor);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static bool chrome(Driver *driver, TaiTabSet *tabs, SDL_WindowID window_id,
                   int width, bool wraps, AddressEditor *editor) {
  switch (driver->step) {
  case 0:
    if (!loaded(tabs, 1, "links")) return true;
    mark();
    click(tabs, window_id, width, wraps, SDL_BUTTON_LEFT, is_tag, "input");
    ADVANCE();
  case 1:
    if (!drained()) return true;
    report(driver->name, "focused", tabs, editor);
    mark();
    click(tabs, window_id, width, wraps, SDL_BUTTON_MIDDLE, is_plain_text,
          NULL);
    ADVANCE();
  case 2:
    if (!drained()) return true;
    report(driver->name, "text", tabs, editor);
    mark();
    set_draft(editor, "typed draft");
    /* Python's CHROME_POINT: above the chrome bottom, which its middle click
     * never hit-tests. */
    press(window_id, SDL_BUTTON_MIDDLE, 400.0, 2.0);
    ADVANCE();
  case 3:
    if (!drained()) return true;
    report(driver->name, "chrome", tabs, editor);
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

static void run(const char *name, Scenario scenario, const char *path,
                const char *href, const char *new_heading) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(css, false,
                                                    url_for("/links"), &error);
  CHECK(tabs && error == NULL);
  CHECK(tai_tabset_set_external_opener_for_test(tabs, opener));
  /* SDL_Quit at the end of each presentation resets hints. */
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  Driver driver = {.name = name, .scenario = scenario, .href = href,
                   .heading = new_heading, .step = -1};
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
  CHECK(snprintf(absolute_href, sizeof(absolute_href), "%s/b", base) > 0);
  size_t length = 0;
  css = tai_read_file(argv[2], &length);
  CHECK(css);
  opener = argv[3];
  run("absolute", opens, "/links", absolute_href, "page-b");
  run("relative", opens, "/links", "/c", "page-c");
  run("fragment", opens, "/links", "#part", "links");
  run("mailto", mailto, "/links", NULL, NULL);
  run("cancelled", opens, "/cancel", "/b", "page-b");
  run("draft", draft, "/links", NULL, NULL);
  run("chrome", chrome, "/links", NULL, NULL);
  free(css);
  puts("DONE");
  fflush(stdout);
  return EXIT_SUCCESS;
}
