#define _POSIX_C_SOURCE 200809L
/* Native half of the overflow-scroll oracle comparison. Driven by
 * tests/overflow_scroll_integration.py, it replays the scenarios of
 * tests/overflow_scroll_oracle_probe.py in the real tabbed presentation loop
 * under SDL's dummy driver. Every click, wheel turn and key press is an SDL
 * event; the loop routes them as it does in a real window.
 *
 * Usage: test_overflow_scroll_window PORT CSS
 * Protocol (one line each, answered with "ok"):
 *   STATE <scenario> <step> <json>   a checkpoint
 * The pagekeys scenario has no oracle counterpart (Python has no
 * PageUp/PageDown) and asserts its values here. */
#include "tai/tabset.h"
#include "../src/presentation_internal.h"

#include <SDL3/SDL.h>
#include <math.h>
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

static const char *const CONTAINERS[] = {"box",    "outer", "inner",
                                         "cancel", "gone",  "flat"};
#define CONTAINER_COUNT (sizeof(CONTAINERS) / sizeof(CONTAINERS[0]))

static char base[128];
static char *css;

static void await_ok(void) {
  char reply[16];
  CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
}

static const char *url_for(const char *path) {
  static char buffer[256];
  CHECK(snprintf(buffer, sizeof(buffer), "%s%s", base, path) > 0);
  return buffer;
}

/* -- page inspection ------------------------------------------------------ */

static const TaiNode *by_id(const TaiNode *node, const char *id) {
  if (node->kind == TAI_ELEMENT) {
    const char *value = tai_map_get(&node->attributes, "id");
    if (value && !strcmp(value, id)) return node;
  }
  for (size_t index = 0; index < node->child_count; index++) {
    const TaiNode *match = by_id(node->children[index], id);
    if (match) return match;
  }
  return NULL;
}

static void append_text(const TaiNode *node, char *out, size_t size) {
  if (node->kind == TAI_TEXT && node->text) {
    size_t used = strlen(out);
    CHECK(snprintf(out + used, size - used, "%s", node->text) >= 0);
  }
  for (size_t index = 0; index < node->child_count; index++)
    append_text(node->children[index], out, size);
}

static const TaiNode *first_tag(const TaiNode *node, const char *tag) {
  if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    const TaiNode *match = first_tag(node->children[index], tag);
    if (match) return match;
  }
  return NULL;
}

/* Python strips the joined <h1> text. */
static bool loaded(TaiTabSet *tabs, const char *expected) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  if (view.loading || !view.page) return false;
  const TaiNode *h1 = first_tag(tai_page_root(view.page), "h1");
  if (!h1) return false;
  char text[256] = "";
  append_text(h1, text, sizeof(text));
  size_t length = strlen(text);
  while (length && text[length - 1] == ' ') text[--length] = '\0';
  size_t start = strspn(text, " ");
  return !strcmp(text + start, expected);
}

static TaiPage *active_page(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view) && view.page);
  return view.page;
}

/* -- SDL input ------------------------------------------------------------ */

static bool drained(void) {
  SDL_PumpEvents();
  return SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) == 0;
}

static void press(SDL_WindowID window_id, double x, double y) {
  SDL_Event event = {.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN,
                                .windowID = window_id,
                                .button = SDL_BUTTON_LEFT,
                                .down = true,
                                .x = (float)x,
                                .y = (float)y}};
  CHECK(SDL_PushEvent(&event));
}

static void wheel(SDL_WindowID window_id, float y) {
  SDL_Event event = {.wheel = {.type = SDL_EVENT_MOUSE_WHEEL,
                               .windowID = window_id,
                               .direction = SDL_MOUSEWHEEL_NORMAL,
                               .y = y}};
  CHECK(SDL_PushEvent(&event));
}

