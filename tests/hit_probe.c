#include "tai/css.h"
#include "tai/layout.h"
#include "tai/render.h"

#include <stdio.h>
#include <stdlib.h>

static void apply_scroll(TaiNode *node) {
  const char *value = tai_map_get(&node->attributes, "data-scroll");
  if (value) node->scroll_y = strtod(value, NULL);
  for (size_t i = 0; i < node->child_count; i++) apply_scroll(node->children[i]);
}

static void print_path(const TaiNode *root, const TaiNode *target) {
  if (root == target) {
    fputs("0", stdout);
    return;
  }
  print_path(root, target->parent);
  for (size_t i = 0; i < target->parent->child_count; i++)
    if (target->parent->children[i] == target) {
      fprintf(stdout, "/%zu", i);
      return;
    }
}

int main(int argc, char **argv) {
  if (argc != 4) return 2;
  char *error = NULL;
  TaiDocument *document = tai_html_parse(argv[1], &error);
  TaiNode *root = document ? tai_document_root(document) : NULL;
  TaiStylesheet *sheet = root ? tai_css_parse(
      "html {display:block} body {display:block} main {display:block} "
      "div {display:block}", &error) : NULL;
  if (!sheet || !tai_css_style(root, sheet, &error)) goto fail;
  apply_scroll(root);
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  TaiDisplayList *display = layout ? tai_display_list_create(layout, &error) : NULL;
  TaiDisplayHit hit = {0};
  bool found = display && tai_display_list_hit_test(
      display, strtod(argv[2], NULL), strtod(argv[3], NULL), &hit);
  if (!found) {
    puts("null");
  } else {
    TaiNode *node = tai_document_node(document, hit.node_id);
    fputs("{\"target\":\"", stdout);
    print_path(root, node);
    fprintf(stdout, "\",\"rect\":[%.17g,%.17g,%.17g,%.17g]}",
            hit.x, hit.y, hit.x + hit.width, hit.y + hit.height);
    fputc('\n', stdout);
  }
  tai_display_list_destroy(display);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
  return 0;
fail:
  fprintf(stderr, "%s\n", error ? error : "hit probe failed");
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
  return 1;
}
