#include "presentation_internal.h"
#include <float.h>

static uint64_t traced_actions; /* SDL owner thread only */

/* Code points, not bytes: the trace keeps how much was typed, never what. */
size_t tai_pres_utf8_length(const char *text) {
  size_t length = 0;
  for (const unsigned char *p = (const unsigned char *)text; p && *p; p++)
    length += (*p & 0xc0) != 0x80;
  return length;
}

/* Keypad keys that type a character; their keycodes carry the scancode
 * mask, so the printable test alone would name them ("Keypad 5"). */
static bool keypad_typing(SDL_Scancode code) {
  return (code >= SDL_SCANCODE_KP_DIVIDE && code <= SDL_SCANCODE_KP_PERIOD &&
          code != SDL_SCANCODE_KP_ENTER) ||
         code == SDL_SCANCODE_KP_EQUALS || code == SDL_SCANCODE_KP_COMMA ||
         code == SDL_SCANCODE_KP_EQUALSAS400 ||
         (code >= SDL_SCANCODE_KP_00 && code <= SDL_SCANCODE_KP_HEXADECIMAL);
}

/* T1 (2026-11-02): a key that types a character has its name withheld;
 * named keys and shortcuts are kept. AltGr types characters too: SDL reports
 * it as Mode, as right Alt, or (Windows) as left Ctrl plus right Alt. */
static const char *trace_key_name(const SDL_KeyboardEvent *key) {
  bool printable = keypad_typing(key->scancode) ||
      (!(key->key & (SDLK_SCANCODE_MASK | SDLK_EXTENDED_MASK)) &&
       key->key >= 0x20 && key->key != 0x7f);
  SDL_Keymod shortcut = key->mod & (SDL_KMOD_CTRL | SDL_KMOD_LALT | SDL_KMOD_GUI);
  bool altgr = (key->mod & SDL_KMOD_MODE) ||
      ((key->mod & SDL_KMOD_RALT) && !(key->mod & (SDL_KMOD_LALT | SDL_KMOD_GUI)));
  if (printable && (!shortcut || altgr)) return "char";
  return SDL_GetKeyName(key->key);
}

static const char *trace_modifiers(SDL_Keymod mod, char *buffer, size_t size) {
  snprintf(buffer, size, "%s%s%s%s", mod & SDL_KMOD_CTRL ? "ctrl+" : "",
           mod & SDL_KMOD_ALT ? "alt+" : "", mod & SDL_KMOD_SHIFT ? "shift+" : "",
           mod & SDL_KMOD_GUI ? "gui+" : "");
  size_t length = strlen(buffer);
  if (length) buffer[length - 1] = 0;
  return buffer;
}

static uint64_t trace_coordinate(float value) {
  return isfinite(value) && value > 0 ? (uint64_t)value : 0;
}

bool tai_pres_trace_input(SDL_WindowID window, const SDL_Event *event) {
  TaiTraceArg args[5] = {{"window", TAI_TRACE_UINT, NULL, window}};
  size_t count = 2;
  char modifiers[24];
  switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
      Uint8 button = event->button.button;
      args[1] = (TaiTraceArg){"kind", TAI_TRACE_STRING, "click", 0};
      args[count++] = (TaiTraceArg){"button", TAI_TRACE_STRING,
          button == SDL_BUTTON_LEFT ? "left" :
          button == SDL_BUTTON_MIDDLE ? "middle" :
          button == SDL_BUTTON_RIGHT ? "right" : "other", 0};
      args[count++] = (TaiTraceArg){"x", TAI_TRACE_UINT, NULL,
                                    trace_coordinate(event->button.x)};
      args[count++] = (TaiTraceArg){"y", TAI_TRACE_UINT, NULL,
                                    trace_coordinate(event->button.y)};
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
      args[1] = (TaiTraceArg){"kind", TAI_TRACE_STRING, "wheel", 0};
      args[count++] = (TaiTraceArg){"direction", TAI_TRACE_STRING,
          event->wheel.y > 0 ? "up" : event->wheel.y < 0 ? "down" : "none", 0};
      break;
    case SDL_EVENT_KEY_DOWN:
      args[1] = (TaiTraceArg){"kind", TAI_TRACE_STRING, "key", 0};
      args[count++] = (TaiTraceArg){"key", TAI_TRACE_STRING,
                                    trace_key_name(&event->key), 0};
      args[count++] = (TaiTraceArg){"mod", TAI_TRACE_STRING,
          trace_modifiers(event->key.mod, modifiers, sizeof modifiers), 0};
      break;
    case SDL_EVENT_TEXT_INPUT:
      args[1] = (TaiTraceArg){"kind", TAI_TRACE_STRING, "text", 0};
      args[count++] = (TaiTraceArg){"length", TAI_TRACE_UINT, NULL,
                                    tai_pres_utf8_length(event->text.text)};
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      args[1] = (TaiTraceArg){"kind", TAI_TRACE_STRING, "resize", 0};
      args[count++] = (TaiTraceArg){"width", TAI_TRACE_UINT, NULL,
          event->window.data1 > 0 ? (uint64_t)event->window.data1 : 0};
      args[count++] = (TaiTraceArg){"height", TAI_TRACE_UINT, NULL,
          event->window.data2 > 0 ? (uint64_t)event->window.data2 : 0};
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      args[1] = (TaiTraceArg){"kind", TAI_TRACE_STRING, "close", 0};
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      args[1] = (TaiTraceArg){"kind", TAI_TRACE_STRING,
          event->type == SDL_EVENT_WINDOW_FOCUS_GAINED ? "focus" : "blur", 0};
      break;
    default:
      return false;
  }
  tai_trace_instant(TAI_INSTANT_INPUT, args, count);
  return true;
}

