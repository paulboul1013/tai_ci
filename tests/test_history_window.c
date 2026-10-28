#define _POSIX_C_SOURCE 200809L
/* Address-draft half of the chrome/history oracle comparison. Driven by
 * tests/history_integration.py with the same one-line protocol as
 * tests/test_tabset_history.c, it replays the probe's address_drafts
 * scenario in the real tabbed presentation loop under SDL's dummy driver and
 * reports the address field each checkpoint shows.
 *
 * Tab-label, page, link and Back clicks go through SDL events. Like the
 * Python probe, the draft is set on the editor directly, and opening a tab,
 * selecting an inactive tab and starting a load in the active tab call the
 * tab set directly. All of it runs on the SDL owner thread through the
 * presentation test observer. */
#include "window_driver.h"

static const char *const scenario = "address_drafts";

static bool url_is(const TaiTabSetView *view, const char *path) {
  return view->url && !strcmp(view->url, url_for(path));
}

/* True when the committed page is idle and its first <h1> holds expected. */
static bool view_loaded(const TaiTabSetView *view, const char *expected) {
  char text[256];
  return !view->loading && heading(view->page, text, sizeof(text)) &&
         strstr(text, expected) != NULL;
}

/* The field shows the draft while focused or dirty, else the active URL. */
static void state(const char *step, const TaiTabSetView *view,
                  const AddressEditor *editor) {
  const char *shown = editor->dirty || editor->focused ? editor->text
                                                       : view->url;
  printf("STATE %s %s 0 {\"address\": ", scenario, step);
  tai_json_string(stdout, shown ? shown : "");
  printf(", \"focused\": %s, \"dirty\": %s}\n",
         editor->focused ? "true" : "false",
         editor->dirty ? "true" : "false");
  fflush(stdout);
  await_ok();
}

enum {
  START,
  SECOND_TAB,
  SWITCHED,
  PAGE_CLICKED,
  FRAG_LOADED,
  FRAGMENT_FOLLOWED,
  BACK_STARTED,
  BACK_LOADED,
  INACTIVE_SELECTED,
  INACTIVE_LOADING,
  FIRST_AGAIN,
  INACTIVE_LOADED,
  PENDING_STARTED,
  PENDING_LOADED,
  FINISHED
};

typedef struct {
  int step;
  struct timespec deadline;
} Driver;

static void click_tab_label(const TaiTabSetView *view, SDL_WindowID window_id,
                            int width, size_t wanted) {
  for (int y = 6; y < 30; y++) {
    for (int x = 30; x < width; x++) {
      size_t index = 0;
      if (tai_pres_tabs_tab_link_hit(view, width, x + 0.5, y + 0.5, &index) &&
          index == wanted) {
        press(window_id, SDL_BUTTON_LEFT, x + 0.5, y + 0.5);
        return;
      }
    }
  }
  CHECK(!"tab label not found");
}

/* True when the inactive tab at index finished loading heading. Selecting
 * and reselecting inside one observer call leaves the loop's draft watch on
 * the same active tab and URL, so it cannot discard or re-baseline. */
static bool inactive_loaded(TaiTabSet *tabs, size_t index, size_t active,
                            const char *heading) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_select(tabs, index) && tai_tabset_view(tabs, &view));
  bool done = view_loaded(&view, heading);
  CHECK(tai_tabset_select(tabs, active));
  return done;
}

