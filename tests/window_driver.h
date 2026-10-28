/* Shared helpers for the dummy-SDL window drivers (tests/test_*_window.c):
 * failure checks, the stdin/stdout checkpoint protocol, page inspection,
 * synthetic SDL input and per-step deadlines. Header-only; every function is
 * static inline so a driver pays nothing for the helpers it does not use.
 *
 * Include after defining _POSIX_C_SOURCE (clock_gettime). */
#ifndef TAI_TESTS_WINDOW_DRIVER_H
#define TAI_TESTS_WINDOW_DRIVER_H

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

/* -- checkpoint protocol -------------------------------------------------- */

/* The fixture server origin, "http://127.0.0.1:PORT"; set by main. */
static char base[128];

static inline void await_ok(void) {
  char reply[16];
  CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
}

/* Sends "VERB[ ARGUMENT]" and waits for the harness to answer. */
static inline void command(const char *verb, const char *argument) {
  printf("%s%s%s\n", verb, argument ? " " : "", argument ? argument : "");
  fflush(stdout);
  await_ok();
}

static inline void mark(void) { command("MARK", NULL); }

/* The caller prints the checkpoint's JSON between these two. */
static inline void state_begin(const char *scenario, const char *step) {
  printf("STATE %s %s ", scenario, step);
}

static inline void state_end(void) {
  putchar('\n');
  fflush(stdout);
  await_ok();
}

/* Four rotating buffers, so one call can take several URLs. */
static inline const char *url_for(const char *path) {
  static char buffer[4][256];
  static int next;
  char *url = buffer[next++ % 4];
  CHECK(snprintf(url, sizeof(buffer[0]), "%s%s", base, path) > 0);
  return url;
}

static inline void navigate(TaiTabSet *tabs, const char *path) {
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, url_for(path), &error));
  CHECK(error == NULL);
}

/* -- page inspection ------------------------------------------------------ */

static inline TaiTabSetView view_of(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  return view;
}

static inline void append_text(const TaiNode *node, char *out, size_t size) {
  if (node->kind == TAI_TEXT && node->text) {
    size_t used = strlen(out);
    CHECK(snprintf(out + used, size - used, "%s", node->text) >= 0);
  }
  for (size_t index = 0; index < node->child_count; index++)
    append_text(node->children[index], out, size);
}

static inline bool contains_text(const TaiNode *node, const char *needle) {
  if (!node) return false;
  if (node->kind == TAI_TEXT && node->text && strstr(node->text, needle))
    return true;
  for (size_t index = 0; index < node->child_count; index++)
    if (contains_text(node->children[index], needle)) return true;
  return false;
}

typedef bool (*Select)(const TaiNode *node, const void *data);

/* The first element in document order that select accepts. */
static inline const TaiNode *find(const TaiNode *node, Select select,
                                  const void *data) {
  if (node->kind == TAI_ELEMENT && select(node, data)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    const TaiNode *match = find(node->children[index], select, data);
    if (match) return match;
  }
  return NULL;
}

static inline bool is_tag(const TaiNode *node, const void *tag) {
  return !strcmp(node->tag, tag);
}

static inline bool has_id(const TaiNode *node, const void *id) {
  const char *value = tai_map_get(&node->attributes, "id");
  return value && !strcmp(value, id);
}

static inline bool is_link(const TaiNode *node, const void *href) {
  const char *value = tai_map_get(&node->attributes, "href");
  return !strcmp(node->tag, "a") && value && !strcmp(value, href);
}

static inline const TaiNode *find_id(const TaiNode *node, const char *id) {
  return find(node, has_id, id);
}

