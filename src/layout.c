#define _POSIX_C_SOURCE 200809L
#include "tai/layout.h"
#include <cairo/cairo.h>
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <utf8proc.h>

typedef struct Box Box;
typedef struct Image Image;
struct Image {
  utf8proc_int32_t codepoint;
  unsigned char *pixels;
  int width, height, stride;
};
struct Box {
  const char *kind;
  TaiNode *node;
  Box **children;
  size_t count;
  double x, y, width, height, ascent, descent, space, font_size, caret_x;
  double content_height, scroll_y;
  bool bold, italic, scrollable;
  TaiControlKind control;
  char *word;
  Image *image;
};
struct TaiLayout {
  Box *root;
  FT_Library ft;
  FcConfig *fc;
  bool rtl, failed;
  Image **images;
  size_t image_count, image_capacity;
};
static const char *property(TaiNode *n, const char *key, const char *fallback) {
  const char *v = tai_map_get(&n->style, key);
  return v ? v : fallback;
}
static bool tag(TaiNode *n, const char *s) {
  return n->kind == TAI_ELEMENT && !strcmp(n->tag, s);
}
static bool ascii_case_equal(const char *left, const char *right) {
  if (!left || !right) return false;
  while (*left && *right) {
    unsigned char a = (unsigned char)*left++, b = (unsigned char)*right++;
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return !*left && !*right;
}
static const char *input_type(const TaiNode *node) {
  const char *value = tai_map_get(&node->attributes, "type");
  return value ? value : "text";
}
static bool hidden_input(TaiNode *node) {
  return tag(node, "input") && ascii_case_equal(input_type(node), "hidden");
}
static bool checkbox_input(TaiNode *node) {
  /* This is deliberately exact: the frozen reference does not casefold the
   * checkbox value even though hidden/password use case-insensitive values. */
  return tag(node, "input") && !strcmp(input_type(node), "checkbox");
}
static bool block(TaiNode *n) {
  return n->kind == TAI_ELEMENT &&
         !strcmp(property(n, "display", "inline"), "block");
}
static Box *box(TaiLayout *l, const char *kind, TaiNode *n) {
  Box *b = calloc(1, sizeof(*b));
  if (!b)
    l->failed = true;
  else {
    b->kind = kind;
    b->node = n;
  }
  return b;
}
static bool append(TaiLayout *l, Box *p, Box *b) {
  if (!b)
    return false;
  Box **v = realloc(p->children, (p->count + 1) * sizeof(*v));
  if (!v) {
    l->failed = true;
    return false;
  }
  p->children = v;
  p->children[p->count++] = b;
  return true;
}
static void release(Box *b) {
  if (!b)
    return;
  for (size_t i = 0; i < b->count; i++)
    release(b->children[i]);
  free(b->children);
  free(b->word);
  free(b);
}
static double px(const char *s, double fallback) {
  char *end;
  double x = strtod(s, &end);
  return end != s && isfinite(x) && !strcmp(end, "px") && floor(x) == x
             ? x
             : fallback;
}
static bool fixed_px(const char *s, double *value) {
  char *end;
  double parsed = strtod(s, &end);
  if (end == s || !isfinite(parsed) || strcmp(end, "px") ||
      floor(parsed) != parsed)
    return false;
  *value = parsed;
  return true;
}
static FT_Face face(TaiLayout *l, TaiNode *node, bool pre, double *size) {
  *size = fmax(1, nearbyint(strtod(property(node, "font-size", "16px"), NULL)));
  if (!isfinite(*size) || *size > 1000000) {
    l->failed = true;
    return NULL;
  }
  const char *family =
      pre ? "Courier New" : property(node, "font-family", "Times");
  char *name = tai_strndup(family, strcspn(family, ","));
  if (!name) {
    l->failed = true;
    return NULL;
  }
  size_t len = strlen(name);
  while (len && name[len - 1] == ' ')
    name[--len] = 0;
  char *start = name;
  while (*start == ' ')
    start++;
  if ((*start == '\'' || *start == '"') && len > 1) {
    start++;
    name[len - 1] = 0;
  }
  if (!strcmp(start, "serif"))
    start = "Times New Roman";
  else if (!strcmp(start, "sans-serif"))
    start = "Arial";
  else if (!strcmp(start, "monospace"))
    start = "Courier New";
  FcPattern *p = FcPatternCreate();
  if (!p) {
    free(name);
    l->failed = true;
    return NULL;
  }
  FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)start);
  const char *weight = property(node, "font-weight", "normal"),
             *slant = property(node, "font-style", "normal");
  FcPatternAddInteger(p, FC_WEIGHT,
                      !strcmp(weight, "bold") || atoi(weight) >= 600
                          ? FC_WEIGHT_BOLD
                          : FC_WEIGHT_REGULAR);
  FcPatternAddInteger(p, FC_SLANT,
                      !strcmp(slant, "italic") || !strcmp(slant, "oblique")
                          ? FC_SLANT_ITALIC
                          : FC_SLANT_ROMAN);
  FcConfigSubstitute(l->fc, p, FcMatchPattern);
  FcDefaultSubstitute(p);
  FcResult result;
  FcPattern *m = FcFontMatch(l->fc, p, &result);
  FcPatternDestroy(p);
  free(name);
  FT_Face f = NULL;
  FcChar8 *file;
  int index = 0;
  if (m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
    FcPatternGetInteger(m, FC_INDEX, 0, &index);
    if (FT_New_Face(l->ft, (const char *)file, index, &f))
      f = NULL;
  }
  if (m)
    FcPatternDestroy(m);
  if (!f) {
    l->failed = true;
    return NULL;
  }
  if (FT_Set_Char_Size(f, 0, (FT_F26Dot6)(*size * 64), 72, 72)) {
    FT_Done_Face(f);
    l->failed = true;
    return NULL;
  }
  return f;
}
static double measure_n(FT_Face f, const char *text, size_t length) {
  double w = 0;
  const unsigned char *p = (const unsigned char *)text;
  const unsigned char *end = p + length;
  while (p < end) {
    utf8proc_int32_t c;
    utf8proc_ssize_t n = utf8proc_iterate(p, (utf8proc_ssize_t)(end - p), &c);
    if (n < 1 || p + n > end) {
      p++;
      continue;
    }
    if (!FT_Load_Char(f, (FT_ULong)c, FT_LOAD_DEFAULT))
      w += (double)f->glyph->advance.x / 64;
    p += (size_t)n;
  }
  return w;
}
static double measure(FT_Face f, const char *text) {
  return measure_n(f, text, strlen(text));
}
static bool whitespace(utf8proc_int32_t c) {
  utf8proc_category_t t = utf8proc_category(c);
  return (c >= 9 && c <= 13) || (c >= 28 && c <= 32) || c == 0x85 ||
         t == UTF8PROC_CATEGORY_ZS || t == UTF8PROC_CATEGORY_ZL ||
         t == UTF8PROC_CATEGORY_ZP;
}
static uint32_t png_u32(const unsigned char *bytes) {
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}
static double round_ties_even(double value) {
  double lower = floor(value);
  double fraction = value - lower;
  if (fraction < 0.5) return lower;
  if (fraction > 0.5) return lower + 1.0;
  return fmod(lower, 2.0) == 0.0 ? lower : lower + 1.0;
}

