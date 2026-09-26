#include "presentation_internal.h"
#include <cairo.h>
#include <stdio.h>

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

static void draw_star(cairo_t *context, double center_x, double center_y,
                      double outer_radius, bool bright) {
  const double pi = 3.14159265358979323846;
  for (int point = 0; point < 10; point++) {
    double angle = -pi / 2.0 + (double)point * pi / 5.0;
    double radius = point % 2 ? outer_radius * 0.46 : outer_radius;
    double x = center_x + cos(angle) * radius;
    double y = center_y + sin(angle) * radius;
    if (point == 0) cairo_move_to(context, x, y);
    else cairo_line_to(context, x, y);
  }
  cairo_close_path(context);
  set_source(context, bright ? 1.0 : 0.52, bright ? 0.75 : 0.52,
             bright ? 0.05 : 0.52);
  cairo_fill_preserve(context);
  set_source(context, bright ? 0.57 : 0.40, bright ? 0.40 : 0.40,
             bright ? 0.02 : 0.40);
  cairo_set_line_width(context, 1.0);
  cairo_stroke(context);
}

/* Python build_lock_path: an outline body with a pointed shackle, stroked in
 * black at 1.8px inside a 14x14 box centered in the lock slot. */
static void draw_lock(cairo_t *context, double center_x, double center_y) {
  double size = TAI_SECURITY_ICON_SIZE;
  double left = center_x - size / 2.0, top = center_y - size / 2.0;
  double right = left + size, bottom = top + size;
  double body_left = left + size * 0.18, body_right = right - size * 0.18;
  double body_top = top + size * 0.46, body_bottom = bottom - size * 0.10;
  double shackle_left = left + size * 0.30;
  double shackle_right = right - size * 0.30;
  cairo_new_path(context);
  cairo_move_to(context, body_left, body_top);
  cairo_line_to(context, body_right, body_top);
  cairo_line_to(context, body_right, body_bottom);
  cairo_line_to(context, body_left, body_bottom);
  cairo_close_path(context);
  cairo_move_to(context, shackle_left, body_top);
  cairo_line_to(context, shackle_left, top + size * 0.30);
  cairo_line_to(context, center_x, top + size * 0.10);
  cairo_line_to(context, shackle_right, top + size * 0.30);
  cairo_line_to(context, shackle_right, body_top);
  set_source(context, 0.0, 0.0, 0.0);
  cairo_set_line_width(context, 1.8);
  cairo_stroke(context);
}

static void draw_bookmarks_button(cairo_t *context, int width) {
  double x = bookmarks_button_x(width), y = bookmarks_button_y(width);
  set_source(context, 0.92, 0.92, 0.92);
  cairo_rectangle(context, x, y, 26.0, 24.0);
  cairo_fill_preserve(context);
  set_source(context, 0.45, 0.45, 0.45);
  cairo_set_line_width(context, 1.0);
  cairo_stroke(context);
  draw_star(context, x + 9.0, y + 11.5, 6.0, false);
  set_source(context, 0.30, 0.30, 0.30);
  cairo_move_to(context, x + 17.0, y + 9.0);
  cairo_line_to(context, x + 23.0, y + 9.0);
  cairo_move_to(context, x + 17.0, y + 13.0);
  cairo_line_to(context, x + 23.0, y + 13.0);
  cairo_move_to(context, x + 17.0, y + 17.0);
  cairo_line_to(context, x + 23.0, y + 17.0);
  cairo_stroke(context);
}

