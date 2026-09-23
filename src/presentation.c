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

/* This is deliberately a narrow SDL adapter: TaiPage owns coordinate
 * conversion, DOM target normalization, JavaScript dispatch, and all frame
 * replacement. Its success result is distinct from a changed frame so a
 * current-window miss never needlessly replaces the retained texture. */
static bool handle_page_event(TaiPage *page, const SDL_Event *event,
                              SDL_WindowID window_id, bool window_focused,
                              bool *changed,
                              char **error) {
  *changed = false;
  if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    if (event->button.windowID != window_id ||
        event->button.button != SDL_BUTTON_LEFT ||
        !isfinite(event->button.x) || !isfinite(event->button.y))
      return true;
    /* The frozen SDL2 reference converts pointer positions to int before
     * hit-testing. Truncating while still a finite double avoids unsafe casts
     * yet preserves its observable fractional-coordinate behavior. */
    return tai_page_activate_viewport(page, trunc(event->button.x),
                                      trunc(event->button.y), changed, error);
  }
  if (event->type == SDL_EVENT_TEXT_INPUT) {
    if (!window_focused || event->text.windowID != window_id ||
        !tai_page_text_input_active(page))
      return true;
    return tai_page_text_input(page, event->text.text, changed, error);
  }
  if (event->type == SDL_EVENT_KEY_DOWN) {
    if (!window_focused || event->key.windowID != window_id) return true;
    TaiPageKey key;
    switch (event->key.key) {
      case SDLK_BACKSPACE: key = TAI_PAGE_KEY_BACKSPACE; break;
      case SDLK_LEFT: key = TAI_PAGE_KEY_LEFT; break;
      case SDLK_RIGHT: key = TAI_PAGE_KEY_RIGHT; break;
      case SDLK_RETURN: key = TAI_PAGE_KEY_RETURN; break;
      default:
        *changed = handle_scroll_event(page, event, window_id);
        return true;
    }
    return tai_page_key(page, key, changed, error);
  }
  *changed = handle_scroll_event(page, event, window_id);
  return true;
}

static bool sync_text_input(SDL_Window *window, const TaiPage *page,
                            bool window_focused, bool *started,
                            char **error) {
  bool should_start = window_focused && tai_page_text_input_active(page);
  if (should_start == *started) return true;
  bool ok = should_start ? SDL_StartTextInput(window) :
                           SDL_StopTextInput(window);
  if (!ok) {
    set_error(error, SDL_GetError());
    return false;
  }
  *started = should_start;
  return true;
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

bool tai_present_window_with_navigation(TaiPage **page_slot, int width,
                                        int height, TaiPresentNavigate navigate,
                                        void *userdata, char **error) {
  if (error) { free(*error); *error = NULL; }
  if (!page_slot || !*page_slot || width <= 0 || height <= 0) {
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
      ok = tai_page_resize(*page_slot, pixel_width, pixel_height, error) &&
           paint(renderer, &texture, *page_slot, pixel_width, pixel_height,
                 error);
    }
  }
  bool running = ok;
  bool window_focused = true;
  bool text_input_started = false;
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
    } else if ((event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) &&
               event.window.windowID == SDL_GetWindowID(window)) {
      window_focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
      if (!sync_text_input(window, *page_slot, window_focused,
                           &text_input_started, error)) {
        ok = false;
        break;
      }
    } else {
      bool changed = false;
      if (!handle_page_event(*page_slot, &event, SDL_GetWindowID(window),
                             window_focused, &changed,
                             error)) {
        if (!error || !*error) set_error(error, SDL_GetError());
        ok = false;
        break;
      }
      TaiNavigationIntent *intent = NULL;
      if (!tai_page_take_navigation_intent(*page_slot, &intent)) {
        set_error(error, "could not take page navigation intent");
        ok = false;
        break;
      }
      if (intent) {
        bool navigated = !navigate ||
            navigate(userdata, page_slot, intent, error);
        tai_navigation_intent_destroy(intent);
        if (!navigated || !*page_slot) {
          if (!error || !*error)
            set_error(error, "navigation handler failed");
          ok = false;
          break;
        }
        /* The callback may have destroyed the previous page. An intent is a
         * repaint boundary even when loading fails and the old page remains. */
        changed = true;
      }
      if (!sync_text_input(window, *page_slot, window_focused,
                           &text_input_started, error)) {
        ok = false;
        break;
      }
      if (changed) {
        int pixel_width = 0, pixel_height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height) ||
            !paint(renderer, &texture, *page_slot, pixel_width, pixel_height,
                   error)) {
          if (!error || !*error) set_error(error, SDL_GetError());
          ok = false;
          break;
        }
      } else if (event.type == SDL_EVENT_WINDOW_EXPOSED &&
                 event.window.windowID == SDL_GetWindowID(window) && texture) {
        if (!present_texture(renderer, texture, *page_slot,
                             (int)tai_page_viewport_width(*page_slot),
                             (int)tai_page_viewport_height(*page_slot))) {
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
             !tai_page_resize(*page_slot, pixel_width, pixel_height, error) ||
             !paint(renderer, &texture, *page_slot, pixel_width, pixel_height,
                    error))) {
          if (!error || !*error)
            set_error(error, "unsupported window pixel dimensions");
          ok = false;
          break;
        }
      }
    }
  }
  if (text_input_started) SDL_StopTextInput(window);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return ok;
}

bool tai_present_window(TaiPage *page, int width, int height, char **error) {
  TaiPage *borrowed_page = page;
  return tai_present_window_with_navigation(&borrowed_page, width, height,
                                            NULL, NULL, error);
}
