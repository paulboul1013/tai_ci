#include "presentation_internal.h"

#include <stdio.h>

/* One native window: its SDL resources, address editor and tab set. All of
 * it is owned and used on the SDL owner thread. */
typedef struct {
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *page_texture;
  SDL_Texture *chrome_texture;
  SDL_WindowID id;
  TaiTabSet *tabs;
  bool owns_tabs;
  AddressEditor editor;
  AddressWatch address_watch;
  /* The title last given to SDL, so SDL is called only when it changes. */
  char *shown_title;
  int pixel_width;
  int pixel_height;
  /* Page viewports end at the chrome bottom, which moves when the tab strip
   * wraps; track the state the committed viewports were sized for. */
  bool row_wraps;
  bool focused;
  bool text_input_started;
  bool close_requested;
  bool page_changed;
  bool chrome_changed;
  bool force_present;
  /* Animation frames of the active page. raf_page only identifies the page
   * the schedule belongs to and is never dereferenced; raf_wanted is its
   * request as of the last window_frame. */
  const TaiPage *raf_page;
  Uint64 next_raf_ns;
  bool raf_wanted;
} PresWindow;

/* Decision 7: the fixed 33 ms frame interval of Python's REFRESH_RATE_SEC,
 * without its adaptive multiples. */
#define PRES_FRAME_NS SDL_MS_TO_NS(33)
enum { PRES_IDLE_WAIT_MS = 16 };

/* The windows of one presentation loop. Without an app the loop presents a
 * single caller-owned tab set, as tai_present_window_with_tabs always did. */
typedef struct {
  TaiBrowserApp *app;
  PresWindow *windows[TAI_PRES_MAX_WINDOWS];
  size_t count;
  int width;
  int height;
} PresBrowser;

typedef bool (*PresFrame)(void *opaque, const TaiPresWindowInfo *windows,
                          size_t count);

static void window_destroy(PresWindow *w) {
  if (!w) return;
  if (w->text_input_started) SDL_StopTextInput(w->window);
  if (w->owns_tabs) tai_tabset_destroy(w->tabs);
  free(w->editor.text);
  free(w->address_watch.url);
  free(w->shown_title);
  SDL_DestroyTexture(w->chrome_texture);
  SDL_DestroyTexture(w->page_texture);
  SDL_DestroyRenderer(w->renderer);
  SDL_DestroyWindow(w->window);
  free(w);
}

/* Python titles each presented frame with the active tab's committed title,
 * or the browser name before that tab commits or without a usable <title>. */
static void window_sync_title(PresWindow *w, const TaiTabSetView *view) {
  char *title = view->page ? tai_page_title(view->page) : NULL;
  /* No usable <title>, or no memory to read it: show the browser name. Every
   * repaint recomputes the title, so a failed read is retried. */
  if (!title || !*title) {
    free(title);
    title = tai_strdup(TAI_BROWSER_NAME);
  }
  if (!title) {
    /* Nothing to cache either; forget the old title so the next repaint
     * sets a real one again. */
    (void)SDL_SetWindowTitle(w->window, TAI_BROWSER_NAME);
    free(w->shown_title);
    w->shown_title = NULL;
    return;
  }
  if (w->shown_title && !strcmp(w->shown_title, title)) {
    free(title);
    return;
  }
  /* A title the window system refuses is not worth stopping the browser;
   * leaving it uncached retries on the next repaint. */
  if (!SDL_SetWindowTitle(w->window, title)) {
    fprintf(stderr, "window title not set: %s\n", SDL_GetError());
    free(title);
    return;
  }
  free(w->shown_title);
  w->shown_title = title;
}

/* Creates the native window, then starts the tab set's first navigation so
 * the window is visible before the page loads. Takes ownership of tabs when
 * owns_tabs, even on failure. */
