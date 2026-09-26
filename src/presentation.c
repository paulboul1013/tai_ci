#include "presentation_internal.h"

static bool present_window_internal(
    TaiPage **page_slot, int width, int height,
    const TaiPresentWindowCallbacks *callbacks, bool chrome_enabled,
    char **error) {
  if (error) { free(*error); *error = NULL; }
  if (!page_slot || !*page_slot || width <= 0 || height <= 0 ||
      (chrome_enabled && (!callbacks || !callbacks->address))) {
    set_error(error, "invalid window input");
    return false;
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    set_error(error, SDL_GetError());
    return false;
  }
  SDL_Window *window = SDL_CreateWindow("Tai Gar", width, height,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
  SDL_Texture *page_texture = NULL;
  SDL_Texture *chrome_texture = NULL;
  AddressEditor editor = {0};
  if (chrome_enabled) editor.text = tai_strdup("");
  bool ok = renderer != NULL;
  if (chrome_enabled && !editor.text) {
    set_error(error, "address editor allocation failed");
    ok = false;
  }
  if (!ok && (!error || !*error)) set_error(error, SDL_GetError());
  int pixel_width = 0, pixel_height = 0;
  if (ok) {
    if (!SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height)) {
      set_error(error, SDL_GetError());
      ok = false;
    } else if (!valid_pixel_dimensions(pixel_width, pixel_height)) {
      set_error(error, "unsupported window pixel dimensions");
      ok = false;
    } else {
      double view_height = content_height(pixel_width, pixel_height,
                                          chrome_enabled);
      if (view_height <= 0.0) {
        set_error(error, "window is too short for browser chrome");
        ok = false;
      } else {
        ok = tai_page_resize(*page_slot, pixel_width, view_height, error) &&
             tai_pres_repaint_scene(renderer, &page_texture, &chrome_texture,
                                    *page_slot, pixel_width, pixel_height,
                                    chrome_enabled, true, chrome_enabled,
                                    &editor, callbacks, error);
      }
    }
  }
  bool running = ok;
  bool window_focused = true;
  bool text_input_started = false;
  SDL_WindowID window_id = window ? SDL_GetWindowID(window) : 0;
  while (running) {
    SDL_Event event;
    if (!SDL_WaitEvent(&event)) {
      set_error(error, SDL_GetError());
      ok = false;
      break;
    }
    tai_pres_pointer_event_to_pixels(window, window_id, &event);
    if (event.type == SDL_EVENT_QUIT ||
        (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
         event.window.windowID == SDL_GetWindowID(window))) {
      running = false;
    } else if ((event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) &&
               event.window.windowID == SDL_GetWindowID(window)) {
      window_focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
      if (!tai_pres_sync_text_input(window, *page_slot, window_focused,
                                    chrome_enabled && editor.focused,
                                    &text_input_started, error)) {
        ok = false;
        break;
      }
    } else {
      bool page_changed = false, chrome_changed = false;
      bool handled = false;
      bool toolbar_click = chrome_enabled &&
          event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
          event.button.windowID == window_id &&
          isfinite(event.button.y) &&
          event.button.y < chrome_bottom(pixel_width);
      bool content_click = chrome_enabled &&
          event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
          event.button.windowID == window_id &&
          event.button.button == SDL_BUTTON_LEFT &&
          isfinite(event.button.y) &&
          event.button.y >= chrome_bottom(pixel_width);
      if (toolbar_click) {
        if (!tai_pres_handle_chrome_click(page_slot, &event, window_id,
                                          pixel_width, &editor, callbacks,
                                          &page_changed, &chrome_changed,
                                          error)) {
          if (!error || !*error) set_error(error, SDL_GetError());
          ok = false;
          break;
        }
        handled = true;
      } else if (content_click && editor.focused) {
        /* Python blurs the address field on a page click but preserves dirty
         * text for display until another chrome control is clicked. */
        editor.focused = false;
        chrome_changed = true;
      }
      if (!handled && chrome_enabled && event.type == SDL_EVENT_TEXT_INPUT) {
        if (!tai_pres_handle_address_text(&event, window_id, window_focused,
                                          &editor, &handled, &chrome_changed,
                                          error)) {
          ok = false;
          break;
        }
      }
      if (!handled && chrome_enabled && event.type == SDL_EVENT_KEY_DOWN) {
        if (!tai_pres_handle_address_key(page_slot, &event, window_id,
                                         window_focused, &editor, callbacks,
                                         &handled, &page_changed,
                                         &chrome_changed, error)) {
          ok = false;
          break;
        }
      }
      bool history_key = !handled && callbacks && callbacks->history &&
          window_focused && !(chrome_enabled && editor.focused) &&
          event.type == SDL_EVENT_KEY_DOWN &&
          event.key.windowID == window_id &&
          (event.key.mod & SDL_KMOD_ALT) &&
          (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT);
      if (history_key) {
        char *previous_url =
            chrome_enabled ? tai_pres_copy_page_url(*page_slot) : NULL;
        if (chrome_enabled && !previous_url) {
          set_error(error, "history URL snapshot allocation failed");
          ok = false;
          break;
        }
        int direction = event.key.key == SDLK_LEFT ? -1 : 1;
        bool moved = callbacks->history(callbacks->userdata, page_slot,
                                        direction, error);
        if (!moved || !*page_slot) {
          free(previous_url);
          if (!error || !*error) set_error(error, "history handler failed");
          ok = false;
          break;
        }
        if (chrome_enabled &&
            tai_pres_page_url_differs(previous_url, *page_slot))
          tai_pres_editor_discard(&editor);
        free(previous_url);
        page_changed = true;
        chrome_changed = chrome_enabled;
      } else if (!handled && !tai_pres_handle_page_event(
                     *page_slot, &event, window_id, window_focused,
                     chrome_enabled ? chrome_bottom(pixel_width) : 0.0,
                     &page_changed, error)) {
        if (!error || !*error) set_error(error, SDL_GetError());
        ok = false;
        break;
      }
      char *fragment_url = NULL;
      if (!tai_page_take_fragment_change(*page_slot, &fragment_url)) {
        set_error(error, "could not take fragment change");
        ok = false;
        break;
      }
      if (fragment_url) {
        bool fragment_url_changed =
            tai_page_fragment_url_changed(*page_slot);
        bool recorded = !callbacks || !callbacks->fragment ||
            callbacks->fragment(callbacks->userdata, fragment_url, error);
        tai_page_finish_fragment_change(*page_slot, recorded);
        free(fragment_url);
        chrome_changed = chrome_changed || chrome_enabled;
        if (recorded && chrome_enabled && fragment_url_changed)
          tai_pres_editor_discard(&editor);
        if (!recorded) {
          if (error) { free(*error); *error = NULL; }
          page_changed = true;
        }
      }
      TaiNavigationIntent *intent = NULL;
      if (!tai_page_take_navigation_intent(*page_slot, &intent)) {
        set_error(error, "could not take page navigation intent");
        ok = false;
        break;
      }
      if (intent) {
        char *previous_url =
            chrome_enabled ? tai_pres_copy_page_url(*page_slot) : NULL;
        if (chrome_enabled && !previous_url) {
          tai_navigation_intent_destroy(intent);
          set_error(error, "navigation URL snapshot allocation failed");
          ok = false;
          break;
        }
        bool navigated = !callbacks || !callbacks->navigate ||
            callbacks->navigate(callbacks->userdata, page_slot, intent,
                                error);
        tai_navigation_intent_destroy(intent);
        if (!navigated || !*page_slot) {
          free(previous_url);
          if (!error || !*error)
            set_error(error, "navigation handler failed");
          ok = false;
          break;
        }
        if (chrome_enabled &&
            tai_pres_page_url_differs(previous_url, *page_slot))
          tai_pres_editor_discard(&editor);
        free(previous_url);
        /* The callback may have destroyed the previous page. An intent is a
         * repaint boundary even when loading fails and the old page remains. */
        page_changed = true;
        chrome_changed = chrome_enabled;
      }
      if (!tai_pres_sync_text_input(window, *page_slot, window_focused,
                                    chrome_enabled && editor.focused,
                                    &text_input_started, error)) {
        ok = false;
        break;
      }
      if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
          event.window.windowID == window_id) {
        int resized_width = event.window.data1;
        int resized_height = event.window.data2;
        /* The Python window ignores minimized and tiny (<11px) dimensions.
         * Keep the last valid frame until a usable resize arrives. */
        if (resized_width > 10 && resized_height > 10) {
          double view_height = content_height(resized_width, resized_height,
                                              chrome_enabled);
          if (!valid_pixel_dimensions(resized_width, resized_height) ||
              !tai_page_resize(*page_slot, resized_width, view_height, error)) {
            if (!error || !*error)
              set_error(error, "unsupported window pixel dimensions");
            ok = false;
            break;
          }
          pixel_width = resized_width;
          pixel_height = resized_height;
          page_changed = true;
          chrome_changed = chrome_enabled;
        }
      }
      if (page_changed || chrome_changed) {
        if (!tai_pres_repaint_scene(renderer, &page_texture, &chrome_texture,
                                    *page_slot, pixel_width, pixel_height,
                                    chrome_enabled, page_changed,
                                    chrome_changed, &editor, callbacks,
                                    error)) {
          ok = false;
          break;
        }
      } else if (event.type == SDL_EVENT_WINDOW_EXPOSED &&
                 event.window.windowID == window_id &&
                 !tai_pres_present_scene(renderer, page_texture, chrome_texture,
                                         *page_slot, pixel_width, pixel_height,
                                         chrome_enabled)) {
        set_error(error, SDL_GetError());
        ok = false;
        break;
      }
    }
  }
  if (text_input_started) SDL_StopTextInput(window);
  free(editor.text);
  SDL_DestroyTexture(chrome_texture);
  SDL_DestroyTexture(page_texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return ok;
}

