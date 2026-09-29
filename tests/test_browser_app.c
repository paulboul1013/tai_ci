#define _POSIX_C_SOURCE 200809L
/* Two windows' tab sets sharing one TaiBrowserApp.
 *
 * Run by tests/browser_app_integration.py with the port of the 127.0.0.1
 * server in tests/new_window_fixture.py. Checks that tab sets stay
 * independent, that completions reach the right tab set even when only
 * another one pumps, that bookmarks and cookies are shared, and that a tab
 * set destroyed with a load in flight leaves the other window working. */
#include "tai/tabset.h"

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
static char base[128];

static char *url(const char *path) {
  static char buffers[8][256];
  static size_t next;
  char *buffer = buffers[next++ % 8];
  CHECK(snprintf(buffer, 256, "%s%s", base, path) > 0);
  return buffer;
}

static bool before(struct timespec deadline) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return now.tv_sec < deadline.tv_sec ||
         (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec);
}

static struct timespec deadline_after(int seconds) {
  struct timespec deadline;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
  deadline.tv_sec += seconds;
  return deadline;
}

static void pause_briefly(void) {
  const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
  (void)nanosleep(&pause, NULL);
}

static TaiTabSetView view_of(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  return view;
}

static bool pump(TaiTabSet *tabs) {
  bool changed = false;
  char *error = NULL;
  CHECK(tai_tabset_pump(tabs, &changed, &error));
  CHECK(error == NULL);
  return changed;
}

static bool contains_text(const TaiNode *node, const char *needle) {
  if (!node) return false;
  if (node->kind == TAI_TEXT && node->text && strstr(node->text, needle))
    return true;
  for (size_t index = 0; index < node->child_count; index++)
    if (contains_text(node->children[index], needle)) return true;
  return false;
}

static bool shows(TaiTabSet *tabs, const char *heading) {
  TaiTabSetView view = view_of(tabs);
  return !view.loading && view.page &&
         contains_text(tai_page_root(view.page), heading);
}

static void await_document(TaiTabSet *tabs, const char *heading) {
  struct timespec deadline = deadline_after(8);
  while (!shows(tabs, heading) && before(deadline)) {
    pump(tabs);
    pause_briefly();
  }
  CHECK(shows(tabs, heading));
}

static TaiTabSet *window_in(TaiBrowserApp *app, const char *path) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_in_app(app, &error);
  CHECK(tabs && !error);
  CHECK(tai_tabset_start(tabs, url(path), 800, 532, &error));
  return tabs;
}

static void navigate(TaiTabSet *tabs, const char *path) {
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, url(path), &error));
  CHECK(!error);
}

/* A completion for one window is routed to it even when only the other
 * window pumps, and it is committed by that window's next pump. */
static void routing_scenario(TaiBrowserApp *app) {
  TaiTabSet *first = window_in(app, "/a");
  TaiTabSet *second = window_in(app, "/b");
  await_document(first, "page-a");
  /* A local page loads in milliseconds; keep pumping only the first window
   * long enough for page-b's completion to be routed to the second. */
  struct timespec deadline = deadline_after(1);
  while (before(deadline)) {
    pump(first);
    pause_briefly();
  }
  TaiTabSetView second_view = view_of(second);
  CHECK(second_view.loading && second_view.page == NULL);
  CHECK(pump(second));
  CHECK(shows(second, "page-b"));
  CHECK(shows(first, "page-a"));

  /* Independent tabs and history. */
  char *error = NULL;
  CHECK(tai_tabset_new_tab(second, &error));
  await_document(second, "home");
  navigate(first, "/c");
  await_document(first, "page-c");
  TaiTabSetView a = view_of(first), b = view_of(second);
  CHECK(a.tab_count == 1 && a.history_count == 2 && a.history_index == 1);
  CHECK(b.tab_count == 2 && b.active_index == 1 && b.history_count == 1);
  CHECK(!strcmp(a.url, url("/c")) && !strcmp(b.url, url("/home")));
  tai_tabset_destroy(first);
  tai_tabset_destroy(second);
}