static PresWindow *window_open(TaiTabSet *tabs, bool owns_tabs,
                               const char *initial_url, int width, int height,
                               char **error) {
  PresWindow *w = calloc(1, sizeof(*w));
  if (!w) {
    if (owns_tabs) tai_tabset_destroy(tabs);
    set_error(error, "window allocation failed");
    return NULL;
  }
  w->tabs = tabs;
  w->owns_tabs = owns_tabs;
  w->focused = true;
  w->window = SDL_CreateWindow(TAI_BROWSER_NAME, width, height,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  w->renderer = w->window ? SDL_CreateRenderer(w->window, NULL) : NULL;
  w->editor.text = tai_strdup("");
  bool ok = w->renderer && w->editor.text;
  if (!w->renderer) set_error(error, SDL_GetError());
  else if (!w->editor.text) set_error(error, "address editor allocation failed");
  w->id = w->window ? SDL_GetWindowID(w->window) : 0;

  if (ok) {
    const TaiTabSetView first_tab = {.tab_count = 1};
    if (!SDL_GetWindowSizeInPixels(w->window, &w->pixel_width,
                                   &w->pixel_height)) {
      set_error(error, SDL_GetError());
      ok = false;
    } else if (!valid_pixel_dimensions(w->pixel_width, w->pixel_height)) {
      set_error(error, "unsupported tabbed window pixel dimensions");
      ok = false;
    } else {
      w->row_wraps = tai_pres_tab_row_wraps(&first_tab, w->pixel_width);
      ok = tai_tabset_start(tabs, initial_url, w->pixel_width,
                            tabs_content_height(w->pixel_width,
                                                w->pixel_height, w->row_wraps),
                            error);
    }
  }
  TaiTabSetView view = {0};
  if (ok && !tai_tabset_view(tabs, &view)) {
    set_error(error, "initial tab snapshot unavailable");
    ok = false;
  }
  bool initial_discard = false;
  if (ok && !tai_pres_address_follow_view(&w->address_watch, &view,
                                          &w->editor, &initial_discard)) {
    set_error(error, "address URL snapshot allocation failed");
    ok = false;
  }
  if (ok && !tai_pres_repaint_tabs_scene(w->renderer, &w->page_texture,
                                         &w->chrome_texture, &view,
                                         w->pixel_width, w->pixel_height,
                                         true, true, &w->editor, error))
    ok = false;
  if (ok) window_sync_title(w, &view);
  if (!ok) {
    window_destroy(w);
    return NULL;
  }
  return w;
}

static bool refresh_text_input(PresWindow *w, TaiTabSetView *view,
                               char **error) {
  return tai_tabset_view(w->tabs, view) &&
         tai_pres_sync_text_input(w->window, view->page, w->focused,
                                  w->editor.focused, &w->text_input_started,
                                  error);
}

/* Python opens a window for any Ctrl+N, whatever else is held and wherever
 * focus is; native ignores key repeat so holding the keys opens one. */
static bool is_new_window_key(const SDL_Event *event) {
  return event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_N &&
         (event->key.mod & SDL_KMOD_CTRL) && !event->key.repeat;
}

/* Applies one SDL event addressed to w. Returns false on a fatal error. */
static bool window_event(PresWindow *w, SDL_Event *event, char **error) {
  TaiTabSet *tabs = w->tabs;
  SDL_WindowID window_id = w->id;
  TaiTabSetView view = {0};
  tai_pres_pointer_event_to_pixels(w->window, window_id, event);
  if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
    w->close_requested = true;
    return true;
  }
  if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST ||
      event->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
    w->focused = event->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
    return refresh_text_input(w, &view, error);
  }
  if (!tai_tabset_view(tabs, &view)) {
    set_error(error, "active tab snapshot unavailable");
    return false;
  }
  bool handled = false;
  double chrome_bottom_y = tabs_chrome_bottom(w->pixel_width, w->row_wraps);
  bool toolbar_click = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
      event->button.windowID == window_id && isfinite(event->button.y) &&
      event->button.y < chrome_bottom_y;
  bool content_click = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
      event->button.windowID == window_id &&
      event->button.button == SDL_BUTTON_LEFT &&
      isfinite(event->button.y) && event->button.y >= chrome_bottom_y;
  if (toolbar_click) {
    if (!tai_pres_handle_tabs_chrome_click(tabs, event, window_id,
                                           w->pixel_width, &w->editor,
                                           &w->page_changed,
                                           &w->chrome_changed, error)) {
      if (!error || !*error) set_error(error, SDL_GetError());
      return false;
    }
    handled = true;
  } else if (content_click && w->editor.focused) {
    w->editor.focused = false;
    w->chrome_changed = true;
  }
  if (!handled && event->type == SDL_EVENT_TEXT_INPUT &&
      !tai_pres_handle_address_text(event, window_id, w->focused, &w->editor,
                                    &handled, &w->chrome_changed, error))
    return false;
  if (!handled && event->type == SDL_EVENT_KEY_DOWN &&
      !tai_pres_handle_tabs_address_key(tabs, event, window_id, w->focused,
                                        &w->editor, &handled,
                                        &w->chrome_changed, error))
    return false;
  bool history_key = !handled && w->focused && !w->editor.focused &&
      event->type == SDL_EVENT_KEY_DOWN && event->key.windowID == window_id &&
      (event->key.mod & SDL_KMOD_ALT) &&
      (event->key.key == SDLK_LEFT || event->key.key == SDLK_RIGHT);
  if (history_key) {
    int direction = event->key.key == SDLK_LEFT ? -1 : 1;
    if (tai_tabset_history_available(tabs, direction)) {
      char *history_error = NULL;
      if (!tai_tabset_history(tabs, direction, &history_error)) {
        fprintf(stderr, "history navigation failed: %s\n",
                history_error ? history_error : "navigation failed");
        free(history_error);
      }
      w->chrome_changed = true;
    }
    handled = true;
  }

  if (!handled && view.page) {
    bool changed = false;
    if (!tai_pres_handle_page_event(view.page, event, window_id, w->focused,
                                    chrome_bottom_y, &changed, error)) {
      if (!error || !*error) set_error(error, SDL_GetError());
      return false;
    }
    w->page_changed = w->page_changed || changed;

    char *fragment_url = NULL;
    if (!tai_page_take_fragment_change(view.page, &fragment_url)) {
      set_error(error, "could not take fragment change");
      return false;
    }
    if (fragment_url) {
      char *fragment_error = NULL;
      bool recorded = tai_tabset_record_fragment(tabs, fragment_url,
                                                  &fragment_error);
      tai_page_finish_fragment_change(view.page, recorded);
      if (!recorded) {
        fprintf(stderr, "fragment history failed: %s\n",
                fragment_error ? fragment_error : "allocation failed");
        free(fragment_error);
        w->page_changed = true;
      }
      free(fragment_url);
      w->chrome_changed = true;
    }

    TaiNavigationIntent *intent = NULL;
    if (!tai_page_take_navigation_intent(view.page, &intent)) {
      set_error(error, "could not take page navigation intent");
      return false;
    }
    if (intent) {
      char *navigation_error = NULL;
      if (!tai_tabset_navigate(tabs, intent, &navigation_error)) {
        fprintf(stderr, "link navigation failed: %s\n",
                navigation_error ? navigation_error : "navigation failed");
        free(navigation_error);
      }
      tai_navigation_intent_destroy(intent);
      w->chrome_changed = true;
    }
  }

  if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
    int resized_width = event->window.data1;
    int resized_height = event->window.data2;
    if (resized_width > 10 && resized_height > 10) {
      bool resized_wraps = tai_pres_tab_row_wraps(&view, resized_width);
      double view_height = tabs_content_height(resized_width, resized_height,
                                               resized_wraps);
      if (!valid_pixel_dimensions(resized_width, resized_height) ||
          !tai_tabset_resize(tabs, resized_width, view_height, error)) {
        if (!error || !*error)
          set_error(error, "unsupported tab window pixel dimensions");
        return false;
      }
      w->pixel_width = resized_width;
      w->pixel_height = resized_height;
      w->row_wraps = resized_wraps;
      w->page_changed = true;
      w->chrome_changed = true;
    }
  }
  if (!refresh_text_input(w, &view, error)) return false;
  if (event->type == SDL_EVENT_WINDOW_EXPOSED) w->force_present = true;
  return true;
}