static void select_address_font(cairo_t *context) {
  cairo_select_font_face(context, "sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(context, 12.0);
}

size_t tai_pres_editor_cursor_from_x(const char *text, double local_x) {
  if (local_x <= 0.0) return 0;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                         1, 1);
  cairo_t *context = cairo_create(surface);
  select_address_font(context);
  size_t length = strlen(text), offset = 0, index = 0;
  double previous = 0.0;
  while (offset < length) {
    offset += tai_pres_utf8_width(text, length, offset, NULL);
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

bool tai_pres_render_chrome_texture(SDL_Renderer *renderer,
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
    size_t cursor_byte = tai_pres_utf8_byte_at(editor->text, editor->cursor);
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

/* The Python chrome lays the inline links out after styling the active label.
 * Keep the link origin and its hit region in the same flow. */
static double tab_link_width(size_t index, size_t active) {
  size_t digits = 1;
  for (size_t n = index; n >= 10; n /= 10) digits++;
  return (index == active ? 42.0 : 29.0) + 8.0 * (double)digits;
}

static double tab_link_left(const TaiTabSetView *view, size_t index) {
  double left = 34.0;
  for (size_t previous = 0; previous < index; previous++)
    left += tab_link_width(previous, view->active_index) + 4.0;
  return left;
}

static bool compact_tab_slot(const TaiTabSetView *view, int width,
                             double *slot_width) {
  if (view->tab_count < 3 || width <= 34) return false;
  size_t last = view->tab_count - 1;
  double natural_right = tab_link_left(view, last) +
                         tab_link_width(last, view->active_index);
  if (natural_right <= (double)width) return false;
  *slot_width = ((double)width - 34.0) / (double)view->tab_count;
  return true;
}

static void draw_compact_tabs(cairo_t *context, const TaiTabSetView *view,
                              double slot_width) {
  for (size_t index = 0; index < view->tab_count; index++) {
    double left = 34.0 + slot_width * (double)index;
    bool active = index == view->active_index;
    set_source(context, active ? 0.97 : 0.86,
                        active ? 0.97 : 0.86,
                        active ? 0.97 : 0.86);
    cairo_rectangle(context, left, 6.0, slot_width, 24.0);
    cairo_fill_preserve(context);
    set_source(context, 0.50, 0.50, 0.50);
    cairo_set_line_width(context, 1.0);
    cairo_stroke(context);
    if (slot_width < 12.0) continue;
    char label[32];
    (void)snprintf(label, sizeof(label), "%zu", index);
    cairo_select_font_face(context, "Times New Roman", CAIRO_FONT_SLANT_NORMAL,
                           active ? CAIRO_FONT_WEIGHT_BOLD
                                  : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, slot_width < 24.0 ? 12.0 : 14.0);
    cairo_text_extents_t extents;
    cairo_text_extents(context, label, &extents);
    cairo_save(context);
    cairo_rectangle(context, left + 1.0, 7.0, slot_width - 2.0, 22.0);
    cairo_clip(context);
    if (active) set_source(context, 0.0, 0.0, 0.0);
    else set_source(context, 0.0, 0.0, 0.75);
    cairo_move_to(context, left + (slot_width - extents.x_advance) / 2.0,
                  23.0);
    cairo_show_text(context, label);
    cairo_restore(context);
  }
}

bool tai_pres_render_tabs_chrome_texture(SDL_Renderer *renderer,
                                      SDL_Texture **texture,
                                      const TaiTabSetView *view, int width,
                                      const AddressEditor *editor,
                                      char **error) {
  double bottom = tabs_chrome_bottom(width);
  TaiAddressField field = tai_tabs_address_field(width, view->secure);
  double field_x = field.x;
  double field_y = tabs_address_y(width);
  double field_width = field.width;
  double forward_x = forward_button_x(width);
  double forward_y = tabs_forward_button_y(width);
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
  set_source(context, view->tab_count >= TAI_MAX_TABS ? 0.83 : 0.90,
                      view->tab_count >= TAI_MAX_TABS ? 0.83 : 0.90,
                      view->tab_count >= TAI_MAX_TABS ? 0.83 : 0.90);
  cairo_rectangle(context, 0.0, 6.0, 30.0, 24.0);
  cairo_fill_preserve(context);
  set_source(context, 0.45, 0.45, 0.45);
  cairo_set_line_width(context, 1.0);
  cairo_stroke(context);
  set_source(context, view->tab_count >= TAI_MAX_TABS ? 0.50 : 0.12,
                      view->tab_count >= TAI_MAX_TABS ? 0.50 : 0.12,
                      view->tab_count >= TAI_MAX_TABS ? 0.50 : 0.12);
  cairo_set_line_width(context, 1.8);
  cairo_move_to(context, 15.0, 12.0);
  cairo_line_to(context, 15.0, 24.0);
  cairo_move_to(context, 9.0, 18.0);
  cairo_line_to(context, 21.0, 18.0);
  cairo_stroke(context);

  double compact_slot_width = 0.0;
  if (compact_tab_slot(view, width, &compact_slot_width)) {
    draw_compact_tabs(context, view, compact_slot_width);
  } else {
    for (size_t index = 0; index < view->tab_count; index++) {
      char label[64];
      tab_label(index, view->active_index, label, sizeof(label));
      cairo_select_font_face(context, "Times New Roman", CAIRO_FONT_SLANT_NORMAL,
          index == view->active_index ? CAIRO_FONT_WEIGHT_BOLD
                                      : CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(context, 16.0);
      cairo_text_extents_t extents;
      cairo_text_extents(context, label, &extents);
      if (width < 125 && index > 0) {
        const char *space = strchr(label, ' ');
        if (space) {
          char first[64], second[64];
          size_t first_length = (size_t)(space - label);
          memcpy(first, label, first_length);
          first[first_length] = '\0';
          (void)snprintf(second, sizeof(second), "%s", space + 1);
          if (index == view->active_index) set_source(context, 0.0, 0.0, 0.0);
          else set_source(context, 0.0, 0.0, 0.75);
          cairo_move_to(context, tab_link_left(view, index), 32.0);
          cairo_show_text(context, first);
          cairo_move_to(context, 0.0, 52.0);
          cairo_show_text(context, second);
          continue;
        }
      }
      double tab_x = tab_link_left(view, index);
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
  }

  draw_button(context, 0.0, tabs_back_button_y(width), 45.0, 24.0,
              view->can_go_back, false);
  draw_button(context, forward_x, forward_y, 45.0, 24.0,
              view->can_go_forward, true);
  draw_bookmarks_button(context, width);
  if (view->secure)
    draw_lock(context, field.slot_x + TAI_SECURITY_ICON_SLOT / 2.0,
              field_y + TAI_ADDRESS_HEIGHT / 2.0);
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
                  fmax(0.0, field_width - 28.0), TAI_ADDRESS_HEIGHT - 2.0);
  cairo_clip(context);
  select_address_font(context);
  set_source(context, 0.08, 0.08, 0.08);
  cairo_move_to(context, field_x + 5.0, field_y + 12.5);
  cairo_show_text(context, text ? text : "");
  if (editor->focused) {
    size_t cursor_byte = tai_pres_utf8_byte_at(editor->text, editor->cursor);
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
  draw_star(context, field_x + field_width - 12.0,
            field_y + TAI_ADDRESS_HEIGHT / 2.0, 7.0,
            view->bookmarked);
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

bool tai_pres_tabs_tab_link_hit(const TaiTabSetView *view, int width,
                                double x, double y, size_t *index) {
  double compact_slot_width = 0.0;
  if (compact_tab_slot(view, width, &compact_slot_width)) {
    if (x < 34.0 || x >= (double)width || y < 6.0 || y >= 30.0)
      return false;
    *index = (size_t)((x - 34.0) / compact_slot_width);
    return *index < view->tab_count;
  }
  for (size_t tab = 0; tab < view->tab_count; tab++) {
    double left = tab_link_left(view, tab);
    double right = left + tab_link_width(tab, view->active_index);
    double top = 19.0, bottom = 35.0;
    if (width < 125 && tab > 0) {
      left = 0.0;
      right = tab == view->active_index ? 108.0 : 113.0;
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
