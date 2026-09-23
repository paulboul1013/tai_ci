#define _POSIX_C_SOURCE 200809L
#include "tai/presentation.h"
#include "tai/session.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  SDL_Event events[64];
  size_t count;
  bool injected;
  bool queued;
} EventSequence;

typedef struct {
  TaiSession *session;
  size_t back_calls;
  size_t forward_calls;
} SessionCallbacks;

static bool inject_events(void *opaque, SDL_Event *event) {
  EventSequence *sequence = opaque;
  if (event->type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
      sequence->injected) return true;
  sequence->injected = true;
  SDL_WindowID window_id = event->window.windowID;
  for (size_t index = 0; index < sequence->count; index++) {
    SDL_Event next = sequence->events[index];
    switch (next.type) {
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        next.window.windowID = window_id;
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        next.button.windowID = window_id;
        break;
      case SDL_EVENT_TEXT_INPUT:
        next.text.windowID = window_id;
        break;
      case SDL_EVENT_KEY_DOWN:
        next.key.windowID = window_id;
        break;
      default:
        assert(false);
    }
    if (!SDL_PushEvent(&next)) return false;
  }
  sequence->queued = true;
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

static SDL_Event resize_event(int width, int height) {
  return (SDL_Event){.window = {
      .type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED,
      .data1 = width,
      .data2 = height,
  }};
}

static SDL_Event click_event(float x, float y) {
  return (SDL_Event){.button = {
      .type = SDL_EVENT_MOUSE_BUTTON_DOWN,
      .button = SDL_BUTTON_LEFT,
      .x = x,
      .y = y,
      .down = true,
  }};
}

static SDL_Event text_event(const char *text) {
  SDL_Event event = {.type = SDL_EVENT_TEXT_INPUT};
  event.text.text = text;
  return event;
}

static SDL_Event key_event(SDL_Keycode key) {
  return (SDL_Event){.key = {
      .type = SDL_EVENT_KEY_DOWN,
      .key = key,
      .down = true,
  }};
}

static bool navigate_intent(void *opaque, TaiPage **page,
                            const TaiNavigationIntent *intent,
                            char **error) {
  SessionCallbacks *callbacks = opaque;
  (void)tai_session_navigate(callbacks->session, intent, error);
  if (error) { free(*error); *error = NULL; }
  *page = tai_session_page(callbacks->session);
  return true;
}

static bool navigate_address(void *opaque, TaiPage **page, const char *text,
                             char **error) {
  SessionCallbacks *callbacks = opaque;
  (void)tai_session_navigate_address(callbacks->session, text, error);
  if (error) { free(*error); *error = NULL; }
  *page = tai_session_page(callbacks->session);
  return true;
}

static bool traverse_history(void *opaque, TaiPage **page, int direction,
                             char **error) {
  SessionCallbacks *callbacks = opaque;
  if (direction < 0) callbacks->back_calls++;
  else callbacks->forward_calls++;
  bool ok = direction < 0 ? tai_session_back(callbacks->session, error)
                          : tai_session_forward(callbacks->session, error);
  if (!ok && error) { free(*error); *error = NULL; }
  *page = tai_session_page(callbacks->session);
  return true;
}

static bool history_available(void *opaque, int direction) {
  SessionCallbacks *callbacks = opaque;
  size_t index = tai_session_history_index(callbacks->session);
  size_t count = tai_session_history_length(callbacks->session);
  return direction < 0 ? index > 0 : index + 1 < count;
}

static bool record_fragment(void *opaque, const char *url, char **error) {
  SessionCallbacks *callbacks = opaque;
  return tai_session_record_fragment(callbacks->session, url, error);
}

static TaiPage *load_page(TaiNetwork *network, const char *text,
                          const char *css, double height) {
  TaiUrl *url = tai_url_parse(text);
  char *error = NULL;
  TaiPage *page = url ? tai_page_load(network, url, css, 320.0, height,
                                     false, &error) : NULL;
  assert(page && !error);
  tai_url_destroy(url);
  free(error);
  return page;
}

static void test_address_edit_and_history(void) {
  static const char css[] = "html {display:block} body {display:block}";
  TaiNetwork *network = tai_network_create();
  assert(network);
  TaiPage *initial = load_page(network, "data:text/html,start", css, 160.0);
  TaiSession *session = tai_session_create(network, initial, css, false);
  assert(session);
  SessionCallbacks context = {.session = session};
  TaiPresentWindowCallbacks callbacks = {
      .navigate = navigate_intent,
      .history = traverse_history,
      .fragment = record_fragment,
      .address = navigate_address,
      .history_available = history_available,
      .userdata = &context,
  };
  EventSequence sequence = {.events = {
      resize_event(0, 0),
      resize_event(320, 60),
      resize_event(320, 240),
      click_event(300.0f, 54.0f),
      text_event("abc"),
      key_event(SDLK_LEFT),
      text_event("\351\233\252"),
      key_event(SDLK_BACKSPACE),
      key_event(SDLK_RIGHT),
      key_event(SDLK_RETURN),
      click_event(27.0f, 53.0f),
      click_event(78.0f, 53.0f),
  }, .count = 12};
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  SDL_SetEventFilter(inject_events, &sequence);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  TaiPage *page = tai_session_page(session);
  char *error = NULL;
  assert(tai_present_window_with_chrome(&page, 320, 200, &callbacks, &error));
  assert(pthread_join(thread, NULL) == 0);
  SDL_SetEventFilter(NULL, NULL);
  assert(!error && sequence.injected && sequence.queued);
  assert(tai_session_history_length(session) == 2 &&
         tai_session_history_index(session) == 1);
  assert(page == tai_session_page(session));
  assert(!strcmp(tai_url_string(tai_page_url(page)), "data:text/html,startabc"));
  assert(tai_page_viewport_width(page) == 320.0);
  assert(fabs(tai_page_viewport_height(page) - (240.0 - 68.34)) < 0.001);
  free(error);
  tai_session_destroy(session);
  tai_network_destroy(network);
}

static void test_toolbar_boundary_and_page_coordinates(void) {
  static const char css[] =
      "html {display:block} body {display:block; margin:0px} a {display:block}";
  TaiNetwork *network = tai_network_create();
  assert(network);
  TaiPage *initial = load_page(network,
      "data:text/html,%3Ca%20href%3D%22data%3Atext%2Fhtml%2Cclicked%22%3Ego%3C/a%3E",
      css, 120.0);
  TaiSession *session = tai_session_create(network, initial, css, false);
  assert(session);
  SessionCallbacks context = {.session = session};
  TaiPresentWindowCallbacks callbacks = {
      .navigate = navigate_intent,
      .history = traverse_history,
      .fragment = record_fragment,
      .address = navigate_address,
      .history_available = history_available,
      .userdata = &context,
  };
  EventSequence sequence = {.events = {
      click_event(14.0f, 68.0f),
      click_event(14.0f, 90.0f),
  }, .count = 2};
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  SDL_SetEventFilter(inject_events, &sequence);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  TaiPage *page = tai_session_page(session);
  char *error = NULL;
  assert(tai_present_window_with_chrome(&page, 320, 200, &callbacks, &error));
  assert(pthread_join(thread, NULL) == 0);
  SDL_SetEventFilter(NULL, NULL);
  assert(!error && sequence.injected && sequence.queued);
  assert(tai_session_history_length(session) == 2 &&
         tai_session_history_index(session) == 1);
  assert(!strcmp(tai_url_string(tai_page_url(page)), "data:text/html,clicked"));
  free(error);
  tai_session_destroy(session);
  tai_network_destroy(network);
}

static void test_page_click_blurs_address_and_keeps_page_input(void) {
  static const char css[] =
      "html {display:block} body {display:block; margin:0px} input {display:block}";
  TaiNetwork *network = tai_network_create();
  assert(network);
  TaiPage *initial = load_page(network, "data:text/html,%3Cinput%3E", css,
                               120.0);
  TaiSession *session = tai_session_create(network, initial, css, false);
  assert(session);
  SessionCallbacks context = {.session = session};
  TaiPresentWindowCallbacks callbacks = {
      .navigate = navigate_intent,
      .history = traverse_history,
      .fragment = record_fragment,
      .address = navigate_address,
      .history_available = history_available,
      .userdata = &context,
  };
  EventSequence sequence = {.events = {
      resize_event(200, 240),
      resize_event(100, 240),
      click_event(50.0f, 99.0f),
      text_event("address-edit"),
      resize_event(200, 240),
      click_event(14.0f, 103.0f),
      text_event("page-input"),
  }, .count = 7};
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  SDL_SetEventFilter(inject_events, &sequence);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  TaiPage *page = tai_session_page(session);
  char *error = NULL;
  assert(tai_present_window_with_chrome(&page, 320, 200, &callbacks, &error));
  assert(pthread_join(thread, NULL) == 0);
  SDL_SetEventFilter(NULL, NULL);
  assert(!error && sequence.injected && sequence.queued);
  assert(tai_session_history_length(session) == 1);
  assert(!strcmp(tai_url_string(tai_page_url(page)), "data:text/html,%3Cinput%3E"));
  assert(tai_page_viewport_width(page) == 200.0);
  assert(fabs(tai_page_viewport_height(page) - 158.0) < 0.001);
  assert(tai_page_text_input_active(page));
  char *json = NULL;
  size_t json_length = 0;
  FILE *stream = open_memstream(&json, &json_length);
  assert(stream);
  tai_page_json(stream, page);
  assert(fclose(stream) == 0);
  assert(json_length > 0 && strstr(json, "\"value\":\"page-input\""));
  free(json);
  free(error);
  tai_session_destroy(session);
  tai_network_destroy(network);
}

static void test_changed_page_discards_stale_address_edit(void) {
  static const char css[] =
      "html {display:block} body {display:block; margin:0px} a {display:block}";
  TaiNetwork *network = tai_network_create();
  assert(network);
  TaiPage *initial = load_page(network,
      "data:text/html,%3Ca%20href%3D%22data%3Atext%2Fhtml%2Cclicked%22%3Ego%3C/a%3E",
      css, 120.0);
  TaiSession *session = tai_session_create(network, initial, css, false);
  assert(session);
  SessionCallbacks context = {.session = session};
  TaiPresentWindowCallbacks callbacks = {
      .navigate = navigate_intent,
      .history = traverse_history,
      .fragment = record_fragment,
      .address = navigate_address,
      .history_available = history_available,
      .userdata = &context,
  };
  EventSequence sequence = {.events = {
      click_event(300.0f, 54.0f),
      text_event("stale-edit"),
      click_event(14.0f, 90.0f),
      click_event(300.0f, 54.0f),
      key_event(SDLK_RETURN),
  }, .count = 5};
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  SDL_SetEventFilter(inject_events, &sequence);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  TaiPage *page = tai_session_page(session);
  char *error = NULL;
  assert(tai_present_window_with_chrome(&page, 320, 200, &callbacks, &error));
  assert(pthread_join(thread, NULL) == 0);
  SDL_SetEventFilter(NULL, NULL);
  assert(!error && sequence.injected && sequence.queued);
  assert(tai_session_history_length(session) == 3 &&
         tai_session_history_index(session) == 2);
  assert(!strcmp(tai_url_string(tai_page_url(page)), "data:text/html,clicked"));
  free(error);
  tai_session_destroy(session);
  tai_network_destroy(network);
}

static void test_narrow_address_rows_and_forward_button(void) {
  static const char css[] = "html {display:block} body {display:block}";
  TaiNetwork *network = tai_network_create();
  assert(network);
  TaiPage *initial = load_page(network, "about:blank", css, 120.0);
  TaiSession *session = tai_session_create(network, initial, css, false);
  assert(session);
  SessionCallbacks context = {.session = session};
  TaiPresentWindowCallbacks callbacks = {
      .navigate = navigate_intent,
      .history = traverse_history,
      .fragment = record_fragment,
      .address = navigate_address,
      .history_available = history_available,
      .userdata = &context,
  };
  EventSequence sequence = {.events = {
      resize_event(128, 240),
      click_event(99.0f, 68.0f),
      text_event("?x"),
      key_event(SDLK_RETURN),
      resize_event(127, 240),
      click_event(99.0f, 99.0f),
      text_event("?y"),
      key_event(SDLK_RETURN),
      resize_event(79, 240),
      click_event(76.0f, 99.0f),
      text_event("?w"),
      key_event(SDLK_RETURN),
      click_event(10.0f, 48.0f),
      click_event(10.0f, 75.0f),
      resize_event(78, 240),
      click_event(76.0f, 128.0f),
      text_event("?h"),
      key_event(SDLK_RETURN),
      click_event(10.0f, 48.0f),
      click_event(10.0f, 75.0f),
      resize_event(93, 240),
      click_event(10.0f, 48.0f),
      click_event(10.0f, 75.0f),
      resize_event(94, 240),
      click_event(10.0f, 48.0f),
      click_event(60.0f, 48.0f),
      resize_event(77, 240),
      click_event(76.0f, 128.0f),
      text_event("?z"),
      key_event(SDLK_RETURN),
      resize_event(231, 240),
      click_event(99.0f, 68.0f),
      text_event("?a"),
      key_event(SDLK_RETURN),
      resize_event(232, 240),
      click_event(200.0f, 54.0f),
      text_event("?b"),
      key_event(SDLK_RETURN),
  }, .count = 38};
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  SDL_SetEventFilter(inject_events, &sequence);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  TaiPage *page = tai_session_page(session);
  char *error = NULL;
  assert(tai_present_window_with_chrome(&page, 320, 200, &callbacks, &error));
  assert(pthread_join(thread, NULL) == 0);
  SDL_SetEventFilter(NULL, NULL);
  assert(!error && sequence.injected && sequence.queued);
  assert(tai_session_history_length(session) == 8 &&
         tai_session_history_index(session) == 7);
  assert(!strncmp(tai_url_string(tai_page_url(page)), "about:", 6));
  assert(tai_page_viewport_width(page) == 232.0);
  assert(fabs(tai_page_viewport_height(page) - (240.0 - 68.34)) < 0.001);
  assert(context.back_calls == 4 && context.forward_calls == 4);
  free(error);
  tai_session_destroy(session);
  tai_network_destroy(network);
}

static void test_tiny_resize_is_ignored(void) {
  static const char css[] = "html {display:block} body {display:block}";
  TaiNetwork *network = tai_network_create();
  assert(network);
  TaiPage *initial = load_page(network, "about:blank", css, 120.0);
  TaiSession *session = tai_session_create(network, initial, css, false);
  assert(session);
  SessionCallbacks context = {.session = session};
  TaiPresentWindowCallbacks callbacks = {
      .navigate = navigate_intent,
      .history = traverse_history,
      .fragment = record_fragment,
      .address = navigate_address,
      .history_available = history_available,
      .userdata = &context,
  };
  EventSequence sequence = {.events = {
      resize_event(0, 0),
      resize_event(10, 10),
      resize_event(10, 200),
      resize_event(200, 10),
      click_event(300.0f, 54.0f),
      text_event("?ok"),
      key_event(SDLK_RETURN),
  }, .count = 7};
  assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  assert(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  SDL_SetEventFilter(inject_events, &sequence);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, request_quit, NULL) == 0);
  TaiPage *page = tai_session_page(session);
  char *error = NULL;
  assert(tai_present_window_with_chrome(&page, 320, 200, &callbacks, &error));
  assert(pthread_join(thread, NULL) == 0);
  SDL_SetEventFilter(NULL, NULL);
  assert(!error && sequence.injected && sequence.queued);
  assert(tai_session_history_length(session) == 2);
  assert(!strcmp(tai_url_string(tai_page_url(page)), "about:blank?ok"));
  assert(tai_page_viewport_width(page) == 320.0);
  assert(fabs(tai_page_viewport_height(page) - (200.0 - 68.34)) < 0.001);
  free(error);
  tai_session_destroy(session);
  tai_network_destroy(network);
}

int main(void) {
  test_address_edit_and_history();
  test_toolbar_boundary_and_page_coordinates();
  test_page_click_blurs_address_and_keeps_page_input();
  test_changed_page_discards_stale_address_edit();
  test_narrow_address_rows_and_forward_button();
  test_tiny_resize_is_ignored();
  return 0;
}
