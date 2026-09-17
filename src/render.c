#include "tai/render.h"

#include <cairo/cairo.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  TaiDisplayCommand *items;
  size_t count, capacity;
  bool failed;
} Build;

struct TaiDisplayList {
  TaiDisplayCommand *items;
  size_t count;
};

static void set_error(char **error, const char *message) {
  if (error && !*error) *error = tai_strdup(message);
}

static bool append(Build *build, TaiDisplayCommand command) {
  if (build->count == SIZE_MAX / sizeof(*build->items)) {
    build->failed = true;
    return false;
  }
  if (build->count == build->capacity) {
    size_t capacity = build->capacity ? build->capacity * 2 : 8;
    if (capacity < build->count + 1 ||
        capacity > SIZE_MAX / sizeof(*build->items)) {
      build->failed = true;
      return false;
    }
    TaiDisplayCommand *next = realloc(build->items,
                                      capacity * sizeof(*build->items));
    if (!next) {
      build->failed = true;
      return false;
    }
    build->items = next;
    build->capacity = capacity;
  }
  build->items[build->count++] = command;
  return true;
}

static uint8_t hex(char c) {
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
  return 255;
}

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
}

static uint32_t color(const char *value, bool *transparent) {
  if (transparent) *transparent = false;
  if (!value || !*value) return rgb(0, 0, 0, 255);
  while (isspace((unsigned char)*value)) value++;
  if (!strcmp(value, "transparent")) {
    if (transparent) *transparent = true;
    return 0;
  }
  if (!strcmp(value, "white")) return rgb(255, 255, 255, 255);
  if (!strcmp(value, "red")) return rgb(255, 0, 0, 255);
  if (!strcmp(value, "green")) return rgb(0, 128, 0, 255);
  if (!strcmp(value, "blue")) return rgb(0, 0, 255, 255);
  if (!strcmp(value, "yellow")) return rgb(255, 255, 0, 255);
  if (!strcmp(value, "gray") || !strcmp(value, "grey"))
    return rgb(128, 128, 128, 255);
  if (!strcmp(value, "lightgray") || !strcmp(value, "lightgrey"))
    return rgb(211, 211, 211, 255);
  if (value[0] == '#') {
    size_t length = strlen(value + 1);
    if (length == 3 || length == 4 || length == 6 || length == 8) {
      uint8_t digits[8];
      bool valid = true;
      for (size_t i = 0; i < length; i++) {
        digits[i] = hex(value[i + 1]);
        if (digits[i] == 255) valid = false;
      }
      if (valid) {
        uint8_t r, g, b, a = 255;
        if (length <= 4) {
          r = (uint8_t)(digits[0] * 17);
          g = (uint8_t)(digits[1] * 17);
          b = (uint8_t)(digits[2] * 17);
          if (length == 4) a = (uint8_t)(digits[3] * 17);
        } else {
          r = (uint8_t)(digits[0] * 16 + digits[1]);
          g = (uint8_t)(digits[2] * 16 + digits[3]);
          b = (uint8_t)(digits[4] * 16 + digits[5]);
          if (length == 8) a = (uint8_t)(digits[6] * 16 + digits[7]);
        }
        return rgb(r, g, b, a);
      }
    }
  }
  /* Match the Python renderer's conservative fallback for unknown colors. */
  return rgb(0, 0, 0, 255);
}

static const char *style(const TaiNode *node, const char *key,
                         const char *fallback) {
  const char *value = node ? tai_map_get(&node->style, key) : NULL;
  return value ? value : fallback;
}

static char *font_family(const char *value) {
  const char *end = strchr(value, ',');
  size_t length = end ? (size_t)(end - value) : strlen(value);
  while (length && isspace((unsigned char)value[length - 1])) length--;
  while (length && isspace((unsigned char)*value)) {
    value++;
    length--;
  }
  if ((length >= 2) &&
      ((value[0] == '\'' && value[length - 1] == '\'') ||
       (value[0] == '"' && value[length - 1] == '"'))) {
    value++;
    length -= 2;
  }
  const char *replacement = NULL;
  if (length == 5 && !strncmp(value, "serif", length))
    replacement = "Times New Roman";
  else if (length == 10 && !strncmp(value, "sans-serif", length))
    replacement = "Arial";
  else if (length == 9 && !strncmp(value, "monospace", length))
    replacement = "Courier New";
  if (replacement) return tai_strdup(replacement);
  return tai_strndup(value, length);
}

static bool make_fill(Build *build, const TaiLayoutItem *item) {
  const char *value = tai_map_get(&item->node->style, "background-color");
  if (!value) value = tai_map_get(&item->node->style, "background");
  bool transparent = false;
  uint32_t rgba = color(value, &transparent);
  if (!value || transparent || item->width <= 0 || item->height <= 0)
    return true;
  TaiDisplayCommand command = {
      .kind = TAI_DRAW_FILL_RECT,
      .x = item->x,
      .y = item->y,
      .width = item->width,
      .height = item->height,
      .rgba = rgba,
  };
  return append(build, command);
}

