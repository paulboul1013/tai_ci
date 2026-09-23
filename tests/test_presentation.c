#define _POSIX_C_SOURCE 200809L
#include "tai/presentation.h"
#include "../src/presentation_geometry.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
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
  INPUT_RETURN,
  INPUT_WINDOW_FOCUS_LOST,
  INPUT_WINDOW_FOCUS_GAINED,
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
      kind == INPUT_CURRENT_TEXT) {
    return (SDL_Event){.text = {
        .type = SDL_EVENT_TEXT_INPUT,
        .windowID = kind == INPUT_OTHER_WINDOW_TEXT ? window_id + 1 :
                                                     window_id,
        .text = kind == INPUT_UNFOCUSED_TEXT ? "ignored-before-focus" : "\303\251x",
    }};
  }
  if (kind == INPUT_OTHER_WINDOW_BACKSPACE || kind == INPUT_BACKSPACE ||
      kind == INPUT_LEFT || kind == INPUT_RIGHT) {
    return (SDL_Event){.key = {
        .type = SDL_EVENT_KEY_DOWN,
        .windowID = kind == INPUT_OTHER_WINDOW_BACKSPACE ? window_id + 1 :
                                                         window_id,
        .key = kind == INPUT_BACKSPACE || kind == INPUT_OTHER_WINDOW_BACKSPACE
                   ? SDLK_BACKSPACE
               : kind == INPUT_LEFT ? SDLK_LEFT : SDLK_RIGHT,
        .down = true,
    }};
  }
  if (kind == INPUT_UNRELATED_LEFT_CLICK || kind == INPUT_NONLEFT_CLICK ||
      kind == INPUT_NONFINITE_CLICK || kind == INPUT_MISS_CLICK ||
      kind == INPUT_LEFT_CLICK) {
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

static void *request_quit(void *unused) {
  (void)unused;
  struct timespec pause = {.tv_sec = 1, .tv_nsec = 0};
  nanosleep(&pause, NULL);
  SDL_Event event = {.type = SDL_EVENT_QUIT};
  SDL_PushEvent(&event);
  return NULL;
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

  tai_page_destroy(click_page);
  tai_url_destroy(click_url);

  tai_page_destroy(input_page);
  tai_url_destroy(input_url);
  tai_page_destroy(page);
  tai_url_destroy(url);
  tai_network_destroy(network);
  return 0;
}
