#include "tai/presentation.h"
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdlib.h>

static void set_error(char **error, const char *message) {
  if (error && !*error) *error = tai_strdup(message);
}

static bool present_texture(SDL_Renderer *renderer, SDL_Texture *texture) {
  return SDL_RenderClear(renderer) &&
         SDL_RenderTexture(renderer, texture, NULL, NULL) &&
         SDL_RenderPresent(renderer);
}

static bool valid_pixel_dimensions(int width, int height) {
  return width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
         (int64_t)width * height <= 25000000;
}

static bool paint(SDL_Renderer *renderer, SDL_Texture **texture,
                  const TaiPage *page, int width, int height, char **error) {
  if (!valid_pixel_dimensions(width, height)) {
    set_error(error, "unsupported window pixel dimensions");
    return false;
  }
  unsigned char *pixels = NULL;
  int stride = 0;
  const TaiDisplayList *list = tai_page_display_list(page);
  if (!list || !tai_display_list_raster_region(list, width, height, 0,
                                      tai_page_scroll_y(page),
                                      &pixels, &stride, error)) return false;
  SDL_Texture *next = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, width, height);
  /* Cairo ARGB32 and SDL ARGB8888 are both native-endian 0xAARRGGBB.
   * Opaque white backing means there is no premultiplied-alpha blend seam. */
  bool ok = next && SDL_UpdateTexture(next, NULL, pixels, stride) &&
            present_texture(renderer, next);
  free(pixels);
  if (!ok) {
    set_error(error, SDL_GetError());
    SDL_DestroyTexture(next);
    return false;
  }
  SDL_DestroyTexture(*texture);
  *texture = next;
  return true;
}

bool tai_present_window(TaiPage *page, int width, int height, char **error) {
  if (error) { free(*error); *error = NULL; }
  if (!page || width <= 0 || height <= 0) {
    set_error(error, "invalid window input");
    return false;
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    set_error(error, SDL_GetError());
    return false;
  }
  SDL_Window *window = SDL_CreateWindow("Tai Gar", width, height,
                                         SDL_WINDOW_RESIZABLE);
  SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
  SDL_Texture *texture = NULL;
  bool ok = renderer != NULL;
  if (!ok) set_error(error, SDL_GetError());
  if (ok) {
    int pixel_width = 0, pixel_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height)) {
      set_error(error, SDL_GetError());
      ok = false;
    } else if (!valid_pixel_dimensions(pixel_width, pixel_height)) {
      set_error(error, "unsupported window pixel dimensions");
      ok = false;
    } else {
      ok = tai_page_resize(page, pixel_width, pixel_height, error) &&
           paint(renderer, &texture, page, pixel_width, pixel_height, error);
    }
  }
  bool running = ok;
  while (running) {
    SDL_Event event;
    if (!SDL_WaitEvent(&event)) {
      set_error(error, SDL_GetError());
      ok = false;
      break;
    }
    if (event.type == SDL_EVENT_QUIT ||
        (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
         event.window.windowID == SDL_GetWindowID(window))) {
      running = false;
    } else if (event.type == SDL_EVENT_WINDOW_EXPOSED &&
               event.window.windowID == SDL_GetWindowID(window) && texture) {
      if (!present_texture(renderer, texture)) {
        set_error(error, SDL_GetError());
        ok = false;
        break;
      }
    } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
               event.window.windowID == SDL_GetWindowID(window)) {
      int pixel_width = event.window.data1;
      int pixel_height = event.window.data2;
      if (pixel_width > 0 && pixel_height > 0 &&
          (!valid_pixel_dimensions(pixel_width, pixel_height) ||
           !tai_page_resize(page, pixel_width, pixel_height, error) ||
           !paint(renderer, &texture, page, pixel_width, pixel_height,
                  error))) {
        if (!error || !*error)
          set_error(error, "unsupported window pixel dimensions");
        ok = false;
        break;
      }
    }
  }
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return ok;
}
