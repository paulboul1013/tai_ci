#define _POSIX_C_SOURCE 200809L
/* Native half of the full-pipeline E2E comparison (docs/e2e-plan.md, layer
 * 1). Driven by tests/e2e_integration.py, it replays the script of
 * tests/e2e_oracle_probe.py in the real tabbed presentation loop under SDL's
 * dummy driver. Every click, typed character and Back press is an SDL event:
 * page clicks hit an element's innermost box, typing is one
 * SDL_EVENT_TEXT_INPUT per character, and Back presses the chrome Back
 * button, which runs the tab's history like the probe's schedule_go_back.
 *
 * Usage: test_e2e_window PORT CSS WIDTH
 * Protocol (one line each, answered with "ok"):
 *   STATE <scenario> <step> <json>   a checkpoint; the harness attaches the
 *                                    requests the server saw since the
 *                                    previous one
 * The JSON holds "page" (tai_page_json), "title", "back", "forward",
 * "scroll" and "inputs". */
#include "window_driver.h"

static char *css;
static int window_width;

/* -- reporting ------------------------------------------------------------ */

static void inputs_json(const TaiNode *node, bool *first) {
  if (node->kind != TAI_ELEMENT) return;
  const char *id = tai_map_get(&node->attributes, "id");
  if (!strcmp(node->tag, "input") && id) {
    const char *value = tai_map_get(&node->attributes, "value");
    printf("%s", *first ? "" : ", ");
    tai_json_string(stdout, id);
    printf(": {\"value\": ");
    tai_json_string(stdout, value ? value : "");
    printf(", \"checked\": %s}", node->checked ? "true" : "false");
    *first = false;
  }
  for (size_t index = 0; index < node->child_count; index++)
    inputs_json(node->children[index], first);
}

static void report(const char *scenario, const char *step, TaiTabSet *tabs) {
  TaiTabSetView view = view_of(tabs);
  CHECK(view.page);
  char *title = tai_page_title(view.page);
  CHECK(title);
  state_begin(scenario, step);
  printf("{\"page\": ");
  tai_page_json(stdout, view.page);
  printf(", \"title\": ");
  tai_json_string(stdout, title);
  printf(", \"back\": %s, \"forward\": %s, \"scroll\": %.17g, \"inputs\": {",
         tai_tabset_history_available(tabs, -1) ? "true" : "false",
         tai_tabset_history_available(tabs, 1) ? "true" : "false",
         tai_page_scroll_y(view.page));
  bool first = true;
  inputs_json(tai_page_root(view.page), &first);
  printf("}}");
  free(title);
  state_end();
}

/* -- the script ----------------------------------------------------------- */

typedef enum {
  READY,  /* text: title once scripts ran; id: an element the page has */
  CLICK,  /* id: element whose innermost box is clicked */
  TYPE,   /* text: typed one SDL_EVENT_TEXT_INPUT per character */
  BACK,   /* press the chrome Back button */
  URL,    /* text: path and fragment the active page must reach */
  REPORT, /* text: checkpoint name */
  END
} Op;

typedef struct {
  Op op;
  const char *text;
  const char *id;
} Action;

typedef struct {
  const char *name;
  const Action *actions;
  size_t next;
  size_t typed;
  char character[2];
  struct timespec deadline;
  bool finished;
} Driver;

/* Not loading, titled title, and holding the element: a page whose script
 * retitles it or adds the element has run that script. */
static bool ready(TaiTabSet *tabs, const char *title, const char *id) {
  TaiTabSetView view = view_of(tabs);
  if (view.loading || !view.page || !find_id(tai_page_root(view.page), id))
    return false;
  char *current = tai_page_title(view.page);
  CHECK(current);
  bool same = !strcmp(current, title);
  free(current);
  return same;
}

static bool at_url(TaiTabSet *tabs, const char *path) {
  TaiTabSetView view = view_of(tabs);
  return view.page &&
         !strcmp(tai_url_string(tai_page_url(view.page)), url_for(path));
}

static bool frame(void *opaque, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  (void)editor;
  Driver *driver = opaque;
  check_deadline(&driver->deadline, driver->name, (long)driver->next);
  if (!drained()) return true;
  const Action *action = &driver->actions[driver->next];
  switch (action->op) {
  case READY:
    if (!ready(tabs, action->text, action->id)) return true;
    break;
  case CLICK:
    click(tabs, window_id, width, wraps, SDL_BUTTON_LEFT, has_id,
          action->id);
    break;
  case TYPE:
    if (action->text[driver->typed]) {
      driver->character[0] = action->text[driver->typed++];
      text_input(window_id, driver->character);
      return true;
    }
    driver->typed = 0;
    break;
  case BACK:
    press(window_id, SDL_BUTTON_LEFT, 22.0, tabs_back_button_y(wraps) + 12.0);
    break;
  case URL:
    if (!at_url(tabs, action->text)) return true;
    break;
  case REPORT:
    report(driver->name, action->text, tabs);
    break;
  case END:
    driver->finished = true;
    return false;
  }
  driver->next++;
  restart_deadline(&driver->deadline);
  return true;
}

/* Titles, ids and typed values from tests/e2e_fixture.py. */
#define INDEX_READY {READY, "E2E ready", "added"}
#define FORM_READY {READY, "E2E form", "name"}

static const Action PIPELINE[] = {
    INDEX_READY,
    {REPORT, "open", NULL},
    {CLICK, NULL, "to-bottom"},
    {URL, "/#bottom", NULL},
    {REPORT, "fragment", NULL},
    {CLICK, NULL, "to-form"},
    FORM_READY,
    {REPORT, "form", NULL},
    {CLICK, NULL, "name"},
    {TYPE, "alice", NULL},
    {CLICK, NULL, "secret"},
    {TYPE, "s3cret", NULL},
    {CLICK, NULL, "agree"},
    {REPORT, "filled", NULL},
    {CLICK, NULL, "send"},
    {READY, "E2E submitted", "field-name"},
    {REPORT, "submit", NULL},
    {BACK, NULL, NULL},
    FORM_READY,
    {REPORT, "back_form", NULL},
    {BACK, NULL, NULL},
    INDEX_READY,
    {REPORT, "back_index", NULL},
    {END, NULL, NULL}};

static void run(const char *name, const char *path, const Action *actions) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(css, false,
                                                    url_for(path), &error);
  CHECK(tabs && error == NULL);
  /* SDL_Quit at the end of each presentation resets hints. */
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  Driver driver = {.name = name, .actions = actions};
  restart_deadline(&driver.deadline);
  TaiPresTabsObserver observer = {.frame = frame, .opaque = &driver};
  bool presented = tai_pres_present_tabs_observed(
      tabs, url_for(path), window_width, 600, &observer, &error);
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
  window_width = atoi(argv[3]);
  CHECK(window_width > 0);
  run("pipeline", "/", PIPELINE);
  free(css);
  puts("DONE");
  fflush(stdout);
  return EXIT_SUCCESS;
}
