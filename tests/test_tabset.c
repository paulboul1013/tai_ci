#define _POSIX_C_SOURCE 200809L
#include "tai/tabset.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,      \
              #condition);                                                     \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

static const char *default_css =
    "html {display:block} body {display:block} p {display:block} "
    "a {display:block} h1 {display:block}";
static const int timeout_seconds = 8;

static struct timespec deadline_after(int seconds) {
  struct timespec deadline;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
  deadline.tv_sec += seconds;
  return deadline;
}

static bool before_deadline(struct timespec deadline) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return now.tv_sec < deadline.tv_sec ||
         (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec);
}

static void pause_briefly(void) {
  const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
  (void)nanosleep(&pause, NULL);
}

static void print_checkpoint(const char *name) {
  puts(name);
  fflush(stdout);
  int character = getchar();
  CHECK(character == '\n' || character == '\r');
}

static TaiTabSetView read_view(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  return view;
}

static bool contains_text(const TaiNode *node, const char *needle);

static void pump_once(TaiTabSet *tabs) {
  bool changed = false;
  char *error = NULL;
  CHECK(tai_tabset_pump(tabs, &changed, &error));
  CHECK(error == NULL);
  free(error);
}

static TaiTabSetView pump_until_loading(TaiTabSet *tabs, bool loading) {
  struct timespec deadline = deadline_after(timeout_seconds);
  TaiTabSetView view = read_view(tabs);
  while (view.loading != loading && before_deadline(deadline)) {
    pump_once(tabs);
    pause_briefly();
    view = read_view(tabs);
  }
  CHECK(view.loading == loading);
  return view;
}

static TaiTabSetView await_active_document(TaiTabSet *tabs,
                                           const char *heading) {
  struct timespec deadline = deadline_after(timeout_seconds);
  TaiTabSetView view = read_view(tabs);
  while ((view.loading || !view.page) && before_deadline(deadline)) {
    pump_once(tabs);
    pause_briefly();
    view = read_view(tabs);
  }
  CHECK(!view.loading && view.page);
  CHECK(heading == NULL || contains_text(tai_page_root(view.page), heading));
  return view;
}

static bool contains_text(const TaiNode *node, const char *needle) {
  if (!node) return false;
  if (node->kind == TAI_TEXT && node->text && strstr(node->text, needle))
    return true;
  for (size_t index = 0; index < node->child_count; index++)
    if (contains_text(node->children[index], needle)) return true;
  return false;
}

static TaiNode *find_element(TaiNode *node, const char *tag,
                             const char *attribute, const char *value) {
  if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag) &&
      (!attribute || !strcmp(tai_map_get(&node->attributes, attribute)
                                ? tai_map_get(&node->attributes, attribute)
                                : "",
                            value)))
    return node;
  for (size_t index = 0; index < node->child_count; index++) {
    TaiNode *match = find_element(node->children[index], tag, attribute, value);
    if (match) return match;
  }
  return NULL;
}

typedef struct {
  const TaiNode *target;
  double x;
  double y;
  bool found;
} LinkPoint;

static bool find_link_point(const TaiLayoutItem *item, void *opaque) {
  LinkPoint *point = opaque;
  if (point->found ||
      (item->kind != TAI_LAYOUT_TEXT && item->kind != TAI_LAYOUT_BUTTON))
    return true;
  for (const TaiNode *node = item->node; node; node = node->parent) {
    if (node == point->target) {
      point->x = item->x + item->width / 2.0;
      point->y = item->y + item->height / 2.0;
      point->found = true;
      break;
    }
  }
  return true;
}

static TaiNavigationIntent *click_link(TaiPage *page, const char *href) {
  TaiNode *anchor = find_element(tai_page_root(page), "a", "href", href);
  CHECK(anchor);
  LinkPoint point = {.target = anchor};
  char *error = NULL;
  CHECK(tai_layout_visit(tai_page_layout(page), find_link_point, &point,
                         &error));
  CHECK(error == NULL && point.found);
  bool changed = false;
  CHECK(tai_page_activate_viewport(page, point.x, point.y, &changed, &error));
  CHECK(error == NULL);
  TaiNavigationIntent *intent = NULL;
  CHECK(tai_page_take_navigation_intent(page, &intent));
  CHECK(intent);
  return intent;
}

static void navigate_via_link(TaiTabSet *tabs, TaiPage *page,
                              const char *href, const char *expected_url) {
  TaiNavigationIntent *intent = click_link(page, href);
  CHECK(!strcmp(tai_navigation_intent_url(intent), expected_url));
  char *error = NULL;
  CHECK(tai_tabset_navigate(tabs, intent, &error));
  CHECK(error == NULL);
  tai_navigation_intent_destroy(intent);
  free(error);
}