/* Python strips the joined <h1> text. */
static inline bool heading(TaiPage *page, char *out, size_t size) {
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

/* The active tab has finished loading and its <h1> reads expected. */
static inline bool loaded(TaiTabSet *tabs, const char *expected) {
  TaiTabSetView view = view_of(tabs);
  char text[256];
  return !view.loading && heading(view.page, text, sizeof(text)) &&
         !strcmp(text, expected);
}

/* -- SDL input ------------------------------------------------------------ */

static inline void push(SDL_Event *event) { CHECK(SDL_PushEvent(event)); }

/* The presentation loop has consumed every pushed event. */
static inline bool drained(void) {
  SDL_PumpEvents();
  return SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) == 0;
}

static inline void press(SDL_WindowID window_id, Uint8 button, double x,
                         double y) {
  SDL_Event event = {.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN,
                                .windowID = window_id,
                                .button = button,
                                .down = true,
                                .x = (float)x,
                                .y = (float)y}};
  push(&event);
}

static inline void wheel(SDL_WindowID window_id, float y) {
  SDL_Event event = {.wheel = {.type = SDL_EVENT_MOUSE_WHEEL,
                               .windowID = window_id,
                               .direction = SDL_MOUSEWHEEL_NORMAL,
                               .y = y}};
  push(&event);
}

static inline void key(SDL_WindowID window_id, SDL_Keycode keycode,
                       SDL_Keymod mod, bool repeat) {
  SDL_Event event = {.key = {.type = SDL_EVENT_KEY_DOWN,
                             .windowID = window_id,
                             .key = keycode,
                             .mod = mod,
                             .down = true,
                             .repeat = repeat}};
  push(&event);
}

static inline void ctrl_n(SDL_WindowID window_id) {
  key(window_id, SDLK_N, SDL_KMOD_LCTRL, false);
}

typedef struct {
  const TaiNode *target;
  double x, y;
  bool found;
} TargetPoint;

/* Keeps the last match: the innermost box, as in the Python probes. The
 * first box of a block element can be an empty line box beside its text. */
static inline bool find_target_point(const TaiLayoutItem *item,
                                     void *opaque) {
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

/* Presses button over the centre of the innermost box of the first element
 * of the active page that select accepts. */
static inline void click(TaiTabSet *tabs, SDL_WindowID window_id, int width,
                         bool wraps, Uint8 button, Select select,
                         const void *data) {
  TaiTabSetView view = view_of(tabs);
  CHECK(view.page);
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

/* Python probe set_draft: the typed text, dirty unless empty. */
static inline void set_draft(AddressEditor *editor, const char *text,
                             bool focused) {
  char *copy = tai_strdup(text);
  CHECK(copy);
  free(editor->text);
  editor->text = copy;
  editor->cursor = strlen(copy);
  editor->dirty = *copy != '\0';
  editor->focused = focused;
}

/* -- step deadlines ------------------------------------------------------- */

/* SDL_Quit after each presentation restarts SDL_GetTicks, so drivers time
 * their steps with the monotonic clock. */
static inline Uint64 now_ms(void) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (Uint64)now.tv_sec * 1000u + (Uint64)now.tv_nsec / 1000000u;
}

static inline void restart_deadline(struct timespec *deadline) {
  CHECK(clock_gettime(CLOCK_MONOTONIC, deadline) == 0);
  deadline->tv_sec += 10;
}

static inline void next_step(int *step, struct timespec *deadline) {
  (*step)++;
  restart_deadline(deadline);
}

static inline void check_deadline(const struct timespec *deadline,
                                  const char *name, long step) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  if (now.tv_sec > deadline->tv_sec ||
      (now.tv_sec == deadline->tv_sec && now.tv_nsec > deadline->tv_nsec)) {
    fprintf(stderr, "%s timed out in step %ld\n", name, step);
    exit(EXIT_FAILURE);
  }
}

/* For scenario functions whose Driver has step, deadline and finished. */
#define ADVANCE()                                                              \
  do {                                                                         \
    next_step(&driver->step, &driver->deadline);                               \
    return true;                                                               \
  } while (0)
#define FINISH()                                                               \
  do {                                                                         \
    driver->finished = true;                                                   \
    return false;                                                              \
  } while (0)

#endif