static bool bounded_png(FILE *file) {
  struct stat info;
  if (fstat(fileno(file), &info) != 0 || info.st_size < 24 ||
      info.st_size > 64 * 1024 * 1024)
    return false;
  unsigned char header[24];
  rewind(file);
  bool read = fread(header, 1, sizeof(header), file) == sizeof(header);
  static const unsigned char signature[8] = {
      137, 80, 78, 71, 13, 10, 26, 10};
  if (!read || memcmp(header, signature, sizeof(signature)) ||
      memcmp(header + 12, "IHDR", 4))
    return false;
  uint32_t width = png_u32(header + 16);
  uint32_t height = png_u32(header + 20);
  const uint32_t max_dimension = 16384;
  const uint32_t max_pixels = 25000000;
  return width && height && width <= max_dimension && height <= max_dimension &&
         width <= max_pixels / height;
}

typedef struct {
  FILE *file;
  size_t remaining;
} PngReader;

static cairo_status_t read_png(void *closure, unsigned char *data,
                               unsigned int length) {
  PngReader *reader = closure;
  if ((size_t)length > reader->remaining) return CAIRO_STATUS_READ_ERROR;
  size_t count = fread(data, 1, length, reader->file);
  reader->remaining -= count;
  return count == length ? CAIRO_STATUS_SUCCESS : CAIRO_STATUS_READ_ERROR;
}