static void assert_view_url(const TaiTabSetView *view, const char *url) {
  CHECK(view->url && !strcmp(view->url, url));
}

static bool close_enough(double left, double right) {
  return fabs(left - right) < 0.001;
}

static void assert_dimensions(const TaiTabSetView *view, double width,
                             double height) {
  CHECK(view->page);
  CHECK(close_enough(tai_page_viewport_width(view->page), width));
  CHECK(close_enough(tai_page_viewport_height(view->page), height));
}

static TaiTabSet *create_set(const char *home_url) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(
      default_css, false, home_url, &error);
  CHECK(tabs && error == NULL);
  free(error);
  return tabs;
}

static void start_set(TaiTabSet *tabs, const char *url) {
  char *error = NULL;
  CHECK(tai_tabset_start(tabs, url, 320.0, 180.0, &error));
  CHECK(error == NULL);
  free(error);
}

static void select_tab(TaiTabSet *tabs, size_t index) {
  CHECK(tai_tabset_select(tabs, index));
  TaiTabSetView view = read_view(tabs);
  CHECK(view.active_index == index);
}

static void pump_history(TaiTabSet *tabs, size_t index, const char *url,
                         size_t history_index) {
  TaiTabSetView view = pump_until_loading(tabs, false);
  CHECK(view.page);
  assert_view_url(&view, url);
  CHECK(view.history_count == 2);
  CHECK(view.history_index == history_index);
  CHECK(view.can_go_back == (history_index > 0));
  CHECK(view.can_go_forward == (history_index == 0));
  CHECK(view.active_index == index);
}

