#include "tai/render.h"

#include <cairo/cairo.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t outer_start;
  size_t scroll_start;
  bool scroll;
} ClipStart;

typedef struct {
  TaiDisplayCommand *items;
  size_t count, capacity;
  ClipStart *clip_starts;
  size_t clip_count, clip_capacity;
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

static bool remember_clip(Build *build, ClipStart start) {
  if (build->clip_count == build->clip_capacity) {
    size_t capacity = build->clip_capacity ? build->clip_capacity * 2 : 4;
    if (capacity < build->clip_count + 1 ||
        capacity > SIZE_MAX / sizeof(*build->clip_starts)) {
      build->failed = true;
      return false;
    }
    ClipStart *next = realloc(build->clip_starts,
                              capacity * sizeof(*build->clip_starts));
    if (!next) {
      build->failed = true;
      return false;
    }
    build->clip_starts = next;
    build->clip_capacity = capacity;
  }
  build->clip_starts[build->clip_count++] = start;
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
  if (!strcmp(value, "lightblue")) return rgb(173, 216, 230, 255);
  if (!strcmp(value, "lightgreen")) return rgb(144, 238, 144, 255);
  if (!strcmp(value, "orange")) return rgb(255, 165, 0, 255);
  if (!strcmp(value, "orangered")) return rgb(255, 69, 0, 255);
  if (value[0] == '#') {
    size_t length = strlen(value + 1);
    if (length == 3 || length == 6 || length == 8) {
      uint8_t digits[8];
      bool valid = true;
      for (size_t i = 0; i < length; i++) {
        digits[i] = hex(value[i + 1]);
        if (digits[i] == 255) valid = false;
      }
      if (valid) {
        uint8_t r, g, b, a = 255;
        if (length == 3) {
          r = (uint8_t)(digits[0] * 17);
          g = (uint8_t)(digits[1] * 17);
          b = (uint8_t)(digits[2] * 17);
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

static double effect_radius(const TaiNode *node) {
  const char *value = style(node, "border-radius", "0px");
  char *end = NULL;
  double radius = strtod(value, &end);
  if (end == value) return 0.0;
  while (isspace((unsigned char)*end)) end++;
  if (*end &&
      (tolower((unsigned char)end[0]) != 'p' ||
       tolower((unsigned char)end[1]) != 'x'))
    return 0.0;
  if (*end) {
    end += 2;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return 0.0;
  }
  return fmax(0.0, radius);
}

static double fill_radius(const TaiNode *node) {
  const char *value = style(node, "border-radius", "0px");
  errno = 0;
  char *end = NULL;
  long radius = strtol(value, &end, 10);
  if (end == value || errno == ERANGE) return 0.0;
  while (isspace((unsigned char)*end)) end++;
  if (strcmp(end, "px")) return 0.0;
  return radius > 0 ? (double)radius : 0.0;
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
      .radius = fill_radius(item->node),
      .hit_radius = effect_radius(item->node),
      .node_id = item->node->id,
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
      .hit_radius = effect_radius(item->node),
      .bold = item->bold,
      .italic = item->italic,
      .node_id = item->node->id,
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

static bool is_scroll_container(const TaiLayoutItem *item) {
  return item->kind == TAI_LAYOUT_BLOCK && item->scrollable;
}

static bool is_clip_container(const TaiLayoutItem *item) {
  if (item->kind != TAI_LAYOUT_BLOCK || !item->node ||
      item->node->kind != TAI_ELEMENT)
    return false;
  const char *overflow = style(item->node, "overflow", "visible");
  return !strcmp(overflow, "clip") || !strcmp(overflow, "scroll");
}

static bool collect(const TaiLayoutItem *item, TaiLayoutVisitEvent event,
                    void *opaque) {
  Build *build = opaque;
  if (event == TAI_LAYOUT_LEAVE) {
    if (is_clip_container(item)) {
      if (!build->clip_count) {
        build->failed = true;
        return false;
      }
      ClipStart start = build->clip_starts[--build->clip_count];
      if (start.scroll) {
        if (build->count == start.scroll_start + 1)
          build->count--;
        else if (!append(build, (TaiDisplayCommand){
                                   .kind = TAI_POP_CLIP_SCROLL}))
          return false;
      }
      if (build->count == start.outer_start + 1) {
        build->count = start.outer_start;
        return true;
      }
      if (build->count == start.outer_start + 2 &&
          build->items[start.outer_start + 1].kind == TAI_DRAW_HIT_TEST) {
        build->items[start.outer_start] = build->items[start.outer_start + 1];
        build->count = start.outer_start + 1;
        return true;
      }
      return append(build, (TaiDisplayCommand){.kind = TAI_POP_CLIP});
    }
    return true;
  }
  if (is_clip_container(item)) {
    ClipStart start = {
        .outer_start = build->count,
        .scroll = is_scroll_container(item),
    };
    if (!append(build, (TaiDisplayCommand){
                         .kind = TAI_PUSH_CLIP,
                         .x = item->x, .y = item->y,
                         .width = item->width, .height = item->height,
                         .radius = effect_radius(item->node),
                     }))
      return false;
    if (start.scroll) {
      if (item->kind == TAI_LAYOUT_BLOCK && !make_fill(build, item)) return false;
      if (!append(build, (TaiDisplayCommand){
                           .kind = TAI_DRAW_HIT_TEST,
                           .x = item->x, .y = item->y,
                           .width = item->width, .height = item->height,
                           .hit_radius = effect_radius(item->node),
                           .node_id = item->node->id,
                       }))
        return false;
      start.scroll_start = build->count;
      if (!append(build, (TaiDisplayCommand){
                           .kind = TAI_PUSH_CLIP_SCROLL,
                           .x = item->x, .y = item->y,
                           .width = item->width, .height = item->height,
                           .scroll_y = item->scroll_y,
                       }))
        return false;
    } else if (item->kind == TAI_LAYOUT_BLOCK && !make_fill(build, item)) {
      return false;
    }
    return remember_clip(build, start);
  }
  if (item->kind == TAI_LAYOUT_BLOCK && !make_fill(build, item)) return false;
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
  if (!tai_layout_walk(layout, collect, &build, error) || build.failed) {
    for (size_t i = 0; i < build.count; i++) {
      free((char *)build.items[i].text);
      free((char *)build.items[i].font_family);
    }
    free(build.items);
    free(build.clip_starts);
    if (error && !*error) *error = tai_strdup("display list allocation failed");
    return NULL;
  }
  TaiDisplayList *list = calloc(1, sizeof(*list));
  free(build.clip_starts);
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

static bool contains(const TaiDisplayCommand *command, double x, double y) {
  double left = (float)command->x;
  double top = (float)command->y;
  double right = (float)(command->x + command->width);
  double bottom = (float)(command->y + command->height);
  return x >= left && y >= top && x < right && y < bottom;
}

static bool rounded_contains(const TaiDisplayCommand *command, double x,
                             double y) {
  if (!contains(command, x, y)) return false;
  double left = (double)(float)command->x;
  double top = (double)(float)command->y;
  double right = (double)(float)(command->x + command->width);
  double bottom = (double)(float)(command->y + command->height);
  double width = fmax(0.0, right - left);
  double height = fmax(0.0, bottom - top);
  if (width == 0.0 || height == 0.0) return false;
  double radius = fmax(0.0, command->hit_radius);
  radius = fmin(radius, fmin(width / 2.0, height / 2.0));
  if (radius == 0.0) return true;
  double inner_left = left + radius;
  double inner_right = right - radius;
  double inner_top = top + radius;
  double inner_bottom = bottom - radius;
  if ((inner_left <= x && x <= inner_right) ||
      (inner_top <= y && y <= inner_bottom))
    return true;
  double center_x = x < inner_left ? inner_left : inner_right;
  double center_y = y < inner_top ? inner_top : inner_bottom;
  double dx = (x - center_x) / radius;
  double dy = (y - center_y) / radius;
  return dx * dx + dy * dy <= 1.0;
}

static bool hit_range(const TaiDisplayList *list, size_t begin, size_t end,
                      double x, double y, TaiDisplayHit *hit) {
  size_t index = end;
  while (index > begin) {
    const TaiDisplayCommand *command = &list->items[--index];
    if (command->kind == TAI_POP_CLIP ||
        command->kind == TAI_POP_CLIP_SCROLL) {
      size_t depth = 1;
      size_t push = index;
      while (push > begin && depth) {
        const TaiDisplayCommand *candidate = &list->items[--push];
        if (candidate->kind == TAI_POP_CLIP ||
            candidate->kind == TAI_POP_CLIP_SCROLL)
          depth++;
        else if (candidate->kind == TAI_PUSH_CLIP_SCROLL ||
                 candidate->kind == TAI_PUSH_CLIP)
          depth--;
      }
      if (depth) return false;
      const TaiDisplayCommand *clip = &list->items[push];
      if ((clip->kind == TAI_PUSH_CLIP || contains(clip, x, y)) &&
          hit_range(list, push + 1, index, x,
                    y + (clip->kind == TAI_PUSH_CLIP_SCROLL
                             ? clip->scroll_y : 0.0), hit))
        return true;
      index = push;
      continue;
    }
    if (command->kind == TAI_PUSH_CLIP_SCROLL ||
        command->kind == TAI_PUSH_CLIP)
      return false;
    if ((command->kind == TAI_DRAW_FILL_RECT ||
         command->kind == TAI_DRAW_TEXT ||
         command->kind == TAI_DRAW_HIT_TEST) &&
        rounded_contains(command, x, y)) {
      *hit = (TaiDisplayHit){
          .node_id = command->node_id, .kind = command->kind,
          .x = command->x, .y = command->y,
          .width = command->width, .height = command->height,
      };
      return true;
    }
  }
  return false;
}

bool tai_display_list_hit_test(const TaiDisplayList *list, double x, double y,
                               TaiDisplayHit *hit) {
  if (hit) *hit = (TaiDisplayHit){0};
  if (!list || !hit || !isfinite(x) || !isfinite(y)) return false;
  return hit_range(list, 0, list->count, x, y, hit);
}

void tai_display_list_json(FILE *out, const TaiDisplayList *list) {
  fputc('[', out);
  if (list) {
    for (size_t i = 0; i < list->count; i++) {
      const TaiDisplayCommand *command = &list->items[i];
      if (i) fputc(',', out);
      const char *kind = command->kind == TAI_DRAW_TEXT ? "text" :
                         command->kind == TAI_DRAW_FILL_RECT ? "fill_rect" :
                         command->kind == TAI_DRAW_HIT_TEST ? "hit_test" :
                         command->kind == TAI_PUSH_CLIP ? "push_clip" :
                         command->kind == TAI_PUSH_CLIP_SCROLL ?
                             "push_clip_scroll" :
                         command->kind == TAI_POP_CLIP ? "pop_clip" :
                             "pop_clip_scroll";
      fprintf(out, "{\"kind\":\"%s\"", kind);
      if (command->kind == TAI_POP_CLIP ||
          command->kind == TAI_POP_CLIP_SCROLL) {
        fputc('}', out);
        continue;
      }
      fprintf(out, ",\"x\":%.17g,\"y\":%.17g"
                   ",\"width\":%.17g,\"height\":%.17g,\"rgba\":%u",
              command->x, command->y, command->width, command->height,
              command->rgba);
      if (command->kind == TAI_PUSH_CLIP_SCROLL)
        fprintf(out, ",\"scroll_y\":%.17g", command->scroll_y);
      if (command->radius > 0.0) {
        if (isinf(command->radius))
          fputs(",\"radius\":Infinity", out);
        else
          fprintf(out, ",\"radius\":%.17g", command->radius);
      }
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

static void cairo_rounded_rectangle(cairo_t *context,
                                    const TaiDisplayCommand *command) {
  double radius = fmax(0.0, command->radius);
  radius = fmin(radius, fmin(command->width / 2.0, command->height / 2.0));
  if (radius == 0.0) {
    cairo_rectangle(context, command->x, command->y, command->width,
                    command->height);
    return;
  }
  static const double pi = 3.14159265358979323846;
  double left = command->x;
  double top = command->y;
  double right = command->x + command->width;
  double bottom = command->y + command->height;
  cairo_new_sub_path(context);
  cairo_arc(context, right - radius, top + radius, radius, -pi / 2.0, 0.0);
  cairo_arc(context, right - radius, bottom - radius, radius, 0.0, pi / 2.0);
  cairo_arc(context, left + radius, bottom - radius, radius, pi / 2.0, pi);
  cairo_arc(context, left + radius, top + radius, radius, pi, 3.0 * pi / 2.0);
  cairo_close_path(context);
}

bool tai_display_list_write_png_region(const TaiDisplayList *list,
                                       const char *path, int width, int height,
                                       double document_x, double document_y,
                                       char **error) {
  if (error) {
    free(*error);
    *error = NULL;
  }
  if (!list || !path || width <= 0 || height <= 0 ||
      !isfinite(document_x) || !isfinite(document_y)) {
    set_error(error, "invalid PNG output input");
    return false;
  }
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          width, height);
  cairo_t *context = cairo_create(surface);
  const TaiDisplayCommand **clip_stack =
      list->count ? calloc(list->count, sizeof(*clip_stack)) : NULL;
  if (list->count && !clip_stack) {
    cairo_destroy(context);
    cairo_surface_destroy(surface);
    set_error(error, "PNG clip stack allocation failed");
    return false;
  }
  size_t clip_depth = 0;
  bool valid = true;
  cairo_set_source_rgb(context, 1, 1, 1);
  cairo_paint(context);
  cairo_translate(context, -document_x, -document_y);
  for (size_t i = 0; i < list->count; i++) {
    const TaiDisplayCommand *command = &list->items[i];
    double r, g, b, a;
    cairo_color(command->rgba, &r, &g, &b, &a);
    cairo_set_source_rgba(context, r, g, b, a);
    if (command->kind == TAI_PUSH_CLIP) {
      cairo_save(context);
      cairo_push_group(context);
      clip_stack[clip_depth++] = command;
    } else if (command->kind == TAI_PUSH_CLIP_SCROLL) {
      cairo_save(context);
      cairo_rectangle(context, command->x, command->y,
                      command->width, command->height);
      cairo_clip(context);
      cairo_translate(context, 0, -command->scroll_y);
      clip_stack[clip_depth++] = command;
    } else if (command->kind == TAI_POP_CLIP ||
               command->kind == TAI_POP_CLIP_SCROLL) {
      if (!clip_depth) {
        valid = false;
        break;
      }
      const TaiDisplayCommand *clip = clip_stack[--clip_depth];
      if (clip->kind == TAI_PUSH_CLIP) {
        cairo_pattern_t *group = cairo_pop_group(context);
        cairo_set_antialias(context, CAIRO_ANTIALIAS_NONE);
        cairo_rounded_rectangle(context, clip);
        cairo_clip(context);
        cairo_set_source(context, group);
        cairo_paint(context);
        cairo_pattern_destroy(group);
      }
      cairo_restore(context);
    } else if (command->kind == TAI_DRAW_HIT_TEST) {
      continue;
    } else if (command->kind == TAI_DRAW_FILL_RECT) {
      cairo_rounded_rectangle(context, command);
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
  if (clip_depth) valid = false;
  cairo_status_t status = cairo_status(context);
  if (valid && status == CAIRO_STATUS_SUCCESS)
    status = cairo_surface_write_to_png(surface, path);
  cairo_destroy(context);
  cairo_surface_destroy(surface);
  free(clip_stack);
  if (!valid) {
    set_error(error, "invalid display list clip pairing");
    return false;
  }
  if (status != CAIRO_STATUS_SUCCESS) {
    set_error(error, cairo_status_to_string(status));
    return false;
  }
  return true;
}

bool tai_display_list_write_png(const TaiDisplayList *list, const char *path,
                                int width, int height, char **error) {
  return tai_display_list_write_png_region(list, path, width, height, 0.0, 0.0,
                                           error);
}
