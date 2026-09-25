#include "tai/presentation.h"
#include "tai/tabset.h"
#include "presentation_geometry.h"
#include <SDL3/SDL.h>
#include <cairo.h>
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

static void set_error(char **error, const char *message) {
  if (error && !*error) *error = tai_strdup(message);
}

static bool valid_pixel_dimensions(int width, int height) {
  return width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
         (int64_t)width * height <= 25000000;
}

static double chrome_bottom(int width) {
  return width >= 232 ? 68.34 : width >= 128 ? 82.0 :
         width >= 79 ? 112.0 : 142.0;
}

static double tabs_chrome_bottom(int width) {
  return chrome_bottom(width) + (width >= 128 ? 6.48 : 26.48);
}

static double address_x(int width) { return width >= 232 ? 132.0 : 0.0; }
static double address_y(int width) {
  return width >= 232 ? TAI_ADDRESS_STANDARD_Y :
         width >= 128 ? TAI_ADDRESS_NARROW_Y :
         width >= 79 ? 92.732 : 122.732;
}
static double tabs_address_y(int width) {
  return address_y(width) + (width >= 128 ? 0.0 : 26.48);
}
static double address_width(int width) {
  return width >= 232 ? fmax(100.0, width - 150.0) : 100.0;
}
static double forward_button_x(int width) { return width >= 94 ? 49.0 : 0.0; }
static double forward_button_y(int width) { return width >= 94 ? 36.0 : 66.0; }

static double content_height(int width, int height, bool chrome_enabled) {
  return chrome_enabled ? fmax(1.0, (double)height - chrome_bottom(width))
                        : (double)height;
}

static int content_pixel_height(int width, int height, bool chrome_enabled) {
  return (int)ceil(content_height(width, height, chrome_enabled));
}

static double tabs_content_height(int width, int height) {
  return fmax(1.0, (double)height - tabs_chrome_bottom(width));
}

static int tabs_content_pixel_height(int width, int height) {
  return (int)ceil(tabs_content_height(width, height));
}

static bool present_tabs_scene(SDL_Renderer *renderer,
                               SDL_Texture *page_texture,
                               SDL_Texture *chrome_texture,
                               const TaiTabSetView *view, int width,
                               int height) {
  if (!SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255) ||
      !SDL_RenderClear(renderer)) return false;
  if (view->page && page_texture) {
    SDL_FRect page_rect = {0.0f, (float)tabs_chrome_bottom(width),
                           (float)width,
                           (float)tabs_content_pixel_height(width, height)};
    if (!SDL_RenderTexture(renderer, page_texture, NULL, &page_rect))
      return false;
    TaiScrollbarRect bar;
    if (tai_scrollbar_geometry(width, tabs_content_height(width, height),
                               tai_page_scroll_y(view->page),
                               tai_page_max_scroll_y(view->page), &bar)) {
      SDL_FRect rect = {bar.x, (float)(bar.y + tabs_chrome_bottom(width)),
                        bar.w, bar.h};
      if (!SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255) ||
          !SDL_RenderFillRect(renderer, &rect)) return false;
    }
  }
  SDL_FRect chrome_rect = {0.0f, 0.0f, (float)width,
                           (float)ceil(tabs_chrome_bottom(width))};
  if (!SDL_RenderTexture(renderer, chrome_texture, NULL, &chrome_rect))
    return false;
  return SDL_RenderPresent(renderer);
}

static bool present_scene(SDL_Renderer *renderer, SDL_Texture *page_texture,
                          SDL_Texture *chrome_texture, const TaiPage *page,
                          int width, int height, bool chrome_enabled) {
  if (!SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255) ||
      !SDL_RenderClear(renderer)) return false;
  double view_height = content_height(width, height, chrome_enabled);
  if (chrome_enabled) {
    SDL_FRect page_rect = {0.0f, (float)chrome_bottom(width), (float)width,
                           (float)content_pixel_height(width, height, true)};
    if (!SDL_RenderTexture(renderer, page_texture, NULL, &page_rect))
      return false;
  } else if (!SDL_RenderTexture(renderer, page_texture, NULL, NULL)) {
    return false;
  }
  TaiScrollbarRect bar;
  if (tai_scrollbar_geometry(width, view_height, tai_page_scroll_y(page),
                             tai_page_max_scroll_y(page), &bar)) {
    SDL_FRect rect = {bar.x,
                      (float)(bar.y + (chrome_enabled ? chrome_bottom(width) : 0)),
                      bar.w, bar.h};
    if (!SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255) ||
        !SDL_RenderFillRect(renderer, &rect)) return false;
  }
  if (chrome_enabled) {
    SDL_FRect chrome_rect = {0.0f, 0.0f, (float)width,
                             (float)ceil(chrome_bottom(width))};
    if (!SDL_RenderTexture(renderer, chrome_texture, NULL, &chrome_rect))
      return false;
  }
  return SDL_RenderPresent(renderer);
}

static bool scroll_page(TaiPage *page, double delta) {
  double old_scroll = tai_page_scroll_y(page);
  double proposed = old_scroll + delta;
  if (!isfinite(proposed) || !tai_page_set_scroll_y(page, proposed)) return false;
  return tai_page_scroll_y(page) != old_scroll;
}