static void main_tabs_scenario(const char *base) {
  char initial_url[512], secondary_url[512], next_url[512];
  char css_url[512], fail_url[512], replace_url[512], replacement_url[512];
  CHECK(snprintf(initial_url, sizeof(initial_url), "%s/initial-delay", base) > 0);
  CHECK(snprintf(secondary_url, sizeof(secondary_url), "%s/secondary-delay", base) > 0);
  CHECK(snprintf(next_url, sizeof(next_url), "%s/first-next", base) > 0);
  CHECK(snprintf(css_url, sizeof(css_url), "%s/with-css", base) > 0);
  CHECK(snprintf(fail_url, sizeof(fail_url), "%s/later-fail", base) > 0);
  CHECK(snprintf(replace_url, sizeof(replace_url), "%s/replace-delay", base) > 0);
  CHECK(snprintf(replacement_url, sizeof(replacement_url),
                 "%s/replacement-fast", base) > 0);

  TaiTabSet *tabs = create_set(secondary_url);
  start_set(tabs, initial_url);
  CHECK(tai_tabset_new_tab(tabs, NULL));
  TaiTabSetView view = read_view(tabs);
  CHECK(view.tab_count == 2 && view.active_index == 1);
  CHECK(view.loading && view.page == NULL);
  assert_view_url(&view, secondary_url);
  CHECK(view.history_count == 1 && view.history_index == 0);

  select_tab(tabs, 0);
  view = read_view(tabs);
  CHECK(view.loading && view.page == NULL);
  CHECK(view.history_count == 1 && view.history_index == 0);
  assert_view_url(&view, initial_url);

  char *error = NULL;
  CHECK(tai_tabset_resize(tabs, 400.0, 240.0, &error));
  CHECK(error == NULL);
  free(error);
  print_checkpoint("DOCS_PENDING");

  /* Complete the background tab first. Pumping it must not change selection
   * or replace the still-pending initial tab's view. */
  select_tab(tabs, 1);
  view = await_active_document(tabs, "secondary-page");
  assert_view_url(&view, secondary_url);
  CHECK(view.history_count == 1 && view.history_index == 0);
  assert_dimensions(&view, 400.0, 240.0);
  print_checkpoint("SECONDARY_COMMITTED");

  select_tab(tabs, 0);
  view = await_active_document(tabs, "initial-page");
  assert_view_url(&view, initial_url);
  CHECK(view.history_count == 1 && view.history_index == 0);
  assert_dimensions(&view, 400.0, 240.0);

  /* History belongs to the first session only. */
  navigate_via_link(tabs, view.page, "/first-next", next_url);
  view = await_active_document(tabs, "first-next");
  assert_view_url(&view, next_url);
  CHECK(view.history_count == 2 && view.history_index == 1);
  CHECK(view.can_go_back && !view.can_go_forward);

  error = NULL;
  CHECK(tai_tabset_history(tabs, -1, &error));
  CHECK(error == NULL);
  free(error);
  pump_history(tabs, 0, initial_url, 0);
  error = NULL;
  CHECK(tai_tabset_history(tabs, 1, &error));
  CHECK(error == NULL);
  free(error);
  pump_history(tabs, 0, next_url, 1);

  /* The two pages retain separate document offsets through a resize. */
  view = read_view(tabs);
  double first_max = tai_page_max_scroll_y(view.page);
  CHECK(first_max > 100.0);
  CHECK(tai_page_set_scroll_y(view.page, first_max * 0.75));
  double first_scroll = tai_page_scroll_y(view.page);
  select_tab(tabs, 1);
  view = read_view(tabs);
  CHECK(view.history_count == 1 && view.history_index == 0);
  assert_view_url(&view, secondary_url);
  double second_max = tai_page_max_scroll_y(view.page);
  CHECK(second_max > 100.0);
  CHECK(tai_page_set_scroll_y(view.page, second_max * 0.45));
  double second_scroll = tai_page_scroll_y(view.page);
  CHECK(!close_enough(first_scroll, second_scroll));

  error = NULL;
  CHECK(tai_tabset_resize(tabs, 600.0, 400.0, &error));
  CHECK(error == NULL);
  free(error);
  select_tab(tabs, 0);
  view = read_view(tabs);
  assert_dimensions(&view, 600.0, 400.0);
  CHECK(close_enough(tai_page_scroll_y(view.page), first_scroll));
  CHECK(view.history_count == 2 && view.history_index == 1);
  select_tab(tabs, 1);
  view = read_view(tabs);
  assert_dimensions(&view, 600.0, 400.0);
  CHECK(close_enough(tai_page_scroll_y(view.page), second_scroll));
  CHECK(view.history_count == 1 && view.history_index == 0);

  /* External CSS keeps the active page's replacement pending. UI-owner
   * commands continue to run and the eventual page uses the latest viewport. */
  select_tab(tabs, 0);
  view = read_view(tabs);
  CHECK(tai_page_set_scroll_y(view.page, 0.0));
  navigate_via_link(tabs, view.page, "/with-css", css_url);
  view = read_view(tabs);
  CHECK(view.loading && view.page);
  assert_view_url(&view, css_url);
  CHECK(view.history_count == 3 && view.history_index == 2);
  CHECK(view.can_go_back && !view.can_go_forward);
  print_checkpoint("CSS_PENDING");

  select_tab(tabs, 1);
  view = read_view(tabs);
  CHECK(!view.loading && contains_text(tai_page_root(view.page), "secondary-page"));
  error = NULL;
  CHECK(tai_tabset_resize(tabs, 640.0, 360.0, &error));
  CHECK(error == NULL);
  free(error);
  select_tab(tabs, 0);
  view = read_view(tabs);
  CHECK(view.loading && contains_text(tai_page_root(view.page), "first-next"));
  print_checkpoint("CSS_RESIZE_DONE");

  view = await_active_document(tabs, "styled-page");
  assert_view_url(&view, css_url);
  CHECK(view.history_count == 3 && view.history_index == 2);
  assert_dimensions(&view, 640.0, 360.0);
  TaiNode *styled = find_element(tai_page_root(view.page), "p", "id", "styled");
  CHECK(styled && !strcmp(tai_map_get(&styled->style, "color"), "red"));

  /* A failed replacement on a tab that already has a page is intentionally
   * rolled back: page identity, visible URL, and history stay unchanged. */
  TaiPage *old_page = view.page;
  size_t old_count = view.history_count;
  size_t old_index = view.history_index;
  CHECK(tai_page_set_scroll_y(view.page, 25.0));
  double old_scroll = tai_page_scroll_y(view.page);
  char old_url[512];
  CHECK(snprintf(old_url, sizeof(old_url), "%s", view.url) > 0);
  error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, fail_url, &error));
  CHECK(error == NULL);
  free(error);
  view = read_view(tabs);
  CHECK(view.loading && view.page == old_page);
  CHECK(close_enough(tai_page_scroll_y(view.page), old_scroll));
  assert_view_url(&view, fail_url);
  CHECK(view.history_count == old_count + 1 &&
        view.history_index == old_index + 1);
  CHECK(view.can_go_back && !view.can_go_forward);
  view = await_active_document(tabs, NULL);
  CHECK(view.page == old_page);
  CHECK(close_enough(tai_page_scroll_y(view.page), old_scroll));
  assert_view_url(&view, old_url);
  CHECK(view.history_count == old_count && view.history_index == old_index);
  CHECK(contains_text(tai_page_root(view.page), "styled-page"));

  /* A failed link navigation follows the same later-navigation rollback as
   * the failed address navigation, preserving the forward history branch. */
  error = NULL;
  CHECK(tai_tabset_history(tabs, -1, &error));
  CHECK(error == NULL);
  free(error);
  view = pump_until_loading(tabs, false);
  assert_view_url(&view, next_url);
  CHECK(view.history_count == 3 && view.history_index == 1);
  CHECK(view.can_go_back && view.can_go_forward);
  TaiPage *next_page = view.page;
  navigate_via_link(tabs, next_page, "/later-fail", fail_url);
  view = read_view(tabs);
  CHECK(view.loading && view.page == next_page);
  assert_view_url(&view, fail_url);
  CHECK(view.history_count == 3 && view.history_index == 2);
  view = await_active_document(tabs, NULL);
  CHECK(view.page == next_page);
  assert_view_url(&view, next_url);
  CHECK(view.history_count == 3 && view.history_index == 1);

  /* Superseding a pending URL cancels its request. The replacement request's
   * Referer is the provisional URL, and releasing the canceled response later
   * must not overwrite the committed replacement page. */
  error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, replace_url, &error));
  CHECK(error == NULL);
  free(error);
  view = read_view(tabs);
  CHECK(view.loading && view.page == next_page);
  assert_view_url(&view, replace_url);
  print_checkpoint("REPLACE_PENDING");
  error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, replacement_url, &error));
  CHECK(error == NULL);
  free(error);
  view = await_active_document(tabs, "replacement-fast");
  assert_view_url(&view, replacement_url);
  CHECK(view.history_count == 3 && view.history_index == 2);
  print_checkpoint("REPLACED_COMMITTED");
  struct timespec late_deadline = deadline_after(1);
  while (before_deadline(late_deadline)) {
    pump_once(tabs);
    pause_briefly();
  }
  view = read_view(tabs);
  CHECK(!view.loading && contains_text(tai_page_root(view.page),
                                      "replacement-fast"));
  assert_view_url(&view, replacement_url);
  print_checkpoint("REPLACE_LATE_DONE");

  select_tab(tabs, 1);
  view = read_view(tabs);
  CHECK(!view.loading && view.history_count == 1 && view.history_index == 0);
  assert_view_url(&view, secondary_url);
  CHECK(contains_text(tai_page_root(view.page), "secondary-page"));
  tai_tabset_destroy(tabs);
}

