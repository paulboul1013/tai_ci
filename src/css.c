#include "tai/css.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

typedef enum {
  TAG,
  CLASS,
  ID,
  VISITED,
  HAS,
  SEQUENCE,
  DESCENDANT
} SelectorKind;
struct TaiSelector {
  SelectorKind kind;
  int priority;
  char *name;
  struct TaiSelector **children;
  size_t count;
};
typedef struct {
  TaiSelector *selector;
  TaiMap declarations;
} Rule;
struct TaiStylesheet {
  Rule *rules;
  size_t count;
};
typedef struct {
  const char *s;
  size_t i;
  bool oom;
  unsigned depth;
} Parser;
static bool diagnostic(char **error, const char *message) {
  if (error && !*error)
    *error = tai_strdup(message);
  return false;
}
/* Python uses Unicode isspace/isalnum and full casefold, not byte ctype. */
static size_t codepoint(const char *s, utf8proc_int32_t *cp) {
  utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)s, -1, cp);
  if (n <= 0) {
    *cp = (unsigned char)*s;
    return *s ? 1 : 0;
  }
  return (size_t)n;
}
static bool whitespace(const char *s) {
  utf8proc_int32_t cp;
  codepoint(s, &cp);
  utf8proc_category_t cat = utf8proc_category(cp);
  return (cp >= 9 && cp <= 13) || (cp >= 28 && cp <= 32) || cp == 0x85 ||
         cat == UTF8PROC_CATEGORY_ZS || cat == UTF8PROC_CATEGORY_ZL ||
         cat == UTF8PROC_CATEGORY_ZP;
}
static void space(Parser *p) {
  while (p->s[p->i] && whitespace(p->s + p->i)) {
    utf8proc_int32_t cp;
    p->i += codepoint(p->s + p->i, &cp);
  }
}
static char *slice(Parser *p, size_t start, bool lower) {
  char *s = tai_strndup(p->s + start, p->i - start);
  if (!s) {
    p->oom = true;
    return NULL;
  }
  if (lower) {
    utf8proc_uint8_t *folded = NULL;
    if (utf8proc_map((const utf8proc_uint8_t *)s, 0, &folded,
                     UTF8PROC_NULLTERM | UTF8PROC_CASEFOLD) < 0) {
      free(s);
      p->oom = true;
      return NULL;
    }
    free(s);
    s = (char *)folded;
  }
  return s;
}
static char *identifier(Parser *p, bool lower, bool word) {
  size_t start = p->i;
  while (p->s[p->i]) {
    utf8proc_int32_t cp;
    size_t n = codepoint(p->s + p->i, &cp);
    utf8proc_category_t cat = utf8proc_category(cp);
    bool alnum = (cat >= UTF8PROC_CATEGORY_LU && cat <= UTF8PROC_CATEGORY_LO) ||
                 (cat >= UTF8PROC_CATEGORY_ND && cat <= UTF8PROC_CATEGORY_NO);
    if (alnum || cp == '-' ||
        (word ? (cp == '#' || cp == '.' || cp == '%') : cp == '_'))
      p->i += n;
    else
      break;
  }
  return p->i == start ? NULL : slice(p, start, lower);
}
void tai_selector_destroy(TaiSelector *s) {
  if (!s)
    return;
  for (size_t i = 0; i < s->count; i++)
    tai_selector_destroy(s->children[i]);
  free(s->children);
  free(s->name);
  free(s);
}
static TaiSelector *selector_new(Parser *p, SelectorKind kind, char *name,
                                 int priority) {
  TaiSelector *s = calloc(1, sizeof(*s));
  if (!s) {
    p->oom = true;
    free(name);
    return NULL;
  }
  s->kind = kind;
  s->name = name;
  s->priority = priority;
  return s;
}
static bool child_add(Parser *p, TaiSelector *parent, TaiSelector *child) {
  if (!child)
    return false;
  if (parent->priority > INT_MAX - child->priority) {
    tai_selector_destroy(child);
    return false;
  }
  if (parent->count >= SIZE_MAX / sizeof(*parent->children)) {
    p->oom = true;
    tai_selector_destroy(child);
    return false;
  }
  TaiSelector **v = realloc(parent->children, (parent->count + 1) * sizeof(*v));
  if (!v) {
    p->oom = true;
    tai_selector_destroy(child);
    return false;
  }
  parent->children = v;
  parent->children[parent->count++] = child;
  parent->priority += child->priority;
  return true;
}
static TaiSelector *selector(Parser *p);
static TaiSelector *simple(Parser *p) {
  TaiSelector *out = selector_new(p, SEQUENCE, NULL, 0);
  if (!out)
    return NULL;
  char c = p->s[p->i];
  if (c && c != '.' && c != ':' && c != '#') {
    char *name = identifier(p, true, false);
    if (!name || !child_add(p, out, selector_new(p, TAG, name, 1)))
      goto fail;
  }
  while ((c = p->s[p->i]) == '.' || c == ':' || c == '#') {
    p->i++;
    char *name = identifier(p, c == ':', false);
    if (!name)
      goto fail;
    TaiSelector *part = NULL;
    if (c == '.')
      part = selector_new(p, CLASS, name, 10);
    else if (c == '#')
      part = selector_new(p, ID, name, 100);
    else {
      if (strcmp(name, "visited") == 0)
        part = selector_new(p, VISITED, NULL, 10);
      else if (strcmp(name, "has") == 0 && p->s[p->i] == '(' &&
               p->depth < 128) {
        p->i++;
        size_t start = p->i;
        unsigned depth = 1;
        while (p->s[p->i] && depth) {
          if (p->s[p->i] == '(')
            depth++;
          else if (p->s[p->i] == ')')
            depth--;
          if (depth)
            p->i++;
        }
        if (depth == 0) {
          char *inner = slice(p, start, false);
          if (inner) {
            Parser nested = {.s = inner, .depth = p->depth + 1};
            TaiSelector *inside = selector(&nested);
            p->oom |= nested.oom;
            free(inner);
            p->i++;
            if (inside) {
              part = selector_new(p, HAS, NULL, 10);
              if (!part)
                tai_selector_destroy(inside);
              else if (!child_add(p, part, inside)) {
                tai_selector_destroy(part);
                part = NULL;
              }
            }
          }
        }
      }
      free(name);
    }
    if (!child_add(p, out, part))
      goto fail;
  }
  if (!out->count)
    goto fail;
  if (out->count == 1) {
    TaiSelector *one = out->children[0];
    free(out->children);
    free(out);
    return one;
  }
  return out;
fail:
  tai_selector_destroy(out);
  return NULL;
}
static TaiSelector *selector(Parser *p) {
  TaiSelector *out = selector_new(p, DESCENDANT, NULL, 0);
  if (!out)
    return NULL;
  do {
    if (!child_add(p, out, simple(p))) {
      tai_selector_destroy(out);
      return NULL;
    }
    space(p);
  } while (p->s[p->i] && p->s[p->i] != '{');
  if (out->count == 1) {
    TaiSelector *one = out->children[0];
    free(out->children);
    free(out);
    return one;
  }
  return out;
}
TaiSelector *tai_selector_parse(const char *text, char **error) {
  Parser p = {.s = text ? text : ""};
  TaiSelector *s = selector(&p);
  if (!s)
    diagnostic(error, p.oom ? "CSS allocation failed" : "Invalid CSS selector");
  return s;
}
static bool has_descendant(const TaiSelector *s, const TaiNode *n) {
  for (size_t i = 0; i < n->child_count; i++)
    if (tai_selector_matches(s, n->children[i]) ||
        has_descendant(s, n->children[i]))
      return true;
  return false;
}
bool tai_selector_matches(const TaiSelector *s, const TaiNode *n) {
  if (!s || !n)
    return false;
  if (s->kind == SEQUENCE) {
    for (size_t i = 0; i < s->count; i++)
      if (!tai_selector_matches(s->children[i], n))
        return false;
    return true;
  }
  if (s->kind == DESCENDANT) {
    size_t i = s->count;
    if (!tai_selector_matches(s->children[--i], n))
      return false;
    for (n = n->parent; i && n; n = n->parent)
      if (tai_selector_matches(s->children[i - 1], n))
        i--;
    return i == 0;
  }
  if (n->kind != TAI_ELEMENT)
    return false;
  if (s->kind == TAG)
    return strcmp(n->tag, s->name) == 0;
  if (s->kind == VISITED)
    return n->visited && strcmp(n->tag, "a") == 0;
  if (s->kind == HAS)
    return has_descendant(s->children[0], n);
  const char *value =
      tai_map_get(&n->attributes, s->kind == ID ? "id" : "class");
  if (!value)
    value = "";
  if (s->kind == ID)
    return strcmp(value, s->name) == 0;
  while (*value) {
    while (*value && whitespace(value)) {
      utf8proc_int32_t cp;
      value += codepoint(value, &cp);
    }
    const char *start = value;
    while (*value && !whitespace(value))
      value++;
    if ((size_t)(value - start) == strlen(s->name) &&
        memcmp(start, s->name, (size_t)(value - start)) == 0)
      return true;
  }
  return false;
}
static bool append_text(Parser *p, char **out, const char *text) {
  size_t a = *out ? strlen(*out) : 0, b = strlen(text);
  if (b > SIZE_MAX - (a ? 2u : 1u) || a > SIZE_MAX - b - (a ? 2u : 1u)) {
    p->oom = true;
    return false;
  }
  char *v = realloc(*out, a + b + (a ? 2 : 1));
  if (!v) {
    p->oom = true;
    return false;
  }
  *out = v;
  if (a)
    v[a++] = ' ';
  memcpy(v + a, text, b + 1);
  return true;
}
static bool ends(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && strcmp(s + a - b, suffix) == 0;
}
static bool declaration(Parser *p, TaiMap *map) {
  char *prop = identifier(p, true, true), *value = NULL, *family = NULL;
  TaiMap expanded = {0};
  bool important = false, ok = false, saw_size = false;
  if (!prop)
    goto done;
  space(p);
  if (p->s[p->i] != ':')
    goto done;
  p->i++;
  space(p);
  while (p->s[p->i] && p->s[p->i] != ';' && p->s[p->i] != '}') {
    if (p->s[p->i] == '!') {
      p->i++;
      space(p);
      char *name = identifier(p, true, false);
      bool valid = name && strcmp(name, "important") == 0;
      free(name);
      if (!valid)
        goto done;
      important = true;
      space(p);
      continue;
    }
    size_t start = p->i;
    unsigned depth = 0;
    while (p->s[p->i]) {
      char c = p->s[p->i];
      if (!depth &&
          (whitespace(p->s + p->i) || c == ';' || c == '}' || c == '!'))
        break;
      if (c == '(')
        depth++;
      else if (c == ')') {
        if (!depth)
          goto done;
        depth--;
      }
      p->i++;
    }
    if (depth || p->i == start)
      goto done;
    char *token = slice(p, start, strcmp(prop, "font") == 0);
    if (!token)
      goto done;
    bool stored = true;
    if (strcmp(prop, "font") == 0) {
      if (strcmp(token, "italic") == 0)
        stored = tai_map_set(&expanded, "font-style", token, 0);
      else if (strcmp(token, "bold") == 0)
        stored = tai_map_set(&expanded, "font-weight", token, 0);
      else if (strcmp(token, "normal") == 0)
        stored = tai_map_set(&expanded, "font-style", token, 0) &&
                 tai_map_set(&expanded, "font-weight", token, 0);
      else if (ends(token, "px") || ends(token, "%")) {
        stored = tai_map_set(&expanded, "font-size", token, 0);
        saw_size = true;
      } else if (saw_size)
        stored = append_text(p, &family, token);
    } else
      stored = append_text(p, &value, token);
    free(token);
    if (!stored) {
      p->oom = true;
      goto done;
    }
    space(p);
  }
  if (strcmp(prop, "font") == 0) {
    if (family && !tai_map_set(&expanded, "font-family", family, 0)) {
      p->oom = true;
      goto done;
    }
    for (size_t i = 0; i < expanded.count; i++)
      if (!tai_map_set(map, expanded.items[i].key, expanded.items[i].value,
                       important ? 1 : 0)) {
        p->oom = true;
        goto done;
      }
  } else if (!tai_map_set(map, prop, value ? value : "", important ? 1 : 0)) {
    p->oom = true;
    goto done;
  }
  ok = true;
done:
  free(prop);
  free(value);
  free(family);
  tai_map_clear(&expanded);
  return ok;
}
static bool body(Parser *p, TaiMap *map) {
  space(p);
  while (p->s[p->i] && p->s[p->i] != '}') {
    if (!declaration(p, map)) {
      if (p->oom)
        return false;
      while (p->s[p->i] && p->s[p->i] != ';' && p->s[p->i] != '}')
        p->i++;
    }
    if (p->s[p->i] == ';')
      p->i++;
    space(p);
  }
  return !p->oom;
}
bool tai_css_extend(TaiStylesheet *sheet, const char *css, char **error) {
  if (!sheet)
    return diagnostic(error, "Missing stylesheet");
  Parser p = {.s = css ? css : ""};
  while (p.s[p.i]) {
    space(&p);
    TaiSelector *s = selector(&p);
    TaiMap declarations = {0};
    bool valid = s && p.s[p.i] == '{';
    if (valid) {
      p.i++;
      valid = body(&p, &declarations) && p.s[p.i] == '}';
    }
    if (valid) {
      Rule *v = sheet->count >= SIZE_MAX / sizeof(*sheet->rules)
                    ? NULL
                    : realloc(sheet->rules, (sheet->count + 1) * sizeof(*v));
      if (!v)
        p.oom = true;
      else {
        sheet->rules = v;
        sheet->rules[sheet->count++] = (Rule){s, declarations};
        s = NULL;
        declarations = (TaiMap){0};
      }
    }
    tai_selector_destroy(s);
    tai_map_clear(&declarations);
    if (p.oom)
      return diagnostic(error, "CSS allocation failed");
    while (p.s[p.i] && p.s[p.i] != '}')
      p.i++;
    if (p.s[p.i])
      p.i++;
  }
  return true;
}
TaiStylesheet *tai_css_parse(const char *css, char **error) {
  TaiStylesheet *s = calloc(1, sizeof(*s));
  if (!s) {
    diagnostic(error, "CSS allocation failed");
    return NULL;
  }
  if (!tai_css_extend(s, css, error)) {
    tai_css_destroy(s);
    return NULL;
  }
  return s;
}
void tai_css_destroy(TaiStylesheet *s) {
  if (!s)
    return;
  for (size_t i = 0; i < s->count; i++) {
    tai_selector_destroy(s->rules[i].selector);
    tai_map_clear(&s->rules[i].declarations);
  }
  free(s->rules);
  free(s);
}
static const char *const keys[] = {
    "font-size",  "font-style", "font-weight",    "color",   "font-family",
    "text-align", "width",      "height",         "display", "border-radius",
    "overflow",   "opacity",    "mix-blend-mode", "filter"};
