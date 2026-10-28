#define _POSIX_C_SOURCE 200809L
#include "tai/layout.h"
#include <cairo/cairo.h>
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <limits.h>
#include <math.h>
#include <stdint.h>
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
  double content_height, scroll_y, text_width;
  /* sup text aligns its top with the line's normal text; wrapper is
   * TaiLayoutItem.wrapper. */
  bool bold, italic, scrollable, sup, wrapper;
  TaiControlKind control;
  char *word;
  const char *family; /* borrowed from the layout's font cache */
  Image *image;
  TaiLayoutCaption caption; /* strings borrowed: static text, cached family */
};
/* One resolved family/weight/slant/size, memoized per layout like the
 * reference's TYPEFACES cache. The FcFontMatch and FT_New_Face cost is paid
 * once per key instead of once per word. Advances are memoized per codepoint
 * with the same FT_Load_Char(FT_LOAD_DEFAULT) result the uncached path used;
 * a failed load is not memoized, so it is retried like before. */
typedef struct {
  utf8proc_int32_t codepoint; /* -1 marks an empty slot */
  double advance;
} Advance;
typedef struct {
  char *family;
  bool bold, italic;
  double size;
  FT_Face face;
  bool ascii_known[128];
  double ascii_advance[128];
  Advance *table; /* open addressing, capacity is a power of two */
  size_t table_count, table_capacity;
} Font;
struct TaiLayout {
  Box *root;
  FT_Library ft;
  FcConfig *fc;
  bool rtl, failed;
  /* Owned; each Font is separately allocated so borrowed pointers survive
   * growth. Faces belong to ft and are released before it. */
  Font **fonts;
  size_t font_count, font_capacity;
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
static void font_destroy(Font *font) {
  if (!font)
    return;
  if (font->face)
    FT_Done_Face(font->face);
  free(font->table);
  free(font->family);
  free(font);
}
/* Opens one sized face for a resolved key; NULL means the uncached path
 * would also have failed, so the caller marks the layout failed. */
static Font *font_open(TaiLayout *l, const char *family, bool bold,
                       bool italic, double size) {
  Font *font = calloc(1, sizeof(*font));
  if (!font)
    return NULL;
  font->family = tai_strdup(family);
  font->bold = bold;
  font->italic = italic;
  font->size = size;
  FcPattern *p = font->family ? FcPatternCreate() : NULL;
  if (!p) {
    font_destroy(font);
    return NULL;
  }
  FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)family);
  FcPatternAddInteger(p, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
  FcPatternAddInteger(p, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
  FcConfigSubstitute(l->fc, p, FcMatchPattern);
  FcDefaultSubstitute(p);
  FcResult result;
  FcPattern *m = FcFontMatch(l->fc, p, &result);
  FcPatternDestroy(p);
  FcChar8 *file;
  int index = 0;
  if (m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
    FcPatternGetInteger(m, FC_INDEX, 0, &index);
    if (FT_New_Face(l->ft, (const char *)file, index, &font->face))
      font->face = NULL;
  }
  if (m)
    FcPatternDestroy(m);
  if (!font->face ||
      FT_Set_Char_Size(font->face, 0, (FT_F26Dot6)(size * 64), 72, 72)) {
    font_destroy(font);
    return NULL;
  }
  return font;
}
/* Returns a font borrowed from l, valid until tai_layout_destroy. */
static Font *font_get(TaiLayout *l, const char *family, bool bold, bool italic,
                      double size) {
  Font *font = NULL;
  for (size_t i = 0; i < l->font_count && !font; i++) {
    Font *candidate = l->fonts[i];
    if (candidate->bold == bold && candidate->italic == italic &&
        candidate->size == size && !strcmp(candidate->family, family))
      font = candidate;
  }
  if (!font && l->font_count == l->font_capacity) {
    size_t capacity = l->font_capacity ? l->font_capacity * 2 : 8;
    Font **fonts = capacity <= SIZE_MAX / sizeof(*fonts)
                       ? realloc(l->fonts, capacity * sizeof(*fonts))
                       : NULL;
    if (!fonts) {
      l->failed = true;
      return NULL;
    }
    l->fonts = fonts;
    l->font_capacity = capacity;
  }
  if (!font) {
    font = font_open(l, family, bold, italic, size);
    if (font)
      l->fonts[l->font_count++] = font;
  }
  if (!font)
    l->failed = true;
  return font;
}
static bool text_bold(TaiNode *node) {
  const char *weight = property(node, "font-weight", "normal");
  return !strcmp(weight, "bold") || atoi(weight) >= 600;
}
static bool text_italic(TaiNode *node) {
  const char *slant = property(node, "font-style", "normal");
  return !strcmp(slant, "italic") || !strcmp(slant, "oblique");
}
/* The font the reference uses for node's text. sup halves the size and
 * small_caps (abbr's lowercase letters) takes 80% of that in bold, each
 * truncated like Python's int(). */
static Font *face(TaiLayout *l, TaiNode *node, bool pre, bool sup,
                  bool small_caps, double *size) {
  *size = fmax(1, nearbyint(strtod(property(node, "font-size", "16px"), NULL)));
  if (!isfinite(*size) || *size > 1000000) {
    l->failed = true;
    return NULL;
  }
  if (sup)
    *size = fmax(1, floor(*size / 2));
  if (small_caps)
    *size = fmax(1, floor(*size * 0.8));
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
  Font *font = font_get(l, start, small_caps || text_bold(node),
                        text_italic(node), *size);
  free(name);
  return font;
}
static double font_ascent(const Font *f) {
  return (double)f->face->ascender * f->size / f->face->units_per_EM;
}
static double font_descent(const Font *f) {
  return -(double)f->face->descender * f->size / f->face->units_per_EM;
}
static Advance *advance_slot(Advance *table, size_t capacity,
                             utf8proc_int32_t c) {
  size_t i = ((size_t)(uint32_t)c * 2654435761u) & (capacity - 1);
  while (table[i].codepoint != -1 && table[i].codepoint != c)
    i = (i + 1) & (capacity - 1);
  return &table[i];
}
/* Grows the table so one more entry keeps the load factor at most 1/2.
 * Failure only disables memoization of that codepoint. */
static bool advance_reserve(Font *font) {
  if ((font->table_count + 1) * 2 <= font->table_capacity)
    return true;
  size_t capacity = font->table_capacity ? font->table_capacity * 2 : 64;
  if (capacity > SIZE_MAX / sizeof(Advance))
    return false;
  Advance *table = malloc(capacity * sizeof(*table));
  if (!table)
    return false;
  for (size_t i = 0; i < capacity; i++)
    table[i].codepoint = -1;
  for (size_t i = 0; i < font->table_capacity; i++)
    if (font->table[i].codepoint != -1)
      *advance_slot(table, capacity, font->table[i].codepoint) =
          font->table[i];
  free(font->table);
  font->table = table;
  font->table_capacity = capacity;
  return true;
}
/* Python's measureText sums per-glyph advances; a glyph FreeType cannot load
 * contributes nothing, exactly as the uncached loop skipped it. */
static bool advance(Font *font, utf8proc_int32_t c, double *out) {
  if (c >= 0 && c < 128 && font->ascii_known[c]) {
    *out = font->ascii_advance[c];
    return true;
  }
  Advance *slot = c >= 128 && font->table_capacity
                      ? advance_slot(font->table, font->table_capacity, c)
                      : NULL;
  if (slot && slot->codepoint == c) {
    *out = slot->advance;
    return true;
  }
  if (FT_Load_Char(font->face, (FT_ULong)c, FT_LOAD_DEFAULT))
    return false;
  double value = (double)font->face->glyph->advance.x / 64;
  if (c >= 0 && c < 128) {
    font->ascii_advance[c] = value;
    font->ascii_known[c] = true;
  } else if (c >= 128 && advance_reserve(font)) {
    slot = advance_slot(font->table, font->table_capacity, c);
    *slot = (Advance){.codepoint = c, .advance = value};
    font->table_count++;
  }
  *out = value;
  return true;
}
static double measure_n(Font *font, const char *text, size_t length) {
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
    double a;
    if (advance(font, c, &a))
      w += a;
    p += (size_t)n;
  }
  return w;
}
static double measure(Font *font, const char *text) {
  return measure_n(font, text, strlen(text));
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
  /* Python's is_pre/is_sup/is_abbr: set by the open tag, cleared by the close
   * tag, so an inner </sup> also ends an outer <sup>. */
  bool pre, sup, abbr;
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
static Box *text_box(TaiLayout *l, TaiNode *node, char *word, Font *f,
                     bool bold, bool sup) {
  Box *b = box(l, "TextLayout", node);
  if (!b) {
    free(word);
    return NULL;
  }
  b->word = word;
  b->width = b->text_width = measure(f, word);
  b->font_size = f->size;
  b->family = f->family;
  b->bold = bold;
  b->italic = text_italic(node);
  b->sup = sup;
  b->ascent = font_ascent(f);
  b->descent = font_descent(f);
  b->height = b->ascent + b->descent;
  return b;
}
static void release_all(Box **boxes, size_t count) {
  for (size_t i = 0; i < count; i++)
    release(boxes[i]);
  free(boxes);
}
/* Python str.upper() results longer than one code point (SpecialCasing),
 * generated with Python 3.12.3 / Unicode 15.0.0.
 * utf8proc_toupper maps these characters to themselves. */
static const struct {
  utf8proc_int32_t lower;
  const char *upper;
} SPECIAL_UPPER[] = {
    {0xdf, "\123\123"}, {0x149, "\312\274\116"}, {0x1f0, "\112\314\214"},
    {0x390, "\316\231\314\210\314\201"}, {0x3b0, "\316\245\314\210\314\201"},
    {0x587, "\324\265\325\222"}, {0x1e96, "\110\314\261"},
    {0x1e97, "\124\314\210"}, {0x1e98, "\127\314\212"},
    {0x1e99, "\131\314\212"}, {0x1e9a, "\101\312\276"},
    {0x1f50, "\316\245\314\223"}, {0x1f52, "\316\245\314\223\314\200"},
    {0x1f54, "\316\245\314\223\314\201"}, {0x1f56, "\316\245\314\223\315\202"},
    {0x1f80, "\341\274\210\316\231"}, {0x1f81, "\341\274\211\316\231"},
    {0x1f82, "\341\274\212\316\231"}, {0x1f83, "\341\274\213\316\231"},
    {0x1f84, "\341\274\214\316\231"}, {0x1f85, "\341\274\215\316\231"},
    {0x1f86, "\341\274\216\316\231"}, {0x1f87, "\341\274\217\316\231"},
    {0x1f90, "\341\274\250\316\231"}, {0x1f91, "\341\274\251\316\231"},
    {0x1f92, "\341\274\252\316\231"}, {0x1f93, "\341\274\253\316\231"},
    {0x1f94, "\341\274\254\316\231"}, {0x1f95, "\341\274\255\316\231"},
    {0x1f96, "\341\274\256\316\231"}, {0x1f97, "\341\274\257\316\231"},
    {0x1fa0, "\341\275\250\316\231"}, {0x1fa1, "\341\275\251\316\231"},
    {0x1fa2, "\341\275\252\316\231"}, {0x1fa3, "\341\275\253\316\231"},
    {0x1fa4, "\341\275\254\316\231"}, {0x1fa5, "\341\275\255\316\231"},
    {0x1fa6, "\341\275\256\316\231"}, {0x1fa7, "\341\275\257\316\231"},
    {0x1fb2, "\341\276\272\316\231"}, {0x1fb3, "\316\221\316\231"},
    {0x1fb4, "\316\206\316\231"}, {0x1fb6, "\316\221\315\202"},
    {0x1fb7, "\316\221\315\202\316\231"}, {0x1fc2, "\341\277\212\316\231"},
    {0x1fc3, "\316\227\316\231"}, {0x1fc4, "\316\211\316\231"},
    {0x1fc6, "\316\227\315\202"}, {0x1fc7, "\316\227\315\202\316\231"},
    {0x1fd2, "\316\231\314\210\314\200"}, {0x1fd3, "\316\231\314\210\314\201"},
    {0x1fd6, "\316\231\315\202"}, {0x1fd7, "\316\231\314\210\315\202"},
    {0x1fe2, "\316\245\314\210\314\200"}, {0x1fe3, "\316\245\314\210\314\201"},
    {0x1fe4, "\316\241\314\223"}, {0x1fe6, "\316\245\315\202"},
    {0x1fe7, "\316\245\314\210\315\202"}, {0x1ff2, "\341\277\272\316\231"},
    {0x1ff3, "\316\251\316\231"}, {0x1ff4, "\316\217\316\231"},
    {0x1ff6, "\316\251\315\202"}, {0x1ff7, "\316\251\315\202\316\231"},
    {0xfb00, "\106\106"}, {0xfb01, "\106\111"}, {0xfb02, "\106\114"},
    {0xfb03, "\106\106\111"}, {0xfb04, "\106\106\114"}, {0xfb05, "\123\124"},
    {0xfb06, "\123\124"}, {0xfb13, "\325\204\325\206"},
    {0xfb14, "\325\204\324\265"}, {0xfb15, "\325\204\324\273"},
    {0xfb16, "\325\216\325\206"}, {0xfb17, "\325\204\324\275"},
};
/* Python's char.upper() as UTF-8, owned by the caller. */
static char *upper(utf8proc_int32_t c) {
  for (size_t i = 0; i < sizeof(SPECIAL_UPPER) / sizeof(*SPECIAL_UPPER); i++)
    if (SPECIAL_UPPER[i].lower == c)
      return tai_strdup(SPECIAL_UPPER[i].upper);
  utf8proc_uint8_t utf8[4];
  utf8proc_ssize_t length = utf8proc_encode_char(utf8proc_toupper(c), utf8);
  return tai_strndup((const char *)utf8, (size_t)length);
}
/* The reference's abbr_word: one box per character, lowercase letters
 * uppercased in bold small caps; the word wraps as a whole and only its last
 * box carries the space. */
static void abbr_word(Inline *in, TaiNode *node, const char *s, size_t n) {
  TaiLayout *l = in->owner;
  double size;
  Font *normal = face(l, node, false, in->sup, false, &size);
  if (!normal)
    return;
  Font *caps = NULL;
  Box **pieces = NULL;
  size_t count = 0;
  double total = 0;
  for (size_t i = 0; i < n;) {
    utf8proc_int32_t c;
    utf8proc_ssize_t used =
        utf8proc_iterate((const utf8proc_uint8_t *)s + i, (utf8proc_ssize_t)(n - i), &c);
    if (used < 1) {
      used = 1;
      c = 0xfffd;
    }
    i += (size_t)used;
    if (c == 0xad)
      continue;
    bool lower = utf8proc_islower(c);
    if (lower && !caps)
      caps = face(l, node, false, in->sup, true, &size);
    Font *f = lower ? caps : normal;
    utf8proc_uint8_t utf8[4];
    char *text = !f ? NULL : lower ? upper(c) :
        tai_strndup((const char *)utf8, (size_t)utf8proc_encode_char(c, utf8));
    Box **grown = text ? realloc(pieces, (count + 1) * sizeof(*grown)) : NULL;
    if (!grown) {
      free(text);
      l->failed = true;
      release_all(pieces, count);
      return;
    }
    pieces = grown;
    pieces[count] = text_box(l, node, text, f, lower || text_bold(node), in->sup);
    if (!pieces[count]) {
      release_all(pieces, count);
      return;
    }
    total += pieces[count++]->width;
  }
  if (!count)
    return;
  double space = measure(normal, " ");
  pieces[count - 1]->space = space;
  if (in->cursor + total > in->block->width && in->line->count &&
      !newline(in)) {
    release_all(pieces, count);
    return;
  }
  for (size_t i = 0; i < count; i++)
    if (!append(l, in->line, pieces[i])) {
      for (size_t j = i; j < count; j++)
        release(pieces[j]);
      free(pieces);
      return;
    }
  free(pieces);
  in->cursor += total + space;
}
static void word(Inline *in, TaiNode *node, const char *s, size_t n) {
  TaiLayout *l = in->owner;
  if (in->abbr && !in->pre) {
    abbr_word(in, node, s, n);
    return;
  }
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
  Font *f = face(l, node, in->pre, in->sup, false, &size);
  if (!f) {
    release(b);
    return;
  }
  b->width = image ? 22.0 : measure(f, b->word);
  b->text_width = b->width;
  b->font_size = size;
  b->family = f->family;
  b->bold = text_bold(node);
  b->italic = text_italic(node);
  b->sup = in->sup && !image;
  b->image = image;
  double image_height = image ? fmax(1.0, round_ties_even(
      (double)image->height * 22.0 / (double)image->width)) : 0.0;
  b->ascent = image ? image_height : font_ascent(f);
  b->descent = image ? 0.0 : font_descent(f);
  b->height = b->ascent + b->descent;
  b->space = in->pre ? 0 : measure(f, " ");
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

static Box *layout_block(TaiLayout *l, TaiNode **nodes, size_t count, double x,
                         double y, double width, bool contents);

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
  Font *f = face(layout, node, false, false, false, &size);
  if (!f) { release(box_control); return; }
  box_control->font_size = size;
  box_control->family = f->family;
  box_control->bold = text_bold(node);
  box_control->italic = text_italic(node);
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
    box_control->ascent = font_ascent(f);
    box_control->descent = font_descent(f);
    box_control->height = box_control->ascent + box_control->descent;
    if (button) {
      /* The reference lays the children out as a block 4px inside the
       * button; it is placed at the origin here and moved with the button
       * once the line positions it. */
      double line = box_control->height, content_height = line;
      if (node->child_count) {
        Box *content = layout_block(layout, &node, 1, 0, 0,
                                    fmax(1, box_control->width - 8), true);
        if (!append(layout, box_control, content)) {
          release(content);
          release(box_control);
          return;
        }
        content_height = content->height;
      }
      box_control->height = fmax(content_height, line) + 8.0;
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
        box_control->text_width = measure(f, box_control->word);
      }
    }
  }
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
  bool pre = tag(n, "pre"), paragraph = tag(n, "p"), sup = tag(n, "sup"),
       abbr = tag(n, "abbr");
  if (pre)
    in->pre = true;
  if (sup)
    in->sup = true;
  if (abbr)
    in->abbr = true;
  if ((pre || paragraph) && in->line->count)
    newline(in);
  for (size_t i = 0; i < n->child_count; i++)
    recurse(in, n->children[i]);
  if (pre)
    in->pre = false;
  if (sup)
    in->sup = false;
  if (abbr)
    in->abbr = false;
  if ((pre || paragraph) && in->line->count)
    newline(in);
}
static void move_subtree(Box *b, double dx, double dy) {
  b->x += dx;
  b->y += dy;
  for (size_t i = 0; i < b->count; i++)
    move_subtree(b->children[i], dx, dy);
}
static bool whitespace_text(const TaiNode *n) {
  if (n->kind != TAI_TEXT || !*n->text)
    return false;
  for (const char *p = n->text; *p;) {
    utf8proc_int32_t c;
    utf8proc_ssize_t len = utf8proc_iterate((const unsigned char *)p, -1, &c);
    if (len < 1 || !whitespace(c))
      return false;
    p += len;
  }
  return true;
}
/* Lays out one child group below cy and moves cy past it. */
static void place_group(TaiLayout *l, Box *b, TaiNode **group, size_t count,
                  double *cy) {
  Box *child = layout_block(l, group, count, b->x, *cy, b->width, false);
  if (append(l, b, child))
    *cy += child->height;
  else
    release(child);
}
static bool push_node(TaiLayout *l, TaiNode ***list, size_t *used, TaiNode *n) {
  TaiNode **next = realloc(*list, (*used + 1) * sizeof(*next));
  if (!next) {
    l->failed = true;
    return false;
  }
  *list = next;
  (*list)[(*used)++] = n;
  return true;
}
/* The reference's child_groups: consecutive inline children share a group,
 * each block child is its own group, and an h6 followed (past whitespace
 * text) by a <p> shares one group with that paragraph's children. */
