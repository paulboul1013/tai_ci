#include "tai/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *css = "html {display:block} body {display:block} a {display:block} p {display:block}";
static void assert_page(TaiSession *session, const char *url,
                        size_t length, size_t index);

static TaiNode *find_link(TaiNode *node, size_t *remaining) {
  if (node->kind == TAI_ELEMENT && !strcmp(node->tag, "a")) {
    if (!*remaining) return node;
    --*remaining;
  }
  for (size_t i = 0; i < node->child_count; ++i) {
    TaiNode *found = find_link(node->children[i], remaining);
    if (found) return found;
  }
  return NULL;
}

static TaiNode *find_tag(TaiNode *node, const char *tag) {
  if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
  for (size_t i = 0; i < node->child_count; ++i) {
    TaiNode *found = find_tag(node->children[i], tag);
    if (found) return found;
  }
  return NULL;
}

typedef struct { const TaiNode *target; double x, y; bool found; } Point;
static bool link_point(const TaiLayoutItem *item, void *opaque) {
  Point *point = opaque;
  for (const TaiNode *node = item->node; node; node = node->parent) {
    if (node == point->target &&
        (item->kind == TAI_LAYOUT_TEXT || item->kind == TAI_LAYOUT_BUTTON)) {
      point->x = item->x + item->width / 2.0;
      point->y = item->y + item->height / 2.0;
      point->found = true;
      break;
    }
  }
  return true;
}

static void activate_link(TaiPage *page, size_t index) {
  char *error = NULL;
  TaiNode *target = find_link(tai_page_root(page), &index);
  assert(target);
  Point point = {.target = target};
  assert(tai_layout_visit(tai_page_layout(page), link_point, &point, &error));
  assert(point.found && !error);
  bool changed = false;
  assert(tai_page_activate_viewport(page, point.x, point.y, &changed, &error));
  assert(!error);
}

static TaiNavigationIntent *click_link(TaiPage *page, size_t index) {
  activate_link(page, index);
  TaiNavigationIntent *intent = NULL;
  assert(tai_page_take_navigation_intent(page, &intent));
  assert(intent);
  return intent;
}

static void http_history(int port) {
  char source[128], post[128];
  assert(snprintf(source, sizeof(source), "http://127.0.0.1:%d/history-source",
                  port) > 0);
  assert(snprintf(post, sizeof(post), "http://127.0.0.1:%d/history-post",
                  port) > 0);
  char *error = NULL;
  TaiNetwork *network = tai_network_create();
  TaiUrl *url = tai_url_parse(source);
  assert(network && url);
  TaiPage *page = tai_page_load(network, url, css, 320, 160, false, &error);
  assert(page && !error);
  TaiSession *session = tai_session_create(network, page, css, false);
  assert(session);
  tai_url_destroy(url);
  TaiNode *button = find_tag(tai_page_root(page), "button");
  assert(button);
  Point point = {.target = button};
  assert(tai_layout_visit(tai_page_layout(page), link_point, &point, &error));
  assert(point.found && !error);
  bool changed = false;
  assert(tai_page_activate_viewport(page, point.x, point.y, &changed, &error));
  assert(!error);
  TaiNavigationIntent *intent = NULL;
  assert(tai_page_take_navigation_intent(page, &intent));
  assert(intent && tai_navigation_intent_is_post(intent));
  assert(tai_session_navigate(session, intent, &error));
  tai_navigation_intent_destroy(intent);
  assert(!error);
  assert_page(session, post, 2, 1);
  assert(tai_session_back(session, &error));
  assert(!error);
  assert_page(session, source, 2, 0);
  assert(tai_session_forward(session, &error));
  assert(!error);
  assert_page(session, post, 2, 1);
  tai_session_destroy(session);
  tai_network_destroy(network);
}