static const char *const defaults[] = {
    "16px", "normal", "normal", "black",   "Times", "left",   "auto",
    "auto", "inline", "0px",    "visible", "1.0",   "normal", "none"};
static bool apply(TaiNode *node, const TaiMap *map, int priority) {
  for (size_t i = 0; i < map->count; i++) {
    const TaiPair *p = &map->items[i];
    int rank = priority + (p->priority ? 10000 : 0);
    if (rank >= tai_map_priority(&node->style, p->key) &&
        !tai_map_set(&node->style, p->key, p->value, rank))
      return false;
  }
  return true;
}
/* Parse Python float syntax, including Unicode decimal digits and underscores.
 */
static bool python_float(const char *s, size_t length, double *value,
                         bool *oom) {
  char *ascii = malloc(length + 1);
  if (!ascii) {
    *oom = true;
    return false;
  }
  size_t out = 0;
  for (size_t i = 0; i < length;) {
    utf8proc_int32_t cp;
    size_t n = codepoint(s + i, &cp);
    if (n > length - i) {
      free(ascii);
      return false;
    }
    if (cp < 128)
      ascii[out++] = (char)tolower((unsigned char)cp);
    else if (whitespace(s + i))
      ascii[out++] = ' ';
    else if (utf8proc_category(cp) == UTF8PROC_CATEGORY_ND) {
      utf8proc_int32_t start = cp;
      while (start > 0 && utf8proc_category(start - 1) == UTF8PROC_CATEGORY_ND)
        start--;
      ascii[out++] = (char)('0' + (cp - start) % 10);
    } else {
      free(ascii);
      return false;
    }
    i += n;
  }
  ascii[out] = '\0';
  size_t first = 0;
  while (isspace((unsigned char)ascii[first]))
    first++;
  while (out > first && isspace((unsigned char)ascii[out - 1]))
    ascii[--out] = '\0';
  size_t write = first;
  for (size_t i = first; i < out; i++) {
    if (ascii[i] == '_') {
      if (i == first || i + 1 == out || !isdigit((unsigned char)ascii[i - 1]) ||
          !isdigit((unsigned char)ascii[i + 1])) {
        free(ascii);
        return false;
      }
    } else
      ascii[write++] = ascii[i];
  }
  ascii[write] = '\0';
  char *number = ascii + first, *signless = number;
  if (*signless == '+' || *signless == '-')
    signless++;
  bool special = !strcmp(signless, "nan") || !strcmp(signless, "inf") ||
                 !strcmp(signless, "infinity");
  if (!special) {
    bool digit = false, point = false, exponent = false, exp_digit = false;
    for (char *q = signless; *q; q++) {
      if (isdigit((unsigned char)*q)) {
        digit = true;
        if (exponent)
          exp_digit = true;
      } else if (*q == '.' && !point && !exponent)
        point = true;
      else if (*q == 'e' && digit && !exponent) {
        exponent = true;
        if (q[1] == '+' || q[1] == '-')
          q++;
      } else {
        free(ascii);
        return false;
      }
    }
    if (!digit || (exponent && !exp_digit)) {
      free(ascii);
      return false;
    }
  }
  char *end = NULL;
  *value = strtod(number, &end);
  bool ok = end != number && !*end;
  free(ascii);
  return ok;
}
/* Find the shortest round-tripping mantissa, then use Python's notation
 * cutoffs. */
