/* Native half of tests/js_dom_integration.py: loads one tests/js_page_fixture.py
 * page and replays its actions through the public TaiPage input seam.
 *
 *   js_page_probe [--tabset] CSS WIDTH HEIGHT URL ACTION...
 *
 * ACTION is click:ID, type:TEXT or state:LABEL (see js_page_fixture.py). The
 * output is one JSON object mapping each label to its checkpoint.
 *
 * By default the page loads synchronously on this thread (the headless path:
 * scripts use the network directly). --tabset loads it through a TaiTabSet:
 * load-time scripts run on the loader thread, and the actions run here, as
 * on the SDL thread, so event-time XHR goes through the loader's queue. */
#define _POSIX_C_SOURCE 200809L
#include "tai/tabset.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static TaiNode *find_id(TaiNode *node, const char *id) {
  const char *value =
      node->kind == TAI_ELEMENT ? tai_map_get(&node->attributes, "id") : NULL;
  if (value && !strcmp(value, id)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    TaiNode *found = find_id(node->children[index], id);
    if (found) return found;
  }
  return NULL;
}

static const TaiNode *find_focused(const TaiNode *node) {
  if (node->focused) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    const TaiNode *found = find_focused(node->children[index]);
    if (found) return found;
  }
  return NULL;
}

typedef struct {
  const TaiNode *target;
  double x, y;
  bool found;
} Point;

/* The first text box inside target, or target's own control box. */
static bool find_point(const TaiLayoutItem *item, void *opaque) {
  Point *point = opaque;
  if (point->found) return true;
  bool hit = false;
  if (point->target->kind == TAI_ELEMENT &&
      !strcmp(point->target->tag, "input")) {
    hit = item->kind == TAI_LAYOUT_INPUT && item->node == point->target;
  } else if (item->kind == TAI_LAYOUT_TEXT) {
    for (const TaiNode *node = item->node; node && !hit; node = node->parent)
      hit = node == point->target;
  }
  if (hit) {
    point->x = item->x + item->width / 2.0;
    point->y = item->y + item->height / 2.0;
    point->found = true;
  }
  return true;
}

static void checkpoint(const TaiPage *page) {
  TaiNode *root = tai_page_root(page);
  char *title = tai_page_title(page);
  if (!title) {
    fputs("title allocation failed\n", stderr);
    exit(EXIT_FAILURE);
  }
  const TaiNode *focused = find_focused(root);
  const char *focus_id =
      focused ? tai_map_get(&focused->attributes, "id") : NULL;
  fputs("{\"dom\":", stdout);
  tai_dom_json(stdout, root, false);
  fputs(",\"title\":", stdout);
  tai_json_string(stdout, title);
  printf(",\"scroll\":%.17g,\"url\":", tai_page_scroll_y(page));
  tai_json_string(stdout, tai_url_string(tai_page_url(page)));
  fputs(",\"focus\":", stdout);
  if (focus_id) tai_json_string(stdout, focus_id);
  else fputs("null", stdout);
  fputc('}', stdout);
  free(title);
}

static bool run_action(TaiPage *page, const char *action, bool *first,
                       char **error) {
  bool changed = false;
  if (!strncmp(action, "click:", 6)) {
    TaiNode *target = find_id(tai_page_root(page), action + 6);
    Point point = {.target = target};
    if (!target ||
        !tai_layout_visit(tai_page_layout(page), find_point, &point, error) ||
        !point.found) {
      fprintf(stderr, "no layout box for #%s\n", action + 6);
      return false;
    }
    /* Viewport coordinates, truncated like the SDL event path. */
    return tai_page_activate_viewport(
        page, trunc(point.x), trunc(point.y - tai_page_scroll_y(page)),
        &changed, error);
  }
  if (!strncmp(action, "type:", 5)) {
    /* One keypress per character, like the oracle's Tab.keypress. */
    for (const char *p = action + 5; *p;) {
      size_t length = 1;
      while (((unsigned char)p[length] & 0xC0) == 0x80) length++;
      char text[8] = {0};
      if (length >= sizeof(text)) return false;
      memcpy(text, p, length);
      if (!tai_page_text_input(page, text, &changed, error)) return false;
      p += length;
    }
    return true;
  }
  if (!strncmp(action, "state:", 6)) {
    fputs(*first ? "" : ",", stdout);
    *first = false;
    tai_json_string(stdout, action + 6);
    fputc(':', stdout);
    checkpoint(page);
    return true;
  }
  fprintf(stderr, "unknown action %s\n", action);
  return false;
}

static TaiPage *tabset_page(TaiTabSet *tabs, const char *url, double width,
                            double height, char **error) {
  if (!tai_tabset_start(tabs, url, width, height, error)) return NULL;
  TaiTabSetView view = {0};
  for (int attempt = 0; attempt < 6000; attempt++) {
    bool changed = false;
    if (!tai_tabset_pump(tabs, &changed, error) ||
        !tai_tabset_view(tabs, &view))
      return NULL;
    if (!view.loading) return view.page;
    struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
  }
  fputs("page did not load within 60 s\n", stderr);
  return NULL;
}

int main(int argc, char **argv) {
  bool use_tabset = argc > 1 && !strcmp(argv[1], "--tabset");
  if (use_tabset) {
    argv++;
    argc--;
  }
  if (argc < 5) {
    fputs("usage: js_page_probe [--tabset] CSS WIDTH HEIGHT URL ACTION...\n",
          stderr);
    return 2;
  }
  size_t css_length = 0;
  char *css = tai_read_file(argv[1], &css_length);
  char *error = NULL;
  double width = strtod(argv[2], NULL), height = strtod(argv[3], NULL);
  TaiNetwork *network = NULL;
  TaiTabSet *tabs = NULL;
  TaiUrl *url = tai_url_parse(argv[4]);
  TaiPage *page = NULL;
  if (css && url && use_tabset) {
    tabs = tai_tabset_create_with_home_url(css, false, "about:blank", &error);
    page = tabs ? tabset_page(tabs, argv[4], width, height, &error) : NULL;
  } else if (css && url) {
    network = tai_network_create();
    page = network ? tai_page_load(network, url, css, width, height, false,
                                   &error)
                   : NULL;
  }
  bool ok = page != NULL, first = true;
  if (ok) fputc('{', stdout);
  for (int index = 5; ok && index < argc; index++)
    ok = run_action(page, argv[index], &first, &error);
  if (ok) puts("}");
  else fprintf(stderr, "js_page_probe failed: %s\n", error ? error : "");
  /* The tab set owns its committed page; the headless page goes before the
   * network its scripts use. */
  if (tabs) tai_tabset_destroy(tabs);
  else tai_page_destroy(page);
  tai_url_destroy(url);
  tai_network_destroy(network);
  free(css);
  free(error);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