static bool frame(void *opaque, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  Driver *driver = opaque;
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  check_deadline(&driver->deadline, scenario, driver->step);
  switch (driver->step) {
  case START:
    if (!view_loaded(&view, "page-a")) return true;
    /* New Tab: the home URL /b */
    press(window_id, SDL_BUTTON_LEFT, 15.0, 18.0);
    break;
  case SECOND_TAB:
    if (view.tab_count != 2 || !view_loaded(&view, "page-b")) return true;
    set_draft(editor, "typed draft", true);
    click_tab_label(&view, window_id, width, 0);
    break;
  case SWITCHED:
    if (view.active_index != 0) return true;
    state("switch_tab", &view, editor);
    set_draft(editor, "typed draft", true);
    press(window_id, SDL_BUTTON_LEFT, 4.0,
          tabs_chrome_bottom(width, wraps) + 4.0);
    break;
  case PAGE_CLICKED:
    if (editor->focused) return true;
    state("page_click", &view, editor);
    {
      char *error = NULL;
      CHECK(tai_tabset_new_tab(tabs, &error));
      CHECK(error == NULL);
    }
    navigate(tabs, "/frag");
    break;
  case FRAG_LOADED:
    if (view.active_index != 2 || !view_loaded(&view, "frag")) return true;
    set_draft(editor, "typed draft", true);
    click(tabs, window_id, width, wraps, SDL_BUTTON_LEFT, is_tag, "a");
    break;
  case FRAGMENT_FOLLOWED:
    if (!url_is(&view, "/frag#target")) return true;
    state("fragment_link", &view, editor);
    set_draft(editor, "typed draft", true);
    press(window_id, SDL_BUTTON_LEFT, 22.0,
          tabs_back_button_y(wraps) + 12.0);
    break;
  case BACK_STARTED:
    if (!url_is(&view, "/frag")) return true;
    /* Back to the fragment-less entry reloads the document with GET. */
    CHECK(view.loading);
    state("back_button_click", &view, editor);
    break;
  case BACK_LOADED:
    if (!view_loaded(&view, "frag")) return true;
    state("back_button_loaded", &view, editor);
    CHECK(tai_tabset_select(tabs, 1));
    break;
  case INACTIVE_SELECTED:
    /* One iteration later the draft watch follows tab 1. */
    CHECK(view.active_index == 1);
    command("MARK", NULL);
    navigate(tabs, "/delay-c");
    command("SEEN", "/delay-c");
    break;
  case INACTIVE_LOADING:
    CHECK(tai_tabset_select(tabs, 0));
    break;
  case FIRST_AGAIN:
    CHECK(view.active_index == 0);
    set_draft(editor, "typed draft", true);
    command("RELEASE", "/delay-c");
    break;
  case INACTIVE_LOADED:
    if (!inactive_loaded(tabs, 1, 0, "delay-c")) return true;
    CHECK(tai_tabset_view(tabs, &view));
    state("inactive_load", &view, editor);
    command("RESET", "/delay-c");
    set_draft(editor, "typed draft", true);
    command("MARK", NULL);
    navigate(tabs, "/delay-b");
    command("SEEN", "/delay-b");
    break;
  case PENDING_STARTED:
    CHECK(url_is(&view, "/delay-b") && view.loading);
    state("active_pending", &view, editor);
    command("RELEASE", "/delay-b");
    break;
  case PENDING_LOADED:
    if (!view_loaded(&view, "delay-b")) return true;
    command("RESET", "/delay-b");
    next_step(&driver->step, &driver->deadline);
    return false;
  default:
    CHECK(!"unexpected step");
  }
  next_step(&driver->step, &driver->deadline);
  return true;
}

int main(int argc, char **argv) {
  CHECK(argc == 3);
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  size_t length = 0;
  char *css = tai_read_file(argv[2], &length);
  CHECK(css);
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(css, false, url_for("/b"),
                                                    &error);
  CHECK(tabs && error == NULL);
  Driver driver = {.step = START - 1};
  next_step(&driver.step, &driver.deadline);
  TaiPresTabsObserver observer = {.frame = frame, .opaque = &driver};
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  bool presented = tai_pres_present_tabs_observed(tabs, url_for("/a"), 800,
                                                  600, &observer, &error);
  if (!presented)
    fprintf(stderr, "presentation failed: %s\n", error ? error : "unknown");
  CHECK(presented && error == NULL && driver.step == FINISHED);
  tai_tabset_destroy(tabs);
  free(css);
  puts("DONE");
  fflush(stdout);
  return EXIT_SUCCESS;
}
