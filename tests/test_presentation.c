#define _POSIX_C_SOURCE 200809L
#include "tai/presentation.h"
#include "tai/tabset.h"
#include "../src/presentation_geometry.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  int width;
  int height;
} PresentationEvents;

static bool resize_injected = false;

typedef enum {
  INPUT_WHEEL_DOWN,
  INPUT_WHEEL_UP,
  INPUT_PAGE_DOWN,
  INPUT_PAGE_UP,
  INPUT_ARROW_DOWN,
  INPUT_ARROW_UP,
  INPUT_UNRELATED_WHEEL,
  INPUT_FLIPPED_DOWN,
  INPUT_FRACTIONAL_WHEEL,
  INPUT_NONFINITE_WHEEL,
  INPUT_UNKNOWN_DIRECTION,
  INPUT_UNRELATED_LEFT_CLICK,
  INPUT_NONLEFT_CLICK,
  INPUT_NONFINITE_CLICK,
  INPUT_MISS_CLICK,
  INPUT_LEFT_CLICK,
  INPUT_UNFOCUSED_TEXT,
  INPUT_OTHER_WINDOW_TEXT,
  INPUT_OTHER_WINDOW_BACKSPACE,
  INPUT_CURRENT_TEXT,
  INPUT_BACKSPACE,
  INPUT_LEFT,
  INPUT_RIGHT,
  INPUT_ALT_LEFT,
  INPUT_ALT_RIGHT,
  INPUT_OTHER_WINDOW_ALT_LEFT,
  INPUT_UNFOCUSED_ALT_RIGHT,
  INPUT_RETURN,
  INPUT_WINDOW_FOCUS_LOST,
  INPUT_WINDOW_FOCUS_GAINED,
  INPUT_TABS_ADDRESS,
  INPUT_TABS_TEXT,
  INPUT_TABS_URL_TEXT,
  INPUT_TABS_NEW_TAB,
  INPUT_TABS_FIRST,
  INPUT_TABS_SECOND,
} InputKind;

typedef struct {
  const InputKind *kinds;
  size_t count;
  const SDL_FPoint *click_positions;
} InputEvents;

static bool input_injected = false;
static bool input_events_queued = false;

static bool inject_resize(void *opaque, SDL_Event *event) {
  const PresentationEvents *events = opaque;
  if (event->type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || resize_injected)
    return true;
  resize_injected = true;
  event->window.data1 = events->width;
  event->window.data2 = events->height;
  return true;
}

static SDL_Event make_input_event(InputKind kind, SDL_WindowID window_id) {
  if (kind == INPUT_WINDOW_FOCUS_LOST ||
      kind == INPUT_WINDOW_FOCUS_GAINED) {
    return (SDL_Event){.window = {
        .type = kind == INPUT_WINDOW_FOCUS_LOST
                    ? SDL_EVENT_WINDOW_FOCUS_LOST
                    : SDL_EVENT_WINDOW_FOCUS_GAINED,
        .windowID = window_id,
    }};
  }
  if (kind == INPUT_UNFOCUSED_TEXT || kind == INPUT_OTHER_WINDOW_TEXT ||
      kind == INPUT_CURRENT_TEXT || kind == INPUT_TABS_TEXT ||
      kind == INPUT_TABS_URL_TEXT) {
    return (SDL_Event){.text = {
        .type = SDL_EVENT_TEXT_INPUT,
        .windowID = kind == INPUT_OTHER_WINDOW_TEXT ? window_id + 1 :
                                                     window_id,
        .text = kind == INPUT_UNFOCUSED_TEXT ? "ignored-before-focus" :
                kind == INPUT_TABS_URL_TEXT
                    ? "data:text/html,<p>draft</p>"
                    : kind == INPUT_TABS_TEXT ? "draft" : "\303\251x",
    }};
  }
  if (kind == INPUT_OTHER_WINDOW_BACKSPACE || kind == INPUT_BACKSPACE ||
      kind == INPUT_LEFT || kind == INPUT_RIGHT ||
      kind == INPUT_ALT_LEFT || kind == INPUT_ALT_RIGHT ||
      kind == INPUT_OTHER_WINDOW_ALT_LEFT ||
      kind == INPUT_UNFOCUSED_ALT_RIGHT) {
    return (SDL_Event){.key = {
        .type = SDL_EVENT_KEY_DOWN,
        .windowID = kind == INPUT_OTHER_WINDOW_BACKSPACE ||
                    kind == INPUT_OTHER_WINDOW_ALT_LEFT ? window_id + 1 :
                                                         window_id,
        .key = kind == INPUT_BACKSPACE || kind == INPUT_OTHER_WINDOW_BACKSPACE
                   ? SDLK_BACKSPACE
               : kind == INPUT_LEFT || kind == INPUT_ALT_LEFT ||
                 kind == INPUT_OTHER_WINDOW_ALT_LEFT ? SDLK_LEFT : SDLK_RIGHT,
        .mod = kind == INPUT_ALT_LEFT || kind == INPUT_ALT_RIGHT ||
               kind == INPUT_OTHER_WINDOW_ALT_LEFT ||
               kind == INPUT_UNFOCUSED_ALT_RIGHT ? SDL_KMOD_ALT : 0,
        .down = true,
    }};
  }
  if (kind == INPUT_UNRELATED_LEFT_CLICK || kind == INPUT_NONLEFT_CLICK ||
      kind == INPUT_NONFINITE_CLICK || kind == INPUT_MISS_CLICK ||
      kind == INPUT_LEFT_CLICK || kind == INPUT_TABS_ADDRESS ||
      kind == INPUT_TABS_NEW_TAB || kind == INPUT_TABS_FIRST ||
      kind == INPUT_TABS_SECOND) {
    return (SDL_Event){.button = {
        .type = SDL_EVENT_MOUSE_BUTTON_DOWN,
        .windowID = kind == INPUT_UNRELATED_LEFT_CLICK ? window_id + 1 :
                                                        window_id,
        .button = kind == INPUT_NONLEFT_CLICK ? SDL_BUTTON_RIGHT :
                                                SDL_BUTTON_LEFT,
        .down = true,
        .x = kind == INPUT_NONFINITE_CLICK ? NAN :
             kind == INPUT_MISS_CLICK ? 299.0f : 14.0f,
        .y = 21.0f,
    }};
  }
  if (kind == INPUT_WHEEL_DOWN || kind == INPUT_WHEEL_UP ||
      kind == INPUT_UNRELATED_WHEEL || kind == INPUT_FLIPPED_DOWN ||
      kind == INPUT_FRACTIONAL_WHEEL || kind == INPUT_NONFINITE_WHEEL ||
      kind == INPUT_UNKNOWN_DIRECTION) {
    return (SDL_Event){.wheel = {
        .type = SDL_EVENT_MOUSE_WHEEL,
        .windowID = kind == INPUT_UNRELATED_WHEEL ? window_id + 1 : window_id,
        .y = kind == INPUT_NONFINITE_WHEEL ? INFINITY :
             kind == INPUT_FRACTIONAL_WHEEL ? -0.5f :
             kind == INPUT_WHEEL_UP || kind == INPUT_FLIPPED_DOWN ? 1.0f : -1.0f,
        .direction = kind == INPUT_FLIPPED_DOWN ? SDL_MOUSEWHEEL_FLIPPED :
                     kind == INPUT_UNKNOWN_DIRECTION ? 99 : SDL_MOUSEWHEEL_NORMAL,
    }};
  }
  return (SDL_Event){.key = {
      .type = SDL_EVENT_KEY_DOWN,
      .windowID = window_id,
      .key = kind == INPUT_PAGE_UP ? SDLK_PAGEUP :
             kind == INPUT_PAGE_DOWN ? SDLK_PAGEDOWN :
             kind == INPUT_ARROW_UP ? SDLK_UP :
             kind == INPUT_RETURN ? SDLK_RETURN : SDLK_DOWN,
      .down = true,
  }};
}

