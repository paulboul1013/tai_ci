#ifndef TAI_PRESENTATION_INTERNAL_H
#define TAI_PRESENTATION_INTERNAL_H

/* Private to the tai_presentation library. Declares only what crosses its
 * translation units:
 *   presentation.c          SDL window lifecycle and event loops
 *   presentation_events.c   pointer mapping and SDL -> TaiPage event adapter
 *   presentation_scene.c    page textures and SDL scene composition
 *   presentation_chrome.c   Cairo toolbar/tab-strip rendering and tab layout
 *   presentation_address.c  address editor state and chrome click/key input
 * Toolbar geometry is shared as static inline helpers so every unit hit-tests
 * and paints against the same layout. */

#include "tai/presentation.h"
#include "tai/tabset.h"
#include "presentation_geometry.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAI_PRESENTATION_SCROLL_STEP 100.0
#define TAI_ADDRESS_HEIGHT 16.0
#define TAI_ADDRESS_STANDARD_Y 49.072
#define TAI_ADDRESS_NARROW_Y 62.732

typedef struct {
  char *text;
  size_t cursor;
  bool focused;
  bool dirty;
} AddressEditor;

static inline void set_error(char **error, const char *message) {
  if (error && !*error) *error = tai_strdup(message);
}

static inline bool valid_pixel_dimensions(int width, int height) {
  return width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
         (int64_t)width * height <= 25000000;
}

static inline double chrome_bottom(int width) {
  return width >= 232 ? 68.34 : width >= 128 ? 82.0 :
         width >= 79 ? 112.0 : 142.0;
}

static inline double tabs_toolbar_offset(int width) {
  /* The second tab line wraps below 125px in the frozen Python chrome. */
  return width >= 125 ? 6.48 : 26.48;
}

static inline double tabs_chrome_bottom(int width) {
  return chrome_bottom(width) + tabs_toolbar_offset(width);
}

static inline double address_x(int width) { return width >= 232 ? 132.0 : 0.0; }
static inline double address_y(int width) {
  return width >= 232 ? TAI_ADDRESS_STANDARD_Y :
         width >= 128 ? TAI_ADDRESS_NARROW_Y :
         width >= 79 ? 92.732 : 122.732;
}
static inline double tabs_address_y(int width) {
  return address_y(width) + tabs_toolbar_offset(width);
}
static inline double address_width(int width) {
  return width >= 232 ? fmax(100.0, width - 150.0) : 100.0;
}
/* The tabbed chrome keeps the field inside narrow windows so its bookmark
 * star stays visible and clickable; Python's fixed 100px field would clip it. */
static inline double tabs_address_width(int width) {
  return fmin(address_width(width), (double)width);
}
static inline double forward_button_x(int width) { return width >= 94 ? 49.0 : 0.0; }
static inline double forward_button_y(int width) { return width >= 94 ? 36.0 : 66.0; }
static inline double tabs_back_button_y(int width) {
  return 36.0 + tabs_toolbar_offset(width);
}
static inline double tabs_forward_button_y(int width) {
  return forward_button_y(width) + tabs_toolbar_offset(width);
}

static inline double bookmarks_button_x(int width) {
  return width >= 128 ? 98.0 : width >= 79 && width < 94 ? 49.0 : 0.0;
}

static inline double bookmarks_button_y(int width) {
  if (width >= 128) return tabs_back_button_y(width);
  if (width >= 94) return tabs_back_button_y(width) + 30.0;
  if (width >= 79) return tabs_forward_button_y(width);
  return tabs_forward_button_y(width) + 30.0;
}

static inline double content_height(int width, int height, bool chrome_enabled) {
  return chrome_enabled ? fmax(1.0, (double)height - chrome_bottom(width))
                        : (double)height;
}

static inline int content_pixel_height(int width, int height, bool chrome_enabled) {
  return (int)ceil(content_height(width, height, chrome_enabled));
}

static inline double tabs_content_height(int width, int height) {
  return fmax(1.0, (double)height - tabs_chrome_bottom(width));
}

static inline int tabs_content_pixel_height(int width, int height) {
  return (int)ceil(tabs_content_height(width, height));
}

