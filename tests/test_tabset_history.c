#define _POSIX_C_SOURCE 200809L
/* Native half of the chrome/history oracle comparison. Driven by
 * tests/history_integration.py, which serves tests/history_fixture.py and
 * compares every STATE line with tests/fixtures/history_oracle.json.
 *
 * Protocol (one line each way): this program writes a command and waits for
 * "ok" on stdin before continuing.
 *   MARK                        start a request window
 *   SEEN <path>                 wait until <path> was requested since MARK
 *   RELEASE|RESET <path>        open or re-arm a gated document
 *   FAIL|UNFAIL <path>          make a document fail or succeed again
 *   STATE <scenario> <step> <requests> <json>
 *                               record a state; requests=1 attaches the
 *                               server requests seen since MARK
 * Address-draft behaviour lives in the presentation layer and is checked by
 * tests/test_presentation.c instead. */
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

static char base[128];
static char *default_css;
/* The oracle tab is 800 CSS pixels wide; its height only bounds the fragment
 * scroll clamp, which the fixture pages never reach. */
static const double viewport_width = 800.0;
static const double viewport_height = 500.0;

static void command(const char *verb, const char *argument) {
  printf("%s%s%s\n", verb, argument ? " " : "", argument ? argument : "");
  fflush(stdout);
  char reply[16];
  CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
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

/* The first <h1>'s text with surrounding whitespace removed, like the probe. */
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

static TaiTabSetView settle(TaiTabSet *tabs, const char *expected_heading) {
  struct timespec deadline;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
  deadline.tv_sec += 8;
  TaiTabSetView view = read_view(tabs);
  for (;;) {
    if (!view.loading && view.page) break;
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    CHECK(now.tv_sec < deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
    pump_once(tabs);
    pause_briefly();
    view = read_view(tabs);
  }
  char text[256];
  heading(view.page, text, sizeof(text));
  if (expected_heading && strcmp(text, expected_heading)) {
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
         "\"can_go_forward\": %s, \"scroll\": %.3f, \"secure\": %s, "
         "\"address\": ",
         view.history_index, view.can_go_back ? "true" : "false",
         view.can_go_forward ? "true" : "false",
         view.page ? tai_page_scroll_y(view.page) : 0.0,
         view.secure ? "true" : "false");
  /* With no draft, the address field shows the view URL. */
  json_string(stdout, view.url);
  printf("}\n");
  fflush(stdout);
  char reply[16];
  CHECK(fgets(reply, sizeof(reply), stdin) && !strcmp(reply, "ok\n"));
}

static TaiTabSet *open_set(const char *path, const char *home_path,
                           const char *expected_heading) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(
      default_css, false, url_for(home_path ? home_path : "/other"), &error);
  CHECK(tabs && error == NULL);
  CHECK(tai_tabset_start(tabs, url_for(path), viewport_width,
                         viewport_height, &error));
  CHECK(error == NULL);
  settle(tabs, expected_heading);
  return tabs;
}

static void go(TaiTabSet *tabs, const char *path) {
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, url_for(path), &error));
  CHECK(error == NULL);
}

static void load(TaiTabSet *tabs, const char *path, const char *heading_text) {
  go(tabs, path);
  settle(tabs, heading_text);
}

static void traverse(TaiTabSet *tabs, int direction) {
  CHECK(tai_tabset_history_available(tabs, direction));
  char *error = NULL;
  CHECK(tai_tabset_history(tabs, direction, &error));
  CHECK(error == NULL);
}

typedef struct {
  const TaiNode *target;
  double x, y;
  bool found;
} TargetPoint;