static bool inject_input(void *opaque, SDL_Event *event) {
  const InputEvents *inputs = opaque;
  if (event->type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || input_injected)
    return true;
  input_injected = true;
  SDL_WindowID window_id = event->window.windowID;
  for (size_t index = 0; index < inputs->count; index++) {
    SDL_Event input = make_input_event(inputs->kinds[index], window_id);
    if (inputs->click_positions &&
        input.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
      input.button.x = inputs->click_positions[index].x;
      input.button.y = inputs->click_positions[index].y;
    }
    if (!SDL_PushEvent(&input)) return false;
  }
  input_events_queued = true;
  return false;
}

typedef struct {
  atomic_uint window_id;
  const InputKind *kinds;
  const SDL_FPoint *positions;
  size_t count;
  bool queued;
} DelayedInputs;

static bool capture_window_id(void *opaque, SDL_Event *event) {
  DelayedInputs *inputs = opaque;
  if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
    atomic_store(&inputs->window_id, event->window.windowID);
  return true;
}

static void *post_delayed_inputs(void *opaque) {
  DelayedInputs *inputs = opaque;
  SDL_WindowID window_id = 0;
  for (int attempts = 0; attempts < 500 && !window_id; attempts++) {
    window_id = atomic_load(&inputs->window_id);
    if (!window_id) {
      struct timespec pause = {.tv_nsec = 1000000};
      nanosleep(&pause, NULL);
    }
  }
  if (!window_id) return NULL;
  struct timespec commit_wait = {.tv_nsec = 250000000};
  nanosleep(&commit_wait, NULL);
  for (size_t index = 0; index < inputs->count; index++) {
    SDL_Event input = make_input_event(inputs->kinds[index], window_id);
    if (inputs->positions && input.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
      input.button.x = inputs->positions[index].x;
      input.button.y = inputs->positions[index].y;
    }
    if (!SDL_PushEvent(&input)) return NULL;
  }
  inputs->queued = true;
  struct timespec settle_wait = {.tv_nsec = 300000000};
  nanosleep(&settle_wait, NULL);
  SDL_Event quit = {.type = SDL_EVENT_QUIT};
  (void)SDL_PushEvent(&quit);
  return NULL;
}

static void *request_quit(void *unused) {
  (void)unused;
  struct timespec pause = {.tv_sec = 1, .tv_nsec = 0};
  nanosleep(&pause, NULL);
  SDL_Event event = {.type = SDL_EVENT_QUIT};
  SDL_PushEvent(&event);
  return NULL;
}

typedef struct {
  bool injected;
  bool queued;
  int logical_width, logical_height;
  int pixel_width, pixel_height;
} HidpiProbe;

static TaiNode *find(TaiNode *node, const char *tag);
static void present_delayed_bookmark_inputs(TaiTabSet *tabs,
    const char *initial_url, int width, int height,
    const InputKind *kinds, const SDL_FPoint *positions, size_t count,
    bool dummy_driver);

static bool inject_hidpi_clicks(void *opaque, SDL_Event *event) {
  HidpiProbe *probe = opaque;
  if (event->type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || probe->injected)
    return true;
  SDL_Window *window = SDL_GetWindowFromID(event->window.windowID);
  if (!window) return true;
  probe->injected = true;
  if (!SDL_GetWindowSize(window, &probe->logical_width, &probe->logical_height) ||
      !SDL_GetWindowSizeInPixels(window, &probe->pixel_width,
                                 &probe->pixel_height))
    return false;
  const SDL_FPoint logical_clicks[] = {
      {7.5f, 9.0f},  /* Physical (15,18): New Tab. */
      {20.0f, 13.0f}, /* Physical (40,26): first tab, not New Tab. */
  };
  for (size_t index = 0; index < sizeof(logical_clicks) / sizeof(logical_clicks[0]);
       index++) {
    SDL_Event click = {.button = {
        .type = SDL_EVENT_MOUSE_BUTTON_DOWN,
        .windowID = event->window.windowID,
        .button = SDL_BUTTON_LEFT,
        .down = true,
        .x = logical_clicks[index].x,
        .y = logical_clicks[index].y,
    }};
    if (!SDL_PushEvent(&click)) return false;
  }
  SDL_Event quit = {.type = SDL_EVENT_QUIT};
  probe->queued = SDL_PushEvent(&quit);
  return false;
}

