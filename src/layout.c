#include "tai/layout.h"
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

typedef struct Box Box;
struct Box {
  const char *kind;
  TaiNode *node;
  Box **children;
  size_t count;
  double x, y, width, height, ascent, descent, space, font_size;
  bool bold, italic;
  char *word;
};
struct TaiLayout {
  Box *root;
  FT_Library ft;
  FcConfig *fc;
  bool rtl, failed;
};
static const char *property(TaiNode *n, const char *key, const char *fallback) {
  const char *v = tai_map_get(&n->style, key);
  return v ? v : fallback;
}
static bool tag(TaiNode *n, const char *s) {
  return n->kind == TAI_ELEMENT && !strcmp(n->tag, s);
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
static double measure(FT_Face f, const char *text) {
  double w = 0;
  const unsigned char *p = (const unsigned char *)text;
  while (*p) {
    utf8proc_int32_t c;
    utf8proc_ssize_t n = utf8proc_iterate(p, -1, &c);
    if (n < 1) {
      p++;
      continue;
    }
    if (!FT_Load_Char(f, (FT_ULong)c, FT_LOAD_DEFAULT))
      w += (double)f->glyph->advance.x / 64;
    p += (size_t)n;
  }
  return w;
}
static bool whitespace(utf8proc_int32_t c) {
  utf8proc_category_t t = utf8proc_category(c);
  return (c >= 9 && c <= 13) || (c >= 28 && c <= 32) || c == 0x85 ||
         t == UTF8PROC_CATEGORY_ZS || t == UTF8PROC_CATEGORY_ZL ||
         t == UTF8PROC_CATEGORY_ZP;
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
  Box *b = box(l, "TextLayout", node);
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
  b->width = measure(f, b->word);
  b->font_size = size;
  b->bold = !strcmp(property(node, "font-weight", "normal"), "bold") ||
            atoi(property(node, "font-weight", "normal")) >= 600;
  b->italic = !strcmp(property(node, "font-style", "normal"), "italic") ||
              !strcmp(property(node, "font-style", "normal"), "oblique");
  b->ascent = (double)f->ascender * size / f->units_per_EM;
  b->descent = -(double)f->descender * size / f->units_per_EM;
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
  b->height = px(property(nodes[0], "height", "auto"), b->height);
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
  free(l);
}
double tai_layout_height(const TaiLayout *l) {
  return l && l->root ? l->root->height : 0;
}
static TaiLayoutItemKind item_kind(const Box *b) {
  if (!strcmp(b->kind, "DocumentLayout")) return TAI_LAYOUT_DOCUMENT;
  if (!strcmp(b->kind, "BlockLayout")) return TAI_LAYOUT_BLOCK;
  if (!strcmp(b->kind, "LineLayout")) return TAI_LAYOUT_LINE;
  return TAI_LAYOUT_TEXT;
}
static bool visit_box(const Box *b, TaiLayoutVisitor visitor, void *opaque) {
  TaiLayoutItem item = {
      .kind = item_kind(b),
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
      .bold = b->bold,
      .italic = b->italic,
  };
  if (!visitor(&item, opaque)) return false;
  for (size_t i = 0; i < b->count; i++)
    if (!visit_box(b->children[i], visitor, opaque)) return false;
  return true;
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
static void json_box(FILE *out, const Box *b) {
  fputs("{\"kind\":", out);
  tai_json_string(out, b->kind);
  fprintf(out, ",\"x\":%.17g,\"y\":%.17g,\"width\":%.17g,\"height\":%.17g",
          b->x, b->y, b->width, b->height);
  if (b->word) {
    fputs(",\"word\":", out);
    tai_json_string(out, b->word);
  }
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
