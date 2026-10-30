#define _POSIX_C_SOURCE 200809L
/* Native half of the stale-completion check (docs/stale-callback-plan.md).
 * Driven by tests/stale_integration.py, which serves tests/stale_fixture.py
 * and compares every STATE line with tests/fixtures/stale_oracle.json.
 *
 * Protocol (one line each way): this program writes a command and waits for
 * "ok" on stdin before continuing.
 *   MARK                  start a request window
 *   SEEN <path>           wait until <path> was requested since MARK
 *   SERVED <path>         wait until the server finished answering <path>
 *   RELEASE|RESET <path>  open or re-arm a gated path
 *   STATE <scenario> <step> <requests> <json>
 *                         record a state; requests=1 attaches the paths the
 *                         server received since MARK
 *   VISITED <commit> <quiet> <restyle>
 *                         native-only visited-snapshot observation (G4) */
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

static char base[128];
static char *default_css;
static const double viewport_width = 800.0;
static const double viewport_height = 500.0;

static void wait_ok(void) {
  char reply[16];
  CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
}

static void command(const char *verb, const char *argument) {
  printf("%s%s%s\n", verb, argument ? " " : "", argument ? argument : "");
  fflush(stdout);
  wait_ok();
}

static const char *url_for(const char *path) {
  static char buffer[4][256];
  static int next;
  char *url = buffer[next++ % 4];
  CHECK(snprintf(url, sizeof(buffer[0]), "%s%s", base, path) > 0);
  return url;
}

static void pause_briefly(void) {
  const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
  (void)nanosleep(&pause, NULL);
}

static double monotonic_seconds(void) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static TaiTabSetView read_view(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  return view;
}

static void pump_once(TaiTabSet *tabs) {
  bool changed = false;
  char *error = NULL;
  CHECK(tai_tabset_pump(tabs, &changed, &error));
  CHECK(error == NULL);
  CHECK(tai_tabset_run_tasks(tabs, tai_js_clock(), 32, &changed, NULL,
                             &error));
  CHECK(error == NULL);
}

/* After a late response was written, give the loader and this thread time to
 * (wrongly) apply it. Absence cannot be awaited, so this is a bounded window;
 * ordering itself always comes from the server's gates. */
static void quiet(TaiTabSet *tabs, double seconds) {
  double deadline = monotonic_seconds() + seconds;
  while (monotonic_seconds() < deadline) {
    pump_once(tabs);
    pause_briefly();
  }
}

static void append_text(const TaiNode *node, char *out, size_t size) {
  if (node->kind == TAI_TEXT && node->text) {
    size_t used = strlen(out);
    CHECK(snprintf(out + used, size - used, "%s", node->text) >= 0);
  }
  for (size_t index = 0; index < node->child_count; index++)
    append_text(node->children[index], out, size);
}

static const TaiNode *find_tag(const TaiNode *node, const char *tag) {
  if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    const TaiNode *match = find_tag(node->children[index], tag);
    if (match) return match;
  }
  return NULL;
}

static void heading(const TaiPage *page, char *out, size_t size) {
  out[0] = '\0';
  const TaiNode *h1 = page ? find_tag(tai_page_root(page), "h1") : NULL;
  if (!h1) return;
  append_text(h1, out, size);
  size_t start = strspn(out, " \t\r\n");
  memmove(out, out + start, strlen(out + start) + 1);
  size_t length = strlen(out);
  while (length && strchr(" \t\r\n", out[length - 1])) out[--length] = '\0';
}

static bool has_heading(const TaiPage *page, const char *expected) {
  char text[256];
  heading(page, text, sizeof(text));
  return !strcmp(text, expected);
}

static TaiTabSetView settle(TaiTabSet *tabs, const char *expected_heading) {
  double deadline = monotonic_seconds() + 8.0;
  TaiTabSetView view = read_view(tabs);
  while (view.loading || !view.page) {
    CHECK(monotonic_seconds() < deadline);
    pump_once(tabs);
    pause_briefly();
    view = read_view(tabs);
  }
  if (!has_heading(view.page, expected_heading)) {
    char text[256];
    heading(view.page, text, sizeof(text));
    fprintf(stderr, "expected heading %s, got %s\n", expected_heading, text);
    exit(EXIT_FAILURE);
  }
  return view;
}

static void json_string(FILE *out, const char *value) {
  fputc('"', out);
  for (const char *p = value; *p; p++) {
    if (*p == '"' || *p == '\\') fputc('\\', out);
    fputc(*p, out);
  }
  fputc('"', out);
}

static void state(TaiTabSet *tabs, const char *scenario, const char *step,
                  bool requests) {
  TaiTabSetView view = read_view(tabs);
  char text[256];
  heading(view.page, text, sizeof(text));
  printf("STATE %s %s %d {\"url\": ", scenario, step, requests ? 1 : 0);
  json_string(stdout, view.url);
  printf(", \"heading\": ");
  json_string(stdout, text);
  printf(", \"history\": [");
  for (size_t index = 0; index < view.history_count; index++) {
    char *url = tai_tabset_history_url(tabs, index);
    CHECK(url);
    if (index) printf(", ");
    json_string(stdout, url);
    free(url);
  }
  printf("], \"history_index\": %zu, \"can_go_back\": %s, "
         "\"can_go_forward\": %s}\n",
         view.history_index, view.can_go_back ? "true" : "false",
         view.can_go_forward ? "true" : "false");
  fflush(stdout);
  wait_ok();
}

static TaiTabSet *open_set(void) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(
      default_css, false, url_for("/start"), &error);
  CHECK(tabs && error == NULL);
  CHECK(tai_tabset_start(tabs, url_for("/start"), viewport_width,
                         viewport_height, &error));
  CHECK(error == NULL);
  settle(tabs, "start");
  return tabs;
}