static int run_hidpi_probe(void) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  HidpiProbe probe = {0};
  SDL_SetEventFilter(inject_hidpi_clicks, &probe);
  bool presented = tai_present_window_with_tabs(
      tabs, "data:text/html,<p>initial</p>", 300, 100, &error);
  TaiTabSetView view = {0};
  bool viewed = tai_tabset_view(tabs, &view);
  fprintf(stderr, "hidpi probe: logical=%dx%d physical=%dx%d "
                  "injected=%d queued=%d presented=%d tabs=%zu active=%zu error=%s\n",
          probe.logical_width, probe.logical_height,
          probe.pixel_width, probe.pixel_height,
          probe.injected, probe.queued, presented,
          viewed ? view.tab_count : 0, viewed ? view.active_index : 0,
          error ? error : "none");
  assert(presented && !error && probe.injected && probe.queued && viewed);
  assert(probe.logical_width > 0 && probe.logical_height > 0);
  assert(probe.pixel_width > probe.logical_width &&
         probe.pixel_height > probe.logical_height);
  assert(view.tab_count == 2 && view.active_index == 0);
  tai_tabset_destroy(tabs);

  static const InputKind bookmark_kinds[] = {
      INPUT_TABS_FIRST, INPUT_TABS_FIRST,
  };
  /* A 300x100 logical window is 600x200 physical pixels in this probe. */
  static const SDL_FPoint bookmark_positions[] = {
      {285.0f, 30.0f}, {55.0f, 26.0f},
  };
  tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  present_delayed_bookmark_inputs(tabs, "data:text/html,<p>initial</p>",
      300, 100, bookmark_kinds, bookmark_positions, 2, false);
  assert(tai_tabset_view(tabs, &view));
  assert(view.page && !view.loading &&
         !strcmp(view.url, "about:bookmarks") && view.history_count == 2);
  TaiNode *saved_link = find(tai_page_root(view.page), "a");
  assert(saved_link && !strcmp(tai_map_get(&saved_link->attributes, "href"),
                               "data:text/html,<p>initial</p>"));
  tai_tabset_destroy(tabs);
  return 0;
}

static void present_delayed_bookmark_inputs(TaiTabSet *tabs,
    const char *initial_url, int width, int height,
    const InputKind *kinds, const SDL_FPoint *positions, size_t count,
    bool dummy_driver) {
  DelayedInputs inputs = {.kinds = kinds, .positions = positions,
                          .count = count};
  atomic_init(&inputs.window_id, 0);
  char *error = NULL;
  pthread_t poster;
  if (dummy_driver) {
    assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
    assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  }
  SDL_SetEventFilter(capture_window_id, &inputs);
  assert(pthread_create(&poster, NULL, post_delayed_inputs, &inputs) == 0);
  bool presented = tai_present_window_with_tabs(tabs, initial_url,
                                                width, height, &error);
  assert(pthread_join(poster, NULL) == 0);
  if (!presented)
    fprintf(stderr, "bookmark presentation failed: %s\n",
            error ? error : "unknown error");
  assert(presented && !error && atomic_load(&inputs.window_id) &&
         inputs.queued);
}

static TaiNode *find(TaiNode *node, const char *tag) {
  if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    TaiNode *result = find(node->children[index], tag);
    if (result) return result;
  }
  return NULL;
}

typedef struct {
  TaiNetwork *network;
  const char *css;
  size_t calls;
} NavigationFixture;

typedef struct {
  size_t back;
  size_t forward;
} HistoryFixture;

static bool record_history(void *opaque, TaiPage **page, int direction,
                           char **error) {
  (void)error;
  assert(page && *page);
  HistoryFixture *fixture = opaque;
  if (direction == -1) fixture->back++;
  else if (direction == 1) fixture->forward++;
  else assert(false);
  return true;
}

static bool replace_page(void *opaque, TaiPage **current_page,
                         const TaiNavigationIntent *intent, char **error) {
  NavigationFixture *fixture = opaque;
  fixture->calls++;
  bool loaded = tai_page_replace_from_intent(
      fixture->network, current_page, intent, fixture->css, false, error);
  if (!loaded && error) { free(*error); *error = NULL; }
  if (error) { free(*error); *error = NULL; }
  return true;
}