static bool present_tabset_window(TaiTabSet *tabs, const char *initial_url,
                                  int width, int height, char **error) {
  if (error) { free(*error); *error = NULL; }
  if (!tabs || !initial_url || width <= 0 || height <= 0) {
    set_error(error, "invalid tabbed window input");
    return false;
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    set_error(error, SDL_GetError());
    return false;
  }
  SDL_Window *window = SDL_CreateWindow("Tai Gar", width, height,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
  SDL_Texture *page_texture = NULL;
  SDL_Texture *chrome_texture = NULL;
  AddressEditor editor = {.text = tai_strdup("")};
  bool ok = renderer && editor.text;
  if (!renderer) set_error(error, SDL_GetError());
  else if (!editor.text) set_error(error, "address editor allocation failed");

  int pixel_width = 0, pixel_height = 0;
  /* Page viewports end at the chrome bottom, which moves when the tab strip
   * wraps; track the state the committed viewports were sized for. */
  bool row_wraps = false;
  if (ok) {
    const TaiTabSetView first_tab = {.tab_count = 1};
    if (!SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height)) {
      set_error(error, SDL_GetError());
      ok = false;
    } else if (!valid_pixel_dimensions(pixel_width, pixel_height)) {
      set_error(error, "unsupported tabbed window pixel dimensions");
      ok = false;
    } else {
      row_wraps = tai_pres_tab_row_wraps(&first_tab, pixel_width);
      ok = tai_tabset_start(tabs, initial_url, pixel_width,
                            tabs_content_height(pixel_width, pixel_height,
                                                row_wraps),
                            error);
    }
  }
  TaiTabSetView view = {0};
  if (ok && !tai_tabset_view(tabs, &view)) {
    set_error(error, "initial tab snapshot unavailable");
    ok = false;
  }
  if (ok && !tai_pres_repaint_tabs_scene(renderer, &page_texture,
                                         &chrome_texture, &view, pixel_width,
                                         pixel_height, true, true, &editor,
                                         error)) ok = false;

  bool running = ok;
  bool window_focused = true;
  bool text_input_started = false;
  SDL_WindowID window_id = window ? SDL_GetWindowID(window) : 0;
  while (running) {
    SDL_Event event;
    bool has_event = SDL_WaitEventTimeout(&event, 16);
    bool page_changed = false;
    bool chrome_changed = false;
    bool force_present = false;
    if (has_event) {
      tai_pres_pointer_event_to_pixels(window, window_id, &event);
      if (event.type == SDL_EVENT_QUIT ||
          (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
           event.window.windowID == window_id)) {
        running = false;
      } else if ((event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                  event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) &&
                 event.window.windowID == window_id) {
        window_focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        if (!tai_tabset_view(tabs, &view) ||
            !tai_pres_sync_text_input(window, view.page, window_focused,
                                      editor.focused, &text_input_started,
                                      error)) {
          ok = false;
          break;
        }
      } else {
        if (!tai_tabset_view(tabs, &view)) {
          set_error(error, "active tab snapshot unavailable");
          ok = false;
          break;
        }
        bool handled = false;
        bool toolbar_click = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.windowID == window_id &&
            isfinite(event.button.y) &&
            event.button.y < tabs_chrome_bottom(pixel_width, row_wraps);
        bool content_click = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.windowID == window_id &&
            event.button.button == SDL_BUTTON_LEFT &&
            isfinite(event.button.y) &&
            event.button.y >= tabs_chrome_bottom(pixel_width, row_wraps);
        if (toolbar_click) {
          if (!tai_pres_handle_tabs_chrome_click(tabs, &event, window_id,
                                                 pixel_width, &editor,
                                                 &page_changed,
                                                 &chrome_changed, error)) {
            if (!error || !*error) set_error(error, SDL_GetError());
            ok = false;
            break;
          }
          handled = true;
        } else if (content_click && editor.focused) {
          editor.focused = false;
          chrome_changed = true;
        }
        if (!handled && event.type == SDL_EVENT_TEXT_INPUT) {
          if (!tai_pres_handle_address_text(&event, window_id, window_focused,
                                            &editor, &handled, &chrome_changed,
                                            error)) {
            ok = false;
            break;
          }
        }
        if (!handled && event.type == SDL_EVENT_KEY_DOWN) {
          if (!tai_pres_handle_tabs_address_key(tabs, &event, window_id,
                                                window_focused, &editor,
                                                &handled, &chrome_changed,
                                                error)) {
            ok = false;
            break;
          }
        }
        bool history_key = !handled && window_focused && !editor.focused &&
            event.type == SDL_EVENT_KEY_DOWN &&
            event.key.windowID == window_id &&
            (event.key.mod & SDL_KMOD_ALT) &&
            (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT);
        if (history_key) {
          int direction = event.key.key == SDLK_LEFT ? -1 : 1;
          if (tai_tabset_history_available(tabs, direction)) {
            char *history_error = NULL;
            if (!tai_tabset_history(tabs, direction, &history_error)) {
              fprintf(stderr, "history navigation failed: %s\n",
                      history_error ? history_error : "navigation failed");
              free(history_error);
            }
            tai_pres_editor_discard(&editor);
            chrome_changed = true;
          }
          handled = true;
        }

        if (!handled && view.page) {
          bool changed = false;
          if (!tai_pres_handle_page_event(view.page, &event, window_id,
                  window_focused, tabs_chrome_bottom(pixel_width, row_wraps),
                  &changed,
                  error)) {
            if (!error || !*error) set_error(error, SDL_GetError());
            ok = false;
            break;
          }
          page_changed = page_changed || changed;

          char *fragment_url = NULL;
          if (!tai_page_take_fragment_change(view.page, &fragment_url)) {
            set_error(error, "could not take fragment change");
            ok = false;
            break;
          }
          if (fragment_url) {
            bool fragment_url_changed =
                tai_page_fragment_url_changed(view.page);
            char *fragment_error = NULL;
            bool recorded = tai_tabset_record_fragment(tabs, fragment_url,
                                                        &fragment_error);
            tai_page_finish_fragment_change(view.page, recorded);
            if (!recorded) {
              fprintf(stderr, "fragment history failed: %s\n",
                      fragment_error ? fragment_error : "allocation failed");
              free(fragment_error);
              page_changed = true;
            }
            free(fragment_url);
            chrome_changed = true;
            if (recorded && fragment_url_changed)
              tai_pres_editor_discard(&editor);
          }

          TaiNavigationIntent *intent = NULL;
          if (!tai_page_take_navigation_intent(view.page, &intent)) {
            set_error(error, "could not take page navigation intent");
            ok = false;
            break;
          }
          if (intent) {
            char *navigation_error = NULL;
            if (!tai_tabset_navigate(tabs, intent, &navigation_error)) {
              fprintf(stderr, "link navigation failed: %s\n",
                      navigation_error ? navigation_error :
                                         "navigation failed");
              free(navigation_error);
            }
            tai_navigation_intent_destroy(intent);
            tai_pres_editor_discard(&editor);
            chrome_changed = true;
          }
        }

        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
            event.window.windowID == window_id) {
          int resized_width = event.window.data1;
          int resized_height = event.window.data2;
          if (resized_width > 10 && resized_height > 10) {
            bool resized_wraps = tai_pres_tab_row_wraps(&view, resized_width);
            double view_height = tabs_content_height(resized_width,
                                                      resized_height,
                                                      resized_wraps);
            if (!valid_pixel_dimensions(resized_width, resized_height) ||
                !tai_tabset_resize(tabs, resized_width, view_height, error)) {
              if (!error || !*error)
                set_error(error, "unsupported tab window pixel dimensions");
              ok = false;
              break;
            }
            pixel_width = resized_width;
            pixel_height = resized_height;
            row_wraps = resized_wraps;
            page_changed = true;
            chrome_changed = true;
          }
        }
        if (!tai_tabset_view(tabs, &view) ||
            !tai_pres_sync_text_input(window, view.page, window_focused,
                                      editor.focused, &text_input_started,
                                      error)) {
          ok = false;
          break;
        }
        force_present = event.type == SDL_EVENT_WINDOW_EXPOSED &&
                        event.window.windowID == window_id;
      }
    }

    bool completion_changed = false;
    char *pump_error = NULL;
    if (!tai_tabset_pump(tabs, &completion_changed, &pump_error)) {
      if (pump_error) {
        set_error(error, pump_error);
        free(pump_error);
      } else {
        set_error(error, "tab navigation completion failed");
      }
      ok = false;
      break;
    }
    free(pump_error);
    if (completion_changed) {
      page_changed = true;
      chrome_changed = true;
    }
    if (!running) break;
    if (!tai_tabset_view(tabs, &view)) {
      set_error(error, "active tab snapshot unavailable");
      ok = false;
      break;
    }
    /* New Tab (or switching which label is bold) can wrap or unwrap the tab
     * strip; keep every page viewport equal to the area below the chrome. */
    bool wraps_now = tai_pres_tab_row_wraps(&view, pixel_width);
    if (wraps_now != row_wraps) {
      if (!tai_tabset_resize(tabs, pixel_width,
                             tabs_content_height(pixel_width, pixel_height,
                                                 wraps_now),
                             error)) {
        ok = false;
        break;
      }
      row_wraps = wraps_now;
      page_changed = true;
      chrome_changed = true;
    }
    if (page_changed || chrome_changed || force_present) {
      if (!tai_tabset_view(tabs, &view) ||
          !tai_pres_repaint_tabs_scene(renderer, &page_texture,
                                       &chrome_texture, &view, pixel_width,
                                       pixel_height, page_changed,
                                       chrome_changed, &editor, error)) {
        ok = false;
        break;
      }
    }
  }
  if (text_input_started) SDL_StopTextInput(window);
  free(editor.text);
  SDL_DestroyTexture(chrome_texture);
  SDL_DestroyTexture(page_texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return ok;
}