static void pending_session_commits(void) {
  char *error = NULL;
  TaiNetwork *network = tai_network_create();
  assert(network);
  TaiSession *session = tai_session_create_empty(css, false);
  assert(session && !tai_session_page(session));
  assert(tai_session_history_length(session) == 0);

  TaiUrl *first_url = tai_url_parse("data:text/html,first");
  TaiPage *first = tai_page_load(network, first_url, css, 320, 160, false,
                                 &error);
  assert(first && !error);
  assert(tai_session_commit_navigation(session, first, &error));
  assert(!error && tai_session_page(session) == first);
  assert_page(session, "data:text/html,first", 1, 0);

  TaiUrl *second_url = tai_url_parse("data:text/html,second");
  TaiPage *second = tai_page_load(network, second_url, css, 320, 160, false,
                                  &error);
  assert(second && !error);
  assert(tai_session_commit_navigation(session, second, &error));
  assert(!error && tai_session_page(session) == second);
  assert_page(session, "data:text/html,second", 2, 1);

  char *target_url = NULL;
  size_t target_index = 0;
  assert(tai_session_history_target(session, -1, &target_url, &target_index));
  assert(target_index == 0 && !strcmp(target_url, "data:text/html,first"));
  TaiPage *back_page = tai_page_load(network, first_url, css, 320, 160, false,
                                     &error);
  assert(back_page && !error);
  assert(tai_session_commit_history(session, back_page, target_index, &error));
  assert(!error && tai_session_page(session) == back_page);
  assert_page(session, "data:text/html,first", 2, 0);
  free(target_url);
  target_url = NULL;
  assert(tai_session_history_target(session, 1, &target_url, &target_index));
  assert(target_index == 1 && !strcmp(target_url, "data:text/html,second"));

  free(target_url);
  tai_url_destroy(first_url);
  tai_url_destroy(second_url);
  tai_session_destroy(session);
  tai_network_destroy(network);
  free(error);
}

static void assert_page(TaiSession *session, const char *url,
                        size_t length, size_t index) {
  assert(!strcmp(tai_url_string(tai_page_url(tai_session_page(session))), url));
  assert(tai_session_history_length(session) == length);
  assert(tai_session_history_index(session) == index);
}

