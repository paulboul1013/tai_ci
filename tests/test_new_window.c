#define _POSIX_C_SOURCE 200809L
/* Native half of the new-window oracle comparison. Driven by
 * tests/new_window_integration.py, it replays every scenario of
 * tests/new_window_oracle_probe.py in the real multi-window presentation loop
 * under SDL's dummy driver and reports the same checkpoints as JSON.
 *
 * Each scenario runs a fresh TaiBrowserApp whose New Tab URL is the fixture's
 * /home. Keys, text, wheel, clicks and close requests are pushed as SDL
 * events addressed to a window ID. Like the Python probe, drafts are set on
 * the editor directly and loads, New Tab and bookmark toggles call the tab
 * set directly, all on the SDL owner thread through the observer.
 *
 * Protocol (one line each, answered with "ok"):
 *   STATE <scenario> <step> <json>   a checkpoint
 *   MARK | SEEN <path> | RELEASE <path>
 *   REQUESTS <scenario> <step>       requests since MARK are the checkpoint */
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

static bool page_contains(const TaiNode *node, const char *needle) {
  if (node->kind == TAI_TEXT && node->text && strstr(node->text, needle))
    return true;
  for (size_t index = 0; index < node->child_count; index++)
    if (page_contains(node->children[index], needle)) return true;
  return false;
}

/* -- SDL input ------------------------------------------------------------ */

static void push(SDL_Event *event) { CHECK(SDL_PushEvent(event)); }

static void key(SDL_WindowID window_id, SDL_Keycode keycode, SDL_Keymod mod,
                bool repeat) {
  SDL_Event event = {.key = {.type = SDL_EVENT_KEY_DOWN,
                             .windowID = window_id,
                             .key = keycode,
                             .mod = mod,
                             .down = true,
                             .repeat = repeat}};
  push(&event);
}

static void ctrl_n(SDL_WindowID window_id) {
  key(window_id, SDLK_N, SDL_KMOD_LCTRL, false);
}

static void text(SDL_WindowID window_id, const char *value) {
  /* SDL keeps the pointer until the event is handled. */
  static char buffer[256];
  CHECK(snprintf(buffer, sizeof(buffer), "%s", value) > 0);
  SDL_Event event = {.text = {.type = SDL_EVENT_TEXT_INPUT,
                              .windowID = window_id,
                              .text = buffer}};
  push(&event);
}

static void wheel(SDL_WindowID window_id, float y) {
  SDL_Event event = {.wheel = {.type = SDL_EVENT_MOUSE_WHEEL,
                               .windowID = window_id,
                               .y = y,
                               .direction = SDL_MOUSEWHEEL_NORMAL}};
  push(&event);
}

static void click(SDL_WindowID window_id, double x, double y) {
  SDL_Event event = {.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN,
                                .windowID = window_id,
                                .button = SDL_BUTTON_LEFT,
                                .down = true,
                                .x = (float)x,
                                .y = (float)y}};
  push(&event);
}

static void close_request(SDL_WindowID window_id) {
  SDL_Event event = {.window = {.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                                .windowID = window_id}};
  push(&event);
}

/* The loop handles one event per iteration; this reports that every pushed
 * event has been handled. */
static bool drained(void) {
  SDL_PumpEvents();
  return SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) == 0;
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

static void click_page_link(const TaiPresWindowInfo *window) {
  TaiTabSetView view = view_of(window->tabs);
  const TaiNode *link = find_tag(tai_page_root(view.page), "a");
  CHECK(link);
  TargetPoint point = {.target = link};
  char *error = NULL;
  CHECK(tai_layout_visit(tai_page_layout(view.page), find_target_point,
                         &point, &error));
  CHECK(error == NULL && point.found);
  click(window->window_id, point.x,
        point.y - tai_page_scroll_y(view.page) +
            tabs_chrome_bottom(window->width, window->wraps));
}

/* -- tab-set actions ------------------------------------------------------ */

static void navigate(TaiTabSet *tabs, const char *path) {
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, url_for(path), &error));
  CHECK(error == NULL);
}

/* Python probe set_draft: dirty exactly when there is text. */
static void set_draft(AddressEditor *editor, const char *value, bool focused) {
  char *copy = tai_strdup(value);
  CHECK(copy);
  free(editor->text);
  editor->text = copy;
  editor->cursor = strlen(copy);
  editor->dirty = *copy != '\0';
  editor->focused = focused;
}

/* -- reporting ------------------------------------------------------------ */

static void json_string(const char *value) {
  putchar('"');
  for (const char *p = value; *p; p++) {
    if (*p == '"' || *p == '\\') putchar('\\');
    putchar(*p);
  }
  putchar('"');
}

