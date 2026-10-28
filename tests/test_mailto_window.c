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
#include "window_driver.h"

static char *css;
static const char *opener;

/* -- reporting ------------------------------------------------------------ */

static void report(const char *scenario, const char *step, TaiTabSet *tabs,
                   const AddressEditor *editor) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  char text[256];
  bool has_heading = heading(view.page, text, sizeof(text));
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

static bool frame(void *opaque, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  Driver *driver = opaque;
  check_deadline(&driver->deadline, driver->name, driver->step);
  return driver->scenario(driver, tabs, window_id, width, wraps, editor);
}

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
  set_draft(editor, addresses[step][1], true);
  key(window_id, SDLK_RETURN, SDL_KMOD_NONE, false);
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
  set_draft(editor, "typed draft", true);
  click(tabs, window_id, width, wraps, SDL_BUTTON_LEFT, is_link,
        links_hrefs[step][1]);
  ADVANCE();
}

static bool cancelled(Driver *driver, TaiTabSet *tabs, SDL_WindowID window_id,
                      int width, bool wraps, AddressEditor *editor) {
  switch (driver->step) {
  case 0:
    if (!loaded(tabs, "cancel")) return true;
    mark();
    click(tabs, window_id, width, wraps, SDL_BUTTON_LEFT, is_link,
          "mailto:cancel@example.com");
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
    click(tabs, window_id, width, wraps, SDL_BUTTON_LEFT, is_tag, "button");
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