int main(int argc, char **argv) {
  assert(argc == 1 || argc == 2);
  pending_session_commits();
  if (argc == 2) {
    int port = atoi(argv[1]);
    assert(port > 0 && port <= 65535);
    http_history(port);
    return 0;
  }
  const char *a = "data:text/html,%3Ca%20href%3D%22data%3Atext%2Fhtml%2C%253Ca%2520href%253D%2522data%253Atext%252Fhtml%252CC%2522%253EC%253C%252Fa%253E%253Ca%2520href%253D%2522data%253Atext%252Fhtml%252CD%2522%253ED%253C%252Fa%253E%22%3EB%3C%2Fa%3E";
  const char *b = "data:text/html,%3Ca%20href%3D%22data%3Atext%2Fhtml%2CC%22%3EC%3C%2Fa%3E%3Ca%20href%3D%22data%3Atext%2Fhtml%2CD%22%3ED%3C%2Fa%3E";
  const char *c = "data:text/html,C";
  const char *d = "data:text/html,D";
  char *error = NULL;
  TaiNetwork *network = tai_network_create();
  TaiUrl *initial = tai_url_parse(a);
  assert(network && initial);
  TaiPage *page = tai_page_load(network, initial, css, 320, 160, false, &error);
  assert(page && !error);
  TaiSession *session = tai_session_create(network, page, css, false);
  assert(session);
  tai_url_destroy(initial);
  assert_page(session, a, 1, 0);
  assert(tai_session_back(session, &error));
  assert(tai_session_forward(session, &error));
  assert_page(session, a, 1, 0);

  TaiNavigationIntent *intent = click_link(tai_session_page(session), 0);
  assert(tai_session_navigate(session, intent, &error));
  tai_navigation_intent_destroy(intent);
  assert(!error);
  assert_page(session, b, 2, 1);
  intent = click_link(tai_session_page(session), 0);
  assert(tai_session_navigate(session, intent, &error));
  tai_navigation_intent_destroy(intent);
  assert(!error);
  assert_page(session, c, 3, 2);
  assert(tai_session_back(session, &error));
  assert_page(session, b, 3, 1);
  assert(tai_session_forward(session, &error));
  assert_page(session, c, 3, 2);
  assert(tai_session_back(session, &error));
  intent = click_link(tai_session_page(session), 1);
  assert(tai_session_navigate(session, intent, &error));
  tai_navigation_intent_destroy(intent);
  assert(!error);
  assert_page(session, d, 3, 2);
  assert(tai_session_forward(session, &error));
  assert_page(session, d, 3, 2);

  assert(tai_session_record_fragment(session, "data:text/html,D#frag", &error) == false);
  assert(error);
  free(error);
  error = NULL;
  assert_page(session, d, 3, 2);
  tai_session_destroy(session);

  const char *fragment_document =
      "data:text/html,%3Ca%20href%3D%22%23target%22%3Ejump%3C%2Fa%3E"
      "%3Cdiv%20style%3D%22height%3A300px%22%3Egap%3C%2Fdiv%3E"
      "%3Cp%20id%3Dtarget%3Etarget%3C%2Fp%3E";
  TaiUrl *fragment_url = tai_url_parse(fragment_document);
  page = tai_page_load(network, fragment_url, css, 320, 80, false, &error);
  assert(page && !error);
  session = tai_session_create(network, page, css, false);
  assert(session);
  tai_url_destroy(fragment_url);
  activate_link(page, 0);
  assert(!strcmp(tai_url_fragment(tai_page_url(page)), "target"));
  assert(tai_page_scroll_y(page) > 0);
  assert(tai_page_set_scroll_y(page, 0));
  size_t first_link = 0;
  TaiNode *pending_link = find_link(tai_page_root(page), &first_link);
  Point pending_point = {.target = pending_link};
  assert(tai_layout_visit(tai_page_layout(page), link_point,
                          &pending_point, &error));
  bool pending_changed = false;
  assert(!tai_page_activate_viewport(page, pending_point.x,
                                    pending_point.y, &pending_changed, &error));
  assert(error && !strcmp(tai_url_fragment(tai_page_url(page)), "target"));
  free(error);
  error = NULL;
  char *fragment_change = NULL;
  assert(tai_page_take_fragment_change(page, &fragment_change));
  assert(fragment_change && !strcmp(fragment_change,
      tai_url_string(tai_page_url(page))));
  tai_page_finish_fragment_change(page, false);
  free(fragment_change);
  assert(!*tai_url_fragment(tai_page_url(page)));
  assert(tai_page_scroll_y(page) == 0);
  assert(tai_session_history_length(session) == 1);
  activate_link(page, 0);
  fragment_change = NULL;
  assert(tai_page_take_fragment_change(page, &fragment_change));
  assert(fragment_change);
  assert(tai_session_record_fragment(session, fragment_change, &error));
  tai_page_finish_fragment_change(page, true);
  free(fragment_change);
  assert(!error && tai_session_history_length(session) == 2);
  assert(tai_session_back(session, &error));
  assert(!error && tai_session_history_index(session) == 0);
  assert(!*tai_url_fragment(tai_page_url(tai_session_page(session))));
  assert(tai_session_forward(session, &error));
  assert(!error && tai_session_history_index(session) == 1);
  assert(!strcmp(tai_url_fragment(tai_page_url(tai_session_page(session))), "target"));
  tai_session_destroy(session);

  const char *failed_document =
      "data:text/html,%3Ca%20href%3D%22http%3A%2F%2F127.0.0.1%3A1%2F"
      "%22%3Efail%3C%2Fa%3E";
  TaiUrl *failed_url = tai_url_parse(failed_document);
  page = tai_page_load(network, failed_url, css, 320, 80, false, &error);
  assert(page && !error);
  session = tai_session_create(network, page, css, false);
  assert(session);
  tai_url_destroy(failed_url);
  intent = click_link(page, 0);
  assert(!tai_session_navigate(session, intent, &error));
  assert(error);
  free(error);
  error = NULL;
  tai_navigation_intent_destroy(intent);
  assert(tai_session_page(session) == page);
  assert_page(session, failed_document, 1, 0);
  tai_session_destroy(session);
  tai_network_destroy(network);
  puts("session history traversal and branching passed");
  return 0;
}