/* presentation_events.c */
void tai_pres_pointer_event_to_pixels(SDL_Window *window,
                                      SDL_WindowID window_id,
                                      SDL_Event *event);
bool tai_pres_handle_page_event(TaiPage *page, const SDL_Event *event,
                                SDL_WindowID window_id, bool window_focused,
                                double page_y_offset, bool *changed,
                                char **error);
bool tai_pres_sync_text_input(SDL_Window *window, const TaiPage *page,
                              bool window_focused, bool address_focused,
                              bool *started, char **error);

/* presentation_scene.c: texture slots are owned by the caller's event loop;
 * a successful update destroys the previous texture and stores the new one. */
bool tai_pres_present_scene(SDL_Renderer *renderer,
                            SDL_Texture *page_texture,
                            SDL_Texture *chrome_texture, const TaiPage *page,
                            int width, int height, bool chrome_enabled);
bool tai_pres_repaint_scene(SDL_Renderer *renderer,
                            SDL_Texture **page_texture,
                            SDL_Texture **chrome_texture, const TaiPage *page,
                            int width, int height, bool chrome_enabled,
                            bool update_page, bool update_chrome,
                            const AddressEditor *editor,
                            const TaiPresentWindowCallbacks *callbacks,
                            char **error);
bool tai_pres_repaint_tabs_scene(SDL_Renderer *renderer,
                                 SDL_Texture **page_texture,
                                 SDL_Texture **chrome_texture,
                                 const TaiTabSetView *view, int width,
                                 int height, bool update_page,
                                 bool update_chrome,
                                 const AddressEditor *editor, char **error);

/* presentation_chrome.c */
size_t tai_pres_editor_cursor_from_x(const char *text, double local_x);
bool tai_pres_render_chrome_texture(SDL_Renderer *renderer,
                                    SDL_Texture **texture,
                                    const TaiPage *page, int width,
                                    const AddressEditor *editor,
                                    const TaiPresentWindowCallbacks *callbacks,
                                    char **error);
bool tai_pres_render_tabs_chrome_texture(SDL_Renderer *renderer,
                                         SDL_Texture **texture,
                                         const TaiTabSetView *view,
                                         int width,
                                         const AddressEditor *editor,
                                         char **error);
bool tai_pres_tabs_tab_link_hit(const TaiTabSetView *view, int width,
                                double x, double y, size_t *index);

/* presentation_address.c */
size_t tai_pres_utf8_width(const char *text, size_t length, size_t offset,
                           uint32_t *codepoint);
size_t tai_pres_utf8_byte_at(const char *text, size_t character_index);
void tai_pres_editor_discard(AddressEditor *editor);
/* Returns an owned copy of the page URL, or NULL. */
char *tai_pres_copy_page_url(const TaiPage *page);
bool tai_pres_page_url_differs(const char *previous_url, const TaiPage *page);
bool tai_pres_handle_chrome_click(TaiPage **page_slot, SDL_Event const *event,
                                  SDL_WindowID window_id, int width,
                                  AddressEditor *editor,
                                  const TaiPresentWindowCallbacks *callbacks,
                                  bool *page_changed, bool *chrome_changed,
                                  char **error);
bool tai_pres_handle_tabs_chrome_click(TaiTabSet *tabs,
                                       SDL_Event const *event,
                                       SDL_WindowID window_id, int width,
                                       AddressEditor *editor,
                                       bool *page_changed,
                                       bool *chrome_changed, char **error);
bool tai_pres_handle_address_key(TaiPage **page_slot, const SDL_Event *event,
                                 SDL_WindowID window_id, bool window_focused,
                                 AddressEditor *editor,
                                 const TaiPresentWindowCallbacks *callbacks,
                                 bool *handled, bool *page_changed,
                                 bool *chrome_changed, char **error);
bool tai_pres_handle_tabs_address_key(TaiTabSet *tabs, const SDL_Event *event,
                                      SDL_WindowID window_id,
                                      bool window_focused,
                                      AddressEditor *editor, bool *handled,
                                      bool *chrome_changed, char **error);
bool tai_pres_handle_address_text(const SDL_Event *event,
                                  SDL_WindowID window_id,
                                  bool window_focused, AddressEditor *editor,
                                  bool *handled, bool *chrome_changed,
                                  char **error);

#endif