static void window_json(const TaiPresWindowInfo *window) {
  TaiTabSetView view = view_of(window->tabs);
  const AddressEditor *editor = window->editor;
  const char *shown = editor->dirty || editor->focused ? editor->text
                                                       : view.url;
  int width = 0, height = 0;
  SDL_Window *native = SDL_GetWindowFromID(window->window_id);
  CHECK(native && SDL_GetWindowSize(native, &width, &height));
  char title[256];
  bool has_heading = heading(&view, title, sizeof(title));
  printf("{\"size\": [%d, %d], \"tab_count\": %zu, \"active_index\": %zu, "
         "\"url\": ", width, height, view.tab_count, view.active_index);
  json_string(view.url ? view.url : "");
  printf(", \"heading\": ");
  if (has_heading) json_string(title);
  else printf("null");
  printf(", \"history\": [");
  for (size_t index = 0; index < view.history_count; index++) {
    char *entry = tai_tabset_history_url(window->tabs, index);
    CHECK(entry);
    if (index) printf(", ");
    json_string(entry);
    free(entry);
  }
  printf("], \"history_index\": %zu, \"scroll\": %.3f, \"address\": ",
         view.history_index,
         view.page ? tai_page_scroll_y(view.page) : 0.0);
  json_string(shown ? shown : "");
  printf(", \"address_focused\": %s, \"address_dirty\": %s, "
         "\"bookmarked\": %s}",
         editor->focused ? "true" : "false",
         editor->dirty ? "true" : "false",
         view.bookmarked ? "true" : "false");
}

static void state_begin(const char *scenario, const char *step) {
  printf("STATE %s %s ", scenario, step);
}

static void state_end(void) {
  putchar('\n');
  fflush(stdout);
  char reply[16];
  CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
}

static void report_app(const char *scenario, const char *step,
                       const TaiPresWindowInfo *windows, size_t count) {
  state_begin(scenario, step);
  printf("{\"window_count\": %zu, \"running\": true, \"windows\": [", count);
  for (size_t index = 0; index < count; index++) {
    if (index) printf(", ");
    window_json(&windows[index]);
  }
  printf("]}");
  state_end();
}

static void report_window(const char *scenario, const char *step,
                          const TaiPresWindowInfo *window) {
  state_begin(scenario, step);
  window_json(window);
  state_end();
}

static void report_raw(const char *scenario, const char *step,
                       const char *json) {
  state_begin(scenario, step);
  fputs(json, stdout);
  state_end();
}

static void report_count(const char *scenario, const char *step,
                         size_t count) {
  char json[32];
  CHECK(snprintf(json, sizeof(json), "%zu", count) > 0);
  report_raw(scenario, step, json);
}

/* -- scenario driver ------------------------------------------------------ */

typedef struct Driver Driver;
typedef bool (*Scenario)(Driver *driver, const TaiPresWindowInfo *windows,
                         size_t count);

struct Driver {
  const char *name;
  Scenario scenario;
  int step;
  struct timespec deadline;
  size_t windows_at_quit;
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

/* A newly opened window at index is showing the home page. */
static bool new_window_ready(const TaiPresWindowInfo *windows, size_t count,
                             size_t wanted) {
  return count == wanted && loaded(windows[wanted - 1].tabs, "home");
}

#define ADVANCE() do { next_step(driver); return true; } while (0)

static bool ctrl_n_basic(Driver *driver, const TaiPresWindowInfo *w,
                         size_t count) {
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    ctrl_n(w[0].window_id);
    ADVANCE();
  case 1:
    if (!new_window_ready(w, count, 2)) return true;
    report_app(driver->name, "after", w, count);
    driver->finished = true;
    return false;
  }
  CHECK(!"unexpected step");
  return false;
}

static bool original_unaffected(Driver *driver, const TaiPresWindowInfo *w,
                                size_t count) {
  char *error = NULL;
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    navigate(w[0].tabs, "/b");
    ADVANCE();
  case 1:
    if (!loaded(w[0].tabs, "page-b")) return true;
    CHECK(tai_tabset_new_tab(w[0].tabs, &error) && !error);
    ADVANCE();
  case 2:
    if (view_of(w[0].tabs).tab_count != 2 || !loaded(w[0].tabs, "home"))
      return true;
    set_draft(w[0].editor, "typed draft", true);
    report_window(driver->name, "before", &w[0]);
    ctrl_n(w[0].window_id);
    ADVANCE();
  case 3:
    if (!new_window_ready(w, count, 2)) return true;
    report_app(driver->name, "after", w, count);
    driver->finished = true;
    return false;
  }
  CHECK(!"unexpected step");
  return false;
}