/* Python runs animation frames for the active tab only; a background tab's
 * requests wait on its page and a newly shown page gets a frame at once
 * (set_active_tab). Frames are at least PRES_FRAME_NS apart. */
static bool window_animation_frame(PresWindow *w, TaiPage *page,
                                   char **error) {
  if (page != w->raf_page) {
    w->raf_page = page;
    w->next_raf_ns = 0;
  }
  Uint64 now = SDL_GetTicksNS();
  if (tai_page_needs_animation_frame(page) && now >= w->next_raf_ns) {
    bool changed = false;
    if (!tai_page_run_animation_frame(page, &changed, error)) return false;
    w->next_raf_ns = now + PRES_FRAME_NS;
    w->page_changed = w->page_changed || changed;
  }
  w->raf_wanted = tai_page_needs_animation_frame(page);
  return true;
}

/* Commits completed loads, applies the draft rule and repaints if needed. */
static bool window_frame(PresWindow *w, char **error) {
  TaiTabSet *tabs = w->tabs;
  bool completion_changed = false;
  char *pump_error = NULL;
  if (!tai_tabset_pump(tabs, &completion_changed, &pump_error)) {
    set_error(error, pump_error ? pump_error
                                : "tab navigation completion failed");
    free(pump_error);
    return false;
  }
  free(pump_error);
  if (completion_changed) {
    w->page_changed = true;
    w->chrome_changed = true;
  }
  TaiTabSetView view = {0};
  if (!tai_tabset_view(tabs, &view)) {
    set_error(error, "active tab snapshot unavailable");
    return false;
  }
  /* One place applies Python's draft discard for every URL change made by
   * this event or by completed loads. */
  if (!tai_pres_address_follow_view(&w->address_watch, &view, &w->editor,
                                    &w->chrome_changed)) {
    set_error(error, "address URL snapshot allocation failed");
    return false;
  }
  if (!window_animation_frame(w, view.page, error)) return false;
  /* New Tab (or switching which label is bold) can wrap or unwrap the tab
   * strip; keep every page viewport equal to the area below the chrome. */
  bool wraps_now = tai_pres_tab_row_wraps(&view, w->pixel_width);
  if (wraps_now != w->row_wraps) {
    if (!tai_tabset_resize(tabs, w->pixel_width,
                           tabs_content_height(w->pixel_width,
                                               w->pixel_height, wraps_now),
                           error))
      return false;
    w->row_wraps = wraps_now;
    w->page_changed = true;
    w->chrome_changed = true;
  }
  if (w->page_changed || w->chrome_changed || w->force_present) {
    if (!tai_tabset_view(tabs, &view) ||
        !tai_pres_repaint_tabs_scene(w->renderer, &w->page_texture,
                                     &w->chrome_texture, &view,
                                     w->pixel_width, w->pixel_height,
                                     w->page_changed, w->chrome_changed,
                                     &w->editor, error))
      return false;
    window_sync_title(w, &view);
  }
  w->page_changed = w->chrome_changed = w->force_present = false;
  return true;
}

