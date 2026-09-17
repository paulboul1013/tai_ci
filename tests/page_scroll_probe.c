#include "tai/browser.h"

#include <stdio.h>
#include <stdlib.h>

static void print_path(const TaiNode *root, const TaiNode *target) {
  if (root == target) {
    fputs("0", stdout);
    return;
  }
  print_path(root, target->parent);
  for (size_t index = 0; index < target->parent->child_count; index++)
    if (target->parent->children[index] == target) {
      fprintf(stdout, "/%zu", index);
      return;
    }
}

int main(int argc, char **argv) {
  if (argc != 6) return 2;
  char *error = NULL;
  TaiNetwork *network = tai_network_create();
  TaiUrl *url = tai_url_parse(argv[1]);
  TaiPage *page = network && url ? tai_page_load(
      network, url,
      "html {display:block} body {display:block} div {display:block} "
      "section {display:block}",
      200.0, strtod(argv[2], NULL), false, &error) : NULL;
  if (!page || !tai_page_set_scroll_y(page, strtod(argv[3], NULL))) {
    fprintf(stderr, "%s\n", error ? error : "page scroll probe failed");
    tai_page_destroy(page);
    tai_url_destroy(url);
    tai_network_destroy(network);
    free(error);
    return 1;
  }
  TaiDisplayHit hit = {0};
  TaiNode *node = tai_page_viewport_hit_test(
      page, strtod(argv[4], NULL), strtod(argv[5], NULL), &hit);
  printf("{\"scroll\":%.17g,\"max_scroll\":%.17g,\"target\":",
         tai_page_scroll_y(page), tai_page_max_scroll_y(page));
  if (!node) {
    fputs("null", stdout);
  } else {
    fputc('"', stdout);
    print_path(tai_page_root(page), node);
    fputc('"', stdout);
  }
  puts("}");
  tai_page_destroy(page);
  tai_url_destroy(url);
  tai_network_destroy(network);
  free(error);
  return 0;
}