static void initial_failure_scenario(const char *base) {
  char failure_url[512];
  CHECK(snprintf(failure_url, sizeof(failure_url), "%s/first-fail", base) > 0);
  TaiTabSet *tabs = create_set(failure_url);
  start_set(tabs, failure_url);
  TaiTabSetView view = await_active_document(tabs, "Network Error");
  assert_view_url(&view, failure_url);
  CHECK(view.history_count == 1 && view.history_index == 0);
  CHECK(!view.can_go_back && !view.can_go_forward);
  tai_tabset_destroy(tabs);
}

static void new_tab_failure_scenario(const char *base) {
  char good_url[512], failure_url[512];
  CHECK(snprintf(good_url, sizeof(good_url), "%s/secondary-delay", base) > 0);
  CHECK(snprintf(failure_url, sizeof(failure_url), "%s/first-fail", base) > 0);
  TaiTabSet *tabs = create_set(failure_url);
  start_set(tabs, good_url);
  TaiTabSetView view = await_active_document(tabs, "secondary-page");
  assert_view_url(&view, good_url);
  CHECK(view.history_count == 1 && view.history_index == 0);
  CHECK(tai_tabset_new_tab(tabs, NULL));
  view = await_active_document(tabs, "Network Error");
  assert_view_url(&view, failure_url);
  CHECK(view.history_count == 1 && view.history_index == 0);
  select_tab(tabs, 0);
  view = read_view(tabs);
  assert_view_url(&view, good_url);
  CHECK(view.history_count == 1 && view.history_index == 0);
  CHECK(contains_text(tai_page_root(view.page), "secondary-page"));
  tai_tabset_destroy(tabs);
}