static PresWindow *window_for_event(PresBrowser *b, const SDL_Event *event) {
  SDL_Window *window = SDL_GetWindowFromEvent(event);
  if (!window) return NULL;
  for (size_t index = 0; index < b->count; index++)
    if (b->windows[index]->window == window) return b->windows[index];
  return NULL;
}

static void close_window(PresBrowser *b, size_t index) {
  window_destroy(b->windows[index]);
  memmove(&b->windows[index], &b->windows[index + 1],
          (b->count - index - 1) * sizeof(*b->windows));
  b->count--;
}

static void close_all(PresBrowser *b) {
  while (b->count) close_window(b, b->count - 1);
}

static bool notify_observer(PresBrowser *b, PresFrame frame, void *opaque) {
  TaiPresWindowInfo infos[TAI_PRES_MAX_WINDOWS];
  for (size_t index = 0; index < b->count; index++) {
    PresWindow *w = b->windows[index];
    infos[index] = (TaiPresWindowInfo){
        .tabs = w->tabs,
        .window_id = w->id,
        .width = w->pixel_width,
        .height = w->pixel_height,
        .wraps = w->row_wraps,
        .editor = &w->editor,
    };
  }
  return frame(opaque, infos, b->count);
}

/* Ctrl+N: a new 800x600 window whose single tab opens the app's New Tab URL.
 * Reaching the window limit or failing to create the window only warns; the
 * open windows keep running. */
static void open_new_window(PresBrowser *b) {
  if (b->count >= TAI_PRES_MAX_WINDOWS) {
    fprintf(stderr, "new window ignored: maximum of %d windows reached\n",
            TAI_PRES_MAX_WINDOWS);
    return;
  }
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_in_app(b->app, &error);
  PresWindow *w = tabs ? window_open(tabs, true, tai_browser_app_home_url(
                                         b->app), b->width, b->height, &error)
                       : NULL;
  if (w) b->windows[b->count++] = w;
  else
    fprintf(stderr, "new window failed: %s\n",
            error ? error : "window creation failed");
  free(error);
}

/* Waits for input at most until the next due animation frame, rounded up so
 * the loop does not wake early and spin. */
