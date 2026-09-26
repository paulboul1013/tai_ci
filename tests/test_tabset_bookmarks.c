#define _POSIX_C_SOURCE 200809L
#include "tai/tabset.h"

#include <stdio.h>
#include <unistd.h>
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
    "html {display:block} body {display:block} h1 {display:block} "
    "p {display:block} ul {display:block} li {display:block} a {display:block}";

static struct timespec deadline_after(int seconds) {
  struct timespec value;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
  value.tv_sec += seconds;
  return value;
}

static bool before_deadline(struct timespec deadline) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return now.tv_sec < deadline.tv_sec ||
         (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec);
}

static void pause_briefly(void) {
  const struct timespec duration = {.tv_sec = 0, .tv_nsec = 1000000L};
  (void)nanosleep(&duration, NULL);
}

static TaiTabSetView view_of(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  return view;
}

static void pump_once(TaiTabSet *tabs) {
  char *error = NULL;
  bool changed = false;
  CHECK(tai_tabset_pump(tabs, &changed, &error));
  CHECK(error == NULL);
  free(error);
}

static TaiTabSetView wait_document(TaiTabSet *tabs, const char *url) {
  const struct timespec deadline = deadline_after(8);
  TaiTabSetView view = view_of(tabs);
  while ((view.loading || !view.page || strcmp(view.url, url)) &&
         before_deadline(deadline)) {
    pump_once(tabs);
    pause_briefly();
    view = view_of(tabs);
  }
  CHECK(!view.loading && view.page && !strcmp(view.url, url));
  return view;
}

static void navigate_address(TaiTabSet *tabs, const char *url) {
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, url, &error));
  CHECK(error == NULL);
  free(error);
}

static void open_list(TaiTabSet *tabs) {
  char *error = NULL;
  CHECK(tai_tabset_open_bookmarks(tabs, &error));
  CHECK(error == NULL);
  free(error);
}

static void expect_toggle_rejected(TaiTabSet *tabs) {
  bool changed = true;
  char *error = NULL;
  CHECK(!tai_tabset_toggle_bookmark(tabs, &changed, &error));
  CHECK(error != NULL);
  CHECK(changed);
  free(error);
}

static void toggle_to(TaiTabSet *tabs, bool expected) {
  bool bookmarked = !expected;
  char *error = NULL;
  CHECK(tai_tabset_toggle_bookmark(tabs, &bookmarked, &error));
  CHECK(error == NULL);
  CHECK(bookmarked == expected);
  free(error);
  CHECK(view_of(tabs).bookmarked == expected);
}

static bool append_text(const TaiNode *node, char *out, size_t capacity,
                        size_t *length) {
  if (node->kind == TAI_TEXT && node->text) {
    size_t amount = strlen(node->text);
    if (amount >= capacity - *length) return false;
    memcpy(out + *length, node->text, amount + 1);
    *length += amount;
  }
  for (size_t index = 0; index < node->child_count; index++)
    if (!append_text(node->children[index], out, capacity, length)) return false;
  return true;
}

static bool text_contains(const TaiNode *node, const char *needle) {
  char text[2048] = {0};
  size_t length = 0;
  CHECK(append_text(node, text, sizeof(text), &length));
  return strstr(text, needle) != NULL;
}

typedef struct {
  TaiNode *items[4];
  size_t count;
  bool unexpected_tag;
} Anchors;

static void collect_anchors(TaiNode *node, Anchors *anchors) {
  if (node->kind == TAI_ELEMENT) {
    if (!strcmp(node->tag, "a")) {
      CHECK(anchors->count < sizeof(anchors->items) / sizeof(anchors->items[0]));
      anchors->items[anchors->count++] = node;
    }
    if (!strcmp(node->tag, "tag")) anchors->unexpected_tag = true;
  }
  for (size_t index = 0; index < node->child_count; index++)
    collect_anchors(node->children[index], anchors);
}

static void assert_anchor(TaiNode *node, const char *url) {
  const char *href = tai_map_get(&node->attributes, "href");
  char label[1024] = {0};
  size_t length = 0;
  CHECK(append_text(node, label, sizeof(label), &length));
  if (!href || strcmp(href, url) || strcmp(label, url))
    fprintf(stderr, "bookmark link: expected %s, href %s, text %s\n", url,
            href ? href : "(null)", label);
  CHECK(href && !strcmp(href, url));
  CHECK(!strcmp(label, url));
}

typedef struct {
  const TaiNode *target;
  double x;
  double y;
  bool found;
} LinkPoint;

static bool find_link_point(const TaiLayoutItem *item, void *opaque) {
  LinkPoint *point = opaque;
  if (point->found || item->kind != TAI_LAYOUT_TEXT) return true;
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

static void click_saved_link(TaiTabSet *tabs, TaiPage *page, TaiNode *anchor,
                             const char *expected_url) {
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
  CHECK(!strcmp(tai_navigation_intent_url(intent), expected_url));
  CHECK(tai_tabset_navigate(tabs, intent, &error));
  CHECK(error == NULL);
  tai_navigation_intent_destroy(intent);
  free(error);
}

static void checkpoint(const char *name) {
  puts(name);
  fflush(stdout);
  int character = getchar();
  CHECK(character == '\n' || character == '\r');
}

static TaiTabSet *open_persistent(const char *url) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create(default_css, false, &error);
  if (!tabs || error)
    fprintf(stderr, "tai_tabset_create: %s\n", error ? error : "(no error)");
  CHECK(tabs && error == NULL);
  CHECK(tai_tabset_start(tabs, url, 800.0, 525.0, &error));
  CHECK(error == NULL);
  (void)wait_document(tabs, url);
  return tabs;
}

