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
static const char *const scenario = "address_drafts";

static void command(const char *verb, const char *argument) {
  printf("%s%s%s\n", verb, argument ? " " : "", argument ? argument : "");
  fflush(stdout);
  char reply[16];
  CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
}

static const char *url_for(const char *path) {
  static char buffer[4][256];
  static int next;
  char *url = buffer[next++ % 4];
  CHECK(snprintf(url, sizeof(buffer[0]), "%s%s", base, path) > 0);
  return url;
}

static bool url_is(const TaiTabSetView *view, const char *path) {
  return view->url && !strcmp(view->url, url_for(path));
}

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

/* True when the committed page is idle and its first <h1> reads expected. */
static bool loaded(const TaiTabSetView *view, const char *expected) {
  if (view->loading || !view->page) return false;
  const TaiNode *h1 = find_tag(tai_page_root(view->page), "h1");
  if (!h1) return false;
  char text[256] = "";
  append_text(h1, text, sizeof(text));
  return strstr(text, expected) != NULL;
}

typedef struct {
  const TaiNode *target;
  double x, y;
  bool found;
} TargetPoint;

static bool find_target_point(const TaiLayoutItem *item, void *opaque) {
  TargetPoint *point = opaque;
  if (point->found || item->kind != TAI_LAYOUT_TEXT) return true;
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

static void click(SDL_WindowID window_id, double x, double y) {
  SDL_Event event = {.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN,
                                .windowID = window_id,
                                .button = SDL_BUTTON_LEFT,
                                .down = true,
                                .x = (float)x,
                                .y = (float)y}};
  CHECK(SDL_PushEvent(&event));
}

/* Python probe set_draft: typed text, dirty, focus as requested. */
static void set_draft(AddressEditor *editor, bool focused) {
  char *text = tai_strdup("typed draft");
  CHECK(text);
  free(editor->text);
  editor->text = text;
  editor->cursor = strlen(text);
  editor->dirty = true;
  editor->focused = focused;
}

static void json_string(const char *value) {
  putchar('"');
  for (const char *p = value; *p; p++) {
    if (*p == '"' || *p == '\\') putchar('\\');
    putchar(*p);
  }
  putchar('"');
}

/* The field shows the draft while focused or dirty, else the active URL. */
static void state(const char *step, const TaiTabSetView *view,
                  const AddressEditor *editor) {
  const char *shown = editor->dirty || editor->focused ? editor->text
                                                       : view->url;
  printf("STATE %s %s 0 {\"address\": ", scenario, step);
  json_string(shown ? shown : "");
  printf(", \"focused\": %s, \"dirty\": %s}\n",
         editor->focused ? "true" : "false",
         editor->dirty ? "true" : "false");
  fflush(stdout);
  char reply[16];
  CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
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

static void next_step(Driver *driver) {
  driver->step++;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &driver->deadline) == 0);
  driver->deadline.tv_sec += 10;
}

static void check_deadline(const Driver *driver) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  if (now.tv_sec > driver->deadline.tv_sec ||
      (now.tv_sec == driver->deadline.tv_sec &&
       now.tv_nsec > driver->deadline.tv_nsec)) {
    fprintf(stderr, "timed out in step %d\n", driver->step);
    exit(EXIT_FAILURE);
  }
}

static void navigate(TaiTabSet *tabs, const char *path) {
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, url_for(path), &error));
  CHECK(error == NULL);
}

static void click_tab_label(const TaiTabSetView *view, SDL_WindowID window_id,
                            int width, size_t wanted) {
  for (int y = 6; y < 30; y++) {
    for (int x = 30; x < width; x++) {
      size_t index = 0;
      if (tai_pres_tabs_tab_link_hit(view, width, x + 0.5, y + 0.5, &index) &&
          index == wanted) {
        click(window_id, x + 0.5, y + 0.5);
        return;
      }
    }
  }
  CHECK(!"tab label not found");
}

static void click_page_link(const TaiTabSetView *view, SDL_WindowID window_id,
                            int width, bool wraps) {
  const TaiNode *link = find_tag(tai_page_root(view->page), "a");
  CHECK(link);
  TargetPoint point = {.target = link};
  char *error = NULL;
  CHECK(tai_layout_visit(tai_page_layout(view->page), find_target_point,
                         &point, &error));
  CHECK(error == NULL && point.found);
  click(window_id, point.x,
        point.y - tai_page_scroll_y(view->page) +
            tabs_chrome_bottom(width, wraps));
}

/* True when the inactive tab at index finished loading heading. Selecting
 * and reselecting inside one observer call leaves the loop's draft watch on
 * the same active tab and URL, so it cannot discard or re-baseline. */
static bool inactive_loaded(TaiTabSet *tabs, size_t index, size_t active,
                            const char *heading) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_select(tabs, index) && tai_tabset_view(tabs, &view));
  bool done = loaded(&view, heading);
  CHECK(tai_tabset_select(tabs, active));
  return done;
}

static bool frame(void *opaque, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  Driver *driver = opaque;
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  check_deadline(driver);
  switch (driver->step) {
  case START:
    if (!loaded(&view, "page-a")) return true;
    click(window_id, 15.0, 18.0);  /* New Tab: the home URL /b */
    break;
  case SECOND_TAB:
    if (view.tab_count != 2 || !loaded(&view, "page-b")) return true;
    set_draft(editor, true);
    click_tab_label(&view, window_id, width, 0);
    break;
  case SWITCHED:
    if (view.active_index != 0) return true;
    state("switch_tab", &view, editor);
    set_draft(editor, true);
    click(window_id, 4.0, tabs_chrome_bottom(width, wraps) + 4.0);
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
    if (view.active_index != 2 || !loaded(&view, "frag")) return true;
    set_draft(editor, true);
    click_page_link(&view, window_id, width, wraps);
    break;
  case FRAGMENT_FOLLOWED:
    if (!url_is(&view, "/frag#target")) return true;
    state("fragment_link", &view, editor);
    set_draft(editor, true);
    click(window_id, 22.0, tabs_back_button_y(wraps) + 12.0);
    break;
  case BACK_STARTED:
    if (!url_is(&view, "/frag")) return true;
    /* Back to the fragment-less entry reloads the document with GET. */
    CHECK(view.loading);
    state("back_button_click", &view, editor);
    break;
  case BACK_LOADED:
    if (!loaded(&view, "frag")) return true;
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
    set_draft(editor, true);
    command("RELEASE", "/delay-c");
    break;
  case INACTIVE_LOADED:
    if (!inactive_loaded(tabs, 1, 0, "delay-c")) return true;
    CHECK(tai_tabset_view(tabs, &view));
    state("inactive_load", &view, editor);
    command("RESET", "/delay-c");
    set_draft(editor, true);
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
    if (!loaded(&view, "delay-b")) return true;
    command("RESET", "/delay-b");
    next_step(driver);
    return false;
  default:
    CHECK(!"unexpected step");
  }
  next_step(driver);
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
  next_step(&driver);
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