static Image *emoji(TaiLayout *l, const char *text, size_t length) {
  utf8proc_int32_t codepoint;
  utf8proc_ssize_t bytes = utf8proc_iterate(
      (const unsigned char *)text, (utf8proc_ssize_t)length, &codepoint);
  if (bytes < 1 || (size_t)bytes != length) return NULL;
  for (size_t i = 0; i < l->image_count; i++)
    if (l->images[i]->codepoint == codepoint) return l->images[i];

  char path[64];
  const char *suffixes[] = {"_color.png", ".png"};
  cairo_surface_t *surface = NULL;
  for (size_t i = 0; i < 2; i++) {
    int written = snprintf(path, sizeof(path), "openmoji/%X%s",
                           (unsigned int)codepoint, suffixes[i]);
    if (written < 0 || (size_t)written >= sizeof(path)) return NULL;
    FILE *file = fopen(path, "rb");
    if (!file) {
      if (errno == ENOENT) continue;
      return NULL;
    }
    if (!bounded_png(file)) {
      fclose(file);
      return NULL;
    }
    rewind(file);
    PngReader reader = {file, 64 * 1024 * 1024};
    surface = cairo_image_surface_create_from_png_stream(read_png, &reader);
    fclose(file);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(surface);
      return NULL;
    }
    break;
  }
  if (!surface) return NULL;
  int source_width = cairo_image_surface_get_width(surface);
  int source_height = cairo_image_surface_get_height(surface);
  int source_stride = cairo_image_surface_get_stride(surface);
  if (source_width <= 0 || source_height <= 0 || source_stride <= 0 ||
      source_width > INT_MAX / 4 || source_stride < source_width * 4) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  double scaled = (double)source_height * 22.0 / (double)source_width;
  double rounded = round_ties_even(scaled);
  if (!isfinite(rounded) || rounded > INT_MAX) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  size_t stride = (size_t)source_width * 4;
  if ((size_t)source_height > SIZE_MAX / stride) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  unsigned char *pixels = malloc((size_t)source_height * stride);
  if (!pixels) {
    cairo_surface_destroy(surface);
    l->failed = true;
    return NULL;
  }
  cairo_surface_flush(surface);
  const unsigned char *source = cairo_image_surface_get_data(surface);
  for (int y = 0; y < source_height; y++)
    memcpy(pixels + (size_t)y * stride,
           source + (size_t)y * (size_t)source_stride, stride);
  cairo_surface_destroy(surface);
  if (l->image_count == l->image_capacity) {
    size_t capacity = l->image_capacity ? l->image_capacity * 2 : 4;
    if (capacity < l->image_count + 1 ||
        capacity > SIZE_MAX / sizeof(*l->images)) {
      free(pixels);
      l->failed = true;
      return NULL;
    }
    Image **next = realloc(l->images, capacity * sizeof(*next));
    if (!next) {
      free(pixels);
      l->failed = true;
      return NULL;
    }
    l->images = next;
    l->image_capacity = capacity;
  }
  Image *image = malloc(sizeof(*image));
  if (!image) {
    free(pixels);
    l->failed = true;
    return NULL;
  }
  *image = (Image){codepoint, pixels, source_width, source_height, (int)stride};
  l->images[l->image_count++] = image;
  return image;
}
typedef struct {
  TaiLayout *owner;
  Box *block, *line;
  double cursor;
  bool pre;
} Inline;
static bool newline(Inline *in) {
  Box *line = box(in->owner, "LineLayout", in->block->node);
  if (!append(in->owner, in->block, line)) {
    release(line);
    return false;
  }
  in->line = line;
  in->cursor = 0;
  return true;
}
static void word(Inline *in, TaiNode *node, const char *s, size_t n) {
  TaiLayout *l = in->owner;
  Image *image = in->pre ? NULL : emoji(l, s, n);
  Box *b = box(l, image ? "EmojiLayout" : "TextLayout", node);
  if (!b)
    return;
  b->word = tai_strndup(s, n);
  if (!b->word) {
    release(b);
    l->failed = true;
    return;
  }
  /* Python strips U+00AD only outside pre. */
  if (!in->pre) {
    char *r = b->word, *w = r;
    while (*r) {
      if ((unsigned char)r[0] == 0xc2 && (unsigned char)r[1] == 0xad)
        r += 2;
      else
        *w++ = *r++;
    }
    *w = 0;
  }
  double size;
  FT_Face f = face(l, node, in->pre, &size);
  if (!f) {
    release(b);
    return;
  }
  b->width = image ? 22.0 : measure(f, b->word);
  b->font_size = size;
  b->bold = !strcmp(property(node, "font-weight", "normal"), "bold") ||
            atoi(property(node, "font-weight", "normal")) >= 600;
  b->italic = !strcmp(property(node, "font-style", "normal"), "italic") ||
              !strcmp(property(node, "font-style", "normal"), "oblique");
  b->image = image;
  double image_height = image ? fmax(1.0, round_ties_even(
      (double)image->height * 22.0 / (double)image->width)) : 0.0;
  b->ascent = image ? image_height :
      (double)f->ascender * size / f->units_per_EM;
  b->descent = image ? 0.0 :
      -(double)f->descender * size / f->units_per_EM;
  b->height = b->ascent + b->descent;
  b->space = in->pre ? 0 : measure(f, " ");
  FT_Done_Face(f);
  if (!in->pre && in->cursor + b->width > in->block->width && in->line->count &&
      !newline(in)) {
    release(b);
    return;
  }
  if (!append(l, in->line, b)) {
    release(b);
    return;
  }
  in->cursor += b->width + b->space;
}