static void bookmarks_scenario(TaiBrowserApp *app) {
  TaiTabSet *first = window_in(app, "/a");
  TaiTabSet *second = window_in(app, "/c");
  await_document(first, "page-a");
  await_document(second, "page-c");
  bool bookmarked = false;
  char *error = NULL;
  CHECK(tai_tabset_toggle_bookmark(second, &bookmarked, &error));
  CHECK(bookmarked && view_of(second).bookmarked);
  CHECK(!view_of(first).bookmarked);
  navigate(first, "/c");
  await_document(first, "page-c");
  CHECK(view_of(first).bookmarked);
  CHECK(tai_tabset_open_bookmarks(first, &error));
  await_document(first, url("/c"));
  tai_tabset_destroy(first);
  tai_tabset_destroy(second);
}

static void cookies_scenario(TaiBrowserApp *app) {
  TaiTabSet *first = window_in(app, "/cookie-set");
  await_document(first, "cookie-set");
  TaiTabSet *second = window_in(app, "/cookie-check");
  await_document(second, "cookie=[nw=shared]");
  tai_tabset_destroy(second);
  tai_tabset_destroy(first);
}

/* Destroying a window whose load is in flight must not disturb another
 * window; the cancelled completion is released by the survivor's pump. */
static void destroy_pending_scenario(TaiBrowserApp *app) {
  TaiTabSet *first = window_in(app, "/a");
  TaiTabSet *second = window_in(app, "/b");
  await_document(first, "page-a");
  await_document(second, "page-b");
  navigate(first, "/delay");
  /* Let the request reach the held /delay gate so the load is in flight. */
  struct timespec deadline = deadline_after(1);
  while (before(deadline)) { pump(first); pause_briefly(); }
  CHECK(view_of(first).loading);
  tai_tabset_destroy(first);
  navigate(second, "/c");
  await_document(second, "page-c");
  for (int spin = 0; spin < 50; spin++) { pump(second); pause_briefly(); }
  CHECK(shows(second, "page-c"));
  tai_tabset_destroy(second);
  /* A window created afterwards gets fresh tab IDs and loads normally. */
  TaiTabSet *third = window_in(app, "/a");
  await_document(third, "page-a");
  tai_tabset_destroy(third);
}

/* A completion already routed into a tab set's inbox (and still its tab's
 * active task) is released when that tab set is destroyed unpumped. */
static void destroy_with_inbox_scenario(TaiBrowserApp *app) {
  TaiTabSet *first = window_in(app, "/a");
  TaiTabSet *second = window_in(app, "/b");
  struct timespec deadline = deadline_after(1);
  while (before(deadline)) { pump(first); pause_briefly(); }
  CHECK(shows(first, "page-a"));
  CHECK(view_of(second).loading);
  tai_tabset_destroy(second);
  navigate(first, "/c");
  await_document(first, "page-c");
  tai_tabset_destroy(first);
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  char *error = NULL;
  TaiBrowserApp *app = tai_browser_app_create_with_home_url(
      default_css, false, url("/home"), &error);
  CHECK(app && !error);
  routing_scenario(app);
  bookmarks_scenario(app);
  cookies_scenario(app);
  destroy_pending_scenario(app);
  destroy_with_inbox_scenario(app);
  /* The app outlives every tab set; destroying it with a pending load in a
   * window that is already gone joins the loader cleanly. */
  TaiTabSet *last = window_in(app, "/delay");
  struct timespec deadline = deadline_after(1);
  while (before(deadline)) { pump(last); pause_briefly(); }
  tai_tabset_destroy(last);
  tai_browser_app_destroy(app);
  CHECK(tai_tabset_create_in_app(NULL, &error) == NULL && error);
  free(error);
  puts("browser app: routing, bookmarks, cookies and pending destroy ok");
  return 0;
}