static size_t utf8_width(const char *text, size_t length, size_t offset,
                         uint32_t *codepoint) {
  unsigned char first = (unsigned char)text[offset];
  if (first < 0x80) {
    if (codepoint) *codepoint = first;
    return 1;
  }
  size_t width = first >= 0xc2 && first <= 0xdf ? 2 :
                 first >= 0xe0 && first <= 0xef ? 3 :
                 first >= 0xf0 && first <= 0xf4 ? 4 : 1;
  if (width == 1 || offset + width > length) {
    if (codepoint) *codepoint = first;
    return 1;
  }
  uint32_t value = first & (width == 2 ? 0x1f : width == 3 ? 0x0f : 0x07);
  for (size_t index = 1; index < width; index++) {
    unsigned char next = (unsigned char)text[offset + index];
    if ((next & 0xc0) != 0x80) {
      if (codepoint) *codepoint = first;
      return 1;
    }
    value = (value << 6) | (next & 0x3f);
  }
  if ((width == 3 && value < 0x800) ||
      (width == 4 && value < 0x10000) || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff)) {
    if (codepoint) *codepoint = first;
    return 1;
  }
  if (codepoint) *codepoint = value;
  return width;
}

static size_t utf8_count(const char *text) {
  size_t length = strlen(text), count = 0;
  for (size_t offset = 0; offset < length; count++)
    offset += utf8_width(text, length, offset, NULL);
  return count;
}

static size_t utf8_byte_at(const char *text, size_t character_index) {
  size_t length = strlen(text), offset = 0;
  while (character_index && offset < length) {
    offset += utf8_width(text, length, offset, NULL);
    character_index--;
  }
  return offset;
}

static void editor_discard(AddressEditor *editor) {
  if (editor->text) editor->text[0] = '\0';
  editor->cursor = 0;
  editor->focused = false;
  editor->dirty = false;
}

static bool editor_set_text(AddressEditor *editor, const char *text) {
  char *copy = tai_strdup(text ? text : "");
  if (!copy) return false;
  free(editor->text);
  editor->text = copy;
  editor->cursor = utf8_count(copy);
  return true;
}

static bool editor_insert(AddressEditor *editor, const char *input) {
  size_t input_length = strlen(input);
  char *filtered = malloc(input_length + 1);
  if (!filtered) return false;
  size_t output_length = 0, added_characters = 0;
  for (size_t offset = 0; offset < input_length;) {
    uint32_t codepoint = 0;
    size_t width = utf8_width(input, input_length, offset, &codepoint);
    if (codepoint >= 0x20) {
      memcpy(filtered + output_length, input + offset, width);
      output_length += width;
      added_characters++;
    }
    offset += width;
  }
  filtered[output_length] = '\0';
  if (!output_length) {
    free(filtered);
    return true;
  }
  size_t old_length = strlen(editor->text);
  size_t cursor_byte = utf8_byte_at(editor->text, editor->cursor);
  if (old_length > SIZE_MAX - output_length - 1) {
    free(filtered);
    return false;
  }
  char *next = malloc(old_length + output_length + 1);
  if (!next) {
    free(filtered);
    return false;
  }
  memcpy(next, editor->text, cursor_byte);
  memcpy(next + cursor_byte, filtered, output_length);
  memcpy(next + cursor_byte + output_length, editor->text + cursor_byte,
         old_length - cursor_byte + 1);
  free(filtered);
  free(editor->text);
  editor->text = next;
  editor->cursor += added_characters;
  editor->dirty = true;
  return true;
}

static bool editor_backspace(AddressEditor *editor) {
  if (!editor->cursor) return true;
  size_t end = utf8_byte_at(editor->text, editor->cursor);
  size_t start = utf8_byte_at(editor->text, editor->cursor - 1);
  size_t length = strlen(editor->text);
  memmove(editor->text + start, editor->text + end, length - end + 1);
  editor->cursor--;
  editor->dirty = true;
  return true;
}

static void set_source(cairo_t *context, double red, double green,
                       double blue) {
  cairo_set_source_rgb(context, red, green, blue);
}

static void draw_button(cairo_t *context, double x, double y, double width,
                        double height, bool enabled, bool forward) {
  set_source(context, 0.90, 0.90, 0.90);
  cairo_rectangle(context, x, y, width, height);
  cairo_fill(context);
  set_source(context, 1.0, 1.0, 1.0);
  cairo_move_to(context, x + 0.5, y + height - 0.5);
  cairo_line_to(context, x + 0.5, y + 0.5);
  cairo_line_to(context, x + width - 0.5, y + 0.5);
  cairo_stroke(context);
  set_source(context, 0.45, 0.45, 0.45);
  cairo_move_to(context, x + width - 0.5, y + 0.5);
  cairo_line_to(context, x + width - 0.5, y + height - 0.5);
  cairo_line_to(context, x + 0.5, y + height - 0.5);
  cairo_stroke(context);
  set_source(context, enabled ? 0.12 : 0.52, enabled ? 0.12 : 0.52,
             enabled ? 0.12 : 0.52);
  double center_x = x + width / 2.0;
  double center_y = y + height / 2.0;
  double direction = forward ? -1.0 : 1.0;
  cairo_set_line_width(context, 1.8);
  cairo_move_to(context, center_x + direction * 5.0, center_y);
  cairo_line_to(context, center_x - direction * 3.0, center_y);
  cairo_move_to(context, center_x - direction * 3.0, center_y);
  cairo_line_to(context, center_x + direction * 1.0, center_y - 4.0);
  cairo_move_to(context, center_x - direction * 3.0, center_y);
  cairo_line_to(context, center_x + direction * 1.0, center_y + 4.0);
  cairo_stroke(context);
}

