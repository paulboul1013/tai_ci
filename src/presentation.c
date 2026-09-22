#include "tai/presentation.h"
#include "presentation_geometry.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define TAI_PRESENTATION_SCROLL_STEP 100.0

static void set_error(char **error, const char *message) {
  if (error && !*error) *error = tai_strdup(message);
}

static bool present_texture(SDL_Renderer *renderer, SDL_Texture *texture,
                            const TaiPage *page, int width, int height) {
  if (!SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255) ||
      !SDL_RenderClear(renderer) ||
      !SDL_RenderTexture(renderer, texture, NULL, NULL)) return false;
  TaiScrollbarRect bar;
  if (tai_scrollbar_geometry(width, height, tai_page_scroll_y(page),
                             tai_page_max_scroll_y(page), &bar)) {
    SDL_FRect rect = {bar.x, bar.y, bar.w, bar.h};
    if (!SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255) ||
        !SDL_RenderFillRect(renderer, &rect)) return false;
  }
  return SDL_RenderPresent(renderer);
}

static bool valid_pixel_dimensions(int width, int height) {
  return width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
         (int64_t)width * height <= 25000000;
}

static bool scroll_page(TaiPage *page, double delta) {
  double old_scroll = tai_page_scroll_y(page);
  double proposed = old_scroll + delta;
  if (!isfinite(proposed) || !tai_page_set_scroll_y(page, proposed)) return false;
  return tai_page_scroll_y(page) != old_scroll;
}

static bool handle_scroll_event(TaiPage *page, const SDL_Event *event,
                                SDL_WindowID window_id) {
  if (event->type == SDL_EVENT_MOUSE_WHEEL) {
    if (event->wheel.windowID != window_id || !isfinite(event->wheel.y))
      return false;
    double wheel_y = event->wheel.y;
    if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
      wheel_y = -wheel_y;
    else if (event->wheel.direction != SDL_MOUSEWHEEL_NORMAL)
      return false;
    /* The frozen SDL2 oracle converts wheel y to int before selecting a
     * single 100px step, so fractional SDL3 wheel deltas remain no-ops. */
    if (wheel_y >= 1.0) return scroll_page(page, -TAI_PRESENTATION_SCROLL_STEP);
    if (wheel_y <= -1.0) return scroll_page(page, TAI_PRESENTATION_SCROLL_STEP);
    return false;
  }
  if (event->type != SDL_EVENT_KEY_DOWN || event->key.windowID != window_id)
    return false;
  if (event->key.key == SDLK_PAGEUP || event->key.key == SDLK_UP)
    return scroll_page(page, -TAI_PRESENTATION_SCROLL_STEP);
  if (event->key.key == SDLK_PAGEDOWN || event->key.key == SDLK_DOWN)
    return scroll_page(page, TAI_PRESENTATION_SCROLL_STEP);
  return false;
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
            present_texture(renderer, next, page, width, height);
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
    } else if (handle_scroll_event(page, &event, SDL_GetWindowID(window))) {
      int pixel_width = 0, pixel_height = 0;
      if (!SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height) ||
          !paint(renderer, &texture, page, pixel_width, pixel_height, error)) {
        if (!error || !*error) set_error(error, SDL_GetError());
        ok = false;
        break;
      }
    } else if (event.type == SDL_EVENT_WINDOW_EXPOSED &&
               event.window.windowID == SDL_GetWindowID(window) && texture) {
      if (!present_texture(renderer, texture, page,
                           (int)tai_page_viewport_width(page),
                           (int)tai_page_viewport_height(page))) {
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
