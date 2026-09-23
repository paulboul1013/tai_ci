#include "tai/browser.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TaiNode *find(TaiNode *node, const char *tag) {
  if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    TaiNode *result = find(node->children[index], tag);
    if (result) return result;
  }
  return NULL;
}

typedef struct {
  const TaiNode *target;
  double x, y;
  bool found;
} TargetPoint;

static bool find_target_point(const TaiLayoutItem *item, void *opaque) {
  TargetPoint *point = opaque;
  bool belongs_to_target = false;
  for (const TaiNode *node = item->node; node; node = node->parent)
    if (node == point->target) {
      belongs_to_target = true;
      break;
    }
  if (belongs_to_target &&
      (item->kind == TAI_LAYOUT_INPUT || item->kind == TAI_LAYOUT_BUTTON ||
       item->kind == TAI_LAYOUT_TEXT)) {
    point->x = item->x + item->width / 2.0;
    point->y = item->y + item->height / 2.0;
    point->found = true;
  }
  return true;
}

static bool click_target(TaiPage *page, const TaiNode *target, char **error) {
  TargetPoint point = {.target = target};
  bool changed = false;
  return tai_layout_visit(tai_page_layout(page), find_target_point, &point,
                           error) && point.found &&
         tai_page_activate_viewport(page, point.x, point.y, &changed, error);
}

static TaiPage *load_form(TaiNetwork *network, const char *html, int port,
                          const char *path, const char *method,
                          char **error) {
  char document[4096];
  int count = snprintf(document, sizeof(document),
                       "data:text/html,<form action=\"http://127.0.0.1:%d%s\"%s>%s</form>",
                       port, path, method, html);
  assert(count > 0 && (size_t)count < sizeof(document));
  TaiUrl *url = tai_url_parse(document);
  assert(url);
  TaiPage *page = tai_page_load(network, url,
      "html {display:block} body {display:block} form {display:block}",
      360, 140, false, error);
  tai_url_destroy(url);
  return page;
}

static void assert_loaded_candidate(TaiNetwork *network, TaiPage **page,
                                    TaiNavigationIntent *intent,
                                    char **error) {
  assert(tai_page_replace_from_intent(network, page, intent,
      "html {display:block} body {display:block} p {display:block}", false,
      error));
  assert(!*error && find(tai_page_root(*page), "p"));
}

