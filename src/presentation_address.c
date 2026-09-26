#include "presentation_internal.h"
#include <stdio.h>

size_t tai_pres_utf8_width(const char *text, size_t length, size_t offset,
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
    offset += tai_pres_utf8_width(text, length, offset, NULL);
  return count;
}

size_t tai_pres_utf8_byte_at(const char *text, size_t character_index) {
  size_t length = strlen(text), offset = 0;
  while (character_index && offset < length) {
    offset += tai_pres_utf8_width(text, length, offset, NULL);
    character_index--;
  }
  return offset;
}

void tai_pres_editor_discard(AddressEditor *editor) {
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
    size_t width = tai_pres_utf8_width(input, input_length, offset, &codepoint);
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
  size_t cursor_byte = tai_pres_utf8_byte_at(editor->text, editor->cursor);
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
  size_t end = tai_pres_utf8_byte_at(editor->text, editor->cursor);
  size_t start = tai_pres_utf8_byte_at(editor->text, editor->cursor - 1);
  size_t length = strlen(editor->text);
  memmove(editor->text + start, editor->text + end, length - end + 1);
  editor->cursor--;
  editor->dirty = true;
  return true;
}

bool tai_pres_handle_chrome_click(TaiPage **page_slot, SDL_Event const *event,
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
    editor->cursor = tai_pres_editor_cursor_from_x(
        editor->text, x - field_x - 5.0);
    *chrome_changed = true;
    return true;
  }

  bool back_hit = x >= 0.0 && x < 45.0 && y >= 36.0 && y < 60.0;
  double forward_x = forward_button_x(width);
  double forward_y = forward_button_y(width);
  bool forward_hit = x >= forward_x && x < forward_x + 45.0 &&
      y >= forward_y && y < forward_y + 24.0;
  tai_pres_editor_discard(editor);
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

bool tai_pres_handle_tabs_chrome_click(TaiTabSet *tabs, SDL_Event const *event,
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
  double x = event->button.x, y = event->button.y;
  if (view.tab_count >= TAI_MAX_TABS &&
      x >= 0.0 && x < 30.0 && y >= 6.0 && y < 30.0)
    return true;
  bool was_focused = editor->focused;
  bool was_dirty = editor->dirty;
  editor->focused = false;
  bool blurred = false;
  if (view.page && !tai_page_blur_input(view.page, &blurred, error)) {
    editor->focused = was_focused;
    return false;
  }
  *page_changed = blurred;

  if (x >= 0.0 && x < 30.0 && y >= 6.0 && y < 30.0) {
    if (!tai_tabset_new_tab(tabs, error)) return false;
    tai_pres_editor_discard(editor);
    *page_changed = true;
    *chrome_changed = true;
    return true;
  }
  size_t selected = 0;
  if (tai_pres_tabs_tab_link_hit(&view, width, x, y, &selected)) {
    if (!tai_tabset_select(tabs, selected)) return true;
    tai_pres_editor_discard(editor);
    *page_changed = true;
    *chrome_changed = true;
    return true;
  }

  double field_x = address_x(width), field_y = tabs_address_y(width);
  double field_width = tabs_address_width(width);
  double bookmarks_x = bookmarks_button_x(width);
  double bookmarks_y = bookmarks_button_y(width);
  if (x >= bookmarks_x && x < bookmarks_x + 26.0 &&
      y >= bookmarks_y && y < bookmarks_y + 24.0) {
    tai_pres_editor_discard(editor);
    char *navigation_error = NULL;
    if (!tai_tabset_open_bookmarks(tabs, &navigation_error))
      fprintf(stderr, "bookmarks navigation failed: %s\n",
              navigation_error ? navigation_error : "unknown error");
    free(navigation_error);
    *chrome_changed = true;
    return true;
  }
  bool star_hit = x >= field_x + fmax(0.0, field_width - 23.0) &&
      x < field_x + field_width &&
      y >= field_y && y < field_y + TAI_ADDRESS_HEIGHT;
  if (star_hit) {
    tai_pres_editor_discard(editor);
    if (view.bookmarkable) {
      bool bookmarked = false;
      char *toggle_error = NULL;
      if (!tai_tabset_toggle_bookmark(tabs, &bookmarked, &toggle_error))
        fprintf(stderr, "bookmark toggle failed: %s\n",
                toggle_error ? toggle_error : "unknown error");
      free(toggle_error);
      *chrome_changed = true;
    } else {
      *chrome_changed = was_focused || was_dirty;
    }
    return true;
  }
  bool address_hit = x >= field_x && x < field_x + field_width &&
      y >= field_y && y < field_y + TAI_ADDRESS_HEIGHT;
  if (address_hit) {
    if (!was_focused && !editor->dirty &&
        !editor_set_text(editor, view.url ? view.url : "")) {
      set_error(error, "address field allocation failed");
      return false;
    }
    editor->focused = true;
    editor->cursor = tai_pres_editor_cursor_from_x(editor->text,
                                                    x - field_x - 5.0);
    *chrome_changed = true;
    return true;
  }

  double back_y = tabs_back_button_y(width);
  bool back_hit = x >= 0.0 && x < 45.0 && y >= back_y && y < back_y + 24.0;
  double forward_x = forward_button_x(width);
  double forward_y = tabs_forward_button_y(width);
  bool forward_hit = x >= forward_x && x < forward_x + 45.0 &&
      y >= forward_y && y < forward_y + 24.0;
  tai_pres_editor_discard(editor);
  *chrome_changed = was_focused || was_dirty || back_hit || forward_hit;
  int direction = back_hit ? -1 : forward_hit ? 1 : 0;
  if (direction && tai_tabset_history_available(tabs, direction)) {
    if (!tai_tabset_history(tabs, direction, error)) return false;
    *chrome_changed = true;
  }
  return true;
}

char *tai_pres_copy_page_url(const TaiPage *page) {
  const char *url = page ? tai_url_string(tai_page_url(page)) : NULL;
  return url ? tai_strdup(url) : NULL;
}

bool tai_pres_page_url_differs(const char *previous_url, const TaiPage *page) {
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

bool tai_pres_handle_address_key(TaiPage **page_slot, const SDL_Event *event,
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
  tai_pres_editor_discard(editor);
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

bool tai_pres_handle_tabs_address_key(TaiTabSet *tabs, const SDL_Event *event,
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
  tai_pres_editor_discard(editor);
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

bool tai_pres_handle_address_text(const SDL_Event *event,
                                  SDL_WindowID window_id,
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
