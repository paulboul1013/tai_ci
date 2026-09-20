#define _POSIX_C_SOURCE 200809L
#include "tai/presentation.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
  int width;
  int height;
} PresentationEvents;

static bool resize_injected = false;

static bool inject_resize(void *opaque, SDL_Event *event) {
  const PresentationEvents *events = opaque;
  if (event->type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || resize_injected)
    return true;
  resize_injected = true;
  event->window.data1 = events->width;
  event->window.data2 = events->height;
  return true;
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
  tai_page_destroy(page);
  tai_url_destroy(url);
  tai_network_destroy(network);
  return 0;
}