bool tai_present_window_with_history(TaiPage **page_slot, int width,
                                     int height, TaiPresentNavigate navigate,
                                     TaiPresentHistory history,
                                     TaiPresentFragment fragment,
                                     void *userdata, char **error) {
  TaiPresentWindowCallbacks callbacks = {
      .navigate = navigate,
      .history = history,
      .fragment = fragment,
      .userdata = userdata,
  };
  return present_window_internal(page_slot, width, height, &callbacks, false,
                                error);
}

bool tai_present_window_with_chrome(
    TaiPage **page, int width, int height,
    const TaiPresentWindowCallbacks *callbacks, char **error) {
  return present_window_internal(page, width, height, callbacks, true, error);
}

bool tai_present_window_with_tabs(TaiTabSet *tabs, const char *initial_url,
                                  int width, int height, char **error) {
  return present_tabset_window(tabs, initial_url, width, height, error);
}

bool tai_present_window_with_navigation(TaiPage **page, int width, int height,
                                        TaiPresentNavigate navigate,
                                        void *userdata, char **error) {
  return tai_present_window_with_history(page, width, height, navigate,
                                         NULL, NULL, userdata, error);
}

bool tai_present_window(TaiPage *page, int width, int height, char **error) {
  TaiPage *borrowed_page = page;
  return tai_present_window_with_navigation(&borrowed_page, width, height,
                                            NULL, NULL, error);
}