static bool make_text(Build *build, const TaiLayoutItem *item) {
  if (!item->word || !*item->word) return true;
  TaiDisplayCommand command = {
      .kind = TAI_DRAW_TEXT,
      .x = item->x,
      .y = item->y,
      .width = item->width,
      .height = item->height,
      .rgba = color(style(item->node, "color", "black"), NULL),
      .text = tai_strdup(item->word),
      .font_family = font_family(style(item->node, "font-family", "serif")),
      .font_size = item->font_size > 0 ? item->font_size : 16.0,
      .ascent = item->ascent,
      .bold = item->bold,
      .italic = item->italic,
  };
  if (!command.text || !command.font_family) {
    free((char *)command.text);
    free((char *)command.font_family);
    build->failed = true;
    return false;
  }
  if (!append(build, command)) {
    free((char *)command.text);
    free((char *)command.font_family);
    return false;
  }
  return true;
}

static bool collect(const TaiLayoutItem *item, void *opaque) {
  Build *build = opaque;
  if (item->kind == TAI_LAYOUT_BLOCK &&
      !make_fill(build, item)) return false;
  if (item->kind == TAI_LAYOUT_TEXT && !make_text(build, item)) return false;
  return !build->failed;
}

TaiDisplayList *tai_display_list_create(const TaiLayout *layout, char **error) {
  if (error) {
    free(*error);
    *error = NULL;
  }
  if (!layout) {
    set_error(error, "invalid display list input");
    return NULL;
  }
  Build build = {0};
  if (!tai_layout_visit(layout, collect, &build, error) || build.failed) {
    for (size_t i = 0; i < build.count; i++) {
      free((char *)build.items[i].text);
      free((char *)build.items[i].font_family);
    }
    free(build.items);
    if (error && !*error) *error = tai_strdup("display list allocation failed");
    return NULL;
  }
  TaiDisplayList *list = calloc(1, sizeof(*list));
  if (!list) {
    for (size_t i = 0; i < build.count; i++) {
      free((char *)build.items[i].text);
      free((char *)build.items[i].font_family);
    }
    free(build.items);
    set_error(error, "display list allocation failed");
    return NULL;
  }
  list->items = build.items;
  list->count = build.count;
  return list;
}

void tai_display_list_destroy(TaiDisplayList *list) {
  if (!list) return;
  for (size_t i = 0; i < list->count; i++) {
    free((char *)list->items[i].text);
    free((char *)list->items[i].font_family);
  }
  free(list->items);
  free(list);
}

size_t tai_display_list_count(const TaiDisplayList *list) {
  return list ? list->count : 0;
}

const TaiDisplayCommand *tai_display_list_command(const TaiDisplayList *list,
                                                  size_t index) {
  return list && index < list->count ? &list->items[index] : NULL;
}

void tai_display_list_json(FILE *out, const TaiDisplayList *list) {
  fputc('[', out);
  if (list) {
    for (size_t i = 0; i < list->count; i++) {
      const TaiDisplayCommand *command = &list->items[i];
      if (i) fputc(',', out);
      fprintf(out, "{\"kind\":\"%s\",\"x\":%.17g,\"y\":%.17g"
                   ",\"width\":%.17g,\"height\":%.17g,\"rgba\":%u",
              command->kind == TAI_DRAW_TEXT ? "text" : "fill_rect",
              command->x, command->y, command->width, command->height,
              command->rgba);
      if (command->text) {
        fputs(",\"text\":", out);
        tai_json_string(out, command->text);
        fputs(",\"font_family\":", out);
        tai_json_string(out, command->font_family);
        fprintf(out, ",\"font_size\":%.17g", command->font_size);
        fprintf(out, ",\"bold\":%s,\"italic\":%s",
                command->bold ? "true" : "false",
                command->italic ? "true" : "false");
      }
      fputc('}', out);
    }
  }
  fputc(']', out);
}

static void cairo_color(uint32_t rgba, double *r, double *g, double *b,
                        double *a) {
  *r = (double)((rgba >> 24) & 255) / 255.0;
  *g = (double)((rgba >> 16) & 255) / 255.0;
  *b = (double)((rgba >> 8) & 255) / 255.0;
  *a = (double)(rgba & 255) / 255.0;
}

bool tai_display_list_write_png(const TaiDisplayList *list, const char *path,
                                int width, int height, char **error) {
  if (error) {
    free(*error);
    *error = NULL;
  }
  if (!list || !path || width <= 0 || height <= 0) {
    set_error(error, "invalid PNG output input");
    return false;
  }
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          width, height);
  cairo_t *context = cairo_create(surface);
  cairo_set_source_rgb(context, 1, 1, 1);
  cairo_paint(context);
  for (size_t i = 0; i < list->count; i++) {
    const TaiDisplayCommand *command = &list->items[i];
    double r, g, b, a;
    cairo_color(command->rgba, &r, &g, &b, &a);
    cairo_set_source_rgba(context, r, g, b, a);
    if (command->kind == TAI_DRAW_FILL_RECT) {
      cairo_rectangle(context, command->x, command->y, command->width,
                      command->height);
      cairo_fill(context);
    } else {
      cairo_select_font_face(
          context, command->font_family,
          command->italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
          command->bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(context, command->font_size);
      cairo_move_to(context, command->x, command->y + command->ascent);
      cairo_show_text(context, command->text);
    }
  }
  cairo_status_t status = cairo_status(context);
  if (status == CAIRO_STATUS_SUCCESS)
    status = cairo_surface_write_to_png(surface, path);
  cairo_destroy(context);
  cairo_surface_destroy(surface);
  if (status != CAIRO_STATUS_SUCCESS) {
    set_error(error, cairo_status_to_string(status));
    return false;
  }
  return true;
}