static void read_file(const char *path, char *out, size_t capacity) {
  FILE *file = fopen(path, "rb");
  CHECK(file);
  size_t length = fread(out, 1, capacity - 1, file);
  out[length] = '\0';
  CHECK(fclose(file) == 0);
}

/* tai_tabset_create owns the per-user store. Restarts must read it back, and
 * a corrupt file must neither block startup nor be overwritten. */
static void check_persistence(const char *url) {
  char root[] = "/tmp/tai-bookmarks-tabset-XXXXXX";
  CHECK(mkdtemp(root));
  CHECK(setenv("XDG_DATA_HOME", root, 1) == 0);
  char directory[256], file[256];
  CHECK(snprintf(directory, sizeof(directory), "%s/tai-browser", root) > 0);
  CHECK(snprintf(file, sizeof(file), "%s/bookmarks", directory) > 0);

  TaiTabSet *tabs = open_persistent(url);
  CHECK(!view_of(tabs).bookmarked);
  toggle_to(tabs, true);
  tai_tabset_destroy(tabs);

  tabs = open_persistent(url);
  CHECK(view_of(tabs).bookmarked);
  toggle_to(tabs, false);
  tai_tabset_destroy(tabs);

  tabs = open_persistent(url);
  CHECK(!view_of(tabs).bookmarked);
  tai_tabset_destroy(tabs);

  static const char garbage[] = "not a bookmarks file\n";
  FILE *corrupt = fopen(file, "wb");
  CHECK(corrupt && fputs(garbage, corrupt) >= 0 && fclose(corrupt) == 0);
  tabs = open_persistent(url);
  CHECK(!view_of(tabs).bookmarked);
  toggle_to(tabs, true);
  tai_tabset_destroy(tabs);
  char contents[64];
  read_file(file, contents, sizeof(contents));
  CHECK(!strcmp(contents, garbage));

  CHECK(unlink(file) == 0);
  CHECK(rmdir(directory) == 0);
  CHECK(rmdir(root) == 0);
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  char base[256], zeta_url[512], alpha_url[512], pending_url[512];
  char persist_url[512];
  CHECK(snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]) > 0);
  CHECK(snprintf(zeta_url, sizeof(zeta_url), "%s/zeta", base) > 0);
  CHECK(snprintf(alpha_url, sizeof(alpha_url),
                 "%s/alpha?x=1&y=2#frag\"'<tag>&", base) > 0);
  CHECK(snprintf(pending_url, sizeof(pending_url), "%s/pending", base) > 0);
  CHECK(snprintf(persist_url, sizeof(persist_url), "%s/persist", base) > 0);

  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_with_home_url(
      default_css, false, alpha_url, &error);
  CHECK(tabs && error == NULL);
  CHECK(tai_tabset_start(tabs, "about:blank", 800.0, 525.0, &error));
  CHECK(error == NULL);

  TaiTabSetView view = wait_document(tabs, "about:blank");
  CHECK(!view.bookmarkable && !view.bookmarked);
  expect_toggle_rejected(tabs);

  open_list(tabs);
  view = wait_document(tabs, "about:bookmarks");
  CHECK(!view.bookmarkable && !view.bookmarked);
  CHECK(text_contains(tai_page_root(view.page), "No bookmarks yet."));
  expect_toggle_rejected(tabs);

  navigate_address(tabs, zeta_url);
  view = wait_document(tabs, zeta_url);
  CHECK(view.bookmarkable && !view.bookmarked);
  toggle_to(tabs, true);
  toggle_to(tabs, false);
  toggle_to(tabs, true);

  CHECK(tai_tabset_new_tab(tabs, &error));
  CHECK(error == NULL);
  view = wait_document(tabs, alpha_url);
  CHECK(view.tab_count == 2 && view.active_index == 1);
  CHECK(view.bookmarkable && !view.bookmarked);
  toggle_to(tabs, true);
  CHECK(tai_tabset_select(tabs, 0));
  view = view_of(tabs);
  CHECK(!strcmp(view.url, zeta_url) && view.bookmarked);
  CHECK(tai_tabset_select(tabs, 1));
  view = view_of(tabs);
  CHECK(!strcmp(view.url, alpha_url) && view.bookmarked);

  open_list(tabs);
  view = wait_document(tabs, "about:bookmarks");
  CHECK(!view.bookmarkable && !view.bookmarked);
  CHECK(view.history_count == 2 && view.history_index == 1);
  expect_toggle_rejected(tabs);
  Anchors anchors = {0};
  collect_anchors(tai_page_root(view.page), &anchors);
  CHECK(anchors.count == 2 && !anchors.unexpected_tag);
  assert_anchor(anchors.items[0], alpha_url);
  assert_anchor(anchors.items[1], zeta_url);
  click_saved_link(tabs, view.page, anchors.items[0], alpha_url);
  view = wait_document(tabs, alpha_url);
  CHECK(view.bookmarked && view.bookmarkable);
  CHECK(view.history_count == 3 && view.history_index == 2);

  CHECK(tai_tabset_history(tabs, -1, &error));
  CHECK(error == NULL);
  view = wait_document(tabs, "about:bookmarks");
  CHECK(view.history_count == 3 && view.history_index == 1);
  CHECK(view.can_go_forward && !view.bookmarkable);

  navigate_address(tabs, pending_url);
  view = view_of(tabs);
  CHECK(view.loading && !view.bookmarkable && !view.bookmarked);
  expect_toggle_rejected(tabs);
  checkpoint("BOOKMARK_PENDING");
  view = wait_document(tabs, pending_url);
  CHECK(view.bookmarkable && !view.bookmarked);

  tai_tabset_destroy(tabs);
  check_persistence(persist_url);
  puts("bookmark tab-set integration passed");
  return EXIT_SUCCESS;
}