static void history_failure_scenarios(const char *base) {
  char failure_url[512], stable_url[512], flaky_url[512];
  CHECK(snprintf(failure_url, sizeof(failure_url), "%s/first-fail", base) > 0);
  CHECK(snprintf(stable_url, sizeof(stable_url), "%s/history-stable", base) > 0);
  CHECK(snprintf(flaky_url, sizeof(flaky_url), "%s/flaky", base) > 0);

  /* Back to a committed URL that now fails leaves the current entry/page. */
  TaiTabSet *tabs = create_set(failure_url);
  start_set(tabs, failure_url);
  TaiTabSetView view = await_active_document(tabs, "Network Error");
  CHECK(view.history_count == 1 && view.history_index == 0);
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, stable_url, &error));
  CHECK(error == NULL);
  free(error);
  view = await_active_document(tabs, "history-stable");
  CHECK(view.history_count == 2 && view.history_index == 1);
  TaiPage *stable_page = view.page;
  error = NULL;
  CHECK(tai_tabset_history(tabs, -1, &error));
  CHECK(error == NULL);
  free(error);
  view = read_view(tabs);
  CHECK(view.loading && view.page == stable_page);
  assert_view_url(&view, failure_url);
  CHECK(view.history_count == 2 && view.history_index == 0);
  view = await_active_document(tabs, NULL);
  CHECK(view.page == stable_page);
  assert_view_url(&view, stable_url);
  CHECK(view.history_count == 2 && view.history_index == 1);
  CHECK(view.can_go_back && !view.can_go_forward);
  tai_tabset_destroy(tabs);

  /* A Forward target succeeds once, then fails on the next traversal. */
  tabs = create_set(stable_url);
  start_set(tabs, stable_url);
  view = await_active_document(tabs, "history-stable");
  error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, flaky_url, &error));
  CHECK(error == NULL);
  free(error);
  view = await_active_document(tabs, "flaky-page");
  CHECK(view.history_count == 2 && view.history_index == 1);
  error = NULL;
  CHECK(tai_tabset_history(tabs, -1, &error));
  CHECK(error == NULL);
  free(error);
  view = pump_until_loading(tabs, false);
  assert_view_url(&view, stable_url);
  CHECK(view.history_index == 0 && view.can_go_forward);
  stable_page = view.page;
  error = NULL;
  CHECK(tai_tabset_history(tabs, 1, &error));
  CHECK(error == NULL);
  free(error);
  view = read_view(tabs);
  CHECK(view.loading && view.page == stable_page);
  assert_view_url(&view, flaky_url);
  view = await_active_document(tabs, NULL);
  CHECK(view.page == stable_page);
  assert_view_url(&view, stable_url);
  CHECK(view.history_count == 2 && view.history_index == 0);
  CHECK(!view.can_go_back && view.can_go_forward);
  tai_tabset_destroy(tabs);
}

static void destroy_pending_scenario(const char *base) {
  char pending_url[512];
  CHECK(snprintf(pending_url, sizeof(pending_url), "%s/cancel-delay", base) > 0);
  TaiTabSet *tabs = create_set(pending_url);
  start_set(tabs, pending_url);
  TaiTabSetView view = read_view(tabs);
  CHECK(view.loading && view.page == NULL);
  assert_view_url(&view, pending_url);
  print_checkpoint("DESTROY_PENDING");
  tai_tabset_destroy(tabs);
  puts("DESTROYED_PENDING_LOAD");
  fflush(stdout);
}

static void tab_limit_scenario(void) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(
      default_css, false, "data:text/html,<p>home</p>", &error);
  CHECK(tabs && error == NULL);
  CHECK(tai_tabset_start(tabs, "data:text/html,<p>initial</p>",
                         800.0, 525.0, &error));
  CHECK(error == NULL);
  for (size_t index = 1; index < 25; index++) {
    CHECK(tai_tabset_new_tab(tabs, &error));
    CHECK(error == NULL);
    TaiTabSetView view = read_view(tabs);
    CHECK(view.tab_count == index + 1 && view.active_index == index);
  }
  CHECK(!tai_tabset_new_tab(tabs, &error));
  CHECK(error && strstr(error, "25"));
  free(error);
  TaiTabSetView view = read_view(tabs);
  CHECK(view.tab_count == 25 && view.active_index == 24);
  CHECK(tai_tabset_select(tabs, 0));
  CHECK(tai_tabset_select(tabs, 24));
  tai_tabset_destroy(tabs);
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  char base[256];
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  main_tabs_scenario(base);
  initial_failure_scenario(base);
  new_tab_failure_scenario(base);
  history_failure_scenarios(base);
  destroy_pending_scenario(base);
  tab_limit_scenario();
  puts("tab-set integration passed: pending routing, CSS, failures, history, "
       "resize/scroll isolation, and destroy cancellation");
  return EXIT_SUCCESS;
}