static void python_float_string(double value, char *out, size_t capacity) {
  if (isnan(value)) {
    snprintf(out, capacity, "nan");
    return;
  }
  if (isinf(value)) {
    snprintf(out, capacity, "%sinf", signbit(value) ? "-" : "");
    return;
  }
  if (value == 0) {
    snprintf(out, capacity, "%s0.0", signbit(value) ? "-" : "");
    return;
  }
  char scientific[64];
  for (int digits = 1; digits <= 17; digits++) {
    snprintf(scientific, sizeof(scientific), "%.*e", digits - 1, value);
    if (strtod(scientific, NULL) == value)
      break;
  }
  char *e = strchr(scientific, 'e');
  int exponent = atoi(e + 1);
  char digits[32];
  size_t count = 0;
  bool negative = scientific[0] == '-';
  for (char *q = scientific + (negative ? 1 : 0); q < e; q++)
    if (*q != '.')
      digits[count++] = *q;
  while (count > 1 && digits[count - 1] == '0')
    count--;
  size_t pos = 0;
  if (negative)
    out[pos++] = '-';
  if (exponent < -4 || exponent >= 16) {
    out[pos++] = digits[0];
    if (count > 1) {
      out[pos++] = '.';
      memcpy(out + pos, digits + 1, count - 1);
      pos += count - 1;
    }
    snprintf(out + pos, capacity - pos, "e%+03d", exponent);
    return;
  }
  int point = exponent + 1;
  if (point <= 0) {
    out[pos++] = '0';
    out[pos++] = '.';
    for (int i = 0; i < -point; i++)
      out[pos++] = '0';
    memcpy(out + pos, digits, count);
    pos += count;
  } else {
    for (int i = 0; i < point; i++)
      out[pos++] = (size_t)i < count ? digits[i] : '0';
    out[pos++] = '.';
    if ((size_t)point < count) {
      memcpy(out + pos, digits + point, count - (size_t)point);
      pos += count - (size_t)point;
    } else
      out[pos++] = '0';
  }
  out[pos] = '\0';
}
bool tai_css_style(TaiNode *node, const TaiStylesheet *sheet, char **error) {
  if (!node || !sheet)
    return diagnostic(error, "Missing style input");
  tai_map_clear(&node->style);
  for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
    const char *v = i < 6 && node->parent
                        ? tai_map_get(&node->parent->style, keys[i])
                        : defaults[i];
    if (!tai_map_set(&node->style, keys[i], v ? v : defaults[i], 0))
      goto oom;
  }
  if (node->kind == TAI_ELEMENT) {
    for (size_t i = 0; i < sheet->count; i++)
      if (tai_selector_matches(sheet->rules[i].selector, node) &&
          !apply(node, &sheet->rules[i].declarations,
                 sheet->rules[i].selector->priority))
        goto oom;
    const char *inline_style = tai_map_get(&node->attributes, "style");
    if (inline_style) {
      Parser p = {.s = inline_style};
      TaiMap map = {0};
      bool ok = body(&p, &map) && apply(node, &map, 1000);
      tai_map_clear(&map);
      if (!ok)
        goto oom;
    }
  }
  for (size_t i = 0; i < 6; i++)
    if (strcmp(tai_map_get(&node->style, keys[i]), "inherit") == 0) {
      const char *v = node->parent ? tai_map_get(&node->parent->style, keys[i])
                                   : defaults[i];
      if (!tai_map_set(&node->style, keys[i], v ? v : defaults[i],
                       tai_map_priority(&node->style, keys[i])))
        goto oom;
    }
  const char *size = tai_map_get(&node->style, "font-size");
  const char *parent = node->parent
                           ? tai_map_get(&node->parent->style, "font-size")
                           : defaults[0];
  if (!parent)
    parent = defaults[0];
  if (!ends(size, "px") && !ends(size, "%")) {
    if (!tai_map_set(&node->style, "font-size", parent,
                     tai_map_priority(&node->style, "font-size")))
      goto oom;
  } else if (ends(size, "%")) {
    double pct, px;
    bool allocation_failed = false;
    if (!python_float(size, strlen(size) - 1, &pct, &allocation_failed) ||
        !python_float(parent, strlen(parent) >= 2 ? strlen(parent) - 2 : 0, &px,
                      &allocation_failed)) {
      if (allocation_failed)
        goto oom;
      return diagnostic(error, "Invalid percentage font-size");
    }
    char buf[96];
    python_float_string(pct / 100.0 * px, buf, sizeof(buf));
    strcat(buf, "px");
    if (!tai_map_set(&node->style, "font-size", buf,
                     tai_map_priority(&node->style, "font-size")))
      goto oom;
  }
  for (size_t i = 0; i < node->child_count; i++)
    if (!tai_css_style(node->children[i], sheet, error))
      return false;
  return true;