static Sint32 wait_timeout(const PresBrowser *b) {
  Uint64 now = SDL_GetTicksNS();
  Sint32 timeout = PRES_IDLE_WAIT_MS;
  for (size_t index = 0; index < b->count; index++) {
    const PresWindow *w = b->windows[index];
    if (!w->raf_wanted) continue;
    Uint64 remaining = w->next_raf_ns > now ? w->next_raf_ns - now : 0;
    Uint64 remaining_ms = (remaining + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS;
    if (remaining_ms < (Uint64)timeout) timeout = (Sint32)remaining_ms;
  }
  return timeout;
}

/* Runs until every window is closed, SDL_EVENT_QUIT arrives or the observer
 * stops it. Windows are destroyed before returning. */
static bool run_windows(PresBrowser *b, PresFrame frame, void *opaque,
                        char **error) {
  bool ok = true;
  while (ok && b->count) {
    bool new_window = false;
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, wait_timeout(b))) {
      if (event.type == SDL_EVENT_QUIT) break;
      PresWindow *target = window_for_event(b, &event);
      new_window = target && b->app && is_new_window_key(&event);
      if (target && !new_window && !window_event(target, &event, error)) {
        ok = false;
        break;
      }
    }
    for (size_t index = b->count; index-- > 0;)
      if (b->windows[index]->close_requested) close_window(b, index);
    if (new_window) open_new_window(b);
    for (size_t index = 0; ok && index < b->count; index++)
      ok = window_frame(b->windows[index], error);
    if (ok && b->count && frame && !notify_observer(b, frame, opaque)) break;
  }
  close_all(b);
  return ok;
}

static bool present_tabs(TaiBrowserApp *app, TaiTabSet *tabs,
                         const char *initial_url, int width, int height,
                         PresFrame frame, void *opaque, char **error) {
  if (error) { free(*error); *error = NULL; }
  if ((!app == !tabs) || !initial_url || width <= 0 || height <= 0) {
    set_error(error, "invalid tabbed window input");
    return false;
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    set_error(error, SDL_GetError());
    return false;
  }
  PresBrowser browser = {.app = app, .width = width, .height = height};
  bool owns_tabs = tabs == NULL;
  if (owns_tabs) tabs = tai_tabset_create_in_app(app, error);
  PresWindow *first = tabs ? window_open(tabs, owns_tabs, initial_url, width,
                                         height, error)
                           : NULL;
  bool ok = first != NULL;
  if (ok) {
    browser.windows[browser.count++] = first;
    ok = run_windows(&browser, frame, opaque, error);
  }
  SDL_Quit();
  return ok;
}

bool tai_present_window_with_tabs(TaiTabSet *tabs, const char *initial_url,
                                  int width, int height, char **error) {
  if (!tabs) {
    if (error) { free(*error); *error = NULL; }
    set_error(error, "invalid tabbed window input");
    return false;
  }
  return present_tabs(NULL, tabs, initial_url, width, height, NULL, NULL,
                      error);
}

bool tai_present_browser(TaiBrowserApp *app, const char *initial_url,
                         int width, int height, char **error) {
  if (!app) {
    if (error) { free(*error); *error = NULL; }
    set_error(error, "invalid browser window input");
    return false;
  }
  return present_tabs(app, NULL, initial_url, width, height, NULL, NULL,
                      error);
}

bool tai_pres_present_browser_observed(TaiBrowserApp *app,
                                       const char *initial_url, int width,
                                       int height,
                                       const TaiPresBrowserObserver *observer,
                                       char **error) {
  if (!app) {
    if (error) { free(*error); *error = NULL; }
    set_error(error, "invalid browser window input");
    return false;
  }
  return present_tabs(app, NULL, initial_url, width, height,
                      observer ? observer->frame : NULL,
                      observer ? observer->opaque : NULL, error);
}

static bool single_window_frame(void *opaque, const TaiPresWindowInfo *windows,
                                size_t count) {
  const TaiPresTabsObserver *observer = opaque;
  return count == 1 &&
         observer->frame(observer->opaque, windows[0].tabs,
                         windows[0].window_id, windows[0].width,
                         windows[0].wraps, windows[0].editor);
}

bool tai_pres_present_tabs_observed(TaiTabSet *tabs, const char *initial_url,
                                    int width, int height,
                                    const TaiPresTabsObserver *observer,
                                    char **error) {
  if (!tabs) {
    if (error) { free(*error); *error = NULL; }
    set_error(error, "invalid tabbed window input");
    return false;
  }
  return present_tabs(NULL, tabs, initial_url, width, height,
                      observer ? single_window_frame : NULL,
                      (void *)observer, error);
}