static bool find_target_point(const TaiLayoutItem *item, void *opaque) {
  TargetPoint *point = opaque;
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

static void activate(TaiPage *page, const char *tag) {
  const TaiNode *target = find_tag(tai_page_root(page), tag);
  CHECK(target);
  TargetPoint point = {.target = target};
  char *error = NULL;
  CHECK(tai_layout_visit(tai_page_layout(page), find_target_point, &point,
                         &error));
  CHECK(error == NULL && point.found);
  bool changed = false;
  CHECK(tai_page_activate_viewport(page, point.x,
                                   point.y - tai_page_scroll_y(page),
                                   &changed, &error));
  CHECK(error == NULL);
}

/* Mirrors the tabbed presentation loop: take the page's intent or fragment
 * change and hand it to the tab set. */
static void consume_page_action(TaiTabSet *tabs, TaiPage *page) {
  char *error = NULL;
  char *fragment_url = NULL;
  CHECK(tai_page_take_fragment_change(page, &fragment_url));
  if (fragment_url) {
    bool recorded = tai_tabset_record_fragment(tabs, fragment_url, &error);
    CHECK(recorded && error == NULL);
    tai_page_finish_fragment_change(page, recorded);
    free(fragment_url);
  }
  TaiNavigationIntent *intent = NULL;
  CHECK(tai_page_take_navigation_intent(page, &intent));
  if (intent) {
    CHECK(tai_tabset_navigate(tabs, intent, &error));
    CHECK(error == NULL);
    tai_navigation_intent_destroy(intent);
  }
}

static void basic_truncation(void) {
  const char *s = "basic_truncation";
  command("MARK", NULL);
  TaiTabSet *tabs = open_set("/a", NULL, "page-a");
  state(tabs, s, "a", false);
  load(tabs, "/b", "page-b");
  state(tabs, s, "b", false);
  load(tabs, "/c", "page-c");
  state(tabs, s, "c", false);
  traverse(tabs, -1);
  settle(tabs, "page-b");
  state(tabs, s, "back_b", false);
  traverse(tabs, -1);
  settle(tabs, "page-a");
  state(tabs, s, "back_a", false);
  traverse(tabs, 1);
  settle(tabs, "page-b");
  state(tabs, s, "forward_b", false);
  traverse(tabs, -1);
  settle(tabs, "page-a");
  load(tabs, "/d", "page-d");
  state(tabs, s, "truncated_d", true);
  tai_tabset_destroy(tabs);
}

static void pending(void) {
  const char *s = "pending";
  TaiTabSet *tabs = open_set("/a", NULL, "page-a");
  command("MARK", NULL);
  go(tabs, "/delay-b");
  command("SEEN", "/delay-b");
  pump_once(tabs);
  state(tabs, s, "pending", false);
  command("RELEASE", "/delay-b");
  settle(tabs, "delay-b");
  state(tabs, s, "loaded", true);
  command("RESET", "/delay-b");
  tai_tabset_destroy(tabs);
}

/* Decision 1: Back while a navigation is pending drops the pending entry. */
static void pending_back(void) {
  const char *s = "pending_back";
  TaiTabSet *tabs = open_set("/a", NULL, "page-a");
  command("MARK", NULL);
  go(tabs, "/delay-b");
  command("SEEN", "/delay-b");
  traverse(tabs, -1);
  settle(tabs, "page-a");
  state(tabs, s, "back", false);
  command("RELEASE", "/delay-b");
  command("SEEN", "/a");
  for (int i = 0; i < 20; i++) {
    pump_once(tabs);
    pause_briefly();
  }
  state(tabs, s, "released", true);
  command("RESET", "/delay-b");
  tai_tabset_destroy(tabs);
}

static void failures(void) {
  const char *s = "failures";
  TaiTabSet *tabs = open_set("/a", NULL, "page-a");
  load(tabs, "/b", "page-b");
  load(tabs, "/fail", "Network Error");
  state(tabs, s, "after_b", false);
  traverse(tabs, -1);
  settle(tabs, "page-b");
  state(tabs, s, "after_b_back", false);
  tai_tabset_destroy(tabs);

  tabs = open_set("/a", NULL, "page-a");
  load(tabs, "/b", "page-b");
  load(tabs, "/c", "page-c");
  traverse(tabs, -1);
  settle(tabs, "page-b");
  load(tabs, "/fail", "Network Error");
  state(tabs, s, "from_middle", false);
  tai_tabset_destroy(tabs);

  tabs = open_set("/a", NULL, "page-a");
  load(tabs, "/e", "page-e");
  traverse(tabs, -1);
  settle(tabs, "page-a");
  command("FAIL", "/e");
  traverse(tabs, 1);
  settle(tabs, "Network Error");
  state(tabs, s, "forward_to_failing", false);
  command("UNFAIL", "/e");
  traverse(tabs, -1);
  settle(tabs, "page-a");
  state(tabs, s, "back_from_error", false);
  command("FAIL", "/a");
  traverse(tabs, 1);
  settle(tabs, "page-e");
  traverse(tabs, -1);
  settle(tabs, "Network Error");
  state(tabs, s, "back_to_failing", false);
  command("UNFAIL", "/a");
  tai_tabset_destroy(tabs);
}

static void same_page_fragment(void) {
  const char *s = "same_page_fragment";
  TaiTabSet *tabs = open_set("/frag", NULL, "frag");
  command("MARK", NULL);
  TaiPage *page = read_view(tabs).page;
  activate(page, "a");
  consume_page_action(tabs, page);
  state(tabs, s, "clicked", true);
  command("MARK", NULL);
  traverse(tabs, -1);
  settle(tabs, "frag");
  state(tabs, s, "back", true);
  command("MARK", NULL);
  traverse(tabs, 1);
  settle(tabs, "frag");
  state(tabs, s, "forward", true);
  tai_tabset_destroy(tabs);
}

static void cross_page_fragment(void) {
  const char *s = "cross_page_fragment";
  TaiTabSet *tabs = open_set("/a", NULL, "page-a");
  load(tabs, "/frag#target", "frag");
  state(tabs, s, "target", false);
  load(tabs, "/b", "page-b");
  load(tabs, "/frag#missing", "frag");
  state(tabs, s, "missing", false);
  tai_tabset_destroy(tabs);
}

static void post_traversal(void) {
  const char *s = "post_traversal";
  TaiTabSet *tabs = open_set("/form", NULL, "form");
  command("MARK", NULL);
  TaiPage *page = read_view(tabs).page;
  activate(page, "button");
  consume_page_action(tabs, page);
  settle(tabs, "posted");
  state(tabs, s, "posted", false);
  load(tabs, "/c", "page-c");
  traverse(tabs, -1);
  settle(tabs, "posted");
  state(tabs, s, "back", false);
  traverse(tabs, 1);
  settle(tabs, "page-c");
  state(tabs, s, "forward", true);
  tai_tabset_destroy(tabs);
}

static void cross_tab(void) {
  const char *s = "cross_tab";
  TaiTabSet *tabs = open_set("/a", "/d", "page-a");
  load(tabs, "/b", "page-b");
  load(tabs, "/c", "page-c");
  char *error = NULL;
  CHECK(tai_tabset_new_tab(tabs, &error));
  CHECK(error == NULL);
  settle(tabs, "page-d");
  load(tabs, "/e", "page-e");
  command("MARK", NULL);
  go(tabs, "/delay-c");
  command("SEEN", "/delay-c");
  pump_once(tabs);
  state(tabs, s, "second_pending", false);
  CHECK(tai_tabset_select(tabs, 0));
  state(tabs, s, "first_active", false);
  CHECK(tai_tabset_select(tabs, 1));
  state(tabs, s, "second_again", false);
  command("RELEASE", "/delay-c");
  settle(tabs, "delay-c");
  traverse(tabs, -1);
  settle(tabs, "page-e");
  state(tabs, s, "second_back", false);
  CHECK(tai_tabset_select(tabs, 0));
  state(tabs, s, "first_after_second_back", false);
  command("RESET", "/delay-c");
  tai_tabset_destroy(tabs);
}

int main(int argc, char **argv) {
  CHECK(argc == 3);
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  size_t length = 0;
  default_css = tai_read_file(argv[2], &length);
  CHECK(default_css);
  basic_truncation();
  pending();
  pending_back();
  failures();
  same_page_fragment();
  cross_page_fragment();
  post_traversal();
  cross_tab();
  free(default_css);
  puts("DONE");
  fflush(stdout);
  return EXIT_SUCCESS;
}
