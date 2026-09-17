#include "tai/dom.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>
struct TaiDocument {
  TaiNode **nodes;
  size_t count, capacity;
  TaiNode *root;
};
typedef struct {
  char *s;
  size_t n, cap;
} Buffer;
static bool grow(void **ptr, size_t *cap, size_t n, size_t width) {
  if (n <= *cap)
    return true;
  size_t c = *cap ? *cap : 16;
  while (c < n) {
    if (c > SIZE_MAX / 2)
      return false;
    c *= 2;
  }
  if (c > SIZE_MAX / width)
    return false;
  void *v = realloc(*ptr, c * width);
  if (!v)
    return false;
  *ptr = v;
  *cap = c;
  return true;
}
static bool put(Buffer *b, const char *s, size_t n) {
  if (n > SIZE_MAX - b->n - 1 ||
      !grow((void **)&b->s, &b->cap, b->n + n + 1, 1))
    return false;
  memcpy(b->s + b->n, s, n);
  b->n += n;
  b->s[b->n] = 0;
  return true;
}
static bool str(Buffer *b, const char *s) { return put(b, s, strlen(s)); }
static void fail(char **e) {
  if (e)
    *e = tai_strdup(
        "HTML parsing failed: allocation failure or invalid parser stack");
}
static TaiNode *node_new(TaiDocument *d, TaiNodeKind kind, const char *value) {
  if (!grow((void **)&d->nodes, &d->capacity, d->count + 1, sizeof(*d->nodes)))
    return NULL;
  TaiNode *n = calloc(1, sizeof(*n));
  if (!n)
    return NULL;
  char *v = tai_strdup(value);
  if (!v) {
    free(n);
    return NULL;
  }
  n->kind = kind;
  n->document = d;
  n->id = d->count;
  if (kind == TAI_TEXT)
    n->text = v;
  else
    n->tag = v;
  d->nodes[d->count++] = n;
  return n;
}
TaiNode *tai_document_root(const TaiDocument *d) { return d ? d->root : NULL; }
TaiNode *tai_document_node(const TaiDocument *d, size_t id) {
  return d && id < d->count ? d->nodes[id] : NULL;
}
void tai_document_destroy(TaiDocument *d) {
  if (!d)
    return;
  for (size_t i = 0; i < d->count; i++) {
    TaiNode *n = d->nodes[i];
    free(n->tag);
    free(n->text);
    free(n->children);
    tai_map_clear(&n->attributes);
    tai_map_clear(&n->style);
    free(n);
  }
  free(d->nodes);
  free(d);
}
bool tai_node_append(TaiNode *p, TaiNode *c) {
  if (!p || !c || p->document != c->document)
    return false;
  for (TaiNode *n = p; n; n = n->parent)
    if (n == c)
      return false;
  if (!grow((void **)&p->children, &p->child_capacity, p->child_count + 1,
            sizeof(*p->children)))
    return false;
  if (c->parent) {
    TaiNode *old = c->parent;
    for (size_t i = 0; i < old->child_count; i++)
      if (old->children[i] == c) {
        memmove(old->children + i, old->children + i + 1,
                (old->child_count - i - 1) * sizeof(*old->children));
        old->child_count--;
        break;
      }
  }
  p->children[p->child_count++] = c;
  c->parent = p;
  return true;
}
static size_t space(const char *s) {
  utf8proc_int32_t c;
  utf8proc_ssize_t n = utf8proc_iterate((const uint8_t *)s, -1, &c);
  if (n < 1)
    return 0;
  return (c == 32 || (c >= 9 && c <= 13) || (c >= 28 && c <= 31) || c == 0x85 ||
          c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
          c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
          c == 0x3000)
             ? (size_t)n
             : 0;
}
static char *fold(const char *s, size_t n) {
  uint8_t *out = NULL;
  if (n > PTRDIFF_MAX)
    return NULL;
  return utf8proc_map((const uint8_t *)s, (utf8proc_ssize_t)n, &out,
                      UTF8PROC_CASEFOLD) < 0
             ? NULL
             : (char *)out;
}
static const struct {
  const char *name, *value;
} entities[] = {
#include "html_entities.inc"
};
static const char *entity(const char *s, size_t n) {
  size_t lo = 0, hi = sizeof(entities) / sizeof(*entities);
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    int c = strncmp(s, entities[m].name, n);
    if (!c) {
      size_t l = strlen(entities[m].name);
      c = n < l ? -1 : n > l ? 1 : 0;
    }
    if (!c)
      return entities[m].value;
    if (c < 0)
      hi = m;
    else
      lo = m + 1;
  }
  return NULL;
}
static char *unescape(const char *s) {
  Buffer b = {0};
  while (*s) {
    if (*s != '&') {
      if (!put(&b, s++, 1))
        goto bad;
      continue;
    }
    const char *start = s++;
    if (*s == '#') {
      const char *p = s + 1;
      bool hex = *p == 'x' || *p == 'X';
      if (hex)
        p++;
      const char *digits = p;
      uint32_t c = 0;
      while (*p) {
        int d = *p >= '0' && *p <= '9'          ? *p - '0'
                : hex && *p >= 'a' && *p <= 'f' ? *p - 'a' + 10
                : hex && *p >= 'A' && *p <= 'F' ? *p - 'A' + 10
                                                : -1;
        if (d < 0)
          break;
        c = c > 0x110000 ? 0x110001 : c * (hex ? 16u : 10u) + (unsigned)d;
        p++;
      }
      if (p != digits) {
        if (*p == ';')
          p++;
        s = p;
        static const uint32_t win[] = {
            0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021,
            0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0x8d,   0x17d,  0x8f,
            0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
            0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
        if (!c || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
          c = 0xfffd;
        else if (c >= 128 && c <= 159)
          c = win[c - 128];
        else if ((c >= 1 && c <= 8) || c == 11 || (c >= 14 && c <= 31) ||
                 c == 127 || (c >= 0xfdd0 && c <= 0xfdef) ||
                 (c & 0xffff) == 0xfffe || (c & 0xffff) == 0xffff)
          continue;
        uint8_t bytes[4];
        utf8proc_ssize_t n = utf8proc_encode_char((int32_t)c, bytes);
        if (!put(&b, (char *)bytes, (size_t)n))
          goto bad;
        continue;
      }
    }
    size_t n = 0;
    while (s[n] && s[n] != ';' && s[n] != '&' && s[n] != '<' && s[n] != '#' &&
           s[n] != ' ' && s[n] != '\t' && s[n] != '\n' && s[n] != '\r' &&
           s[n] != '\f' && n < 32)
      n++;
    if (s[n] == ';')
      n++;
    const char *v = NULL;
    while (n && !((v = entity(s, n))))
      n--;
    if (v) {
      if (!str(&b, v))
        goto bad;
      s += n;
    } else {
      if (!put(&b, start, 1))
        goto bad;
    }
  }
  if (!b.s)
    b.s = tai_strdup("");
  return b.s;
bad:
  free(b.s);
  return NULL;
}
typedef struct {
  TaiDocument *d;
  TaiNode **stack, **format;
  size_t n, cap, nf, cf;
  Buffer *view;
} Parser;
static bool has(const char *t, const char *list) {
  const char *p = list;
  size_t n = strlen(t);
  while (*p) {
    const char *q = strchr(p, ' ');
    size_t l = q ? (size_t)(q - p) : strlen(p);
    if (l == n && !memcmp(t, p, n))
      return true;
    if (!q)
      break;
    p = q + 1;
  }
  return false;
}
static bool push(TaiNode ***a, size_t *n, size_t *c, TaiNode *v) {
  if (!grow((void **)a, c, *n + 1, sizeof(**a)))
    return false;
  (*a)[(*n)++] = v;
  return true;
}
static bool tag(Parser *p, const char *s);
static bool implicit(Parser *p, const char *t) {
  for (;;) {
    if (!p->n && (!t || strcmp(t, "html"))) {
      if (!tag(p, "html"))
        return false;
    } else if (p->n == 1 && !strcmp(p->stack[0]->tag, "html") &&
               (!t || !has(t, "head body /html"))) {
      if (!tag(p, t && has(t, "base basefont bgsound noscript link meta title "
                              "style script")
                      ? "head"
                      : "body"))
        return false;
    } else if (p->n == 2 && !strcmp(p->stack[0]->tag, "html") &&
               !strcmp(p->stack[1]->tag, "head") &&
               (!t || !has(t, "/head base basefont bgsound noscript link meta "
                              "title style script"))) {
      if (!tag(p, "/head"))
        return false;
    } else
      return true;
  }
}
static bool escaped(Buffer *b, const char *s) {
  for (; *s; s++) {
    const char *v = *s == '&'   ? "&amp;"
                    : *s == '<' ? "&lt;"
                    : *s == '>' ? "&gt;"
                                : NULL;
    if (v ? !str(b, v) : !put(b, s, 1))
      return false;
  }
  return true;
}
static bool text_add(Parser *p, const char *s) {
  if (p->view)
    return str(p->view, "<b>") && escaped(p->view, s) && str(p->view, "</b>");
  if (!implicit(p, NULL))
    return false;
  char *v = unescape(s);
  if (!v)
    return false;
  TaiNode *n = node_new(p->d, TAI_TEXT, v);
  free(v);
  return n && p->n && tai_node_append(p->stack[p->n - 1], n);
}
static bool attributes(const char *s, char **name, TaiMap *m) {
  size_t i = 0;
  while (s[i] && !space(s + i))
    i++;
  *name = fold(s, i);
  if (!*name)
    return false;
  while (s[i]) {
    size_t w;
    while ((w = space(s + i)))
      i += w;
    if (!s[i])
      break;
    size_t begin = i;
    char quote = 0;
    while (s[i]) {
      if (s[i] == '\'' || s[i] == '"') {
        if (quote == s[i])
          quote = 0;
        else if (!quote)
          quote = s[i];
      }
      if (!quote && space(s + i))
        break;
      i++;
    }
    size_t end = i, k = begin;
    while (k < end && s[k] != '=')
      k++;
    char *key = fold(s + begin, k - begin);
    size_t v = k < end ? k + 1 : end;
    if (end - v >= 2 && (s[v] == '\'' || s[v] == '"') && s[v] == s[end - 1]) {
      v++;
      end--;
    }
    char *value = tai_strndup(s + v, end - v);
    bool ok = key && value && tai_map_set(m, key, value, 0);
    free(key);
    free(value);
    if (!ok)
      return false;
  }
  return true;
}
static bool pop_attach(Parser *p) {
  if (p->n < 2)
    return false;
  TaiNode *n = p->stack[--p->n];
  return tai_node_append(p->stack[p->n - 1], n);
}
static bool tag(Parser *p, const char *s) {
  if (p->view)
    return str(p->view, "&lt;") && escaped(p->view, s) && str(p->view, "&gt;");
  char *t = NULL;
  TaiMap m = {0};
  bool ok = false;
  if (!attributes(s, &t, &m))
    goto done;
  if (*t == '!') {
    ok = true;
    goto done;
  }
  if (!implicit(p, t))
    goto done;
  if (!strcmp(t, "p")) {
    for (size_t i = 0; i < p->n; i++)
      if (!strcmp(p->stack[i]->tag, "p")) {
        if (!tag(p, "/p"))
          goto done;
        break;
      }
  }
  if (!strcmp(t, "li")) {
    for (size_t i = p->n; i > 0; i--) {
      const char *x = p->stack[i - 1]->tag;
      if (!strcmp(x, "li")) {
        if (!tag(p, "/li"))
          goto done;
        break;
      }
      if (has(x, "ul ol"))
        break;
    }
  }
  if (*t == '/') {
    if (has(t + 1, "b i u small big")) {
      size_t found = p->nf;
      for (size_t i = p->nf; i > 0; i--)
        if (!strcmp(p->format[i - 1]->tag, t + 1)) {
          found = i - 1;
          break;
        }
      if (found == p->nf) {
        ok = true;
        goto done;
      }
      size_t old = p->nf;
      p->nf = found;
      while (p->n) {
        TaiNode *n = p->stack[--p->n];
        if (p->n && !tai_node_append(p->stack[p->n - 1], n))
          goto done;
        if (!strcmp(n->tag, t + 1))
          break;
      }
      for (size_t i = found + 1; i < old; i++) {
        if (!p->n)
          goto done;
        TaiNode *f = p->format[i], *n = node_new(p->d, TAI_ELEMENT, f->tag);
        if (!n || !tai_map_copy(&n->attributes, &f->attributes))
          goto done;
        n->checked = tai_map_get(&n->attributes, "checked") != NULL;
        n->parent = p->stack[p->n - 1];
        if (!push(&p->stack, &p->n, &p->cap, n) ||
            !push(&p->format, &p->nf, &p->cf, n))
          goto done;
      }
      ok = true;
    } else
      ok = p->n == 1 || pop_attach(p);
  } else {
    TaiNode *n = node_new(p->d, TAI_ELEMENT, t);
    if (!n)
      goto done;
    n->attributes = m;
    m = (TaiMap){0};
    n->checked = tai_map_get(&n->attributes, "checked") != NULL;
    if (has(t, "area base br col embed hr img input link meta param source "
               "track wbr")) {
      ok = p->n && tai_node_append(p->stack[p->n - 1], n);
    } else {
      n->parent = p->n ? p->stack[p->n - 1] : NULL;
      ok = push(&p->stack, &p->n, &p->cap, n);
      if (ok && has(t, "b i u small big"))
        ok = push(&p->format, &p->nf, &p->cf, n);
    }
  }
done:
  free(t);
  tai_map_clear(&m);
  return ok;
}
static const char *script_end(const char *s) {
  for (; *s; s++) {
    const char *t = "</script>";
    size_t i = 0;
    while (t[i] && s[i] &&
           ((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]) == t[i])
      i++;
    if (!t[i])
      return s;
  }
  return NULL;
}
static bool parse(Parser *p, const char *s) {
  Buffer b = {0};
  bool in = false, ok = false;
  char quote = 0;
  while (*s) {
    if (!in && !strncmp(s, "<!--", 4)) {
      if (b.n && !text_add(p, b.s))
        goto done;
      b.n = 0;
      if (b.s)
        b.s[0] = 0;
      const char *end = strstr(s + 4, "-->");
      s = end ? end + 3 : s + strlen(s);
      continue;
    }
    char c = *s++;
    if (!in) {
      if (c == '<') {
        in = true;
        if (b.n && !text_add(p, b.s))
          goto done;
        b.n = 0;
        if (b.s)
          b.s[0] = 0;
      } else if (!put(&b, &c, 1))
        goto done;
    } else if (quote) {
      if (c == quote)
        quote = 0;
      if (!put(&b, &c, 1))
        goto done;
    } else if (c == '\'' || c == '"') {
      quote = c;
      if (!put(&b, &c, 1))
        goto done;
    } else if (c == '>') {
      in = false;
      const char *raw = b.s ? b.s : "";
      size_t start = 0;
      while (space(raw + start))
        start += space(raw + start);
      size_t n = start;
      while (raw[n] && !space(raw + n))
        n++;
      char *name = fold(raw + start, n - start);
      if (!name)
        goto done;
      bool script = !strcmp(name, "script");
      free(name);
      if (!tag(p, raw))
        goto done;
      b.n = 0;
      if (b.s)
        b.s[0] = 0;
      if (script) {
        const char *end = script_end(s);
        size_t len = end ? (size_t)(end - s) : strlen(s);
        char *content = tai_strndup(s, len);
        if (!content)
          goto done;
        bool added = !len || text_add(p, content);
        free(content);
        if (!added)
          goto done;
        s += len;
      }
    } else if (!put(&b, &c, 1))
      goto done;
  }
  if (!in && b.n && !text_add(p, b.s))
    goto done;
  if (!p->view) {
    if (!p->n && !implicit(p, NULL))
      goto done;
    while (p->n > 1)
      if (!pop_attach(p))
        goto done;
    if (!p->n)
      goto done;
    p->d->root = p->stack[--p->n];
  }
  ok = true;
done:
  free(b.s);
  free(p->stack);
  free(p->format);
  return ok;
}
TaiDocument *tai_html_parse(const char *html, char **error) {
  if (error)
    *error = NULL;
  TaiDocument *d = calloc(1, sizeof(*d));
  if (!d) {
    fail(error);
    return NULL;
  }
  Parser p = {.d = d};
  if (!html || !parse(&p, html)) {
    tai_document_destroy(d);
    fail(error);
    return NULL;
  }
  return d;
}
char *tai_view_source(const char *html, char **error) {
  if (error)
    *error = NULL;
  Buffer b = {0};
  Parser p = {.view = &b};
  if (!html || !str(&b, "<pre>") || !parse(&p, html) || !str(&b, "</pre>")) {
    free(b.s);
    fail(error);
    return NULL;
  }
  return b.s;
}
bool tai_node_set_inner_html(TaiNode *node, const char *html, char **error) {
  if (error)
    *error = NULL;
  if (!node || !html) {
    fail(error);
    return false;
  }
  Buffer b = {0};
  if (!str(&b, "<html><body>") || !str(&b, html) ||
      !str(&b, "</body></html>")) {
    free(b.s);
    fail(error);
    return false;
  }
  TaiDocument *fragment = tai_html_parse(b.s, error);
  free(b.s);
  if (!fragment)
    return false;
  TaiNode *body = NULL; /* Python find_body performs pre-order traversal. */
  TaiNode **stack = NULL;
  size_t n = 0, cap = 0;
  if (!push(&stack, &n, &cap, fragment->root))
    goto bad;
  while (n) {
    TaiNode *x = stack[--n];
    if (x->kind == TAI_ELEMENT && !strcmp(x->tag, "body")) {
      body = x;
      break;
    }
    for (size_t i = x->child_count; i > 0; i--)
      if (!push(&stack, &n, &cap, x->children[i - 1]))
        goto bad;
  }
  free(stack);
  stack = NULL;
  TaiDocument *d = node->document;
  if (fragment->count > SIZE_MAX - d->count ||
      !grow((void **)&d->nodes, &d->capacity, d->count + fragment->count,
            sizeof(*d->nodes)))
    goto bad;
  for (size_t i = 0; i < node->child_count; i++)
    node->children[i]->parent = NULL;
  free(node->children);
  node->children = body ? body->children : NULL;
  node->child_count = body ? body->child_count : 0;
  node->child_capacity = body ? body->child_capacity : 0;
  if (body) {
    body->children = NULL;
    body->child_count = body->child_capacity = 0;
  }
  for (size_t i = 0; i < node->child_count; i++)
    node->children[i]->parent = node;
  for (size_t i = 0; i < fragment->count; i++) {
    TaiNode *x = fragment->nodes[i];
    x->document = d;
    x->id = d->count;
    d->nodes[d->count++] = x;
  }
  fragment->count = 0;
  tai_document_destroy(fragment);
  return true;
bad:
  free(stack);
  tai_document_destroy(fragment);
  fail(error);
  return false;
}
static void map_json(FILE *out, const TaiMap *m) {
  fputc('{', out);
  for (size_t i = 0; i < m->count; i++) {
    if (i)
      fputc(',', out);
    tai_json_string(out, m->items[i].key);
    fputc(':', out);
    tai_json_string(out, m->items[i].value);
  }
  fputc('}', out);
}
void tai_dom_json(FILE *out, const TaiNode *n, bool style) {
  if (!n) {
    fputs("null", out);
    return;
  }
  fputc('{', out);
  if (n->kind == TAI_TEXT) {
    fputs("\"text\":", out);
    tai_json_string(out, n->text);
  } else {
    fputs("\"tag\":", out);
    tai_json_string(out, n->tag);
    fputs(",\"attributes\":", out);
    map_json(out, &n->attributes);
    fputs(",\"children\":[", out);
    for (size_t i = 0; i < n->child_count; i++) {
      if (i)
        fputc(',', out);
      tai_dom_json(out, n->children[i], style);
    }
    fputc(']', out);
  }
  if (style) {
    fputs(",\"style\":", out);
    map_json(out, &n->style);
  }
  fputc('}', out);
}