int main(void) {
  const char *hidpi_probe = getenv("TAI_PRESENTATION_HIDPI_PROBE");
  if (hidpi_probe && !strcmp(hidpi_probe, "1")) return run_hidpi_probe();
  TaiScrollbarRect bar = {0};
  assert(!tai_scrollbar_geometry(100, 100, 0, 0, &bar));
  assert(tai_scrollbar_geometry(100, 100, 0, 300, &bar));
  assert(bar.x == 88 && bar.y == 0 && bar.w == 12 && bar.h == 25);
  assert(tai_scrollbar_geometry(100, 100, 150, 300, &bar));
  assert(bar.y == 37.5f && bar.h == 25);
  assert(tai_scrollbar_geometry(100, 100, 300, 300, &bar));
  assert(bar.y == 75 && bar.y + bar.h == 100);
  assert(tai_scrollbar_geometry(8, 100, 450, 900, &bar));
  assert(bar.x == 0 && bar.w == 8 && bar.y == 40 && bar.h == 20);
  assert(tai_scrollbar_geometry(8, 10, 90, 90, &bar));
  assert(bar.x == 0 && bar.w == 8 && bar.y == 0 && bar.h == 10);
  assert(!tai_scrollbar_geometry(100, 100, NAN, 300, &bar));
  assert(!tai_scrollbar_geometry(100, 100, 0, INFINITY, &bar));
  assert(!tai_scrollbar_geometry(0, 100, 0, 300, &bar));
  /* SDL mouse events use window-logical coordinates, while this renderer
   * draws and hit-tests in physical pixels. A 2x window maps both axes. */
  double pixel_x = -1.0, pixel_y = -1.0;
  assert(tai_presentation_pointer_to_pixels(14.25, 20.5, 300, 100,
                                            600, 200, &pixel_x, &pixel_y));
  assert(pixel_x == 28.5 && pixel_y == 41.0);
  assert(tai_presentation_pointer_to_pixels(14.25, 20.5, 300, 100,
                                            300, 100, &pixel_x, &pixel_y));
  assert(pixel_x == 14.25 && pixel_y == 20.5);
  /* The tabbed bookmark controls receive physical chrome coordinates. */
  assert(tai_presentation_pointer_to_pixels(135.0, 30.0, 300, 100,
                                            600, 200, &pixel_x, &pixel_y));
  assert(pixel_x == 270.0 && pixel_y == 60.0);
  assert(tai_presentation_pointer_to_pixels(55.0, 26.0, 300, 100,
                                            600, 200, &pixel_x, &pixel_y));
  assert(pixel_x == 110.0 && pixel_y == 52.0);
  assert(!tai_presentation_pointer_to_pixels(NAN, 20.5, 300, 100,
                                             600, 200, &pixel_x, &pixel_y));
  assert(!tai_presentation_pointer_to_pixels(14.25, INFINITY, 300, 100,
                                             600, 200, &pixel_x, &pixel_y));
  assert(!tai_presentation_pointer_to_pixels(14.25, 20.5, 0, 100,
                                             600, 200, &pixel_x, &pixel_y));
  assert(!tai_presentation_pointer_to_pixels(14.25, 20.5, 300, 100,
                                             600, 0, &pixel_x, &pixel_y));
  char *error = NULL;
  assert(!tai_present_window(NULL, 800, 532, &error));
  assert(error);
  free(error);
  error = NULL;

  /* Cairo ARGB32 and SDL ARGB8888 describe the same numeric pixel layout. */
  int bits = 0, bytes = 0;
  Uint32 r = 0, g = 0, b = 0, a = 0;
  assert(SDL_GetMasksForPixelFormat(SDL_PIXELFORMAT_ARGB8888, &bits, &r,
                                    &g, &b, &a));
  bytes = bits / 8;
  assert(bytes == 4 && r == 0x00ff0000U && g == 0x0000ff00U &&
         b == 0x000000ffU && a == 0xff000000U);

  TaiNetwork *network = tai_network_create();
  TaiUrl *url = tai_url_parse("data:text/html,<p>one%20two%20three%20four</p>");
  TaiPage *page = tai_page_load(network, url,
      "html {display:block} body {display:block} p {display:block}",
      64, 64, false, &error);
  assert(network && url && page && !error);
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  assert(!tai_present_window(page, 9000, 64, &error));
  assert(error);
  free(error);
  error = NULL;
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  pthread_t thread;
  PresentationEvents events = {.width = 80, .height = 64};
  resize_injected = false;
  SDL_SetEventFilter(inject_resize, &events);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window(page, 64, 64, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error);
  assert(resize_injected);
  assert(tai_page_viewport_width(page) == 80.0 &&
         tai_page_viewport_height(page) == 64.0);
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  events.width = 9000;
  resize_injected = false;
  SDL_SetEventFilter(inject_resize, &events);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(!tai_present_window(page, 64, 64, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(error);
  free(error);
  error = NULL;
  assert(resize_injected && tai_page_viewport_width(page) == 64.0 &&
         tai_page_viewport_height(page) == 64.0);

  TaiUrl *input_url = tai_url_parse(
      "data:text/html,<p>one%20two%20three%20four%20five%20six%20"
      "seven%20eight%20nine%20ten%20one%20two%20three%20four%20five%20"
      "six%20seven%20eight%20nine%20ten%20one%20two%20three%20four%20"
      "five%20six%20seven%20eight%20nine%20ten</p>");
  TaiPage *input_page = tai_page_load(network, input_url,
      "html {display:block} body {display:block} p {display:block}",
      64, 64, false, &error);
  assert(input_url && input_page && !error);
  assert(tai_page_max_scroll_y(input_page) > 300.0);
  static const InputKind scroll_inputs[] = {
      INPUT_PAGE_UP, INPUT_WHEEL_DOWN, INPUT_PAGE_DOWN, INPUT_PAGE_UP, INPUT_WHEEL_UP,
      INPUT_FLIPPED_DOWN, INPUT_FRACTIONAL_WHEEL, INPUT_NONFINITE_WHEEL,
      INPUT_UNKNOWN_DIRECTION, INPUT_UNRELATED_WHEEL,
      INPUT_ARROW_DOWN, INPUT_ARROW_UP, INPUT_ARROW_DOWN,
  };
  InputEvents input = {
      .kinds = scroll_inputs,
      .count = sizeof(scroll_inputs) / sizeof(scroll_inputs[0]),
  };
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &input);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window(input_page, 64, 64, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued &&
         tai_page_scroll_y(input_page) == 200.0);

  /* Keep the two downward SDL paths independently observable: the mixed
   * sequence above could otherwise still reach its final value if either
   * wheel-down or PageDown were accidentally ignored. */
  static const InputKind downward_inputs[] = {
      INPUT_WHEEL_DOWN,
      INPUT_PAGE_DOWN,
  };
  InputEvents downward = {
      .kinds = downward_inputs,
      .count = sizeof(downward_inputs) / sizeof(downward_inputs[0]),
  };
  assert(tai_page_set_scroll_y(input_page, 100.0));
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &downward);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window(input_page, 64, 64, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued &&
         tai_page_scroll_y(input_page) == 300.0);

  /* RED: presentation accepts only a finite left-button down from the current
   * SDL window. The ignored events below must leave the immutable frame and
   * DOM unchanged before the separate primary click reaches the page/JS seam. */
  TaiUrl *click_url = tai_url_parse(
      "data:text/html,%3Cstyle%3Ep.active%20%7Bcolor%3Ablue%7D%3C%2Fstyle%3E%3Cp%3EClick%3C%2Fp%3E%3Cscript%3Evar%20p%3Ddocument.querySelectorAll%28%27p%27%29%5B0%5D%3Bp.addEventListener%28%27click%27%2Cfunction%28%29%7Bthis.setAttribute%28%27class%27%2C%27active%27%29%3B%7D%29%3B%3C%2Fscript%3E");
  TaiPage *click_page = tai_page_load(network, click_url,
      "html {display:block} body {display:block} p {display:block}",
      300.0, 100.0, false, &error);
  assert(click_url && click_page && !error);
  TaiNode *click_paragraph = find(tai_page_root(click_page), "p");
  assert(click_paragraph);
  const TaiDisplayList *click_before = tai_page_display_list(click_page);
  static const InputKind ignored_clicks[] = {
      INPUT_UNRELATED_LEFT_CLICK,
      INPUT_NONLEFT_CLICK,
      INPUT_NONFINITE_CLICK,
      INPUT_MISS_CLICK,
  };
  InputEvents ignored = {
      .kinds = ignored_clicks,
      .count = sizeof(ignored_clicks) / sizeof(ignored_clicks[0]),
  };
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &ignored);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window(click_page, 300, 100, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued);
  assert(!tai_map_get(&click_paragraph->attributes, "class"));
  assert(tai_page_display_list(click_page) == click_before);

  /* A current-window primary click reaches the listener, makes its CSS rule
   * observable, and replaces the frame that presentation rasterizes. */
  static const InputKind primary_click[] = {INPUT_LEFT_CLICK};
  InputEvents primary = {
      .kinds = primary_click,
      .count = sizeof(primary_click) / sizeof(primary_click[0]),
  };
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &primary);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window(click_page, 300, 100, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued);
  const char *click_class =
      tai_map_get(&click_paragraph->attributes, "class");
  const char *click_color = tai_map_get(&click_paragraph->style, "color");
  assert(click_class && !strcmp(click_class, "active"));
  assert(click_color && !strcmp(click_color, "blue"));
  assert(tai_page_display_list(click_page) != click_before);

  /* A navigation intent crosses into the caller-owned page slot. The callback
   * loads a candidate, replaces the page only after success, and presentation
   * paints and accepts input in the new document while retaining the same SDL
   * window. */
  TaiUrl *navigation_url = tai_url_parse(
      "data:text/html,%3Ca%20href%3D%22data%3Atext%2Fhtml%2C%253Cinput%2520type%253Dcheckbox%2520id%253Dnew%253E%22%3EGo%3C/a%3E");
  TaiPage *navigation_page = tai_page_load(network, navigation_url,
      "html {display:block} body {display:block} a {display:block} p {display:block}",
      300, 100, false, &error);
  assert(navigation_url && navigation_page && !error);
  NavigationFixture navigation = {
      .network = network,
      .css = "html {display:block} body {display:block} a {display:block} p {display:block}",
  };
  static const InputKind navigation_click[] = {
      INPUT_LEFT_CLICK,
      INPUT_LEFT_CLICK,
  };
  static const SDL_FPoint navigation_click_positions[] = {
      {14.0f, 21.0f},
      {20.0f, 28.0f},
  };
  InputEvents navigate = {
      .kinds = navigation_click,
      .count = sizeof(navigation_click) / sizeof(navigation_click[0]),
      .click_positions = navigation_click_positions,
  };
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &navigate);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window_with_navigation(&navigation_page, 300, 100,
      replace_page, &navigation, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued &&
         navigation.calls == 1);
  TaiNode *new_input = find(tai_page_root(navigation_page), "input");
  assert(new_input && new_input->checked);
  assert(!strcmp(tai_url_scheme(tai_page_url(navigation_page)), "data"));
  tai_page_destroy(navigation_page);
  tai_url_destroy(navigation_url);

  /* A failed candidate load keeps the old page alive. A second click in the
   * same event loop can create another intent, and the window stays usable. */
  TaiUrl *failed_url = tai_url_parse(
      "data:text/html,%3Ca%20href%3D%22http%3A%2F%2F127.0.0.1%3A65536%2Ffail%22%3EGo%3C/a%3E");
  TaiPage *failed_page = tai_page_load(network, failed_url,
      "html {display:block} body {display:block} a {display:block}",
      300, 100, false, &error);
  assert(failed_url && failed_page && !error);
  TaiPage *stable_page = failed_page;
  NavigationFixture failed_navigation = {
      .network = network,
      .css = "html {display:block} body {display:block} a {display:block}",
  };
  static const InputKind failed_navigation_clicks[] = {
      INPUT_LEFT_CLICK,
      INPUT_LEFT_CLICK,
  };
  InputEvents fail_twice = {
      .kinds = failed_navigation_clicks,
      .count = sizeof(failed_navigation_clicks) /
               sizeof(failed_navigation_clicks[0]),
  };
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &fail_twice);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window_with_navigation(&failed_page, 300, 100,
      replace_page, &failed_navigation, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued &&
         failed_navigation.calls == 2 && failed_page == stable_page);
  assert(find(tai_page_root(failed_page), "a"));
  tai_page_destroy(failed_page);
  tai_url_destroy(failed_url);

  TaiUrl *enter_url = tai_url_parse(
      "data:text/html,%3Cform%20action%3D%22data%3Atext%2Fhtml%2C%253Cp%253EPosted%253C%252Fp%253E%22%20method%3DPOST%3E%3Cinput%20name%3Dq%20value%3Dx%3E%3C/form%3E");
  TaiPage *enter_page = tai_page_load(network, enter_url,
      "html {display:block} body {display:block} form {display:block}",
      300, 100, false, &error);
  assert(enter_url && enter_page && !error);
  NavigationFixture enter_navigation = {
      .network = network,
      .css = "html {display:block} body {display:block} p {display:block}",
  };
  static const InputKind enter_inputs[] = {
      INPUT_LEFT_CLICK,
      INPUT_RETURN,
  };
  InputEvents enter = {
      .kinds = enter_inputs,
      .count = sizeof(enter_inputs) / sizeof(enter_inputs[0]),
  };
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &enter);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window_with_navigation(&enter_page, 300, 100,
      replace_page, &enter_navigation, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued &&
         enter_navigation.calls == 1);
  assert(find(tai_page_root(enter_page), "p"));
  tai_page_destroy(enter_page);
  tai_url_destroy(enter_url);

  /* RED: SDL text and editing keys must be scoped to the live window and a
   * focused input. Text queued before focus and events for another window are
   * ignored; Unicode text and Backspace/Left/Right update the page value. */
  TaiUrl *edit_url = tai_url_parse("data:text/html,%3Cinput%20value%3Dcat%3E");
  TaiPage *edit_page = tai_page_load(network, edit_url,
      "html {display:block} body {display:block}", 300, 100, false, &error);
  assert(edit_url && edit_page && !error);
  static const InputKind edit_inputs[] = {
      INPUT_UNFOCUSED_TEXT,
      INPUT_OTHER_WINDOW_TEXT,
      INPUT_LEFT_CLICK,
      INPUT_WINDOW_FOCUS_LOST,
      INPUT_CURRENT_TEXT,
      INPUT_WINDOW_FOCUS_GAINED,
      INPUT_CURRENT_TEXT,
      INPUT_OTHER_WINDOW_TEXT,
      INPUT_OTHER_WINDOW_BACKSPACE,
      INPUT_LEFT,
      INPUT_RIGHT,
      INPUT_BACKSPACE,
      INPUT_MISS_CLICK,
      INPUT_CURRENT_TEXT,
  };
  InputEvents editing = {
      .kinds = edit_inputs,
      .count = sizeof(edit_inputs) / sizeof(edit_inputs[0]),
  };
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &editing);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window(edit_page, 300, 100, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued);
  TaiNode *edit_input = find(tai_page_root(edit_page), "input");
  assert(edit_input && !edit_input->focused && edit_input->cursor_index == 1);
  assert(!strcmp(tai_map_get(&edit_input->attributes, "value"), "\303\251cat"));
  tai_page_destroy(edit_page);
  tai_url_destroy(edit_url);

  /* Alt+arrows belong to the window history callback. Ordinary arrows keep
   * editing the focused input; other-window and unfocused keys are ignored. */
  TaiUrl *history_url = tai_url_parse("data:text/html,%3Cinput%20value%3Dcat%3E");
  TaiPage *history_page = tai_page_load(network, history_url,
      "html {display:block} body {display:block}", 300, 100, false, &error);
  assert(history_url && history_page && !error);
  static const InputKind history_inputs[] = {
      INPUT_WINDOW_FOCUS_LOST,
      INPUT_UNFOCUSED_ALT_RIGHT,
      INPUT_WINDOW_FOCUS_GAINED,
      INPUT_OTHER_WINDOW_ALT_LEFT,
      INPUT_LEFT_CLICK,
      INPUT_RIGHT,
      INPUT_ALT_LEFT,
      INPUT_ALT_LEFT,
      INPUT_ALT_RIGHT,
  };
  InputEvents history_events = {
      .kinds = history_inputs,
      .count = sizeof(history_inputs) / sizeof(history_inputs[0]),
  };
  HistoryFixture history = {0};
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &history_events);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window_with_history(&history_page, 300, 100,
      NULL, record_history, NULL, &history, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued);
  assert(history.back == 2 && history.forward == 1);
  TaiNode *history_input = find(tai_page_root(history_page), "input");
  assert(history_input && history_input->focused &&
         history_input->cursor_index == 1);
  assert(!strcmp(tai_map_get(&history_input->attributes, "value"), "cat"));
  tai_page_destroy(history_page);
  tai_url_destroy(history_url);

  /* SDL text reaches the JS keydown seam, and preventDefault suppresses the
   * insertion. The same-page click also demonstrates focus activation. */
  TaiUrl *prevent_url = tai_url_parse(
      "data:text/html,%3Cscript%3Evar%20i%3Ddocument.querySelectorAll%28%27input%27%29%5B0%5D%3Bi.addEventListener%28%27keydown%27%2Cfunction%28e%29%7Be.preventDefault%28%29%3B%7D%29%3B%3C%2Fscript%3E%3Cinput%20value%3Dcat%3E");
  TaiPage *prevent_page = tai_page_load(network, prevent_url,
      "html {display:block} body {display:block}", 300, 100, false, &error);
  assert(prevent_url && prevent_page && !error);
  static const InputKind prevented_inputs[] = {
      INPUT_LEFT_CLICK,
      INPUT_CURRENT_TEXT,
  };
  InputEvents prevented = {
      .kinds = prevented_inputs,
      .count = sizeof(prevented_inputs) / sizeof(prevented_inputs[0]),
  };
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &prevented);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window(prevent_page, 300, 100, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued);
  TaiNode *prevent_input = find(tai_page_root(prevent_page), "input");
  assert(prevent_input && prevent_input->focused);
  assert(!strcmp(tai_map_get(&prevent_input->attributes, "value"), "cat"));
  tai_page_destroy(prevent_page);
  tai_url_destroy(prevent_url);

  static const InputKind tab_inputs[] = {
      INPUT_TABS_ADDRESS, INPUT_TABS_URL_TEXT, INPUT_TABS_NEW_TAB,
      INPUT_TABS_ADDRESS, INPUT_RETURN,
      INPUT_TABS_ADDRESS, INPUT_TABS_URL_TEXT, INPUT_TABS_FIRST,
      INPUT_TABS_SECOND, INPUT_TABS_ADDRESS, INPUT_RETURN,
      INPUT_TABS_NEW_TAB, INPUT_TABS_NEW_TAB, INPUT_TABS_FIRST,
      INPUT_TABS_SECOND, INPUT_TABS_SECOND, INPUT_TABS_FIRST,
      INPUT_TABS_FIRST, INPUT_TABS_SECOND,
  };
  static const SDL_FPoint tab_clicks[] = {
      {200.0f, 56.0f}, {0.0f, 0.0f}, {0.0f, 18.0f},
      {200.0f, 56.0f}, {0.0f, 0.0f},
      {200.0f, 56.0f}, {0.0f, 0.0f}, {34.0f, 27.0f},
      {75.0f, 27.0f}, {200.0f, 56.0f}, {0.0f, 0.0f},
      {15.0f, 18.0f}, {30.0f, 18.0f}, {34.0f, 27.0f},
      {125.0f, 27.0f}, {75.0f, 27.0f}, {71.0f, 27.0f},
      {34.0f, 27.0f}, {75.0f, 27.0f},
  };
  InputEvents tab_events = {
      .kinds = tab_inputs,
      .count = sizeof(tab_inputs) / sizeof(tab_inputs[0]),
      .click_positions = tab_clicks,
  };
  TaiTabSet *tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &tab_events);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window_with_tabs(tabs,
      "data:text/html,<p>initial</p>", 300, 100, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued);
  TaiTabSetView tab_view;
  assert(tai_tabset_view(tabs, &tab_view));
  assert(tab_view.tab_count == 3 && tab_view.active_index == 0 &&
         !strcmp(tab_view.url, "data:text/html,<p>initial</p>"));
  assert(tai_tabset_select(tabs, 2));
  assert(tai_tabset_view(tabs, &tab_view));
  assert(tab_view.active_index == 2 &&
         !strcmp(tab_view.url, "data:text/html,<p>home</p>"));
  assert(tai_tabset_select(tabs, 0));
  tai_tabset_destroy(tabs);

  static const InputKind active_first_inputs[] = {
      INPUT_TABS_NEW_TAB, INPUT_TABS_FIRST, INPUT_TABS_SECOND,
  };
  static const SDL_FPoint active_first_clicks[] = {
      {15.0f, 18.0f}, {34.0f, 27.0f}, {88.0f, 27.0f},
  };
  InputEvents active_first_events = {
      .kinds = active_first_inputs,
      .count = sizeof(active_first_inputs) / sizeof(active_first_inputs[0]),
      .click_positions = active_first_clicks,
  };
  tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &active_first_events);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  bool active_first_presented = tai_present_window_with_tabs(tabs,
      "data:text/html,<p>initial</p>", 300, 100, &error);
  if (!active_first_presented)
    fprintf(stderr, "active-first presentation failed: %s\n",
            error ? error : "unknown error");
  assert(active_first_presented);
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued);
  assert(tai_tabset_view(tabs, &tab_view));
  assert(tab_view.tab_count == 2 && tab_view.active_index == 1);
  tai_tabset_destroy(tabs);

  /* Frozen 120px Python chrome keeps the wrapped, inactive Tab 1 link
   * clickable through x=113 while its second line remains above toolbar. */
  static const InputKind narrow_tab_inputs[] = {
      INPUT_TABS_NEW_TAB, INPUT_TABS_FIRST, INPUT_TABS_SECOND,
  };
  static const SDL_FPoint narrow_tab_clicks[] = {
      {15.0f, 18.0f}, {34.0f, 27.0f}, {110.0f, 45.0f},
  };
  InputEvents narrow_tab_events = {
      .kinds = narrow_tab_inputs,
      .count = sizeof(narrow_tab_inputs) / sizeof(narrow_tab_inputs[0]),
      .click_positions = narrow_tab_clicks,
  };
  tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &narrow_tab_events);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window_with_tabs(tabs,
      "data:text/html,<p>initial</p>", 120, 200, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued);
  assert(tai_tabset_view(tabs, &tab_view));
  assert(tab_view.tab_count == 2 && tab_view.active_index == 1);
  tai_tabset_destroy(tabs);

  InputKind compact_inputs[96];
  SDL_FPoint compact_clicks[96];
  for (size_t index = 0; index < 25; index++) {
    compact_inputs[index] = INPUT_TABS_NEW_TAB;
    compact_clicks[index] = (SDL_FPoint){15.0f, 18.0f};
  }
  compact_inputs[25] = INPUT_TABS_FIRST;
  compact_clicks[25] = (SDL_FPoint){49.0f, 18.0f};
  compact_inputs[26] = INPUT_TABS_SECOND;
  compact_clicks[26] = (SDL_FPoint){784.0f, 18.0f};
  size_t compact_count = 27;
  compact_inputs[compact_count] = INPUT_TABS_ADDRESS;
  compact_clicks[compact_count++] = (SDL_FPoint){740.0f, 56.0f};
  for (size_t index = 0; index < 64; index++)
    compact_inputs[compact_count++] = INPUT_BACKSPACE;
  compact_inputs[compact_count++] = INPUT_TABS_URL_TEXT;
  compact_inputs[compact_count] = INPUT_TABS_NEW_TAB;
  compact_clicks[compact_count++] = (SDL_FPoint){15.0f, 18.0f};
  compact_inputs[compact_count++] = INPUT_RETURN;
  compact_inputs[compact_count] = INPUT_TABS_SECOND;
  compact_clicks[compact_count++] = (SDL_FPoint){417.0f, 18.0f};
  InputEvents compact_events = {
      .kinds = compact_inputs,
      .count = compact_count,
      .click_positions = compact_clicks,
  };
  tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &compact_events);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window_with_tabs(tabs,
      "data:text/html,<p>initial</p>", 800, 100, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued);
  assert(tai_tabset_view(tabs, &tab_view));
  assert(tab_view.tab_count == 25 && tab_view.active_index == 12);
  assert(tai_tabset_select(tabs, 24));
  assert(tai_tabset_view(tabs, &tab_view));
  assert(!strcmp(tab_view.url, "data:text/html,<p>draft</p>"));
  tai_tabset_destroy(tabs);

  /* A click queued before the first pump cannot bookmark the pending page. */
  static const InputKind pending_star_kinds[] = {INPUT_TABS_FIRST};
  static const SDL_FPoint pending_star_positions[] = {{270.0f, 60.0f}};
  InputEvents pending_star = {
      .kinds = pending_star_kinds,
      .count = 1,
      .click_positions = pending_star_positions,
  };
  tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  input_injected = false;
  input_events_queued = false;
  SDL_SetEventFilter(inject_input, &pending_star);
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  assert(tai_present_window_with_tabs(tabs,
      "data:text/html,<p>initial</p>", 300, 180, &error));
  assert(pthread_join(thread, NULL) == 0);
  assert(!error && input_injected && input_events_queued);
  assert(tai_tabset_view(tabs, &tab_view));
  assert(tab_view.page && tab_view.bookmarkable && !tab_view.bookmarked &&
         !tab_view.loading && tab_view.history_count == 1);
  tai_tabset_destroy(tabs);

  /* The other window's star click is ignored; the current star saves the
   * committed URL, then the distinct left button opens the internal list. */
  static const InputKind bookmark_kinds[] = {
      INPUT_UNRELATED_LEFT_CLICK, INPUT_TABS_FIRST, INPUT_TABS_FIRST,
  };
  static const SDL_FPoint bookmark_positions[] = {
      {270.0f, 60.0f}, {270.0f, 60.0f}, {110.0f, 52.0f},
  };
  tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  present_delayed_bookmark_inputs(tabs, "data:text/html,<p>initial</p>",
      300, 180, bookmark_kinds, bookmark_positions, 3, true);
  assert(tai_tabset_view(tabs, &tab_view));
  assert(tab_view.page && !tab_view.loading &&
         !strcmp(tab_view.url, "about:bookmarks") &&
         tab_view.history_count == 2 && tab_view.can_go_back &&
         !tab_view.bookmarkable);
  TaiNode *saved_link = find(tai_page_root(tab_view.page), "a");
  assert(saved_link && !strcmp(tai_map_get(&saved_link->attributes, "href"),
                               "data:text/html,<p>initial</p>"));
  tai_tabset_destroy(tabs);

  static const InputKind toggle_twice_kinds[] = {
      INPUT_TABS_FIRST, INPUT_TABS_FIRST, INPUT_TABS_FIRST,
  };
  static const SDL_FPoint toggle_twice_positions[] = {
      {270.0f, 60.0f}, {270.0f, 60.0f}, {110.0f, 52.0f},
  };
  tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  present_delayed_bookmark_inputs(tabs, "data:text/html,<p>initial</p>",
      300, 180, toggle_twice_kinds, toggle_twice_positions, 3, true);
  assert(tai_tabset_view(tabs, &tab_view));
  assert(tab_view.page && !tab_view.loading &&
         !strcmp(tab_view.url, "about:bookmarks") &&
         tab_view.history_count == 2 &&
         !find(tai_page_root(tab_view.page), "a"));
  tai_tabset_destroy(tabs);

  static const InputKind internal_star_kinds[] = {INPUT_TABS_FIRST};
  static const SDL_FPoint internal_star_positions[] = {{270.0f, 60.0f}};
  tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  present_delayed_bookmark_inputs(tabs, "about:bookmarks", 300, 180,
      internal_star_kinds, internal_star_positions, 1, true);
  assert(tai_tabset_view(tabs, &tab_view));
  assert(tab_view.page && !tab_view.loading &&
         !strcmp(tab_view.url, "about:bookmarks") &&
         !tab_view.bookmarkable && !tab_view.bookmarked &&
         tab_view.history_count == 1 &&
         !find(tai_page_root(tab_view.page), "a"));
  tai_tabset_destroy(tabs);

  /* At 120px the list button moves above the address field and the star
   * remains in the field's rightmost 23px. */
  static const SDL_FPoint narrow_bookmark_positions[] = {
      {90.0f, 130.0f}, {12.0f, 103.0f},
  };
  tabs = tai_tabset_create_with_home_url(
      "html {display:block} body {display:block} p {display:block}", false,
      "data:text/html,<p>home</p>", &error);
  assert(tabs && !error);
  present_delayed_bookmark_inputs(tabs, "data:text/html,<p>initial</p>",
      120, 200, bookmark_kinds + 1, narrow_bookmark_positions, 2, true);
  assert(tai_tabset_view(tabs, &tab_view));
  assert(tab_view.page && !tab_view.loading &&
         !strcmp(tab_view.url, "about:bookmarks") &&
         tab_view.history_count == 2);
  saved_link = find(tai_page_root(tab_view.page), "a");
  assert(saved_link && !strcmp(tai_map_get(&saved_link->attributes, "href"),
                               "data:text/html,<p>initial</p>"));
  tai_tabset_destroy(tabs);

  tai_page_destroy(click_page);
  tai_url_destroy(click_url);

  tai_page_destroy(input_page);
  tai_url_destroy(input_url);
  tai_page_destroy(page);
  tai_url_destroy(url);
  tai_network_destroy(network);
  return 0;
}