oom:
  return diagnostic(error, "CSS allocation failed");
}
static void selector_json(FILE *out, const TaiSelector *s) {
  static const char *const names[] = {"tag", "class",    "id",        "visited",
                                      "has", "sequence", "descendant"};
  fprintf(out, "{\"kind\":\"%s\",\"priority\":%d", names[s->kind], s->priority);
  if (s->name) {
    fputs(",\"name\":", out);
    tai_json_string(out, s->name);
  }
  if (s->count) {
    fputs(",\"children\":[", out);
    for (size_t i = 0; i < s->count; i++) {
      if (i)
        fputc(',', out);
      selector_json(out, s->children[i]);
    }
    fputc(']', out);
  }
  fputc('}', out);
}
void tai_css_json(FILE *out, const TaiStylesheet *sheet) {
  fputc('[', out);
  for (size_t i = 0; i < sheet->count; i++) {
    if (i)
      fputc(',', out);
    fputs("{\"selector\":", out);
    selector_json(out, sheet->rules[i].selector);
    fputs(",\"declarations\":{", out);
    TaiMap *map = &sheet->rules[i].declarations;
    for (size_t j = 0; j < map->count; j++) {
      if (j)
        fputc(',', out);
      tai_json_string(out, map->items[j].key);
      fputs(":[", out);
      tai_json_string(out, map->items[j].value);
      fprintf(out, ",%s]", map->items[j].priority ? "true" : "false");
    }
    fputs("}}", out);
  }
  fputc(']', out);
}