static bool block_children(TaiLayout *l, Box *b, TaiNode **nodes, size_t count,
                           double *cy) {
  TaiNode **children = NULL, **group = NULL, **merged = NULL;
  size_t total = 0, used = 0;
  bool ok = true;
  for (size_t i = 0; ok && i < count; i++)
    for (size_t j = 0; ok && j < nodes[i]->child_count; j++)
      if (!tag(nodes[i]->children[j], "head"))
        ok = push_node(l, &children, &total, nodes[i]->children[j]);
  for (size_t i = 0; ok && i < total;) {
    TaiNode *n = children[i];
    if (tag(n, "h6")) {
      size_t j = i + 1;
      while (j < total && whitespace_text(children[j]))
        j++;
      if (j < total && tag(children[j], "p")) {
        if (used)
          place_group(l, b, group, used, cy);
        used = 0;
        size_t merged_count = 0;
        ok = push_node(l, &merged, &merged_count, n);
        for (size_t k = 0; ok && k < children[j]->child_count; k++)
          ok = push_node(l, &merged, &merged_count, children[j]->children[k]);
        if (ok)
          place_group(l, b, merged, merged_count, cy);
        free(merged);
        merged = NULL;
        i = j + 1;
        continue;
      }
      if (j < total && !block(children[j])) {
        ok = push_node(l, &group, &used, n);
        i++;
        continue;
      }
    }
    if (block(n) || tag(n, "h6")) {
      if (used)
        place_group(l, b, group, used, cy);
      used = 0;
      place_group(l, b, &n, 1, cy);
    } else {
      ok = push_node(l, &group, &used, n);
    }
    i++;
  }
  if (ok && used)
    place_group(l, b, group, used, cy);
  free(group);
  free(children);
  return ok;
}
/* The block-mode header band height above a nav#toc (VSTEP + 4). */
static const double TOC_HEADER = 22.0;
static bool table_of_contents(TaiNode *n) {
  const char *id = n->kind == TAI_ELEMENT ? tai_map_get(&n->attributes, "id") : NULL;
  return tag(n, "nav") && id && !strcmp(id, "toc");
}
/* contents lays out a <button>'s children (nodes[0] is the button): its own
 * tag is not a control and it never scrolls, like the reference's synthetic
 * button-content node. */