static void select_address_font(cairo_t *context) {
  cairo_select_font_face(context, "sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(context, 12.0);
}

static size_t editor_cursor_from_x(const char *text, double local_x) {
  if (local_x <= 0.0) return 0;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                         1, 1);
  cairo_t *context = cairo_create(surface);
  select_address_font(context);
  size_t length = strlen(text), offset = 0, index = 0;
  double previous = 0.0;
  while (offset < length) {
    offset += utf8_width(text, length, offset, NULL);
    cairo_text_extents_t extents;
    char *prefix = malloc(offset + 1);
    if (!prefix) break;
    memcpy(prefix, text, offset);
    prefix[offset] = '\0';
    cairo_text_extents(context, prefix, &extents);
    free(prefix);
    double current = extents.x_advance;
    if (local_x < (previous + current) / 2.0) break;
    previous = current;
    index++;
  }
  cairo_destroy(context);
  cairo_surface_destroy(surface);
  return index;
}

static bool render_chrome_texture(SDL_Renderer *renderer,
                                  SDL_Texture **texture, const TaiPage *page,
                                  int width, const AddressEditor *editor,
                                  const TaiPresentWindowCallbacks *callbacks,
                                  char **error) {
  double bottom = chrome_bottom(width);
  double field_x = address_x(width);
  double field_y = address_y(width);
  double field_width = address_width(width);
  double forward_x = forward_button_x(width);
  double forward_y = forward_button_y(width);
  int height = (int)ceil(bottom);
  if (!valid_pixel_dimensions(width, height)) {
    set_error(error, "unsupported chrome dimensions");
    return false;
  }
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                         width, height);
  cairo_t *context = cairo_create(surface);
  set_source(context, 0.827, 0.827, 0.827);
  cairo_paint(context);
  draw_button(context, 0.0, 36.0, 45.0, 24.0,
              !callbacks->history_available ||
                  callbacks->history_available(callbacks->userdata, -1),
              false);
  draw_button(context, forward_x, forward_y, 45.0, 24.0,
              !callbacks->history_available ||
                  callbacks->history_available(callbacks->userdata, 1),
              true);
  cairo_rectangle(context, field_x, field_y, field_width,
                  TAI_ADDRESS_HEIGHT);
  cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
  cairo_fill_preserve(context);
  set_source(context, 0.35, 0.35, 0.35);
  cairo_set_line_width(context, 1.0);
  cairo_stroke(context);
  const char *text = editor->dirty || editor->focused
                         ? editor->text
                         : tai_url_string(tai_page_url(page));
  cairo_save(context);
  cairo_rectangle(context, field_x + 4.0, field_y + 1.0,
                  fmax(0.0, field_width - 8.0), TAI_ADDRESS_HEIGHT - 2.0);
  cairo_clip(context);
  select_address_font(context);
  set_source(context, 0.08, 0.08, 0.08);
  cairo_move_to(context, field_x + 5.0, field_y + 12.5);
  cairo_show_text(context, text ? text : "");
  if (editor->focused) {
    size_t cursor_byte = utf8_byte_at(editor->text, editor->cursor);
    char *prefix = malloc(cursor_byte + 1);
    if (!prefix) {
      cairo_restore(context);
      cairo_destroy(context);
      cairo_surface_destroy(surface);
      set_error(error, "address caret allocation failed");
      return false;
    }
    memcpy(prefix, editor->text, cursor_byte);
    prefix[cursor_byte] = '\0';
    cairo_text_extents_t extents;
    cairo_text_extents(context, prefix, &extents);
    free(prefix);
    double caret_x = field_x + 5.0 + extents.x_advance;
    set_source(context, 0.05, 0.05, 0.05);
    cairo_set_line_width(context, 1.0);
    cairo_move_to(context, caret_x, field_y + 2.0);
    cairo_line_to(context, caret_x, field_y + TAI_ADDRESS_HEIGHT - 2.0);
    cairo_stroke(context);
  }
  cairo_restore(context);
  set_source(context, 0.45, 0.45, 0.45);
  cairo_set_line_width(context, 1.0);
  cairo_move_to(context, 0.0, bottom - 0.5);
  cairo_line_to(context, width, bottom - 0.5);
  cairo_stroke(context);
  cairo_status_t context_status = cairo_status(context);
  cairo_destroy(context);
  cairo_surface_flush(surface);
  cairo_status_t cairo_ok = context_status == CAIRO_STATUS_SUCCESS
      ? cairo_surface_status(surface) : context_status;
  SDL_Texture *next = cairo_ok == CAIRO_STATUS_SUCCESS
      ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                          SDL_TEXTUREACCESS_STREAMING, width, height)
      : NULL;
  bool ok = next && SDL_UpdateTexture(next, NULL,
      cairo_image_surface_get_data(surface),
      cairo_image_surface_get_stride(surface));
  cairo_surface_destroy(surface);
  if (!ok) {
    set_error(error, cairo_ok == CAIRO_STATUS_SUCCESS ? SDL_GetError()
                                                      : cairo_status_to_string(cairo_ok));
    SDL_DestroyTexture(next);
    return false;
  }
  SDL_DestroyTexture(*texture);
  *texture = next;
  return true;
}