static bool append_text(char **text, size_t *length, const char *part) {
  size_t count = strlen(part);
  if (*length > SIZE_MAX - count - 1) return false;
  char *next = realloc(*text, *length + count + 1);
  if (!next) return false;
  memcpy(next + *length, part, count + 1);
  *text = next;
  *length += count;
  return true;
}

static bool node_text(const TaiNode *node, char **text, size_t *length) {
  if (node->kind == TAI_TEXT) return append_text(text, length, node->text);
  for (size_t i = 0; i < node->child_count; i++)
    if (!node_text(node->children[i], text, length)) return false;
  return true;
}

static char *password_display(const char *value) {
  size_t count = 0;
  for (const unsigned char *p = (const unsigned char *)value; *p;) {
    utf8proc_int32_t codepoint;
    utf8proc_ssize_t used = utf8proc_iterate(p, -1, &codepoint);
    p += used > 0 ? (size_t)used : 1;
    if (count == SIZE_MAX) return NULL;
    count++;
  }
  if (count > (SIZE_MAX - 1) / 1) return NULL;
  char *display = malloc(count + 1);
  if (!display) return NULL;
  memset(display, '*', count);
  display[count] = '\0';
  return display;
}

static void inline_control(Inline *in, TaiNode *node) {
  TaiLayout *layout = in->owner;
  bool checkbox = checkbox_input(node), button = tag(node, "button");
  Box *box_control = box(layout, button ? "ButtonLayout" : "InputLayout", node);
  if (!box_control) return;
  box_control->control = button ? TAI_CONTROL_BUTTON :
      checkbox ? TAI_CONTROL_CHECKBOX :
      ascii_case_equal(input_type(node), "password") ? TAI_CONTROL_PASSWORD :
      TAI_CONTROL_TEXT;

  double size;
  FT_Face f = face(layout, node, false, &size);
  if (!f) { release(box_control); return; }
  box_control->font_size = size;
  box_control->bold = !strcmp(property(node, "font-weight", "normal"), "bold") ||
                      atoi(property(node, "font-weight", "normal")) >= 600;
  box_control->italic = !strcmp(property(node, "font-style", "normal"), "italic") ||
                        !strcmp(property(node, "font-style", "normal"), "oblique");
  box_control->space = measure(f, " ");
  if (checkbox) {
    box_control->width = 13.0;
    box_control->height = 13.0;
    box_control->ascent = 13.0;
    box_control->descent = 0.0;
  } else {
    double css_width;
    box_control->width = fixed_px(property(node, "width", "auto"),
                                   &css_width) && css_width > 0.0
                             ? css_width : 200.0;
    box_control->ascent = (double)f->ascender * size / f->units_per_EM;
    box_control->descent = -(double)f->descender * size / f->units_per_EM;
    box_control->height = box_control->ascent + box_control->descent;
    if (button) {
      char *contents = NULL;
      size_t length = 0;
      if (!node_text(node, &contents, &length)) {
        free(contents);
        FT_Done_Face(f);
        release(box_control);
        layout->failed = true;
        return;
      }
      box_control->word = contents ? contents : tai_strdup("");
      if (!box_control->word) layout->failed = true;
      box_control->height += 8.0;
      box_control->ascent = box_control->height;
      box_control->descent = 0.0;
    } else {
      const char *value = tai_map_get(&node->attributes, "value");
      box_control->word = box_control->control == TAI_CONTROL_PASSWORD
                              ? password_display(value ? value : "")
                              : tai_strdup(value ? value : "");
      if (!box_control->word) layout->failed = true;
      if (box_control->word) {
        size_t byte_index = 0, codepoints = 0;
        size_t length = strlen(box_control->word);
        while (byte_index < length && codepoints < node->cursor_index) {
          utf8proc_int32_t codepoint;
          utf8proc_ssize_t used = utf8proc_iterate(
              (const utf8proc_uint8_t *)box_control->word + byte_index,
              (utf8proc_ssize_t)(length - byte_index), &codepoint);
          byte_index += used > 0 ? (size_t)used : 1;
          codepoints++;
        }
        box_control->caret_x = measure_n(f, box_control->word, byte_index);
      }
    }
  }
  FT_Done_Face(f);
  if (layout->failed) { release(box_control); return; }
  if (in->cursor + box_control->width > in->block->width &&
      in->line->count && !newline(in)) {
    release(box_control);
    return;
  }
  if (!append(layout, in->line, box_control)) {
    release(box_control);
    return;
  }
  in->cursor += box_control->width + box_control->space;
}