static void key(SDL_WindowID window_id, SDL_Keycode code) {
  SDL_Event event = {.key = {.type = SDL_EVENT_KEY_DOWN,
                             .windowID = window_id,
                             .key = code,
                             .down = true}};
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

static void click(TaiTabSet *tabs, SDL_WindowID window_id, int width,
                  bool wraps, const char *id) {
  TaiPage *page = active_page(tabs);
  TargetPoint point = {.target = by_id(tai_page_root(page), id)};
  CHECK(point.target);
  char *error = NULL;
  CHECK(tai_layout_visit(tai_page_layout(page), find_target_point, &point,
                         &error));
  CHECK(error == NULL && point.found);
  press(window_id, point.x,
        point.y - tai_page_scroll_y(page) + tabs_chrome_bottom(width, wraps));
}

/* -- reporting ------------------------------------------------------------ */

static double container_scroll(TaiPage *page, const char *id) {
  const TaiNode *node = by_id(tai_page_root(page), id);
  CHECK(node);
  return node->scroll_y;
}

static void report(const char *scenario, const char *step, TaiTabSet *tabs) {
  TaiPage *page = active_page(tabs);
  printf("STATE %s %s {\"page\": %.17g, \"containers\": {", scenario, step,
         tai_page_scroll_y(page));
  bool first = true;
  for (size_t index = 0; index < CONTAINER_COUNT; index++) {
    const TaiNode *node = by_id(tai_page_root(page), CONTAINERS[index]);
    if (!node) continue;
    printf("%s\"%s\": %.17g", first ? "" : ", ", CONTAINERS[index],
           node->scroll_y);
    first = false;
  }
  puts("}}");
  fflush(stdout);
  await_ok();
}

/* -- scenario scripts ----------------------------------------------------- */

typedef enum {
  OPEN,     /* text: heading the first page shows */
  CLICK,    /* text: id of the element whose innermost box is clicked */
  CHROME,   /* left click above the tab strip */
  WHEEL,    /* value: SDL wheel y */
  KEY,      /* value: SDL keycode */
  WAIT,     /* text: heading of the page a navigation loads */
  REPORT,   /* text: checkpoint name */
  EXPECT,   /* text: container id; value: its scroll_y, page at 0 */
  END
} Op;

typedef struct {
  Op op;
  const char *text;
  double value;
} Action;

typedef struct {
  const char *name;
  const char *path;
  const Action *actions;
  size_t next;
  struct timespec deadline;
  bool finished;
} Driver;

static void restart_deadline(Driver *driver) {
  CHECK(clock_gettime(CLOCK_MONOTONIC, &driver->deadline) == 0);
  driver->deadline.tv_sec += 10;
}

static bool frame(void *opaque, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  (void)editor;
  Driver *driver = opaque;
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  if (now.tv_sec > driver->deadline.tv_sec ||
      (now.tv_sec == driver->deadline.tv_sec &&
       now.tv_nsec > driver->deadline.tv_nsec)) {
    fprintf(stderr, "%s timed out at action %zu\n", driver->name,
            driver->next);
    exit(EXIT_FAILURE);
  }
  if (!drained()) return true;
  const Action *action = &driver->actions[driver->next];
  switch (action->op) {
  case OPEN:
  case WAIT:
    if (!loaded(tabs, action->text)) return true;
    break;
  case CLICK:
    click(tabs, window_id, width, wraps, action->text);
    break;
  case CHROME:
    press(window_id, 400.0, 2.0);
    break;
  case WHEEL:
    wheel(window_id, (float)action->value);
    break;
  case KEY:
    key(window_id, (SDL_Keycode)action->value);
    break;
  case REPORT:
    report(driver->name, action->text, tabs);
    break;
  case EXPECT: {
    TaiPage *page = active_page(tabs);
    CHECK(container_scroll(page, action->text) == action->value);
    CHECK(tai_page_scroll_y(page) == 0.0);
    break;
  }
  case END:
    driver->finished = true;
    return false;
  }
  driver->next++;
  restart_deadline(driver);
  return true;
}

#define DOWN {KEY, NULL, SDLK_DOWN}
#define UP {KEY, NULL, SDLK_UP}
#define WHEEL_DOWN {WHEEL, NULL, -1}
#define WHEEL_UP {WHEEL, NULL, 1}

static const Action CONTAINER[] = {
    {OPEN, "box", 0},       {CLICK, "box-text", 0},
    {REPORT, "clicked", 0}, WHEEL_DOWN,
    {REPORT, "wheel_down", 0}, DOWN,
    {REPORT, "key_down", 0}, WHEEL_UP,
    {REPORT, "wheel_up", 0}, UP,
    {REPORT, "key_up", 0},  DOWN, DOWN, DOWN, DOWN, DOWN,
    {REPORT, "limit", 0},   DOWN,
    {REPORT, "past_limit", 0}, {END, NULL, 0}};

static const Action OUTSIDE[] = {
    {OPEN, "box", 0},         DOWN,
    {REPORT, "unclicked", 0}, UP,
    {CLICK, "box-text", 0},   {CLICK, "outside", 0},
    WHEEL_DOWN,               {REPORT, "outside", 0},
    {END, NULL, 0}};

static const Action NESTED[] = {
    {OPEN, "nested", 0},       {CLICK, "inner-text", 0}, DOWN,
    {REPORT, "inner", 0},      {CLICK, "outer-text", 0}, DOWN,
    {REPORT, "outer", 0},      DOWN, DOWN, DOWN, DOWN,
    {REPORT, "outer_limit", 0}, {END, NULL, 0}};

static const Action CANCELLED[] = {
    {OPEN, "cancel", 0},        {CLICK, "box-text", 0}, DOWN,
    {REPORT, "box", 0},         {CLICK, "cancel-text", 0}, DOWN,
    {REPORT, "cancelled", 0},   {END, NULL, 0}};

static const Action GONE[] = {
    {OPEN, "detach", 0},    {CLICK, "gone-text", 0}, DOWN,
    {REPORT, "removed", 0}, {END, NULL, 0}};

static const Action FLAT[] = {
    {OPEN, "detach", 0},      {CLICK, "flat-text", 0}, DOWN,
    {REPORT, "restyled", 0},  {END, NULL, 0}};

static const Action FRAGMENT[] = {
    {OPEN, "box", 0},       {CLICK, "fragment", 0},
    {REPORT, "clicked", 0}, DOWN,
    {REPORT, "box", 0},     {END, NULL, 0}};

static const Action NAVIGATION[] = {
    {OPEN, "box", 0},     {CLICK, "away", 0}, {WAIT, "after", 0}, DOWN,
    {REPORT, "page", 0},  {END, NULL, 0}};

static const Action CHROME_CLICK[] = {
    {OPEN, "box", 0},    {CLICK, "box-text", 0}, DOWN,
    {REPORT, "box", 0},  {CHROME, NULL, 0},      WHEEL_DOWN,
    {REPORT, "chrome", 0}, {END, NULL, 0}};

/* Native-only (PORTING_PLAN.md): PageUp/PageDown follow Up/Down into the
 * focused container, which stops them at its end like the arrows. */
static const Action PAGE_KEYS[] = {
    {OPEN, "box", 0},
    {CLICK, "box-text", 0},
    {KEY, NULL, SDLK_PAGEDOWN},
    {EXPECT, "box", 100},
    {KEY, NULL, SDLK_PAGEDOWN},
    {KEY, NULL, SDLK_PAGEDOWN},
    {KEY, NULL, SDLK_PAGEUP},
    {KEY, NULL, SDLK_PAGEUP},
    {EXPECT, "box", 0},
    {END, NULL, 0}};

static void run(const char *name, const char *path, const Action *actions) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(css, false,
                                                    url_for(path), &error);
  CHECK(tabs && error == NULL);
  /* SDL_Quit at the end of each presentation resets hints. */
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  Driver driver = {.name = name, .path = path, .actions = actions};
  restart_deadline(&driver);
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
  CHECK(argc == 3);
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  size_t length = 0;
  css = tai_read_file(argv[2], &length);
  CHECK(css);
  run("container", "/box", CONTAINER);
  run("outside", "/box", OUTSIDE);
  run("nested", "/nested", NESTED);
  run("cancelled", "/cancel", CANCELLED);
  run("gone", "/detach", GONE);
  run("flat", "/detach", FLAT);
  run("fragment", "/box", FRAGMENT);
  run("navigation", "/box", NAVIGATION);
  run("chrome", "/box", CHROME_CLICK);
  run("pagekeys", "/box", PAGE_KEYS);
  free(css);
  puts("DONE");
  fflush(stdout);
  return EXIT_SUCCESS;
}
