/* Tab-set threading of synchronous XHR (JS DOM plan slice 5), against the
 * tests/js_page_fixture.py server:
 *
 *   test_tabset_xhr CSS BASE_URL
 *
 * - a load-time XHR loop that catches every error stops once its navigation
 *   is superseded, and the next page commits promptly;
 * - destroying the app during that loop joins the loader promptly;
 * - an event-time XHR (a click listener, run here as on the SDL thread)
 *   completes while another window's load-time XHR is still waiting;
 * - closing that window cancels its load-time XHR;
 * - two tabs whose load-time scripts both block on XHR both finish. */
#define _POSIX_C_SOURCE 200809L
#include "tai/tabset.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *css;
static const char *base;

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void pause_seconds(double seconds) {
  struct timespec t = {(time_t)seconds,
                       (long)((seconds - floor(seconds)) * 1e9)};
  nanosleep(&t, NULL);
}

static char *url(const char *path) {
  size_t length = strlen(base) + strlen(path) + 1;
  char *text = malloc(length);
  assert(text);
  snprintf(text, length, "%s%s", base, path);
  return text;
}

static TaiTabSetView wait_loaded(TaiTabSet *tabs, double limit) {
  double started = now();
  TaiTabSetView view = {0};
  for (;;) {
    bool changed = false;
    char *error = NULL;
    assert(tai_tabset_pump(tabs, &changed, &error));
    free(error);
    assert(tai_tabset_view(tabs, &view));
    if (!view.loading && view.page) return view;
    if (now() - started > limit) {
      fprintf(stderr, "no commit within %.1f s (%s)\n", limit, view.url);
      abort();
    }
    pause_seconds(0.01);
  }
}

static bool still_loading(TaiTabSet *tabs) {
  bool changed = false;
  char *error = NULL;
  assert(tai_tabset_pump(tabs, &changed, &error));
  free(error);
  TaiTabSetView view;
  assert(tai_tabset_view(tabs, &view));
  return view.loading;
}

static const TaiNode *find_attribute(const TaiNode *node, const char *key,
                                     const char *value) {
  const char *actual =
      node->kind == TAI_ELEMENT ? tai_map_get(&node->attributes, key) : NULL;
  if (actual && !strcmp(actual, value)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    const TaiNode *found = find_attribute(node->children[index], key, value);
    if (found) return found;
  }
  return NULL;
}

/* The data-v outcome recorded by js_page_fixture's record(label, ...). */
static const char *outcome(const TaiPage *page, const char *label) {
  const TaiNode *node = find_attribute(tai_page_root(page), "class", label);
  return node ? tai_map_get(&node->attributes, "data-v") : NULL;
}

typedef struct {
  const TaiNode *target;
  double x, y;
  bool found;
} Point;

static bool find_point(const TaiLayoutItem *item, void *opaque) {
  Point *point = opaque;
  if (point->found || item->kind != TAI_LAYOUT_TEXT) return true;
  for (const TaiNode *node = item->node; node; node = node->parent)
    if (node == point->target) {
      point->x = item->x + item->width / 2.0;
      point->y = item->y + item->height / 2.0;
      point->found = true;
    }
  return true;
}

static void click(TaiPage *page, const char *id) {
  Point point = {.target = find_attribute(tai_page_root(page), "id", id)};
  char *error = NULL;
  assert(point.target &&
         tai_layout_visit(tai_page_layout(page), find_point, &point, &error) &&
         point.found);
  bool changed = false;
  assert(tai_page_activate_viewport(page, trunc(point.x),
                                    trunc(point.y - tai_page_scroll_y(page)),
                                    &changed, &error));
  free(error);
}

static void cancel_by_navigation(void) {
  char *error = NULL, *hang = url("/xhr-hang"), *next = url("/click");
  TaiTabSet *tabs = tai_tabset_create_with_home_url(css, false, "about:blank",
                                                    &error);
  assert(tabs && tai_tabset_start(tabs, hang, 800.0, 600.0, &error));
  pause_seconds(0.8); /* the page's script is now blocked in its XHR loop */
  assert(still_loading(tabs));
  double started = now();
  assert(tai_tabset_navigate_address(tabs, next, &error));
  TaiTabSetView view = wait_loaded(tabs, 10.0);
  double elapsed = now() - started;
  fprintf(stderr, "superseded XHR loop: next page after %.2f s\n", elapsed);
  assert(elapsed < 3.0 && !strcmp(view.url, next));
  started = now();
  tai_tabset_destroy(tabs);
  assert(now() - started < 2.0);
  free(hang);
  free(next);
}

