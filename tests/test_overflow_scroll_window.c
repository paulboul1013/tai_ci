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
#include "window_driver.h"

#include <math.h>

static const char *const CONTAINERS[] = {"box",    "outer", "inner",
                                         "cancel", "gone",  "flat"};
#define CONTAINER_COUNT (sizeof(CONTAINERS) / sizeof(CONTAINERS[0]))

static char *css;

/* -- page inspection ------------------------------------------------------ */

static TaiPage *active_page(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view) && view.page);
  return view.page;
}

/* -- reporting ------------------------------------------------------------ */

static double container_scroll(TaiPage *page, const char *id) {
  const TaiNode *node = find_id(tai_page_root(page), id);
  CHECK(node);
  return node->scroll_y;
}

static void report(const char *scenario, const char *step, TaiTabSet *tabs) {
  TaiPage *page = active_page(tabs);
  printf("STATE %s %s {\"page\": %.17g, \"containers\": {", scenario, step,
         tai_page_scroll_y(page));
  bool first = true;
  for (size_t index = 0; index < CONTAINER_COUNT; index++) {
    const TaiNode *node = find_id(tai_page_root(page), CONTAINERS[index]);
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

static bool frame(void *opaque, TaiTabSet *tabs, SDL_WindowID window_id,
                  int width, bool wraps, AddressEditor *editor) {
  (void)editor;
  Driver *driver = opaque;
  check_deadline(&driver->deadline, driver->name, (long)driver->next);
  if (!drained()) return true;
  const Action *action = &driver->actions[driver->next];
  switch (action->op) {
  case OPEN:
  case WAIT:
    if (!loaded(tabs, action->text)) return true;
    break;
  case CLICK:
    click(tabs, window_id, width, wraps, SDL_BUTTON_LEFT, has_id,
          action->text);
    break;
  case CHROME:
    press(window_id, SDL_BUTTON_LEFT, 400.0, 2.0);
    break;
  case WHEEL:
    wheel(window_id, (float)action->value);
    break;
  case KEY:
    key(window_id, (SDL_Keycode)action->value, SDL_KMOD_NONE, false);
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
  restart_deadline(&driver->deadline);
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
  restart_deadline(&driver.deadline);
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