static void recurse(Inline *in, TaiNode *n) {
  if (in->owner->failed)
    return;
  if (n->kind == TAI_TEXT) {
    const char *p = n->text;
    if (in->pre) {
      const char *start = p;
      for (;; p++) {
        if (*p == '\n' || !*p) {
          if (p > start || *p == '\n')
            word(in, n, start, (size_t)(p - start));
          if (!*p)
            break;
          if (!newline(in))
            return;
          start = p + 1;
        }
      }
    } else {
      const char *start = NULL;
      while (*p) {
        utf8proc_int32_t c;
        utf8proc_ssize_t len =
            utf8proc_iterate((const unsigned char *)p, -1, &c);
        if (len < 1) {
          len = 1;
          c = 0xfffd;
        }
        if (whitespace(c)) {
          if (start) {
            word(in, n, start, (size_t)(p - start));
            start = NULL;
          }
        } else if (!start)
          start = p;
        p += len;
      }
      if (start)
        word(in, n, start, (size_t)(p - start));
    }
    return;
  }
  if (tag(n, "script") || tag(n, "style") || tag(n, "head"))
    return;
  if (hidden_input(n)) return;
  if (tag(n, "input") || tag(n, "button")) {
    inline_control(in, n);
    return;
  }
  if (tag(n, "br")) {
    newline(in);
    return;
  }
  bool pre = tag(n, "pre"), paragraph = tag(n, "p");
  if (pre)
    in->pre = true;
  if ((pre || paragraph) && in->line->count)
    newline(in);
  for (size_t i = 0; i < n->child_count; i++)
    recurse(in, n->children[i]);
  if (pre)
    in->pre = false;
  if ((pre || paragraph) && in->line->count)
    newline(in);
}
static Box *layout_block(TaiLayout *l, TaiNode **nodes, size_t count, double x,
                         double y, double width) {
  Box *b = box(l, "BlockLayout", nodes[0]);
  if (!b)
    return NULL;
  b->x = x;
  b->y = y;
  b->width = width;
  if (tag(nodes[0], "li")) {
    b->x += 20;
    b->width -= 20;
  }
  double w = px(property(nodes[0], "width", "auto"), 0);
  if (w)
    b->width = w;
  bool blocks = false;
  for (size_t i = 0; i < count; i++)
    for (size_t j = 0; j < nodes[i]->child_count; j++)
      if (block(nodes[i]->children[j]))
        blocks = true;
  if (blocks) {
    TaiNode **group = NULL;
    size_t used = 0;
    double cy = y;
    for (size_t i = 0; i < count; i++)
      for (size_t j = 0; j < nodes[i]->child_count; j++) {
        TaiNode *n = nodes[i]->children[j];
        if (tag(n, "head"))
          continue;
        if (block(n) && used) {
          Box *child = layout_block(l, group, used, b->x, cy, b->width);
          if (append(l, b, child))
            cy += child->height;
          else
            release(child);
          used = 0;
        }
        if (block(n)) {
          Box *child = layout_block(l, &n, 1, b->x, cy, b->width);
          if (append(l, b, child))
            cy += child->height;
          else
            release(child);
        } else {
          TaiNode **next = realloc(group, (used + 1) * sizeof(*next));
          if (!next) {
            l->failed = true;
            free(group);
            release(b);
            return NULL;
          }
          group = next;
          group[used++] = n;
        }
      }
    if (used) {
      Box *child = layout_block(l, group, used, b->x, cy, b->width);
      if (append(l, b, child))
        cy += child->height;
      else
        release(child);
    }
    free(group);
    b->height = cy - y;
  } else {
    Inline in = {l, b, NULL, 0, false};
    if (!newline(&in)) {
      release(b);
      return NULL;
    }
    for (size_t i = 0; i < count; i++)
      recurse(&in, nodes[i]);
    if (b->count && !b->children[b->count - 1]->count)
      release(b->children[--b->count]);
    double cy = y;
    for (size_t i = 0; i < b->count; i++) {
      Box *line = b->children[i];
      line->x = b->x;
      line->y = cy;
      line->width = b->width;
      double asc = 0, desc = 0, tw = 0;
      for (size_t j = 0; j < line->count; j++) {
        Box *c = line->children[j];
        asc = fmax(asc, c->ascent);
        desc = fmax(desc, c->descent);
        tw += c->width + c->space;
      }
      if (line->count)
        tw -= line->children[line->count - 1]->space;
      const char *align = property(b->node, "text-align", "left");
      double cx = b->x;
      if (!strcmp(align, "center"))
        cx += (b->width - tw) / 2;
      else if (!strcmp(align, "right") || l->rtl)
        cx += b->width - tw;
      for (size_t j = 0; j < line->count; j++) {
        Box *c = line->children[j];
        c->x = cx;
        c->y = cy + 1.25 * asc - c->ascent;
        cx += c->width + c->space;
      }
      line->height = 1.25 * (asc + desc);
      cy += line->height;
    }
    b->height = cy - y;
  }
  b->content_height = b->height;
  double fixed_height;
  if (fixed_px(property(nodes[0], "height", "auto"), &fixed_height))
    b->height = fixed_height;
  if (nodes[0]->kind == TAI_ELEMENT &&
      !strcmp(property(nodes[0], "overflow", "visible"), "scroll") &&
      fixed_px(property(nodes[0], "height", "auto"), &fixed_height)) {
    b->scrollable = true;
    double max_scroll = fmax(0, b->content_height - b->height);
    b->scroll_y = fmax(0, fmin(nodes[0]->scroll_y, max_scroll));
    nodes[0]->scroll_y = b->scroll_y;
  }
  return b;
}
TaiLayout *tai_layout_create(TaiNode *root, double width, bool rtl,
                             char **error) {
  if (error)
    *error = NULL;
  if (!root || !isfinite(width) || width <= 0) {
    if (error)
      *error = tai_strdup("invalid layout input");
    return NULL;
  }
  TaiLayout *l = calloc(1, sizeof(*l));
  if (!l)
    return NULL;
  l->rtl = rtl;
  if (FT_Init_FreeType(&l->ft)) {
    free(l);
    return NULL;
  }
  l->fc = FcInitLoadConfigAndFonts();
  if (!l->fc) {
    tai_layout_destroy(l);
    return NULL;
  }
  l->root = box(l, "DocumentLayout", root);
  if (l->root) {
    l->root->x = 13;
    l->root->y = 18;
    l->root->width = width - 26;
    Box *child = layout_block(l, &root, 1, 13, 18, width - 26);
    if (append(l, l->root, child))
      l->root->height = child->height;
    else
      release(child);
  }
  if (l->failed) {
    if (error)
      *error = tai_strdup("layout allocation/font failure");
    tai_layout_destroy(l);
    return NULL;
  }
  return l;
}
void tai_layout_destroy(TaiLayout *l) {
  if (!l)
    return;
  release(l->root);
  if (l->ft)
    FT_Done_FreeType(l->ft);
  if (l->fc)
    FcConfigDestroy(l->fc);
  for (size_t i = 0; i < l->image_count; i++) {
    free(l->images[i]->pixels);
    free(l->images[i]);
  }
  free(l->images);
  free(l);
}
double tai_layout_height(const TaiLayout *l) {
  return l && l->root ? l->root->height : 0;
}
static TaiLayoutItemKind item_kind(const Box *b) {
  if (!strcmp(b->kind, "DocumentLayout")) return TAI_LAYOUT_DOCUMENT;
  if (!strcmp(b->kind, "BlockLayout")) return TAI_LAYOUT_BLOCK;
  if (!strcmp(b->kind, "LineLayout")) return TAI_LAYOUT_LINE;
  if (!strcmp(b->kind, "EmojiLayout")) return TAI_LAYOUT_IMAGE;
  if (!strcmp(b->kind, "InputLayout")) return TAI_LAYOUT_INPUT;
  if (!strcmp(b->kind, "ButtonLayout")) return TAI_LAYOUT_BUTTON;
  return TAI_LAYOUT_TEXT;
}
static bool visit_box(const Box *b, TaiLayoutVisitor visitor, void *opaque) {
  TaiLayoutItem item = {
      .kind = item_kind(b),
      .control = b->control,
      .node = b->node,
      .word = b->word,
      .x = b->x,
      .y = b->y,
      .width = b->width,
      .height = b->height,
      .ascent = b->ascent,
      .descent = b->descent,
      .space = b->space,
      .font_size = b->font_size,
      .caret_x = b->caret_x,
      .content_height = b->content_height,
      .scroll_y = b->scroll_y,
      .bold = b->bold, .italic = b->italic, .scrollable = b->scrollable,
      .image_pixels = b->image ? b->image->pixels : NULL,
      .image_width = b->image ? b->image->width : 0,
      .image_height = b->image ? b->image->height : 0,
      .image_stride = b->image ? b->image->stride : 0,
  };
  if (!visitor(&item, opaque)) return false;
  for (size_t i = 0; i < b->count; i++)
    if (!visit_box(b->children[i], visitor, opaque)) return false;
  return true;
}
static bool walk_box(const Box *b, TaiLayoutTreeVisitor visitor,
                     void *opaque) {
  TaiLayoutItem item = {
      .kind = item_kind(b), .control = b->control,
      .node = b->node, .word = b->word,
      .x = b->x, .y = b->y, .width = b->width, .height = b->height,
      .ascent = b->ascent, .descent = b->descent, .space = b->space,
      .font_size = b->font_size, .content_height = b->content_height,
      .caret_x = b->caret_x,
      .scroll_y = b->scroll_y, .bold = b->bold, .italic = b->italic,
      .scrollable = b->scrollable,
      .image_pixels = b->image ? b->image->pixels : NULL,
      .image_width = b->image ? b->image->width : 0,
      .image_height = b->image ? b->image->height : 0,
      .image_stride = b->image ? b->image->stride : 0,
  };
  if (!visitor(&item, TAI_LAYOUT_ENTER, opaque)) return false;
  for (size_t i = 0; i < b->count; i++)
    if (!walk_box(b->children[i], visitor, opaque)) return false;
  return visitor(&item, TAI_LAYOUT_LEAVE, opaque);
}
bool tai_layout_visit(const TaiLayout *l, TaiLayoutVisitor visitor,
                      void *opaque, char **error) {
  if (error) {
    free(*error);
    *error = NULL;
  }
  if (!l || !l->root || !visitor) {
    if (error) *error = tai_strdup("invalid layout visitor input");
    return false;
  }
  if (!visit_box(l->root, visitor, opaque)) {
    if (error) *error = tai_strdup("layout visitor aborted");
    return false;
  }
  return true;
}
bool tai_layout_walk(const TaiLayout *l, TaiLayoutTreeVisitor visitor,
                     void *opaque, char **error) {
  if (error) {
    free(*error);
    *error = NULL;
  }
  if (!l || !l->root || !visitor) {
    if (error) *error = tai_strdup("invalid layout tree visitor input");
    return false;
  }
  if (!walk_box(l->root, visitor, opaque)) {
    if (error) *error = tai_strdup("layout tree visitor aborted");
    return false;
  }
  return true;
}

