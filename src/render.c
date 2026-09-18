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
  size_t start;
  bool active;
} BlendStart;

typedef BlendStart BlurStart;

typedef struct {
  TaiDisplayCommand *items;
  size_t count, capacity;
  ClipStart *clip_starts;
  size_t clip_count, clip_capacity;
  BlendStart *blend_starts;
  size_t blend_count, blend_capacity;
  BlurStart *blur_starts;
  size_t blur_count, blur_capacity;
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

static bool remember_blend(Build *build, BlendStart start) {
  if (build->blend_count == build->blend_capacity) {
    size_t capacity = build->blend_capacity ? build->blend_capacity * 2 : 8;
    if (capacity < build->blend_count + 1 ||
        capacity > SIZE_MAX / sizeof(*build->blend_starts)) {
      build->failed = true;
      return false;
    }
    BlendStart *next = realloc(build->blend_starts,
                               capacity * sizeof(*build->blend_starts));
    if (!next) {
      build->failed = true;
      return false;
    }
    build->blend_starts = next;
    build->blend_capacity = capacity;
  }
  build->blend_starts[build->blend_count++] = start;
  return true;
}

static bool remember_blur(Build *build, BlurStart start) {
  if (build->blur_count == build->blur_capacity) {
    size_t capacity = build->blur_capacity ? build->blur_capacity * 2 : 8;
    if (capacity < build->blur_count + 1 ||
        capacity > SIZE_MAX / sizeof(*build->blur_starts)) {
      build->failed = true;
      return false;
    }
    BlurStart *next = realloc(build->blur_starts,
                              capacity * sizeof(*build->blur_starts));
    if (!next) {
      build->failed = true;
      return false;
    }
    build->blur_starts = next;
    build->blur_capacity = capacity;
  }
  build->blur_starts[build->blur_count++] = start;
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

static bool trimmed_equal(const char *value, const char *expected) {
  if (!value) value = "";
  while (isspace((unsigned char)*value)) value++;
  size_t length = strlen(value);
  while (length && isspace((unsigned char)value[length - 1])) length--;
  if (length != strlen(expected)) return false;
  for (size_t i = 0; i < length; i++)
    if (tolower((unsigned char)value[i]) !=
        tolower((unsigned char)expected[i]))
      return false;
  return true;
}

static double effect_opacity(const TaiNode *node) {
  const char *value = style(node, "opacity", "1.0");
  while (isspace((unsigned char)*value)) value++;
  size_t length = strlen(value);
  while (length && isspace((unsigned char)value[length - 1])) length--;
  bool percent = length && value[length - 1] == '%';
  if (percent) {
    length--;
    while (length && isspace((unsigned char)value[length - 1])) length--;
  }
  char *copy = tai_strndup(value, length);
  if (!copy) return NAN;
  if (strchr(copy, 'x') || strchr(copy, 'X')) {
    free(copy);
    return 1.0;
  }
  char *end = NULL;
  double opacity = strtod(copy, &end);
  bool valid = end != copy && !*end;
  free(copy);
  if (!valid || isnan(opacity)) return 1.0;
  if (percent) opacity /= 100.0;
  return fmax(0.0, fmin(1.0, opacity));
}

static TaiBlendMode effect_blend_mode(const TaiNode *node, bool *active) {
  const char *value = style(node, "mix-blend-mode", "normal");
  if (trimmed_equal(value, "") || trimmed_equal(value, "normal") ||
      trimmed_equal(value, "src-over") || trimmed_equal(value, "source-over")) {
    *active = false;
    return TAI_BLEND_SOURCE_OVER;
  }
  *active = true;
  if (trimmed_equal(value, "multiply")) return TAI_BLEND_MULTIPLY;
  if (trimmed_equal(value, "difference")) return TAI_BLEND_DIFFERENCE;
  if (trimmed_equal(value, "destination-in"))
    return TAI_BLEND_DESTINATION_IN;
  return TAI_BLEND_SOURCE_OVER;
}

static double effect_blur_sigma(const TaiNode *node) {
  const char *value = style(node, "filter", "none");
  while (isspace((unsigned char)*value)) value++;
  size_t length = strlen(value);
  while (length && isspace((unsigned char)value[length - 1])) length--;
  if ((length == 0) || (length == 4 &&
      tolower((unsigned char)value[0]) == 'n' &&
      tolower((unsigned char)value[1]) == 'o' &&
      tolower((unsigned char)value[2]) == 'n' &&
      tolower((unsigned char)value[3]) == 'e')) return 0.0;
  if (length < 6 || tolower((unsigned char)value[0]) != 'b' ||
      tolower((unsigned char)value[1]) != 'l' ||
      tolower((unsigned char)value[2]) != 'u' ||
      tolower((unsigned char)value[3]) != 'r' || value[4] != '(' ||
      value[length - 1] != ')') return 0.0;
  const char *argument = value + 5;
  size_t argument_length = length - 6;
  while (argument_length && isspace((unsigned char)*argument)) {
    argument++;
    argument_length--;
  }
  while (argument_length && isspace((unsigned char)argument[argument_length - 1]))
    argument_length--;
  if (!argument_length) return 0.0;
  bool pixels = argument_length >= 2 &&
      tolower((unsigned char)argument[argument_length - 2]) == 'p' &&
      tolower((unsigned char)argument[argument_length - 1]) == 'x';
  if (pixels) {
    argument_length -= 2;
    while (argument_length &&
           isspace((unsigned char)argument[argument_length - 1]))
      argument_length--;
  }
  char *copy = tai_strndup(argument, argument_length);
  if (!copy) return NAN;
  if (!pixels && strcmp(copy, "0") && strcmp(copy, "+0") &&
      strcmp(copy, "-0") && strcmp(copy, "0.0") &&
      strcmp(copy, "+0.0") && strcmp(copy, "-0.0")) {
    free(copy);
    return 0.0;
  }
  char *end = NULL;
  double sigma = strtod(copy, &end);
  bool valid = end != copy && !*end;
  free(copy);
  /* The Python parser returns +infinity, but its display-list JSON then fails
   * and an unbounded kernel has no finite allocation. Keep this boundary
   * deterministic and JSON-safe by treating every non-finite value as none. */
  if (!valid || !isfinite(sigma) || sigma <= 0.0) return 0.0;
  return sigma;
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

static bool make_image(Build *build, const TaiLayoutItem *item) {
  if (!item->image_pixels || item->image_width <= 0 ||
      item->image_height <= 0 || item->image_width > INT_MAX / 4 ||
      item->image_stride < item->image_width * 4)
    return true;
  size_t stride = (size_t)item->image_width * 4;
  if ((size_t)item->image_height > SIZE_MAX / stride) {
    build->failed = true;
    return false;
  }
  size_t byte_count = (size_t)item->image_height * stride;
  unsigned char *pixels = malloc(byte_count);
  if (!pixels) {
    build->failed = true;
    return false;
  }
  for (int y = 0; y < item->image_height; y++)
    memcpy(pixels + (size_t)y * stride,
           item->image_pixels + (size_t)y * (size_t)item->image_stride,
           stride);
  TaiDisplayCommand command = {
      .kind = TAI_DRAW_IMAGE,
      .x = item->x, .y = item->y,
      .width = item->width, .height = item->height,
      .image_pixels = pixels,
      .image_width = item->image_width,
      .image_height = item->image_height,
      .image_stride = (int)stride,
  };
  if (!append(build, command)) {
    free(pixels);
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
    ClipStart *pending_clip = NULL;
    if (is_clip_container(item)) {
      if (!build->clip_count) {
        build->failed = true;
        return false;
      }
      pending_clip = &build->clip_starts[build->clip_count - 1];
      if (pending_clip->scroll) {
        if (build->count == pending_clip->scroll_start + 1)
          build->count--;
        else if (!append(build, (TaiDisplayCommand){
                                   .kind = TAI_POP_CLIP_SCROLL}))
          return false;
      }
    }
    if (!build->blur_count) {
      build->failed = true;
      return false;
    }
    BlurStart blur = build->blur_starts[--build->blur_count];
    if (blur.active) {
      if (build->count == blur.start + 1)
        build->count = blur.start;
      else if (!append(build, (TaiDisplayCommand){.kind = TAI_POP_BLUR}))
        return false;
    }
    if (is_clip_container(item)) {
      ClipStart start = build->clip_starts[--build->clip_count];
      if (build->count == start.outer_start + 1) {
        build->count = start.outer_start;
      } else if (build->count == start.outer_start + 2 &&
                 build->items[start.outer_start + 1].kind ==
                     TAI_DRAW_HIT_TEST) {
        build->items[start.outer_start] = build->items[start.outer_start + 1];
        build->count = start.outer_start + 1;
      } else if (!append(build, (TaiDisplayCommand){.kind = TAI_POP_CLIP})) {
        return false;
      }
    }
    if (!build->blend_count) {
      build->failed = true;
      return false;
    }
    BlendStart blend = build->blend_starts[--build->blend_count];
    if (!blend.active) return true;
    if (build->count == blend.start + 1) {
      build->count = blend.start;
      return true;
    }
    return append(build, (TaiDisplayCommand){.kind = TAI_POP_BLEND});
  }
  BlendStart blend = {.start = build->count};
  if (item->node && item->node->kind == TAI_ELEMENT &&
      (!item->node->tag || strcmp(item->node->tag, "button-content"))) {
    bool special_blend = false;
    TaiBlendMode mode = effect_blend_mode(item->node, &special_blend);
    double opacity = effect_opacity(item->node);
    if (isnan(opacity)) {
      build->failed = true;
      return false;
    }
    blend.active = special_blend || opacity < 1.0;
    if (blend.active &&
        !append(build, (TaiDisplayCommand){
                           .kind = TAI_PUSH_BLEND,
                           .opacity = opacity,
                           .blend_mode = mode,
                       }))
      return false;
  }
  if (!remember_blend(build, blend)) return false;
  ClipStart clip = {0};
  bool clipped = is_clip_container(item);
  if (clipped) {
    clip = (ClipStart){
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
  }
  BlurStart blur = {.start = build->count};
  if (item->node && item->node->kind == TAI_ELEMENT &&
      (!item->node->tag || strcmp(item->node->tag, "button-content"))) {
    double sigma = effect_blur_sigma(item->node);
    if (isnan(sigma)) {
      build->failed = true;
      return false;
    }
    blur.active = sigma > 0.0;
    if (blur.active &&
        !append(build, (TaiDisplayCommand){
                           .kind = TAI_PUSH_BLUR,
                           .sigma = sigma,
                       }))
      return false;
  }
  if (!remember_blur(build, blur)) return false;
  if (clipped) {
    if (clip.scroll) {
      if (item->kind == TAI_LAYOUT_BLOCK && !make_fill(build, item)) return false;
      if (!append(build, (TaiDisplayCommand){
                           .kind = TAI_DRAW_HIT_TEST,
                           .x = item->x, .y = item->y,
                           .width = item->width, .height = item->height,
                           .hit_radius = effect_radius(item->node),
                           .node_id = item->node->id,
                       }))
        return false;
      clip.scroll_start = build->count;
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
    return remember_clip(build, clip);
  }
  if (item->kind == TAI_LAYOUT_BLOCK && !make_fill(build, item)) return false;
  if (item->kind == TAI_LAYOUT_TEXT && !make_text(build, item)) return false;
  if (item->kind == TAI_LAYOUT_IMAGE && !make_image(build, item)) return false;
  return !build->failed;
}

static void release_commands(TaiDisplayCommand *items, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free((char *)items[i].text);
    free((char *)items[i].font_family);
    free((unsigned char *)items[i].image_pixels);
  }
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
  if (!tai_layout_walk(layout, collect, &build, error) || build.failed ||
      build.clip_count || build.blend_count || build.blur_count) {
    release_commands(build.items, build.count);
    free(build.items);
    free(build.clip_starts);
    free(build.blend_starts);
    free(build.blur_starts);
    if (error && !*error) *error = tai_strdup("display list allocation failed");
    return NULL;
  }
  TaiDisplayList *list = calloc(1, sizeof(*list));
  free(build.clip_starts);
  free(build.blend_starts);
  free(build.blur_starts);
  if (!list) {
    release_commands(build.items, build.count);
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
  release_commands(list->items, list->count);
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

static bool is_effect_pop(TaiDrawKind kind) {
  return kind == TAI_POP_CLIP || kind == TAI_POP_CLIP_SCROLL ||
         kind == TAI_POP_BLEND || kind == TAI_POP_BLUR;
}

static bool is_effect_push(TaiDrawKind kind) {
  return kind == TAI_PUSH_CLIP || kind == TAI_PUSH_CLIP_SCROLL ||
         kind == TAI_PUSH_BLEND || kind == TAI_PUSH_BLUR;
}

static bool effect_pair_matches(TaiDrawKind push, TaiDrawKind pop) {
  return (push == TAI_PUSH_CLIP && pop == TAI_POP_CLIP) ||
         (push == TAI_PUSH_CLIP_SCROLL && pop == TAI_POP_CLIP_SCROLL) ||
         (push == TAI_PUSH_BLEND && pop == TAI_POP_BLEND) ||
         (push == TAI_PUSH_BLUR && pop == TAI_POP_BLUR);
}

static bool hit_range(const TaiDisplayList *list, size_t begin, size_t end,
                      double x, double y, TaiDisplayHit *hit) {
  size_t index = end;
  while (index > begin) {
    const TaiDisplayCommand *command = &list->items[--index];
    if (is_effect_pop(command->kind)) {
      size_t depth = 1;
      size_t push = index;
      while (push > begin && depth) {
        const TaiDisplayCommand *candidate = &list->items[--push];
        if (is_effect_pop(candidate->kind))
          depth++;
        else if (is_effect_push(candidate->kind))
          depth--;
      }
      if (depth) return false;
      const TaiDisplayCommand *clip = &list->items[push];
      if (!effect_pair_matches(clip->kind, command->kind)) return false;
      if ((clip->kind == TAI_PUSH_CLIP ||
           clip->kind == TAI_PUSH_BLEND || clip->kind == TAI_PUSH_BLUR ||
           contains(clip, x, y)) &&
          hit_range(list, push + 1, index, x,
                    y + (clip->kind == TAI_PUSH_CLIP_SCROLL
                             ? clip->scroll_y : 0.0), hit))
        return true;
      index = push;
      continue;
    }
    if (is_effect_push(command->kind))
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
                         command->kind == TAI_DRAW_IMAGE ? "image" :
                         command->kind == TAI_DRAW_FILL_RECT ? "fill_rect" :
                         command->kind == TAI_DRAW_HIT_TEST ? "hit_test" :
                         command->kind == TAI_PUSH_CLIP ? "push_clip" :
                         command->kind == TAI_PUSH_CLIP_SCROLL ?
                             "push_clip_scroll" :
                         command->kind == TAI_PUSH_BLEND ? "push_blend" :
                         command->kind == TAI_PUSH_BLUR ? "push_blur" :
                         command->kind == TAI_POP_CLIP ? "pop_clip" :
                         command->kind == TAI_POP_CLIP_SCROLL ?
                             "pop_clip_scroll" :
                         command->kind == TAI_POP_BLEND ? "pop_blend" :
                             "pop_blur";
      fprintf(out, "{\"kind\":\"%s\"", kind);
      if (command->kind == TAI_POP_CLIP ||
          command->kind == TAI_POP_CLIP_SCROLL ||
          command->kind == TAI_POP_BLEND ||
          command->kind == TAI_POP_BLUR) {
        fputc('}', out);
        continue;
      }
      fprintf(out, ",\"x\":%.17g,\"y\":%.17g"
                   ",\"width\":%.17g,\"height\":%.17g,\"rgba\":%u",
              command->x, command->y, command->width, command->height,
              command->rgba);
      if (command->kind == TAI_PUSH_CLIP_SCROLL)
        fprintf(out, ",\"scroll_y\":%.17g", command->scroll_y);
      if (command->kind == TAI_PUSH_BLEND) {
        static const char *const modes[] = {
            "source-over", "multiply", "difference", "destination-in"};
        fprintf(out, ",\"opacity\":%.17g,\"blend_mode\":\"%s\"",
                command->opacity, modes[command->blend_mode]);
      }
      if (command->kind == TAI_PUSH_BLUR)
        fprintf(out, ",\"sigma\":%.17g", command->sigma);
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
      if (command->kind == TAI_DRAW_IMAGE)
        fprintf(out, ",\"image\":{\"width\":%.17g,\"height\":%.17g}",
                command->width, command->height);
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

static bool blur_group_surface(cairo_pattern_t *pattern, double sigma) {
  cairo_surface_t *surface = NULL;
  if (!pattern || !isfinite(sigma) || sigma <= 0.0 ||
      cairo_pattern_get_surface(pattern, &surface) != CAIRO_STATUS_SUCCESS ||
      !surface || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE)
    return false;
  int width = cairo_image_surface_get_width(surface);
  int height = cairo_image_surface_get_height(surface);
  int stride = cairo_image_surface_get_stride(surface);
  if (width <= 0 || height <= 0 || stride <= 0 ||
      (size_t)height > SIZE_MAX / (size_t)stride ||
      sigma > (double)(INT_MAX / 6))
    return false;
  int radius = (int)ceil(3.0 * sigma);
  if (radius <= 0 || radius > (INT_MAX - 1) / 2) return false;
  size_t kernel_count = (size_t)radius * 2 + 1;
  if (kernel_count > SIZE_MAX / sizeof(double)) return false;
  /* Exact separable convolution is intentionally bounded: CSS is untrusted,
   * and an otherwise small allocation with a huge radius can monopolize the
   * raster thread. Larger sigmas remain represented in the display list but
   * fail rasterization instead of silently using a different filter. */
  const size_t max_channel_taps = 25000000;
  size_t pixel_count = (size_t)width * (size_t)height;
  if (pixel_count > SIZE_MAX / 8 ||
      pixel_count * 8 > max_channel_taps / kernel_count)
    return false;
  size_t byte_count = (size_t)height * (size_t)stride;
  double *kernel = malloc(kernel_count * sizeof(*kernel));
  unsigned char *horizontal = malloc(byte_count);
  unsigned char *output = malloc(byte_count);
  if (!kernel || !horizontal || !output) {
    free(kernel);
    free(horizontal);
    free(output);
    return false;
  }
  double sum = 0.0;
  for (int offset = -radius; offset <= radius; offset++) {
    double value = exp(-((double)offset * (double)offset) /
                       (2.0 * sigma * sigma));
    kernel[(size_t)(offset + radius)] = value;
    sum += value;
  }
  for (size_t index = 0; index < kernel_count; index++) kernel[index] /= sum;

  cairo_surface_flush(surface);
  unsigned char *pixels = cairo_image_surface_get_data(surface);
  memset(horizontal, 0, byte_count);
  memset(output, 0, byte_count);
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      for (int channel = 0; channel < 4; channel++) {
        double value = 0.0;
        for (int offset = -radius; offset <= radius; offset++) {
          int source_x = x + offset;
          if (source_x >= 0 && source_x < width)
            value += pixels[(size_t)y * (size_t)stride +
                            (size_t)source_x * 4 + (size_t)channel] *
                     kernel[(size_t)(offset + radius)];
        }
        horizontal[(size_t)y * (size_t)stride + (size_t)x * 4 +
                   (size_t)channel] = (unsigned char)fmin(255.0,
                                                        floor(value + 0.5));
      }
    }
  }
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      for (int channel = 0; channel < 4; channel++) {
        double value = 0.0;
        for (int offset = -radius; offset <= radius; offset++) {
          int source_y = y + offset;
          if (source_y >= 0 && source_y < height)
            value += horizontal[(size_t)source_y * (size_t)stride +
                                (size_t)x * 4 + (size_t)channel] *
                     kernel[(size_t)(offset + radius)];
        }
        output[(size_t)y * (size_t)stride + (size_t)x * 4 +
               (size_t)channel] = (unsigned char)fmin(255.0,
                                                    floor(value + 0.5));
      }
    }
  }
  memcpy(pixels, output, byte_count);
  cairo_surface_mark_dirty(surface);
  free(kernel);
  free(horizontal);
  free(output);
  return true;
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
  const TaiDisplayCommand **effect_stack =
      list->count ? calloc(list->count, sizeof(*effect_stack)) : NULL;
  if (list->count && !effect_stack) {
    cairo_destroy(context);
    cairo_surface_destroy(surface);
    set_error(error, "PNG clip stack allocation failed");
    return false;
  }
  size_t effect_depth = 0;
  bool valid = true;
  bool blur_failed = false;
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
      effect_stack[effect_depth++] = command;
    } else if (command->kind == TAI_PUSH_CLIP_SCROLL) {
      cairo_save(context);
      cairo_rectangle(context, command->x, command->y,
                      command->width, command->height);
      cairo_clip(context);
      cairo_translate(context, 0, -command->scroll_y);
      effect_stack[effect_depth++] = command;
    } else if (command->kind == TAI_PUSH_BLEND) {
      cairo_save(context);
      cairo_push_group(context);
      effect_stack[effect_depth++] = command;
    } else if (command->kind == TAI_PUSH_BLUR) {
      cairo_save(context);
      cairo_push_group(context);
      effect_stack[effect_depth++] = command;
    } else if (command->kind == TAI_POP_CLIP ||
               command->kind == TAI_POP_CLIP_SCROLL ||
               command->kind == TAI_POP_BLEND ||
               command->kind == TAI_POP_BLUR) {
      if (!effect_depth) {
        valid = false;
        break;
      }
      const TaiDisplayCommand *clip = effect_stack[--effect_depth];
      if (!effect_pair_matches(clip->kind, command->kind)) {
        valid = false;
        break;
      }
      if (clip->kind == TAI_PUSH_CLIP) {
        cairo_pattern_t *group = cairo_pop_group(context);
        cairo_set_antialias(context, CAIRO_ANTIALIAS_NONE);
        cairo_rounded_rectangle(context, clip);
        cairo_clip(context);
        cairo_set_source(context, group);
        cairo_paint(context);
        cairo_pattern_destroy(group);
      } else if (clip->kind == TAI_PUSH_BLEND) {
        cairo_pattern_t *group = cairo_pop_group(context);
        cairo_operator_t operation = CAIRO_OPERATOR_OVER;
        if (clip->blend_mode == TAI_BLEND_MULTIPLY)
          operation = CAIRO_OPERATOR_MULTIPLY;
        else if (clip->blend_mode == TAI_BLEND_DIFFERENCE)
          operation = CAIRO_OPERATOR_DIFFERENCE;
        else if (clip->blend_mode == TAI_BLEND_DESTINATION_IN)
          operation = CAIRO_OPERATOR_DEST_IN;
        cairo_set_operator(context, operation);
        cairo_set_source(context, group);
        cairo_paint_with_alpha(context, clip->opacity);
        cairo_pattern_destroy(group);
      } else if (clip->kind == TAI_PUSH_BLUR) {
        cairo_pattern_t *group = cairo_pop_group(context);
        if (!blur_group_surface(group, clip->sigma)) {
          blur_failed = true;
          valid = false;
        } else {
          cairo_set_source(context, group);
          cairo_paint(context);
        }
        cairo_pattern_destroy(group);
      }
      cairo_restore(context);
    } else if (command->kind == TAI_DRAW_HIT_TEST) {
      continue;
    } else if (command->kind == TAI_DRAW_FILL_RECT) {
      cairo_rounded_rectangle(context, command);
      cairo_fill(context);
    } else if (command->kind == TAI_DRAW_IMAGE) {
      cairo_surface_t *image = cairo_image_surface_create_for_data(
          (unsigned char *)command->image_pixels, CAIRO_FORMAT_ARGB32,
          command->image_width, command->image_height, command->image_stride);
      if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(image);
        valid = false;
        break;
      }
      cairo_save(context);
      cairo_translate(context, command->x, command->y);
      cairo_scale(context, command->width / command->image_width,
                  command->height / command->image_height);
      cairo_set_source_surface(context, image, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(context), CAIRO_FILTER_BILINEAR);
      cairo_rectangle(context, 0, 0, command->image_width,
                      command->image_height);
      cairo_fill(context);
      cairo_restore(context);
      cairo_surface_destroy(image);
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
  if (effect_depth) valid = false;
  cairo_status_t status = cairo_status(context);
  if (valid && status == CAIRO_STATUS_SUCCESS)
    status = cairo_surface_write_to_png(surface, path);
  cairo_destroy(context);
  cairo_surface_destroy(surface);
  free(effect_stack);
  if (!valid) {
    set_error(error, blur_failed ? "blur allocation or surface failure" :
                                  "invalid display list effect pairing");
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
