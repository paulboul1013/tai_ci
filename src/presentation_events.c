#include "presentation_internal.h"
#include <float.h>

bool tai_presentation_pointer_to_pixels(double logical_x, double logical_y,
                                        int logical_width, int logical_height,
                                        int pixel_width, int pixel_height,
                                        double *pixel_x, double *pixel_y) {
  if (!pixel_x || !pixel_y || !isfinite(logical_x) || !isfinite(logical_y) ||
      logical_width <= 0 || logical_height <= 0 ||
      pixel_width <= 0 || pixel_height <= 0) return false;
  double x = logical_x * ((double)pixel_width / logical_width);
  double y = logical_y * ((double)pixel_height / logical_height);
  if (!isfinite(x) || !isfinite(y)) return false;
  *pixel_x = x;
  *pixel_y = y;
  return true;
}

/* SDL button positions are window coordinates. The page, chrome, and SDL
 * renderer use physical pixels, so map each event once before routing it. */
void tai_pres_pointer_event_to_pixels(SDL_Window *window,
                                      SDL_WindowID window_id,
                                      SDL_Event *event) {
  if (event->type != SDL_EVENT_MOUSE_BUTTON_DOWN ||
      event->button.windowID != window_id ||
      !isfinite(event->button.x) || !isfinite(event->button.y)) return;
  int logical_width = 0, logical_height = 0;
  int pixel_width = 0, pixel_height = 0;
  double x, y;
  if (!SDL_GetWindowSize(window, &logical_width, &logical_height) ||
      !SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height) ||
      !tai_presentation_pointer_to_pixels(event->button.x, event->button.y,
          logical_width, logical_height, pixel_width, pixel_height, &x, &y) ||
      fabs(x) > FLT_MAX || fabs(y) > FLT_MAX) {
    event->button.x = NAN;
    event->button.y = NAN;
    return;
  }
  event->button.x = (float)x;
  event->button.y = (float)y;
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
bool tai_pres_handle_page_event(TaiPage *page, const SDL_Event *event,
                                SDL_WindowID window_id, bool window_focused,
                                double page_y_offset, bool *changed,
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
                                      trunc(event->button.y - page_y_offset),
                                      changed, error);
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

bool tai_pres_sync_text_input(SDL_Window *window, const TaiPage *page,
                              bool window_focused, bool address_focused,
                              bool *started,
                              char **error) {
  bool should_start = window_focused &&
      (address_focused || (page && tai_page_text_input_active(page)));
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