static const Box *find_control_box(const Box *box_control, size_t node_id) {
  if ((box_control->control == TAI_CONTROL_TEXT ||
       box_control->control == TAI_CONTROL_PASSWORD) &&
      box_control->node && box_control->node->id == node_id)
    return box_control;
  for (size_t i = 0; i < box_control->count; i++) {
    const Box *found = find_control_box(box_control->children[i], node_id);
    if (found) return found;
  }
  return NULL;
}

bool tai_layout_control_caret_index(const TaiLayout *layout, size_t node_id,
                                    double document_x, size_t *index) {
  if (index) *index = 0;
  if (!layout || !layout->root || !isfinite(document_x)) return false;
  const Box *box_control = find_control_box(layout->root, node_id);
  if (!box_control || !box_control->word || document_x <= box_control->x)
    return box_control != NULL;
  double size;
  FT_Face f = face((TaiLayout *)layout, box_control->node, false, &size);
  if (!f) return false;
  size_t offset = 0, codepoints = 0, length = strlen(box_control->word);
  double local_x = document_x - box_control->x;
  double left = 0.0;
  while (offset < length) {
    utf8proc_int32_t codepoint;
    utf8proc_ssize_t used = utf8proc_iterate(
        (const utf8proc_uint8_t *)box_control->word + offset,
        (utf8proc_ssize_t)(length - offset), &codepoint);
    size_t next = offset + (used > 0 ? (size_t)used : 1);
    double right = left;
    if (used > 0 &&
        !FT_Load_Char(f, (FT_ULong)codepoint, FT_LOAD_DEFAULT))
      right += (double)f->glyph->advance.x / 64;
    if (local_x < (left + right) / 2.0) break;
    offset = next;
    codepoints++;
    left = right;
  }
  FT_Done_Face(f);
  if (index) *index = codepoints;
  return true;
}
static void json_box(FILE *out, const Box *b) {
  fputs("{\"kind\":", out);
  tai_json_string(out, b->kind);
  fprintf(out, ",\"x\":%.17g,\"y\":%.17g,\"width\":%.17g,\"height\":%.17g",
          b->x, b->y, b->width, b->height);
  if (b->word) {
    fputs(",\"word\":", out);
    tai_json_string(out, b->word);
  }
  if (b->image)
    fprintf(out, ",\"ascent\":%.17g,\"descent\":%.17g,\"space\":%.17g",
            b->ascent, b->descent, b->space);
  fputs(",\"children\":[", out);
  for (size_t i = 0; i < b->count; i++) {
    if (i)
      fputc(',', out);
    json_box(out, b->children[i]);
  }
  fputs("]}", out);
}
void tai_layout_json(FILE *out, const TaiLayout *l) {
  if (l && l->root)
    json_box(out, l->root);
  else
    fputs("null", out);
}