static Box *layout_block(TaiLayout *l, TaiNode **nodes, size_t count, double x,
                         double y, double width, bool contents) {
  Box *b = box(l, "BlockLayout", nodes[0]);
  if (!b)
    return NULL;
  b->x = x;
  b->y = y;
  b->width = width;
  bool control = tag(nodes[0], "input") || tag(nodes[0], "button");
  b->wrapper = contents || control;
  if (tag(nodes[0], "li")) {
    b->x += 20;
    b->width -= 20;
  }
  double w = px(property(nodes[0], "width", "auto"), 0);
  if (w)
    b->width = w;
  /* A display:block input or button still lays itself out inline. */
  bool blocks = false;
  if (contents || !control)
    for (size_t i = 0; i < count; i++)
      for (size_t j = 0; j < nodes[i]->child_count; j++)
        if (block(nodes[i]->children[j]))
          blocks = true;
  if (blocks) {
    double cy = y + (table_of_contents(nodes[0]) ? TOC_HEADER : 0);
    if (!block_children(l, b, nodes, count, &cy)) {
      release(b);
      return NULL;
    }
    b->height = cy - y;
  } else {
    Inline in = {l, b, NULL, 0, false, false, false};
    if (!newline(&in)) {
      release(b);
      return NULL;
    }
    if (contents)
      for (size_t i = 0; i < nodes[0]->child_count; i++)
        recurse(&in, nodes[0]->children[i]);
    else
      for (size_t i = 0; i < count; i++)
        recurse(&in, nodes[i]);
    if (b->count && !b->children[b->count - 1]->count)
      release(b->children[--b->count]);
    double cy = y;
    for (size_t i = 0; contents && i < b->count; i++)
      b->children[i]->wrapper = true;
    for (size_t i = 0; i < b->count; i++) {
      Box *line = b->children[i];
      line->x = b->x;
      line->y = cy;
      line->width = b->width;
      /* sup text tops align with the tallest normal text, or with the line's
       * tallest child when the line has no normal text. */
      double asc = 0, desc = 0, normal = 0, tw = 0;
      bool has_normal = false;
      for (size_t j = 0; j < line->count; j++) {
        Box *c = line->children[j];
        asc = fmax(asc, c->ascent);
        desc = fmax(desc, c->descent);
        if (!strcmp(c->kind, "TextLayout") && !c->sup) {
          normal = has_normal ? fmax(normal, c->ascent) : c->ascent;
          has_normal = true;
        }
        tw += c->width + c->space;
      }
      if (!has_normal)
        normal = asc;
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
        c->y = cy + 1.25 * asc - (c->sup ? normal : c->ascent);
        if (c->control == TAI_CONTROL_BUTTON && c->count)
          move_subtree(c->children[0], c->x + 4, c->y + 4);
        cx += c->width + c->space;
      }
      line->height = 1.25 * (asc + desc);
      cy += line->height;
    }
    b->height = cy - y;
  }
  if (table_of_contents(nodes[0])) {
    Font *f = font_get(l, "Times New Roman", true, false, 12);
    if (!f) {
      release(b);
      return NULL;
    }
    b->caption = (TaiLayoutCaption){
        .text = "Table of Contents", .font_family = f->family,
        .band_height = TOC_HEADER, .x = 4, .y = 2,
        .width = measure(f, "Table of Contents"),
        .height = font_ascent(f) + font_descent(f), .ascent = font_ascent(f),
        .font_size = 12, .bold = true};
  }
  b->content_height = b->height;
  double fixed_height;
  if (fixed_px(property(nodes[0], "height", "auto"), &fixed_height))
    b->height = fixed_height;
  if (!contents && nodes[0]->kind == TAI_ELEMENT &&
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
    Box *child = layout_block(l, &root, 1, 13, 18, width - 26, false);
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
  for (size_t i = 0; i < l->font_count; i++)
    font_destroy(l->fonts[i]);
  free(l->fonts);
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
static TaiLayoutItem item_of(const Box *b) {
  return (TaiLayoutItem){
      .kind = item_kind(b), .control = b->control,
      .node = b->node, .word = b->word,
      .font_family = b->family, .text_width = b->text_width,
      .x = b->x, .y = b->y, .width = b->width, .height = b->height,
      .ascent = b->ascent, .descent = b->descent, .space = b->space,
      .font_size = b->font_size, .content_height = b->content_height,
      .caret_x = b->caret_x,
      .scroll_y = b->scroll_y, .bold = b->bold, .italic = b->italic,
      .scrollable = b->scrollable, .wrapper = b->wrapper,
      .caption = b->caption,
      .image_pixels = b->image ? b->image->pixels : NULL,
      .image_width = b->image ? b->image->width : 0,
      .image_height = b->image ? b->image->height : 0,
      .image_stride = b->image ? b->image->stride : 0,
  };
}
static bool visit_box(const Box *b, TaiLayoutVisitor visitor, void *opaque) {
  TaiLayoutItem item = item_of(b);
  if (!visitor(&item, opaque)) return false;
  for (size_t i = 0; i < b->count; i++)
    if (!visit_box(b->children[i], visitor, opaque)) return false;
  return true;
}
static bool walk_box(const Box *b, TaiLayoutTreeVisitor visitor,
                     void *opaque) {
  TaiLayoutItem item = item_of(b);
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
  /* The font cache is memoization, not observable layout state; the owning
   * thread has exclusive access to the layout (see layout.h). */
  Font *f = face((TaiLayout *)layout, box_control->node, false, false, false,
                 &size);
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
    double right = left, width;
    if (used > 0 && advance(f, codepoint, &width))
      right += width;
    if (local_x < (left + right) / 2.0) break;
    offset = next;
    codepoints++;
    left = right;
  }
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
