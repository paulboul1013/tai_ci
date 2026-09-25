#define _POSIX_C_SOURCE 200809L
#include "tai/presentation.h"
#include "tai/tabset.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,      \
              #condition);                                                     \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

static const char *css =
    "html {display:block} body {display:block} h1 {display:block} "
    "p {display:block}";

static bool injected;
static SDL_WindowID test_window_id;

static struct timespec deadline_after(int seconds) {
  struct timespec deadline;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
  deadline.tv_sec += seconds;
  return deadline;
}

static bool before_deadline(struct timespec deadline) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return now.tv_sec < deadline.tv_sec ||
         (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec);
}

static void pause_briefly(void) {
  const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
  (void)nanosleep(&pause, NULL);
}

static bool inject_tab_switches(void *opaque, SDL_Event *event) {
  (void)opaque;
  if (event->type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || injected)
    return true;
  injected = true;
  SDL_WindowID window_id = event->window.windowID;
  test_window_id = window_id;
  SDL_Event new_tab = {.button = {
      .type = SDL_EVENT_MOUSE_BUTTON_DOWN,
      .windowID = window_id,
      .button = SDL_BUTTON_LEFT,
      .down = true,
      .x = 15.0f,
      .y = 18.0f,
  }};
  SDL_Event first_tab = {.button = {
      .type = SDL_EVENT_MOUSE_BUTTON_DOWN,
      .windowID = window_id,
      .button = SDL_BUTTON_LEFT,
      .down = true,
      .x = 50.0f,
      .y = 27.0f,
  }};
  SDL_Event resize = {.window = {
      .type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED,
      .windowID = window_id,
      .data1 = 360,
      .data2 = 220,
  }};
  /* The integration fixture releases the initial response only after it sees
   * the New Tab request, so queue resize first to establish a happens-before
   * edge: this resize is handled while the initial document is still pending. */
  CHECK(SDL_PushEvent(&resize));
  CHECK(SDL_PushEvent(&new_tab));
  CHECK(SDL_PushEvent(&first_tab));
  return false;
}

static void *request_quit(void *unused) {
  (void)unused;
  const struct timespec pause = {.tv_sec = 2, .tv_nsec = 0};
  (void)nanosleep(&pause, NULL);
  SDL_Event close = {.window = {
      .type = SDL_EVENT_WINDOW_CLOSE_REQUESTED,
      .windowID = test_window_id,
  }};
  (void)SDL_PushEvent(&close);
  return NULL;
}

static bool contains_text(const TaiNode *node, const char *needle) {
  if (!node) return false;
  if (node->kind == TAI_TEXT && node->text && strstr(node->text, needle))
    return true;
  for (size_t index = 0; index < node->child_count; index++)
    if (contains_text(node->children[index], needle)) return true;
  return false;
}

static TaiNode *find_styled(TaiNode *node) {
  if (node->kind == TAI_ELEMENT && node->tag && !strcmp(node->tag, "p") &&
      tai_map_get(&node->attributes, "id") &&
      !strcmp(tai_map_get(&node->attributes, "id"), "styled"))
    return node;
  for (size_t index = 0; index < node->child_count; index++) {
    TaiNode *match = find_styled(node->children[index]);
    if (match) return match;
  }
  return NULL;
}

static TaiTabSetView await_active_document(TaiTabSet *tabs,
                                           const char *heading) {
  struct timespec deadline = deadline_after(6);
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  while ((view.loading || !view.page) && before_deadline(deadline)) {
    bool changed = false;
    char *error = NULL;
    CHECK(tai_tabset_pump(tabs, &changed, &error));
    CHECK(error == NULL);
    free(error);
    pause_briefly();
    CHECK(tai_tabset_view(tabs, &view));
  }
  CHECK(!view.loading && view.page);
  CHECK(!heading || contains_text(tai_page_root(view.page), heading));
  return view;
}

static void set_active(TaiTabSet *tabs, size_t index) {
  CHECK(tai_tabset_select(tabs, index));
}