static void destroy_during_xhr(void) {
  char *error = NULL, *hang = url("/xhr-hang");
  TaiTabSet *tabs = tai_tabset_create_with_home_url(css, false, "about:blank",
                                                    &error);
  assert(tabs && tai_tabset_start(tabs, hang, 800.0, 600.0, &error));
  pause_seconds(0.8);
  assert(still_loading(tabs));
  double started = now();
  tai_tabset_destroy(tabs); /* owns the app: joins the loader */
  double elapsed = now() - started;
  fprintf(stderr, "app destroyed during XHR loop in %.2f s\n", elapsed);
  assert(elapsed < 2.0);
  free(hang);
}

static void event_during_load_xhr(void) {
  char *error = NULL, *clicks = url("/xhr-click"), *wait = url("/xhr-wait");
  TaiBrowserApp *app =
      tai_browser_app_create_with_home_url(css, false, "about:blank", &error);
  assert(app);
  TaiTabSet *first = tai_tabset_create_in_app(app, &error);
  assert(first && tai_tabset_start(first, clicks, 800.0, 600.0, &error));
  TaiTabSetView view = wait_loaded(first, 10.0);
  TaiTabSet *second = tai_tabset_create_in_app(app, &error);
  assert(second && tai_tabset_start(second, wait, 800.0, 600.0, &error));
  pause_seconds(0.8); /* the second window's script waits 4 s on its XHR */
  assert(still_loading(second));

  /* The click listener's XHR is served inside the loader's nested poll. */
  double started = now();
  click(view.page, "target");
  double elapsed = now() - started;
  fprintf(stderr, "event XHR during a load-time XHR: %.2f s\n", elapsed);
  const char *result = outcome(view.page, "click");
  assert(result && !strncmp(result, "ok:m=GET", 8) && elapsed < 2.0);
  assert(still_loading(second));
  char *title = tai_page_title(view.page);
  assert(title && !strcmp(title, "Fetched 1"));
  free(title);

  /* Closing the waiting window cancels its load; the app then stops at
   * once instead of waiting for the XHR. */
  tai_tabset_destroy(second);
  tai_tabset_destroy(first);
  started = now();
  tai_browser_app_destroy(app);
  assert(now() - started < 2.0);
  free(clicks);
  free(wait);
}

static void two_blocked_tabs(void) {
  char *error = NULL, *slow = url("/xhr-slow");
  TaiTabSet *tabs = tai_tabset_create_with_home_url(css, false, "about:blank",
                                                    &error);
  assert(tabs && tai_tabset_start(tabs, slow, 800.0, 600.0, &error));
  assert(tai_tabset_new_tab(tabs, &error) &&
         tai_tabset_navigate_address(tabs, slow, &error));
  double started = now();
  for (size_t index = 0; index < 2; index++) {
    assert(tai_tabset_select(tabs, index));
    TaiTabSetView view = wait_loaded(tabs, 20.0);
    const char *result = outcome(view.page, "slow");
    const char *after = outcome(view.page, "after");
    assert(result && !strcmp(result, "ok:slow"));
    assert(after && !strcmp(after, "done"));
  }
  fprintf(stderr, "two blocked tabs finished in %.2f s\n", now() - started);
  tai_tabset_destroy(tabs);
  free(slow);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fputs("usage: test_tabset_xhr CSS BASE_URL\n", stderr);
    return 2;
  }
  size_t length = 0;
  char *text = tai_read_file(argv[1], &length);
  assert(text);
  css = text;
  base = argv[2];
  cancel_by_navigation();
  destroy_during_xhr();
  event_during_load_xhr();
  two_blocked_tabs();
  free(text);
  return 0;
}