static bool modifiers(Driver *driver, const TaiPresWindowInfo *w,
                      size_t count) {
  static const struct {
    const char *name;
    SDL_Keycode key;
    SDL_Keymod mod;
    bool repeat;
  } cases[] = {
      {"plain_n", SDLK_N, SDL_KMOD_NONE, false},
      {"right_ctrl", SDLK_N, SDL_KMOD_RCTRL, false},
      {"ctrl_shift", SDLK_N, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT, false},
      {"ctrl_alt", SDLK_N, SDL_KMOD_LCTRL | SDL_KMOD_LALT, false},
      {"key_repeat", SDLK_N, SDL_KMOD_LCTRL, true},
      {"ctrl_m", SDLK_M, SDL_KMOD_LCTRL, false},
  };
  static size_t before;
  size_t n = sizeof(cases) / sizeof(cases[0]);
  if (driver->step == 0) {
    if (!loaded(w[0].tabs, "page-a")) return true;
    ADVANCE();
  }
  size_t index = (size_t)(driver->step - 1) / 2;
  if (index >= n) {
    driver->finished = true;
    return false;
  }
  if ((driver->step - 1) % 2 == 0) {
    before = count;
    key(w[0].window_id, cases[index].key, cases[index].mod,
        cases[index].repeat);
    ADVANCE();
  }
  if (!drained()) return true;
  if (count != before && !new_window_ready(w, count, count)) return true;
  report_count(driver->name, cases[index].name, count);
  ADVANCE();
}

static bool routing(Driver *driver, const TaiPresWindowInfo *w,
                    size_t count) {
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "links")) return true;
    set_draft(w[0].editor, "first draft", true);
    ctrl_n(w[0].window_id);
    ADVANCE();
  case 1:
    if (!new_window_ready(w, count, 2)) return true;
    set_draft(w[1].editor, "", true);
    text(w[1].window_id, url_for("/b"));
    ADVANCE();
  case 2:
    if (!drained()) return true;
    report_app(driver->name, "typed", w, count);
    key(w[1].window_id, SDLK_RETURN, SDL_KMOD_NONE, false);
    ADVANCE();
  case 3:
    if (!loaded(w[1].tabs, "page-b")) return true;
    report_app(driver->name, "entered", w, count);
    wheel(w[1].window_id, -1.0f);
    ADVANCE();
  case 4:
    if (!drained() || tai_page_scroll_y(view_of(w[1].tabs).page) <= 0)
      return true;
    report_app(driver->name, "wheel", w, count);
    set_draft(w[0].editor, "", false);
    click_page_link(&w[0]);
    ADVANCE();
  case 5:
    if (!loaded(w[0].tabs, "page-b")) return true;
    report_app(driver->name, "clicked", w, count);
    ctrl_n(9999);
    ADVANCE();
  case 6:
    if (!drained()) return true;
    report_count(driver->name, "unknown_window_count", count);
    driver->finished = true;
    return false;
  }
  CHECK(!"unexpected step");
  return false;
}

static bool shared_bookmarks(Driver *driver, const TaiPresWindowInfo *w,
                             size_t count) {
  char *error = NULL;
  bool bookmarked = false;
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    ctrl_n(w[0].window_id);
    ADVANCE();
  case 1:
    if (!new_window_ready(w, count, 2)) return true;
    navigate(w[1].tabs, "/c");
    ADVANCE();
  case 2:
    if (!loaded(w[1].tabs, "page-c")) return true;
    CHECK(tai_tabset_toggle_bookmark(w[1].tabs, &bookmarked, &error));
    CHECK(bookmarked && !error);
    report_app(driver->name, "toggled", w, count);
    navigate(w[0].tabs, "/c");
    ADVANCE();
  case 3:
    if (!loaded(w[0].tabs, "page-c")) return true;
    report_app(driver->name, "first_on_c", w, count);
    CHECK(tai_tabset_open_bookmarks(w[0].tabs, &error) && !error);
    ADVANCE();
  case 4:
    if (!loaded(w[0].tabs, "Bookmarks")) return true;
    report_raw(driver->name, "first_list_has_c",
               page_contains(tai_page_root(view_of(w[0].tabs).page),
                             url_for("/c")) ? "true" : "false");
    driver->finished = true;
    return false;
  }
  CHECK(!"unexpected step");
  return false;
}

