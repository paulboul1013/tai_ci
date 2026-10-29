#define _POSIX_C_SOURCE 200809L
/* Visited links in the real multi-window presentation loop under SDL's dummy
 * driver (docs/visited-plan.md). The visited set belongs to the browser app,
 * so every tab and window shares it. Pages are data: URLs and the failing
 * load is a refused 127.0.0.1 connection, so no server is needed.
 *
 *   other tab   a page another tab committed turns this tab's link visited
 *               only when this tab's page is styled again
 *   new window  a Ctrl+N window loads with the shared set
 *   failed      a load that ends in the network error page is not visited */
#include "window_driver.h"

static const char *css = "html {display:block} body {display:block}";

#define TARGET_URL "data:text/html,target"
#define FAILING_URL "http://127.0.0.1:1/"
#define LINKS_URL                                                             \
  "data:text/html,<h1>links</h1>"                                             \
  "<a id=target href=\"" TARGET_URL "\">target</a>"                          \
  "<a id=failing href=\"" FAILING_URL "\">failing</a>"

typedef struct {
  int step;
  struct timespec deadline;
  bool finished;
} Driver;

static bool visited(TaiTabSet *tabs, const char *id) {
  TaiTabSetView view = view_of(tabs);
  CHECK(view.page);
  const TaiNode *link = find_id(tai_page_root(view.page), id);
  CHECK(link);
  return link->visited;
}

static bool settled_at(TaiTabSet *tabs, const char *url) {
  TaiTabSetView view = view_of(tabs);
  return !view.loading && view.page &&
         !strcmp(tai_url_string(tai_page_url(view.page)), url);
}

static void go(TaiTabSet *tabs, const char *url) {
  char *error = NULL;
  CHECK(tai_tabset_navigate_address(tabs, url, &error));
  CHECK(error == NULL);
}

static bool frame(void *opaque, const TaiPresWindowInfo *w, size_t count) {
  Driver *driver = opaque;
  check_deadline(&driver->deadline, "visited", driver->step);
  char *error = NULL;
  switch (driver->step) {
  case 0:
    if (!loaded(w[0].tabs, "links")) return true;
    CHECK(!visited(w[0].tabs, "target") && !visited(w[0].tabs, "failing"));
    CHECK(tai_tabset_new_tab(w[0].tabs, &error) && !error);
    ADVANCE();
  case 1:
    if (view_of(w[0].tabs).tab_count != 2 || !loaded(w[0].tabs, "links"))
      return true;
    go(w[0].tabs, TARGET_URL);
    ADVANCE();
  case 2:
    if (!settled_at(w[0].tabs, TARGET_URL)) return true;
    go(w[0].tabs, FAILING_URL);
    ADVANCE();
  case 3:
    if (!settled_at(w[0].tabs, FAILING_URL)) return true;
    CHECK(tai_tabset_select(w[0].tabs, 0));
    /* Committing a page in another tab does not restyle this one. */
    CHECK(!visited(w[0].tabs, "target"));
    CHECK(tai_page_restyle(view_of(w[0].tabs).page, &error) && !error);
    CHECK(visited(w[0].tabs, "target"));
    CHECK(!visited(w[0].tabs, "failing"));
    ctrl_n(w[0].window_id);
    ADVANCE();
  case 4:
    if (count != 2 || !loaded(w[1].tabs, "links")) return true;
    CHECK(visited(w[1].tabs, "target"));
    CHECK(!visited(w[1].tabs, "failing"));
    FINISH();
  }
  CHECK(!"unexpected step");
  return false;
}

int main(void) {
  char *error = NULL;
  TaiBrowserApp *app =
      tai_browser_app_create_with_home_url(css, false, LINKS_URL, &error);
  CHECK(app && error == NULL);
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  Driver driver = {.step = -1};
  next_step(&driver.step, &driver.deadline);
  TaiPresBrowserObserver observer = {.frame = frame, .opaque = &driver};
  bool presented = tai_pres_present_browser_observed(app, LINKS_URL, 800, 600,
                                                     &observer, &error);
  if (!presented)
    fprintf(stderr, "presentation failed: %s\n", error ? error : "unknown");
  CHECK(presented && error == NULL && driver.finished);
  tai_browser_app_destroy(app);
  puts("visited window tests passed");
  return EXIT_SUCCESS;
}