void tai_pres_trace_action(SDL_WindowID window, const char *action,
                           const TaiTraceArg *details, size_t count) {
  TaiTraceArg args[6] = {{"window", TAI_TRACE_UINT, NULL, window},
                         {"action", TAI_TRACE_STRING, action, 0}};
  if (count > 4) count = 4;
  for (size_t i = 0; i < count; i++) args[2 + i] = details[i];
  traced_actions++;
  tai_trace_instant(TAI_INSTANT_ACTION, args, 2 + count);
}

uint64_t tai_pres_trace_action_count(void) { return traced_actions; }

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

/* Every scroll input goes through tai_page_scroll_by, so a click-chosen
 * overflow container takes it before the page. PageUp/PageDown are native
 * additions routed like Up/Down (PORTING_PLAN.md). */
static bool handle_scroll_event(TaiPage *page, const SDL_Event *event,
                                SDL_WindowID window_id, bool *changed,
                                char **error) {
  double delta;
  if (event->type == SDL_EVENT_MOUSE_WHEEL) {
    if (event->wheel.windowID != window_id || !isfinite(event->wheel.y))
      return true;
    double wheel_y = event->wheel.y;
    if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
      wheel_y = -wheel_y;
    else if (event->wheel.direction != SDL_MOUSEWHEEL_NORMAL)
      return true;
    /* The frozen SDL2 oracle converts wheel y to int before selecting a
     * single 100px step, so fractional SDL3 wheel deltas remain no-ops. */
    if (wheel_y >= 1.0) delta = -TAI_PRESENTATION_SCROLL_STEP;
    else if (wheel_y <= -1.0) delta = TAI_PRESENTATION_SCROLL_STEP;
    else return true;
  } else if (event->type == SDL_EVENT_KEY_DOWN &&
             event->key.windowID == window_id &&
             (event->key.key == SDLK_PAGEUP || event->key.key == SDLK_UP)) {
    delta = -TAI_PRESENTATION_SCROLL_STEP;
  } else if (event->type == SDL_EVENT_KEY_DOWN &&
             event->key.windowID == window_id &&
             (event->key.key == SDLK_PAGEDOWN ||
              event->key.key == SDLK_DOWN)) {
    delta = TAI_PRESENTATION_SCROLL_STEP;
  } else {
    return true;
  }
  bool ok = tai_page_scroll_by(page, delta, changed, error);
  TaiTraceArg details[] = {
      {"direction", TAI_TRACE_STRING, delta < 0 ? "up" : "down", 0},
      {"moved", TAI_TRACE_UINT, NULL, ok && *changed},
  };
  tai_pres_trace_action(window_id, "scroll", details, 2);
  return ok;
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
    TaiTraceArg length = {"length", TAI_TRACE_UINT, NULL,
                          tai_pres_utf8_length(event->text.text)};
    tai_pres_trace_action(window_id, "page_text", &length, 1);
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
        return handle_scroll_event(page, event, window_id, changed, error);
    }
    TaiTraceArg name = {"key", TAI_TRACE_STRING,
                        SDL_GetKeyName(event->key.key), 0};
    tai_pres_trace_action(window_id, "page_key", &name, 1);
    return tai_page_key(page, key, changed, error);
  }
  return handle_scroll_event(page, event, window_id, changed, error);
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