static void run_window_scenario(const char *initial_url, const char *home_url,
                                bool expect_css) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(css, false, home_url,
                                                     &error);
  CHECK(tabs && !error);
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  injected = false;
  SDL_SetEventFilter(inject_tab_switches, NULL);
  pthread_t quit_thread;
  CHECK(pthread_create(&quit_thread, NULL, request_quit, NULL) == 0);
  CHECK(tai_present_window_with_tabs(tabs, initial_url, 300, 180, &error));
  CHECK(pthread_join(quit_thread, NULL) == 0);
  SDL_SetEventFilter(NULL, NULL);
  CHECK(!error && injected);
  free(error);

  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  CHECK(view.tab_count == 2 && view.active_index == 0);
  view = await_active_document(tabs, expect_css ? "subresource-document"
                                                : "window-initial");
  CHECK(!strcmp(view.url, initial_url));
  if (expect_css) {
    TaiNode *styled = find_styled(tai_page_root(view.page));
    CHECK(styled && !strcmp(tai_map_get(&styled->style, "color"), "red"));
  }

  set_active(tabs, 1);
  view = await_active_document(tabs, expect_css ? "window-css-home"
                                                : "window-home");
  CHECK(!strcmp(view.url, home_url));
  CHECK(tai_page_viewport_width(view.page) == 360.0);
  CHECK(tai_page_viewport_height(view.page) > 0.0 &&
        tai_page_viewport_height(view.page) < 220.0);
  CHECK(view.history_count == 1 && view.history_index == 0);
  set_active(tabs, 0);
  CHECK(tai_tabset_view(tabs, &view));
  CHECK(tai_page_viewport_width(view.page) == 360.0);
  CHECK(tai_page_viewport_height(view.page) > 0.0 &&
        tai_page_viewport_height(view.page) < 220.0);
  tai_tabset_destroy(tabs);
}

static void run_close_pending_scenario(const char *initial_url,
                                       const char *home_url) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(css, false, home_url,
                                                     &error);
  CHECK(tabs && !error);
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  injected = false;
  SDL_SetEventFilter(inject_tab_switches, NULL);
  pthread_t quit_thread;
  CHECK(pthread_create(&quit_thread, NULL, request_quit, NULL) == 0);
  CHECK(tai_present_window_with_tabs(tabs, initial_url, 300, 180, &error));
  CHECK(pthread_join(quit_thread, NULL) == 0);
  SDL_SetEventFilter(NULL, NULL);
  CHECK(!error && injected);
  free(error);
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  CHECK(view.tab_count == 2 && view.active_index == 0 && view.loading);
  tai_tabset_destroy(tabs);
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  char base[256], delayed_url[512], home_url[512];
  char subresource_url[512], css_home_url[512];
  char close_url[512], close_home_url[512];
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  CHECK(snprintf(delayed_url, sizeof(delayed_url), "%s/window-delay", base) > 0);
  CHECK(snprintf(home_url, sizeof(home_url), "%s/window-home", base) > 0);
  CHECK(snprintf(subresource_url, sizeof(subresource_url),
                 "%s/window-subresource", base) > 0);
  CHECK(snprintf(css_home_url, sizeof(css_home_url),
                 "%s/window-css-home", base) > 0);
  CHECK(snprintf(close_url, sizeof(close_url),
                 "%s/window-close-delay", base) > 0);
  CHECK(snprintf(close_home_url, sizeof(close_home_url),
                 "%s/window-close-home", base) > 0);

  run_window_scenario(delayed_url, home_url, false);
  puts("tabbed SDL remained interactive during delayed document load");
  fflush(stdout);
  run_window_scenario(subresource_url, css_home_url, true);
  puts("tabbed SDL remained interactive during delayed CSS load");
  fflush(stdout);
  run_close_pending_scenario(close_url, close_home_url);
  puts("tabbed SDL closed while a document remained pending");
  fflush(stdout);
  puts("tabbed SDL delayed-load integration passed");
  return EXIT_SUCCESS;
}
