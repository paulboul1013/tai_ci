#define _POSIX_C_SOURCE 200809L
#include "tai/presentation.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
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
  INPUT_UNRELATED_WHEEL,
  INPUT_FLIPPED_DOWN,
  INPUT_FRACTIONAL_WHEEL,
  INPUT_NONFINITE_WHEEL,
  INPUT_UNKNOWN_DIRECTION,
} InputKind;

typedef struct {
  const InputKind *kinds;
  size_t count;
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
      .key = kind == INPUT_PAGE_UP ? SDLK_PAGEUP : SDLK_PAGEDOWN,
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
    if (!SDL_PushEvent(&input)) return false;
  }
  input_events_queued = true;
  return false;
}

static void *request_quit(void *unused) {
  (void)unused;
  struct timespec pause = {.tv_sec = 1, .tv_nsec = 0};
  nanosleep(&pause, NULL);
  SDL_Event event = {.type = SDL_EVENT_QUIT};
  SDL_PushEvent(&event);
  return NULL;
}

int main(void) {
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
         tai_page_scroll_y(input_page) == 100.0);

  tai_page_destroy(input_page);
  tai_url_destroy(input_url);
  tai_page_destroy(page);
  tai_url_destroy(url);
  tai_network_destroy(network);
  return 0;
}
