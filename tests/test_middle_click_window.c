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
#include "window_driver.h"

static char absolute_href[160];
static char *css;
static const char *opener;

/* -- page inspection ------------------------------------------------------ */

static bool is_plain_text(const TaiNode *node, const void *unused) {
  (void)unused;
  const char *id = tai_map_get(&node->attributes, "id");
  return !strcmp(node->tag, "p") && id && !strcmp(id, "part");
}

static bool loaded_in(TaiTabSet *tabs, size_t tab_count,
                      const char *expected) {
  return view_of(tabs).tab_count == tab_count && loaded(tabs, expected);
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

static bool frame(void *opaque, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  Driver *driver = opaque;
  check_deadline(&driver->deadline, driver->name, driver->step);
  return driver->scenario(driver, tabs, window_id, width, wraps, editor);
}

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
    if (!drained() || !loaded_in(tabs, 2, driver->heading)) return true;
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
    if (!loaded_in(tabs, 1, "links")) return true;
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
    if (!loaded_in(tabs, 1, "links")) return true;
    mark();
    set_draft(editor, "typed draft", true);
    click(tabs, window_id, width, wraps, SDL_BUTTON_MIDDLE, is_plain_text,
          NULL);
    ADVANCE();
  case 1:
    if (!drained()) return true;
    report(driver->name, "text", tabs, editor);
    mark();
    set_draft(editor, "typed draft", true);
    click(tabs, window_id, width, wraps, SDL_BUTTON_MIDDLE, is_link,
          absolute_href);
    ADVANCE();
  case 2:
    if (!drained() || !loaded_in(tabs, 2, "page-b")) return true;
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
    if (!loaded_in(tabs, 1, "links")) return true;
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
    set_draft(editor, "typed draft", true);
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
  next_step(&driver.step, &driver.deadline);
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