static bool shared_cookies(Driver *driver, const TaiPresWindowInfo *w,
                           size_t count) {
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "cookie-set")) return true;
    ctrl_n(w[0].window_id);
    ADVANCE();
  case 1:
    if (!new_window_ready(w, count, 2)) return true;
    command("MARK", NULL);
    navigate(w[1].tabs, "/cookie-check");
    ADVANCE();
  case 2:
    if (!loaded(w[1].tabs, "cookie-check")) return true;
    printf("REQUESTS %s second_requests\n", driver->name);
    fflush(stdout);
    {
      char reply[16];
      CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
    }
    driver->finished = true;
    return false;
  }
  CHECK(!"unexpected step");
  return false;
}

static bool third_window(Driver *driver, const TaiPresWindowInfo *w,
                         size_t count) {
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    ctrl_n(w[0].window_id);
    ADVANCE();
  case 1:
    if (!new_window_ready(w, count, 2)) return true;
    navigate(w[1].tabs, "/b");
    ADVANCE();
  case 2:
    if (!loaded(w[1].tabs, "page-b")) return true;
    ctrl_n(w[1].window_id);
    ADVANCE();
  case 3:
    if (!new_window_ready(w, count, 3)) return true;
    report_app(driver->name, "after", w, count);
    driver->finished = true;
    return false;
  }
  CHECK(!"unexpected step");
  return false;
}

static bool close_one(Driver *driver, const TaiPresWindowInfo *w,
                      size_t count) {
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    ctrl_n(w[0].window_id);
    ADVANCE();
  case 1:
    if (!new_window_ready(w, count, 2)) return true;
    command("MARK", NULL);
    navigate(w[0].tabs, "/delay");
    command("SEEN", "/delay");
    close_request(w[0].window_id);
    ADVANCE();
  case 2:
    if (count != 1) return true;
    report_app(driver->name, "closed_first", w, count);
    command("RELEASE", "/delay");
    navigate(w[0].tabs, "/b");
    ADVANCE();
  case 3:
    if (!loaded(w[0].tabs, "page-b")) return true;
    report_app(driver->name, "second_navigates", w, count);
    close_request(w[0].window_id);
    driver->finished = true;
    ADVANCE();
  default:
    /* The loop returns once the last window has closed. */
    return true;
  }
}

static bool quit_all(Driver *driver, const TaiPresWindowInfo *w,
                     size_t count) {
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "page-a")) return true;
    ctrl_n(w[0].window_id);
    ADVANCE();
  case 1:
    if (!new_window_ready(w, count, 2)) return true;
    driver->windows_at_quit = count;
    {
      SDL_Event event = {.type = SDL_EVENT_QUIT};
      push(&event);
    }
    driver->finished = true;
    ADVANCE();
  default:
    return true;
  }
}

/* Decision D1 (native only): Ctrl+N stops at TAI_PRES_MAX_WINDOWS. */
static bool window_limit(Driver *driver, const TaiPresWindowInfo *w,
                         size_t count) {
  if (driver->step == 0) {
    if (!loaded(w[0].tabs, "page-a")) return true;
    ADVANCE();
  }
  if (!drained() || !loaded(w[count - 1].tabs, count == 1 ? "page-a" : "home"))
    return true;
  if (driver->step <= TAI_PRES_MAX_WINDOWS) {
    CHECK(count == (size_t)driver->step);
    ctrl_n(w[count - 1].window_id);
    ADVANCE();
  }
  CHECK(count == TAI_PRES_MAX_WINDOWS);
  driver->finished = true;
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
  if (scenario == close_one)
    report_raw(name, "closed_last", "{\"window_count\": 0, \"running\": false}");
  if (scenario == quit_all) {
    char json[64];
    CHECK(snprintf(json, sizeof(json),
                   "{\"window_count\": %zu, \"running\": false}",
                   driver.windows_at_quit) > 0);
    report_raw(name, "result", json);
  }
  tai_browser_app_destroy(app);
}

int main(int argc, char **argv) {
  CHECK(argc == 3);
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  size_t length = 0;
  css = tai_read_file(argv[2], &length);
  CHECK(css);
  run("ctrl_n_basic", ctrl_n_basic, "/a");
  run("original_unaffected", original_unaffected, "/a");
  run("modifiers", modifiers, "/a");
  run("routing", routing, "/links");
  run("shared_bookmarks", shared_bookmarks, "/a");
  run("shared_cookies", shared_cookies, "/cookie-set");
  run("third_window", third_window, "/a");
  run("close_one", close_one, "/a");
  run("quit_all", quit_all, "/a");
  run("window_limit", window_limit, "/a");
  free(css);
  puts("DONE");
  fflush(stdout);
  return EXIT_SUCCESS;
}
