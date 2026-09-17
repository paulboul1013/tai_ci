#include "tai/css.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#ifdef TAI_TEST_ALLOC_FAILURE
/* Link with --wrap so every allocation boundary can fail deterministically. */
static long allocation_budget = -1;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
static bool refuse_allocation(void) {
  if (allocation_budget < 0)
    return false;
  if (allocation_budget == 0)
    return true;
  allocation_budget--;
  return false;
}
void *__wrap_malloc(size_t size) {
  return refuse_allocation() ? NULL : __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
  return refuse_allocation() ? NULL : __real_calloc(count, size);
}
void *__wrap_realloc(void *ptr, size_t size) {
  return refuse_allocation() ? NULL : __real_realloc(ptr, size);
}
static void allocation_failures(void) {
  for (long limit = 0; limit < 500; limit++) {
    char *error = NULL;
    TaiNode node = {.kind = TAI_ELEMENT, .tag = "div"};
    allocation_budget = limit;
    TaiStylesheet *sheet =
        tai_css_parse("div:has(a.x) {font:italic 150% STRAẞE} div "
                      "{font-size:33.33%;color:red!important}",
                      &error);
    if (sheet)
      (void)tai_css_style(&node, sheet, &error);
    allocation_budget = -1;
    free(error);
    tai_map_clear(&node.style);
    tai_css_destroy(sheet);
  }
}
#endif
static void styles(FILE *out, TaiNode *node) {
  fputs("{\"style\":{", out);
  for (size_t i = 0; i < node->style.count; i++) {
    if (i)
      fputc(',', out);
    tai_json_string(out, node->style.items[i].key);
    fputc(':', out);
    tai_json_string(out, node->style.items[i].value);
  }
  fputs("},\"children\":[", out);
  for (size_t i = 0; i < node->child_count; i++) {
    if (i)
      fputc(',', out);
    styles(out, node->children[i]);
  }
  fputs("]}", out);
}
static int driver(const char *mode, const char *path) {
  char *input = tai_read_file(path, NULL), *error = NULL;
  assert(input);
  TaiStylesheet *sheet = tai_css_parse(input, &error);
  free(input);
  assert(sheet && !error);
  if (!strcmp(mode, "--json")) {
    tai_css_json(stdout, sheet);
    tai_css_destroy(sheet);
    return 0;
  }
  TaiNode root = {.kind = TAI_ELEMENT, .tag = "div"};
  TaiNode child = {
      .kind = TAI_ELEMENT, .tag = "a", .parent = &root, .visited = true};
  TaiNode text = {.kind = TAI_TEXT, .text = "hello", .parent = &child};
  TaiNode *children[] = {&child}, *texts[] = {&text};
  root.children = children;
  root.child_count = 1;
  child.children = texts;
  child.child_count = 1;
  assert(tai_map_set(&root.attributes, "id", "Root", 0));
  assert(tai_map_set(&root.attributes, "class", "card\xe2\x80\x83wide", 0));
  assert(tai_map_set(&child.attributes, "class", "x", 0));
  assert(tai_map_set(&child.attributes, "style", "  color:inherit; width:33px",
                     0));
  bool ok = tai_css_style(&root, sheet, &error);
  if (ok)
    styles(stdout, &root);
  else
    fprintf(stderr, "%s\n", error);
  free(error);
  tai_css_destroy(sheet);
  tai_map_clear(&root.attributes);
  tai_map_clear(&child.attributes);
  tai_map_clear(&root.style);
  tai_map_clear(&child.style);
  tai_map_clear(&text.style);
  return ok ? 0 : 1;
}
int main(int argc, char **argv) {
  if (argc == 3)
    return driver(argv[1], argv[2]);
  char *error = NULL;
  TaiStylesheet *sheet = tai_css_parse(
      "/* swallowed */ p {color:red} p {color:blue!important;color:green} "
      ".card:has(span) {font:italic bold 150% Times New Roman}",
      &error);
  assert(sheet && !error);
  TaiNode root = {.kind = TAI_ELEMENT, .tag = "p"};
  assert(tai_css_style(&root, sheet, &error));
  assert(strcmp(tai_map_get(&root.style, "color"), "green") == 0);
  assert(tai_map_set(&root.attributes, "style",
                     "color:red!important; color:blue; font-size:150%", 0));
  assert(tai_css_style(&root, sheet, &error));
  assert(strcmp(tai_map_get(&root.style, "font-size"), "24.0px") == 0);
  assert(strcmp(tai_map_get(&root.style, "color"), "blue") == 0);
  TaiSelector *sel = tai_selector_parse("p:has(span)", &error);
  TaiNode child = {.kind = TAI_ELEMENT, .tag = "span", .parent = &root};
  TaiNode *children[] = {&child};
  root.children = children;
  root.child_count = 1;
  assert(sel && tai_selector_matches(sel, &root));
  tai_selector_destroy(sel);
  tai_css_destroy(sheet);
  tai_map_clear(&root.style);
  tai_map_clear(&root.attributes);
#ifdef TAI_TEST_ALLOC_FAILURE
  allocation_failures();
#endif
  return 0;
}