int main(int argc, char **argv) {
  assert(argc == 2);
  int port = atoi(argv[1]);
  assert(port > 0 && port <= 65535);
  TaiNetwork *network = tai_network_create();
  assert(network);
  char *error = NULL;

  TaiPage *get_page = load_form(network,
      "<button>Send</button><input name=\"a b\" value=\"hello world\">"
      "<input type=checkbox name=flag checked><input type=checkbox name=skip>"
      "<input name=empty>", port, "/get?old=1", "", &error);
  assert(get_page && !error);
  TaiNode *button = find(tai_page_root(get_page), "button");
  assert(button && click_target(get_page, button, &error) && !error);
  TaiNavigationIntent *intent = NULL;
  assert(tai_page_take_navigation_intent(get_page, &intent));
  assert(intent && !tai_navigation_intent_is_post(intent));
  char expected_get[256];
  int expected_length = snprintf(expected_get, sizeof(expected_get),
      "http://127.0.0.1:%d/get?old=1&a+b=hello+world&flag=on&empty=", port);
  assert(expected_length > 0 && (size_t)expected_length < sizeof(expected_get));
  assert(!strcmp(tai_navigation_intent_url(intent), expected_get));
  assert_loaded_candidate(network, &get_page, intent, &error);
  tai_navigation_intent_destroy(intent);
  tai_page_destroy(get_page);

  char source_text[256];
  int source_length = snprintf(source_text, sizeof(source_text),
                               "http://127.0.0.1:%d/source", port);
  assert(source_length > 0 && (size_t)source_length < sizeof(source_text));
  TaiUrl *source_url = tai_url_parse(source_text);
  assert(source_url);
  TaiPage *source_page = tai_page_load(network, source_url,
      "html {display:block} body {display:block} a {display:block} p {display:block}",
      360, 140, false, &error);
  tai_url_destroy(source_url);
  assert(source_page && !error);
  TaiNode *anchor = find(tai_page_root(source_page), "a");
  assert(anchor && click_target(source_page, anchor, &error) && !error);
  intent = NULL;
  assert(tai_page_take_navigation_intent(source_page, &intent));
  char expected_fragment[256];
  int fragment_length = snprintf(expected_fragment, sizeof(expected_fragment),
      "http://127.0.0.1:%d/fragment#target", port);
  assert(fragment_length > 0 &&
         (size_t)fragment_length < sizeof(expected_fragment));
  assert(intent && !strcmp(tai_navigation_intent_url(intent),
                           expected_fragment));
  assert_loaded_candidate(network, &source_page, intent, &error);
  assert(tai_page_scroll_y(source_page) > 0.0);
  tai_navigation_intent_destroy(intent);
  tai_page_destroy(source_page);

  TaiPage *post_page = load_form(network,
      "<input name=q value=\"é &\"><button>Send</button>", port,
      "/post", " method=POST", &error);
  assert(post_page && !error);
  TaiNode *input = find(tai_page_root(post_page), "input");
  assert(input && click_target(post_page, input, &error) && !error);
  bool changed = false;
  assert(tai_page_key(post_page, TAI_PAGE_KEY_RETURN, &changed, &error));
  assert(!error && !changed);
  intent = NULL;
  assert(tai_page_take_navigation_intent(post_page, &intent));
  assert(intent && tai_navigation_intent_is_post(intent));
  char expected_post[256];
  int expected_post_length = snprintf(expected_post, sizeof(expected_post),
                                     "http://127.0.0.1:%d/post", port);
  assert(expected_post_length > 0 &&
         (size_t)expected_post_length < sizeof(expected_post));
  assert(!strcmp(tai_navigation_intent_url(intent), expected_post));
  assert(!strcmp(tai_navigation_intent_body(intent), "q=%C3%A9+%26"));
  assert_loaded_candidate(network, &post_page, intent, &error);
  tai_navigation_intent_destroy(intent);
  tai_page_destroy(post_page);

  TaiPage *prevent_page = load_form(network,
      "<script>var f=document.querySelectorAll('form')[0];"
      "f.addEventListener('submit',function(e){e.preventDefault();});"
      "</script><button>Never</button>", port, "/never", "", &error);
  assert(prevent_page && !error);
  button = find(tai_page_root(prevent_page), "button");
  assert(button && click_target(prevent_page, button, &error) && !error);
  intent = NULL;
  assert(tai_page_take_navigation_intent(prevent_page, &intent));
  assert(intent == NULL);
  tai_page_destroy(prevent_page);

  char document[512];
  int html_length = snprintf(document, sizeof(document),
      "<a href=\"http://127.0.0.1:%d/drop\">fail</a>", port);
  assert(html_length > 0 && (size_t)html_length < sizeof(document));
  char document_url[600];
  int document_length = snprintf(document_url, sizeof(document_url),
                                 "data:text/html,%s", document);
  assert(document_length > 0 && (size_t)document_length < sizeof(document_url));
  TaiUrl *drop_url = tai_url_parse(document_url);
  assert(drop_url);
  TaiPage *failure_page = tai_page_load(network, drop_url,
      "html {display:block} body {display:block} a {display:block}",
      360, 140, false, &error);
  tai_url_destroy(drop_url);
  assert(failure_page && !error);
  TaiNode *drop_anchor = find(tai_page_root(failure_page), "a");
  assert(drop_anchor && click_target(failure_page, drop_anchor, &error));
  assert(!error);
  intent = NULL;
  assert(tai_page_take_navigation_intent(failure_page, &intent));
  int drop_url_length = snprintf(expected_post, sizeof(expected_post),
                                 "http://127.0.0.1:%d/drop", port);
  assert(drop_url_length > 0 &&
         (size_t)drop_url_length < sizeof(expected_post));
  assert(intent && !strcmp(tai_navigation_intent_url(intent), expected_post));
  TaiPage *stable_page = failure_page;
  assert(!tai_page_replace_from_intent(network, &failure_page, intent,
      "html {display:block} body {display:block}", false, &error));
  assert(error && failure_page == stable_page &&
         find(tai_page_root(failure_page), "a"));
  tai_navigation_intent_destroy(intent);
  free(error);
  error = NULL;

  tai_page_destroy(failure_page);
  tai_network_destroy(network);
  free(error);
  return 0;
}
