#define _POSIX_C_SOURCE 200809L
/* Native HTTPS security state for tab sets. Driven by tests/https_integration.py,
 * which owns the per-run test CA and the 127.0.0.1 HTTP/HTTPS fixture servers.
 * Expected values follow tests/fixtures/https_oracle.json except where
 * PORTING_PLAN.md records a difference (pending and rollback keep the old
 * page's lock). */
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
    "h1 {display:block}";
static const int timeout_seconds = 8;

static char http_base[128];
static char https_base[128];
static char untrusted_base[128];
static const char *ca_file;

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

static const char *join(char *buffer, size_t size, const char *base,
                        const char *path) {
  CHECK(snprintf(buffer, size, "%s%s", base, path) < (int)size);
  return buffer;
}

static TaiTabSetView read_view(TaiTabSet *tabs) {
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
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

static void pump_once(TaiTabSet *tabs) {
  bool changed = false;
  char *error = NULL;
  CHECK(tai_tabset_pump(tabs, &changed, &error));
  CHECK(error == NULL);
}

/* Waits for the active tab to finish loading and checks the committed page. */
static TaiTabSetView settle(TaiTabSet *tabs, const char *heading,
                            bool secure) {
  struct timespec deadline = deadline_after(timeout_seconds);
  TaiTabSetView view = read_view(tabs);
  while ((view.loading || !view.page) && before_deadline(deadline)) {
    pump_once(tabs);
    pause_briefly();
    view = read_view(tabs);
  }
  CHECK(!view.loading && view.page);
  if (!contains_text(tai_page_root(view.page), heading)) {
    fprintf(stderr, "expected heading %s at %s\n", heading, view.url);
    CHECK(false);
  }
  if (view.secure != secure) {
    fprintf(stderr, "expected secure=%d at %s\n", secure, view.url);
    CHECK(false);
  }
  return view;
}

static TaiTabSet *create_set(const char *home_url, const char *trusted_ca) {
  char *error = NULL;
  TaiTabSet *tabs = trusted_ca
      ? tai_tabset_create_for_test(default_css, false, home_url, trusted_ca,
                                   &error)
      : tai_tabset_create_with_home_url(default_css, false, home_url, &error);
  CHECK(tabs && error == NULL);
  return tabs;
}

static void start_set(TaiTabSet *tabs, const char *url) {
  char *error = NULL;
  CHECK(tai_tabset_start(tabs, url, 320.0, 180.0, &error));
  CHECK(error == NULL);
  TaiTabSetView view = read_view(tabs);
  CHECK(view.loading && !view.page && !view.secure);
}

static void go(TaiTabSet *tabs, const char *url) {
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, url, &error));
  CHECK(error == NULL);
}

static void history(TaiTabSet *tabs, int direction) {
  char *error = NULL;
  CHECK(tai_tabset_history(tabs, direction, &error));
  CHECK(error == NULL);
}

static void transitions_scenario(void) {
  char url[256], home[256];
  TaiTabSet *tabs = create_set(join(home, sizeof(home), http_base,
                                    "/plain-home"), ca_file);
  start_set(tabs, join(url, sizeof(url), https_base, "/secure-home"));
  settle(tabs, "secure-home", true);

  /* Pending: the old HTTPS page and its lock stay until the new commit. */
  go(tabs, join(url, sizeof(url), https_base, "/secure-delay"));
  TaiTabSetView view = read_view(tabs);
  CHECK(view.loading && view.secure);
  print_checkpoint("SECURE_PENDING");
  for (int i = 0; i < 20; i++) {
    pump_once(tabs);
    pause_briefly();
  }
  view = read_view(tabs);
  CHECK(view.loading && view.secure);
  CHECK(contains_text(tai_page_root(view.page), "secure-home"));
  print_checkpoint("SECURE_PENDING_CHECKED");
  settle(tabs, "secure-delayed", true);

  /* Later certificate and transport failures roll back to the old page, so
   * its lock stays (Python shows an insecure error page). */
  go(tabs, join(url, sizeof(url), untrusted_base, "/secure-home"));
  settle(tabs, "secure-delayed", true);
  go(tabs, join(url, sizeof(url), https_base, "/secure-fail"));
  settle(tabs, "secure-delayed", true);

  go(tabs, join(url, sizeof(url), http_base, "/plain-home"));
  settle(tabs, "plain-home", false);
  go(tabs, join(url, sizeof(url), https_base, "/secure-home"));
  settle(tabs, "secure-home", true);
  char *error = NULL;
  CHECK(tai_tabset_open_bookmarks(tabs, &error) && error == NULL);
  settle(tabs, "Bookmarks", false);

  /* Redirects follow the requested URL, as in the oracle. */
  go(tabs, join(url, sizeof(url), http_base, "/to-https"));
  settle(tabs, "redirected-secure", false);
  go(tabs, join(url, sizeof(url), https_base, "/to-http"));
  settle(tabs, "redirected-plain", true);

  /* Back/Forward commits recompute security from the history URL. */
  go(tabs, join(url, sizeof(url), https_base, "/secure-home"));
  settle(tabs, "secure-home", true);
  go(tabs, join(url, sizeof(url), http_base, "/plain-home"));
  settle(tabs, "plain-home", false);
  history(tabs, -1);
  settle(tabs, "secure-home", true);
  history(tabs, 1);
  settle(tabs, "plain-home", false);
  history(tabs, -1);
  settle(tabs, "secure-home", true);

  /* Each tab keeps its own state; the view follows the active tab. */
  CHECK(tai_tabset_new_tab(tabs, &error) && error == NULL);
  view = read_view(tabs);
  CHECK(view.active_index == 1 && !view.secure);
  settle(tabs, "plain-home", false);
  CHECK(tai_tabset_select(tabs, 0));
  view = read_view(tabs);
  CHECK(view.active_index == 0 && view.secure);
  CHECK(tai_tabset_select(tabs, 1));
  view = read_view(tabs);
  CHECK(view.active_index == 1 && !view.secure);
  tai_tabset_destroy(tabs);
}

static void initial_failure_scenarios(void) {
  char url[256];
  /* The first commit of an error page is insecure, like the oracle. */
  TaiTabSet *tabs = create_set(http_base, ca_file);
  start_set(tabs, join(url, sizeof(url), untrusted_base, "/secure-home"));
  settle(tabs, "Certificate Error", false);
  tai_tabset_destroy(tabs);

  tabs = create_set(http_base, ca_file);
  start_set(tabs, join(url, sizeof(url), https_base, "/secure-fail"));
  settle(tabs, "Network Error", false);
  tai_tabset_destroy(tabs);

  /* The ordinary constructor never trusts the test CA. */
  tabs = create_set(http_base, NULL);
  start_set(tabs, join(url, sizeof(url), https_base, "/secure-home"));
  settle(tabs, "Certificate Error", false);
  tai_tabset_destroy(tabs);
}

int main(int argc, char **argv) {
  CHECK(argc == 5);
  CHECK(snprintf(http_base, sizeof(http_base), "http://127.0.0.1:%s",
                 argv[1]) > 0);
  CHECK(snprintf(https_base, sizeof(https_base), "https://127.0.0.1:%s",
                 argv[2]) > 0);
  CHECK(snprintf(untrusted_base, sizeof(untrusted_base),
                 "https://127.0.0.1:%s", argv[3]) > 0);
  ca_file = argv[4];
  transitions_scenario();
  initial_failure_scenarios();
  puts("tab-set HTTPS security passed: success, pending, failures, "
       "redirects, history, and per-tab state");
  return EXIT_SUCCESS;
}