static void tab_label(size_t index, size_t active, char *text,
                      size_t capacity) {
  if (index == active)
    (void)snprintf(text, capacity, "[Tab %zu]", index);
  else
    (void)snprintf(text, capacity, "Tab %zu", index);
}

static bool render_tabs_chrome_texture(SDL_Renderer *renderer,
                                      SDL_Texture **texture,
                                      const TaiTabSetView *view, int width,
                                      const AddressEditor *editor,
                                      char **error) {
  double bottom = tabs_chrome_bottom(width);
  double field_x = address_x(width);
  double field_y = tabs_address_y(width);
  double field_width = address_width(width);
  double forward_x = forward_button_x(width);
  double forward_y = forward_button_y(width);
  int height = (int)ceil(bottom);
  if (!valid_pixel_dimensions(width, height)) {
    set_error(error, "unsupported tab chrome dimensions");
    return false;
  }
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                         width, height);
  cairo_t *context = cairo_create(surface);
  set_source(context, 0.827, 0.827, 0.827);
  cairo_paint(context);

  /* The oracle's first chrome row places a 30x24 New Tab button at (0, 6),
   * followed by links beginning at x=34. */
  set_source(context, 0.90, 0.90, 0.90);
  cairo_rectangle(context, 0.0, 6.0, 30.0, 24.0);
  cairo_fill_preserve(context);
  set_source(context, 0.45, 0.45, 0.45);
  cairo_set_line_width(context, 1.0);
  cairo_stroke(context);
  set_source(context, 0.12, 0.12, 0.12);
  cairo_set_line_width(context, 1.8);
  cairo_move_to(context, 15.0, 12.0);
  cairo_line_to(context, 15.0, 24.0);
  cairo_move_to(context, 9.0, 18.0);
  cairo_line_to(context, 21.0, 18.0);
  cairo_stroke(context);

  for (size_t index = 0; index < view->tab_count; index++) {
    char label[64];
    tab_label(index, view->active_index, label, sizeof(label));
    cairo_select_font_face(context, "serif", CAIRO_FONT_SLANT_NORMAL,
        index == view->active_index ? CAIRO_FONT_WEIGHT_BOLD
                                    : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 16.0);
    cairo_text_extents_t extents;
    cairo_text_extents(context, label, &extents);
    if (width < 128 && index > 0) {
      const char *space = strchr(label, ' ');
      if (space) {
        char first[64], second[64];
        size_t first_length = (size_t)(space - label);
        memcpy(first, label, first_length);
        first[first_length] = '\0';
        (void)snprintf(second, sizeof(second), "%s", space + 1);
        if (index == view->active_index) set_source(context, 0.0, 0.0, 0.0);
        else set_source(context, 0.0, 0.0, 0.75);
        cairo_move_to(context, 75.0, 32.0);
        cairo_show_text(context, first);
        cairo_move_to(context, 0.0, 52.0);
        cairo_show_text(context, second);
        continue;
      }
    }
    double tab_x = index == 0 ? 34.0 : 75.0 + (index - 1) * 54.0;
    double tab_baseline = 32.0;
    if (tab_x + extents.x_advance > width) {
      tab_x = 0.0;
      tab_baseline = 52.0;
    }
    if (index == view->active_index) set_source(context, 0.0, 0.0, 0.0);
    else set_source(context, 0.0, 0.0, 0.75);
    cairo_move_to(context, tab_x, tab_baseline);
    cairo_show_text(context, label);
  }

  draw_button(context, 0.0, 36.0, 45.0, 24.0,
              view->can_go_back, false);
  draw_button(context, forward_x, forward_y, 45.0, 24.0,
              view->can_go_forward, true);
  cairo_rectangle(context, field_x, field_y, field_width,
                  TAI_ADDRESS_HEIGHT);
  cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
  cairo_fill_preserve(context);
  set_source(context, 0.35, 0.35, 0.35);
  cairo_set_line_width(context, 1.0);
  cairo_stroke(context);
  const char *text = editor->dirty || editor->focused
      ? editor->text : view->url;
  cairo_save(context);
  cairo_rectangle(context, field_x + 4.0, field_y + 1.0,
                  fmax(0.0, field_width - 8.0), TAI_ADDRESS_HEIGHT - 2.0);
  cairo_clip(context);
  select_address_font(context);
  set_source(context, 0.08, 0.08, 0.08);
  cairo_move_to(context, field_x + 5.0, field_y + 12.5);
  cairo_show_text(context, text ? text : "");
  if (editor->focused) {
    size_t cursor_byte = utf8_byte_at(editor->text, editor->cursor);
    char *prefix = malloc(cursor_byte + 1);
    if (!prefix) {
      cairo_restore(context);
      cairo_destroy(context);
      cairo_surface_destroy(surface);
      set_error(error, "address caret allocation failed");
      return false;
    }
    memcpy(prefix, editor->text, cursor_byte);
    prefix[cursor_byte] = '\0';
    cairo_text_extents_t extents;
    cairo_text_extents(context, prefix, &extents);
    free(prefix);
    set_source(context, 0.05, 0.05, 0.05);
    cairo_set_line_width(context, 1.0);
    double caret_x = field_x + 5.0 + extents.x_advance;
    cairo_move_to(context, caret_x, field_y + 2.0);
    cairo_line_to(context, caret_x, field_y + TAI_ADDRESS_HEIGHT - 2.0);
    cairo_stroke(context);
  }
  cairo_restore(context);
  set_source(context, 0.45, 0.45, 0.45);
  cairo_set_line_width(context, 1.0);
  cairo_move_to(context, 0.0, bottom - 0.5);
  cairo_line_to(context, width, bottom - 0.5);
  cairo_stroke(context);

  cairo_status_t context_status = cairo_status(context);
  cairo_destroy(context);
  cairo_surface_flush(surface);
  cairo_status_t cairo_ok = context_status == CAIRO_STATUS_SUCCESS
      ? cairo_surface_status(surface) : context_status;
  SDL_Texture *next = cairo_ok == CAIRO_STATUS_SUCCESS
      ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                          SDL_TEXTUREACCESS_STREAMING, width, height)
      : NULL;
  bool ok = next && SDL_UpdateTexture(next, NULL,
      cairo_image_surface_get_data(surface),
      cairo_image_surface_get_stride(surface));
  cairo_surface_destroy(surface);
  if (!ok) {
    set_error(error, cairo_ok == CAIRO_STATUS_SUCCESS ? SDL_GetError()
                                                      : cairo_status_to_string(cairo_ok));
    SDL_DestroyTexture(next);
    return false;
  }
  SDL_DestroyTexture(*texture);
  *texture = next;
  return true;
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