static void go(TaiTabSet *tabs, const char *path) {
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, url_for(path), &error));
  CHECK(error == NULL);
}

static void arrive_late(TaiTabSet *tabs, const char *path) {
  command("RELEASE", path);
  command("SERVED", path);
  quiet(tabs, 0.5);
}

/* A held subresource of a superseded document: the replacement must commit
 * while the gate is still closed, and the late resource must change nothing
 * once it arrives. */
static void subresource_late(const char *s, const char *document,
                             const char *resource, const char *step) {
  TaiTabSet *tabs = open_set();
  TaiPage *start = read_view(tabs).page;
  command("MARK", NULL);
  go(tabs, document);
  command("SEEN", resource);
  TaiTabSetView view = read_view(tabs);
  CHECK(view.loading && view.page == start);
  go(tabs, "/b");
  view = settle(tabs, "page-b");
  TaiPage *committed = view.page;
  size_t history_count = view.history_count;
  size_t history_index = view.history_index;
  state(tabs, s, "b_committed", true);
  arrive_late(tabs, resource);
  view = read_view(tabs);
  CHECK(!view.loading && view.page == committed);
  CHECK(view.history_count == history_count &&
        view.history_index == history_index);
  CHECK(has_heading(view.page, "page-b"));
  state(tabs, s, step, true);
  command("RESET", resource);
  tai_tabset_destroy(tabs);
}

static TaiTabSet *three_pending(const char *s, TaiPage **start) {
  TaiTabSet *tabs = open_set();
  *start = read_view(tabs).page;
  command("MARK", NULL);
  go(tabs, "/slow-a");
  command("SEEN", "/slow-a");
  go(tabs, "/slow-b");
  command("SEEN", "/slow-b");
  go(tabs, "/slow-c");
  command("SEEN", "/slow-c");
  pump_once(tabs);
  state(tabs, s, "pending", false);
  return tabs;
}

static void still_pending(TaiTabSet *tabs, const TaiPage *start) {
  TaiTabSetView view = read_view(tabs);
  CHECK(view.loading && view.page == start);
  CHECK(has_heading(view.page, "start"));
}

static void reset_slow_documents(void) {
  command("RESET", "/slow-a");
  command("RESET", "/slow-b");
  command("RESET", "/slow-c");
}

static void out_of_order_oldest_first(void) {
  const char *s = "out_of_order_oldest_first";
  TaiPage *start = NULL;
  TaiTabSet *tabs = three_pending(s, &start);
  arrive_late(tabs, "/slow-a");
  still_pending(tabs, start);
  state(tabs, s, "a_released", false);
  arrive_late(tabs, "/slow-b");
  still_pending(tabs, start);
  state(tabs, s, "b_released", false);
  command("RELEASE", "/slow-c");
  settle(tabs, "slow-c");
  state(tabs, s, "c_committed", true);
  reset_slow_documents();
  tai_tabset_destroy(tabs);
}

static void out_of_order_newest_first(void) {
  const char *s = "out_of_order_newest_first";
  TaiPage *start = NULL;
  TaiTabSet *tabs = three_pending(s, &start);
  command("RELEASE", "/slow-c");
  TaiTabSetView view = settle(tabs, "slow-c");
  TaiPage *committed = view.page;
  size_t history_count = view.history_count;
  state(tabs, s, "c_committed", false);
  arrive_late(tabs, "/slow-a");
  arrive_late(tabs, "/slow-b");
  view = read_view(tabs);
  CHECK(!view.loading && view.page == committed &&
        view.history_count == history_count);
  state(tabs, s, "a_b_released", true);
  reset_slow_documents();
  tai_tabset_destroy(tabs);
}

static bool link_visited(TaiTabSet *tabs) {
  const TaiNode *link = find_tag(tai_page_root(read_view(tabs).page), "a");
  CHECK(link);
  return link->visited;
}

/* G4: the visited set is copied when the request is sent. Tab 0 requests
 * /links; while it is held, tab 1 commits /x. Report what tab 0's link to
 * /x shows at commit, after more pumping, and after an explicit restyle. */
static void visited_snapshot(void) {
  TaiTabSet *tabs = open_set();
  command("MARK", NULL);
  go(tabs, "/links");
  command("SEEN", "/links");
  char *error = NULL;
  CHECK(tai_tabset_new_tab(tabs, &error));
  CHECK(error == NULL);
  settle(tabs, "start");
  go(tabs, "/x");
  settle(tabs, "page-x");
  CHECK(tai_tabset_select(tabs, 0));
  command("RELEASE", "/links");
  TaiTabSetView view = settle(tabs, "links");
  bool at_commit = link_visited(tabs);
  quiet(tabs, 0.2);
  CHECK(read_view(tabs).page == view.page);
  bool after_quiet = link_visited(tabs);
  CHECK(tai_page_restyle(view.page, &error));
  CHECK(error == NULL);
  bool after_restyle = link_visited(tabs);
  printf("VISITED %d %d %d\n", at_commit, after_quiet, after_restyle);
  fflush(stdout);
  wait_ok();
  command("RESET", "/links");
  tai_tabset_destroy(tabs);
}

int main(int argc, char **argv) {
  CHECK(argc == 3);
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  size_t length = 0;
  default_css = tai_read_file(argv[2], &length);
  CHECK(default_css);
  subresource_late("css_late", "/css-a", "/slow.css", "css_released");
  subresource_late("js_late", "/js-a", "/slow.js", "js_released");
  out_of_order_oldest_first();
  out_of_order_newest_first();
  visited_snapshot();
  free(default_css);
  puts("DONE");
  fflush(stdout);
  return EXIT_SUCCESS;
}