static bool sync_text_input(SDL_Window *window, const TaiPage *page,
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

static bool update_page_texture(SDL_Renderer *renderer,
                                SDL_Texture **texture, const TaiPage *page,
                                int width, int window_height,
                                bool chrome_enabled, char **error) {
  if (!page) {
    SDL_DestroyTexture(*texture);
    *texture = NULL;
    return true;
  }
  int height = content_pixel_height(width, window_height, chrome_enabled);
  if (!valid_pixel_dimensions(width, window_height) || height <= 0 ||
      !valid_pixel_dimensions(width, height)) {
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
  bool ok = next && SDL_UpdateTexture(next, NULL, pixels, stride);
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

static bool update_tabs_page_texture(SDL_Renderer *renderer,
                                     SDL_Texture **texture,
                                     const TaiPage *page, int width,
                                     int window_height, char **error) {
  if (!page) {
    SDL_DestroyTexture(*texture);
    *texture = NULL;
    return true;
  }
  int height = tabs_content_pixel_height(width, window_height);
  if (!valid_pixel_dimensions(width, window_height) || height <= 0 ||
      !valid_pixel_dimensions(width, height)) {
    set_error(error, "unsupported tab window pixel dimensions");
    return false;
  }
  unsigned char *pixels = NULL;
  int stride = 0;
  const TaiDisplayList *list = tai_page_display_list(page);
  if (!list || !tai_display_list_raster_region(list, width, height, 0,
                                      tai_page_scroll_y(page),
                                      &pixels, &stride, error)) return false;
  SDL_Texture *next = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, width,
                                       height);
  bool ok = next && SDL_UpdateTexture(next, NULL, pixels, stride);
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

static bool repaint_scene(SDL_Renderer *renderer, SDL_Texture **page_texture,
                          SDL_Texture **chrome_texture, const TaiPage *page,
                          int width, int height, bool chrome_enabled,
                          bool update_page, bool update_chrome,
                          const AddressEditor *editor,
                          const TaiPresentWindowCallbacks *callbacks,
                          char **error) {
  if (update_page && !update_page_texture(renderer, page_texture, page,
                                           width, height, chrome_enabled,
                                           error)) return false;
  if (chrome_enabled && update_chrome &&
      !render_chrome_texture(renderer, chrome_texture, page, width, editor,
                             callbacks, error)) return false;
  if (!present_scene(renderer, *page_texture, *chrome_texture, page, width,
                     height, chrome_enabled)) {
    set_error(error, SDL_GetError());
    return false;
  }
  return true;
}

static bool repaint_tabs_scene(SDL_Renderer *renderer,
                              SDL_Texture **page_texture,
                              SDL_Texture **chrome_texture,
                              const TaiTabSetView *view, int width,
                              int height, bool update_page,
                              bool update_chrome,
                              const AddressEditor *editor, char **error) {
  if (update_page && !update_tabs_page_texture(renderer, page_texture,
          view->page, width, height, error)) return false;
  if (update_chrome && !render_tabs_chrome_texture(renderer, chrome_texture,
          view, width, editor, error)) return false;
  if (!present_tabs_scene(renderer, *page_texture, *chrome_texture, view,
                          width, height)) {
    set_error(error, SDL_GetError());
    return false;
  }
  return true;
}

static bool handle_chrome_click(TaiPage **page_slot, SDL_Event const *event,
                                SDL_WindowID window_id, int width,
                                AddressEditor *editor,
                                const TaiPresentWindowCallbacks *callbacks,
                                bool *page_changed, bool *chrome_changed,
                                char **error) {
  *page_changed = false;
  *chrome_changed = false;
  if (event->button.windowID != window_id ||
      event->button.button != SDL_BUTTON_LEFT ||
      !isfinite(event->button.x) || !isfinite(event->button.y) ||
      event->button.y >= chrome_bottom(width)) return true;

  bool was_focused = editor->focused;
  bool was_dirty = editor->dirty;
  editor->focused = false;
  bool blurred = false;
  if (!tai_page_blur_input(*page_slot, &blurred, error)) {
    editor->focused = was_focused;
    return false;
  }
  *page_changed = blurred;

  double x = event->button.x, y = event->button.y;
  double field_x = address_x(width), field_y = address_y(width);
  double field_width = address_width(width);
  bool address_hit = x >= field_x && x < field_x + field_width &&
      y >= field_y && y < field_y + TAI_ADDRESS_HEIGHT;
  if (address_hit) {
    if (!was_focused && !editor->dirty &&
        !editor_set_text(editor, tai_url_string(tai_page_url(*page_slot)))) {
      set_error(error, "address field allocation failed");
      return false;
    }
    editor->focused = true;
    editor->cursor = editor_cursor_from_x(
        editor->text, x - field_x - 5.0);
    *chrome_changed = true;
    return true;
  }

  bool back_hit = x >= 0.0 && x < 45.0 && y >= 36.0 && y < 60.0;
  double forward_x = forward_button_x(width);
  double forward_y = forward_button_y(width);
  bool forward_hit = x >= forward_x && x < forward_x + 45.0 &&
      y >= forward_y && y < forward_y + 24.0;
  editor_discard(editor);
  *chrome_changed = was_focused || was_dirty || back_hit || forward_hit;
  if (back_hit || forward_hit) {
    int direction = back_hit ? -1 : 1;
    bool available = !callbacks->history_available ||
        callbacks->history_available(callbacks->userdata, direction);
    if (available && callbacks->history) {
      if (!callbacks->history(callbacks->userdata, page_slot, direction,
                              error) || !*page_slot) {
        if (!error || !*error) set_error(error, "history handler failed");
        return false;
      }
      /* A history callback can destroy the old page even when the serialized
       * URL is unchanged. Treat every accepted traversal as a page boundary. */
      *page_changed = true;
    }
    *chrome_changed = true;
  }
  return true;
}

static bool tabs_tab_link_hit(const TaiTabSetView *view, int width,
                              double x, double y, size_t *index) {
  for (size_t tab = 0; tab < view->tab_count; tab++) {
    double left = tab == 0 ? 34.0 : 75.0 + (tab - 1) * 54.0;
    double right = left + (tab == 0 ? 37.0 : 50.0);
    double top = 19.0, bottom = 35.0;
    if (width < 128 && tab > 0) {
      left = 0.0;
      right = 108.0;
      top = 19.0;
      bottom = 55.0;
    } else if (right > width) {
      left = 0.0;
      right = fmin((double)width, 108.0);
      top = 39.0;
      bottom = 55.0;
    }
    if (x >= left && x < right && y >= top && y < bottom) {
      *index = tab;
      return true;
    }
  }
  return false;
}

static bool handle_tabs_chrome_click(TaiTabSet *tabs, SDL_Event const *event,
                                     SDL_WindowID window_id, int width,
                                     AddressEditor *editor,
                                     bool *page_changed,
                                     bool *chrome_changed, char **error) {
  *page_changed = false;
  *chrome_changed = false;
  if (event->button.windowID != window_id ||
      event->button.button != SDL_BUTTON_LEFT ||
      !isfinite(event->button.x) || !isfinite(event->button.y) ||
      event->button.y >= tabs_chrome_bottom(width)) return true;
  TaiTabSetView view;
  if (!tai_tabset_view(tabs, &view)) {
    set_error(error, "active tab snapshot unavailable");
    return false;
  }
  bool was_focused = editor->focused;
  bool was_dirty = editor->dirty;
  editor->focused = false;
  bool blurred = false;
  if (view.page && !tai_page_blur_input(view.page, &blurred, error)) {
    editor->focused = was_focused;
    return false;
  }
  *page_changed = blurred;

  double x = event->button.x, y = event->button.y;
  if (x >= 0.0 && x < 30.0 && y >= 6.0 && y < 30.0) {
    if (!tai_tabset_new_tab(tabs, error)) return false;
    editor_discard(editor);
    *page_changed = true;
    *chrome_changed = true;
    return true;
  }
  size_t selected = 0;
  if (tabs_tab_link_hit(&view, width, x, y, &selected)) {
    if (!tai_tabset_select(tabs, selected)) return true;
    editor_discard(editor);
    *page_changed = true;
    *chrome_changed = true;
    return true;
  }

  double field_x = address_x(width), field_y = tabs_address_y(width);
  double field_width = address_width(width);
  bool address_hit = x >= field_x && x < field_x + field_width &&
      y >= field_y && y < field_y + TAI_ADDRESS_HEIGHT;
  if (address_hit) {
    if (!was_focused && !editor->dirty &&
        !editor_set_text(editor, view.url ? view.url : "")) {
      set_error(error, "address field allocation failed");
      return false;
    }
    editor->focused = true;
    editor->cursor = editor_cursor_from_x(editor->text,
                                           x - field_x - 5.0);
    *chrome_changed = true;
    return true;
  }

  bool back_hit = x >= 0.0 && x < 45.0 && y >= 36.0 && y < 60.0;
  double forward_x = forward_button_x(width);
  double forward_y = forward_button_y(width);
  bool forward_hit = x >= forward_x && x < forward_x + 45.0 &&
      y >= forward_y && y < forward_y + 24.0;
  editor_discard(editor);
  *chrome_changed = was_focused || was_dirty || back_hit || forward_hit;
  int direction = back_hit ? -1 : forward_hit ? 1 : 0;
  if (direction && tai_tabset_history_available(tabs, direction)) {
    if (!tai_tabset_history(tabs, direction, error)) return false;
    *chrome_changed = true;
  }
  return true;
}

static char *copy_page_url(const TaiPage *page) {
  const char *url = page ? tai_url_string(tai_page_url(page)) : NULL;
  return url ? tai_strdup(url) : NULL;
}

static bool page_url_differs(const char *previous_url, const TaiPage *page) {
  const char *current_url = page ? tai_url_string(tai_page_url(page)) : NULL;
  return previous_url && current_url && strcmp(previous_url, current_url) != 0;
}

static bool handle_address_edit_key(const SDL_Event *event,
                                    SDL_WindowID window_id,
                                    bool window_focused,
                                    AddressEditor *editor, bool *handled,
                                    bool *chrome_changed, char **error) {
  *handled = false;
  *chrome_changed = false;
  if (!editor->focused || !window_focused ||
      event->type != SDL_EVENT_KEY_DOWN || event->key.windowID != window_id)
    return true;
  *handled = true;
  switch (event->key.key) {
    case SDLK_BACKSPACE:
      if (!editor_backspace(editor)) {
        set_error(error, "address editing allocation failed");
        return false;
      }
      *chrome_changed = true;
      return true;
    case SDLK_LEFT:
      if (editor->cursor) editor->cursor--;
      *chrome_changed = true;
      return true;
    case SDLK_RIGHT:
      if (editor->cursor < utf8_count(editor->text)) editor->cursor++;
      *chrome_changed = true;
      return true;
    default:
      /* Chrome owns key focus. Unhandled keys are not sent to the page. */
      return true;
  }
}

static bool handle_address_key(TaiPage **page_slot, const SDL_Event *event,
                               SDL_WindowID window_id, bool window_focused,
                               AddressEditor *editor,
                               const TaiPresentWindowCallbacks *callbacks,
                               bool *handled, bool *page_changed,
                               bool *chrome_changed, char **error) {
  *page_changed = false;
  if (!handle_address_edit_key(event, window_id, window_focused, editor,
                               handled, chrome_changed, error)) return false;
  if (!*handled || event->key.key != SDLK_RETURN) return true;
  char *submission = tai_strdup(editor->text);
  if (!submission) {
    set_error(error, "address submission allocation failed");
    return false;
  }
  editor_discard(editor);
  *chrome_changed = true;
  bool submitted = callbacks->address(callbacks->userdata, page_slot,
                                      submission, error);
  free(submission);
  if (!submitted || !*page_slot) {
    if (!error || !*error) set_error(error, "address handler failed");
    return false;
  }
  /* Submission may replace a same-URL page; avoid comparing a pointer to a
   * page the callback may already have destroyed. */
  *page_changed = true;
  return true;
}

static bool handle_tabs_address_key(TaiTabSet *tabs, const SDL_Event *event,
                                   SDL_WindowID window_id,
                                   bool window_focused,
                                   AddressEditor *editor, bool *handled,
                                   bool *chrome_changed, char **error) {
  if (!handle_address_edit_key(event, window_id, window_focused, editor,
                               handled, chrome_changed, error)) return false;
  if (!*handled || event->key.key != SDLK_RETURN) return true;
  char *submission = tai_strdup(editor->text);
  if (!submission) {
    set_error(error, "address submission allocation failed");
    return false;
  }
  editor_discard(editor);
  *chrome_changed = true;
  char *navigation_error = NULL;
  if (!tai_tabset_navigate_address(tabs, submission, &navigation_error)) {
    fprintf(stderr, "address navigation failed: %s\n",
            navigation_error ? navigation_error : "navigation failed");
    free(navigation_error);
  }
  free(submission);
  return true;
}

static bool handle_address_text(const SDL_Event *event, SDL_WindowID window_id,
                                bool window_focused, AddressEditor *editor,
                                bool *handled, bool *chrome_changed,
                                char **error) {
  *handled = false;
  *chrome_changed = false;
  if (!editor->focused || !window_focused ||
      event->type != SDL_EVENT_TEXT_INPUT || event->text.windowID != window_id)
    return true;
  *handled = true;
  if (!editor_insert(editor, event->text.text)) {
    set_error(error, "address editing allocation failed");
    return false;
  }
  *chrome_changed = true;
  return true;
}

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
                                         SDL_WINDOW_RESIZABLE);
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
             repaint_scene(renderer, &page_texture, &chrome_texture,
                           *page_slot, pixel_width, pixel_height,
                           chrome_enabled, true, chrome_enabled, &editor,
                           callbacks, error);
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
    if (event.type == SDL_EVENT_QUIT ||
        (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
         event.window.windowID == SDL_GetWindowID(window))) {
      running = false;
    } else if ((event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) &&
               event.window.windowID == SDL_GetWindowID(window)) {
      window_focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
      if (!sync_text_input(window, *page_slot, window_focused,
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
        if (!handle_chrome_click(page_slot, &event, window_id, pixel_width,
                                 &editor, callbacks, &page_changed,
                                 &chrome_changed, error)) {
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
        if (!handle_address_text(&event, window_id, window_focused, &editor,
                                 &handled, &chrome_changed, error)) {
          ok = false;
          break;
        }
      }
      if (!handled && chrome_enabled && event.type == SDL_EVENT_KEY_DOWN) {
        if (!handle_address_key(page_slot, &event, window_id, window_focused,
                                &editor, callbacks, &handled, &page_changed,
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
        char *previous_url = chrome_enabled ? copy_page_url(*page_slot) : NULL;
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
        if (chrome_enabled && page_url_differs(previous_url, *page_slot))
          editor_discard(&editor);
        free(previous_url);
        page_changed = true;
        chrome_changed = chrome_enabled;
      } else if (!handled && !handle_page_event(
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
          editor_discard(&editor);
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
        char *previous_url = chrome_enabled ? copy_page_url(*page_slot) : NULL;
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
        if (chrome_enabled && page_url_differs(previous_url, *page_slot))
          editor_discard(&editor);
        free(previous_url);
        /* The callback may have destroyed the previous page. An intent is a
         * repaint boundary even when loading fails and the old page remains. */
        page_changed = true;
        chrome_changed = chrome_enabled;
      }
      if (!sync_text_input(window, *page_slot, window_focused,
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
        if (!repaint_scene(renderer, &page_texture, &chrome_texture,
                           *page_slot, pixel_width, pixel_height,
                           chrome_enabled, page_changed,
                           chrome_changed, &editor, callbacks, error)) {
          ok = false;
          break;
        }
      } else if (event.type == SDL_EVENT_WINDOW_EXPOSED &&
                 event.window.windowID == window_id &&
                 !present_scene(renderer, page_texture, chrome_texture,
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
                                         SDL_WINDOW_RESIZABLE);
  SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
  SDL_Texture *page_texture = NULL;
  SDL_Texture *chrome_texture = NULL;
  AddressEditor editor = {.text = tai_strdup("")};
  bool ok = renderer && editor.text;
  if (!renderer) set_error(error, SDL_GetError());
  else if (!editor.text) set_error(error, "address editor allocation failed");

  int pixel_width = 0, pixel_height = 0;
  if (ok) {
    if (!SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height)) {
      set_error(error, SDL_GetError());
      ok = false;
    } else if (!valid_pixel_dimensions(pixel_width, pixel_height) ||
               tabs_content_height(pixel_width, pixel_height) <= 0.0) {
      set_error(error, "unsupported tabbed window pixel dimensions");
      ok = false;
    } else {
      ok = tai_tabset_start(tabs, initial_url, pixel_width,
                            tabs_content_height(pixel_width, pixel_height),
                            error);
    }
  }
  TaiTabSetView view = {0};
  if (ok && !tai_tabset_view(tabs, &view)) {
    set_error(error, "initial tab snapshot unavailable");
    ok = false;
  }
  if (ok && !repaint_tabs_scene(renderer, &page_texture, &chrome_texture,
                                &view, pixel_width, pixel_height, true, true,
                                &editor, error)) ok = false;

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
      if (event.type == SDL_EVENT_QUIT ||
          (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
           event.window.windowID == window_id)) {
        running = false;
      } else if ((event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                  event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) &&
                 event.window.windowID == window_id) {
        window_focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        if (!tai_tabset_view(tabs, &view) ||
            !sync_text_input(window, view.page, window_focused,
                             editor.focused, &text_input_started, error)) {
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
            event.button.y < tabs_chrome_bottom(pixel_width);
        bool content_click = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.windowID == window_id &&
            event.button.button == SDL_BUTTON_LEFT &&
            isfinite(event.button.y) &&
            event.button.y >= tabs_chrome_bottom(pixel_width);
        if (toolbar_click) {
          if (!handle_tabs_chrome_click(tabs, &event, window_id, pixel_width,
                                        &editor, &page_changed,
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
          if (!handle_address_text(&event, window_id, window_focused, &editor,
                                   &handled, &chrome_changed, error)) {
            ok = false;
            break;
          }
        }
        if (!handled && event.type == SDL_EVENT_KEY_DOWN) {
          if (!handle_tabs_address_key(tabs, &event, window_id,
                                       window_focused, &editor, &handled,
                                       &chrome_changed, error)) {
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
            editor_discard(&editor);
            chrome_changed = true;
          }
          handled = true;
        }

        if (!handled && view.page) {
          bool changed = false;
          if (!handle_page_event(view.page, &event, window_id,
                  window_focused, tabs_chrome_bottom(pixel_width), &changed,
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
              editor_discard(&editor);
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
            editor_discard(&editor);
            chrome_changed = true;
          }
        }

        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
            event.window.windowID == window_id) {
          int resized_width = event.window.data1;
          int resized_height = event.window.data2;
          if (resized_width > 10 && resized_height > 10) {
            double view_height = tabs_content_height(resized_width,
                                                      resized_height);
            if (!valid_pixel_dimensions(resized_width, resized_height) ||
                !tai_tabset_resize(tabs, resized_width, view_height, error)) {
              if (!error || !*error)
                set_error(error, "unsupported tab window pixel dimensions");
              ok = false;
              break;
            }
            pixel_width = resized_width;
            pixel_height = resized_height;
            page_changed = true;
            chrome_changed = true;
          }
        }
        if (!tai_tabset_view(tabs, &view) ||
            !sync_text_input(window, view.page, window_focused,
                             editor.focused, &text_input_started, error)) {
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
    if (page_changed || chrome_changed || force_present) {
      if (!tai_tabset_view(tabs, &view) ||
          !repaint_tabs_scene(renderer, &page_texture, &chrome_texture,
                              &view, pixel_width, pixel_height, page_changed,
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
