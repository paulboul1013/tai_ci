#include "tai/css.h"
#include "tai/layout.h"
#include "tai/render.h"
#include <cairo/cairo.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TaiNode *find_tag(TaiNode *node, const char *tag) {
  if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    TaiNode *found = find_tag(node->children[index], tag);
    if (found) return found;
  }
  return NULL;
}

static void test_scroll_clip(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<main style=\"height:20px;overflow:scroll;background-color:#ff0000\">"
      "<div style=\"background-color:#0000ff\">top</div>"
      "<div style=\"background-color:#00ff00\">bottom</div></main>", &error);
  assert(document && !error);
  TaiNode *root = tai_document_root(document);
  TaiNode *scroller = find_tag(root, "main");
  assert(scroller);
  scroller->scroll_y = 20;
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} main {display:block} "
      "div {display:block}", &error);
  assert(sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  assert(layout && !error);
  TaiDisplayList *list = tai_display_list_create(layout, &error);
  assert(list && !error);

  assert(tai_display_list_count(list) == 8);
  const TaiDisplayCommand *hit_only = tai_display_list_command(list, 1);
  const TaiDisplayCommand *push = tai_display_list_command(list, 2);
  const TaiDisplayCommand *pop = tai_display_list_command(list, 7);
  assert(hit_only && hit_only->kind == TAI_DRAW_HIT_TEST);
  assert(hit_only->node_id == scroller->id);
  assert(push && push->kind == TAI_PUSH_CLIP_SCROLL);
  assert(push->x == 13 && push->y == 18 && push->width == 174);
  assert(push->height == 20 && push->scroll_y == 20);
  assert(pop && pop->kind == TAI_POP_CLIP_SCROLL);

  TaiDisplayHit hit = {0};
  assert(tai_display_list_hit_test(list, 150, 20, &hit));
  TaiNode *bottom = find_tag(root, "div");
  assert(bottom && hit.node_id != bottom->id);
  assert(hit.kind == TAI_DRAW_FILL_RECT);
  assert(!tai_display_list_hit_test(list, 150, 38, &hit));
  assert(hit.node_id == 0 && hit.width == 0);
  assert(tai_display_list_hit_test(list, 13, 18, &hit));
  assert(!tai_display_list_hit_test(list, 187, 18, &hit));

  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-scroll.png", 200, 80,
                                    &error));
  assert(!error);
  cairo_surface_t *surface =
      cairo_image_surface_create_from_png("/tmp/tai-ci-scroll.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  unsigned char *pixels = cairo_image_surface_get_data(surface);
  int stride = cairo_image_surface_get_stride(surface);
  unsigned char *inside = pixels + 20 * stride + 150 * 4;
  unsigned char *below = pixels + 40 * stride + 150 * 4;
  assert(inside[0] == 0 && inside[1] == 255 && inside[2] == 0 &&
         inside[3] == 255);
  assert(below[0] == 255 && below[1] == 255 && below[2] == 255 &&
         below[3] == 255);
  cairo_surface_destroy(surface);
  tai_display_list_destroy(list);
  free(error);
}

static void test_transparent_scroll_hit_region(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<main style=\"height:20px;overflow:scroll\"></main>", &error);
  assert(document && !error);
  TaiNode *root = tai_document_root(document);
  TaiNode *scroller = find_tag(root, "main");
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} main {display:block}", &error);
  assert(scroller && sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  TaiDisplayList *list = tai_display_list_create(layout, &error);
  assert(layout && list && !error);
  assert(tai_display_list_count(list) == 1);
  TaiDisplayHit hit = {0};
  assert(tai_display_list_hit_test(list, 20, 20, &hit));
  assert(hit.kind == TAI_DRAW_HIT_TEST && hit.node_id == scroller->id);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
}

static void test_nested_scroll_raster_and_hit_agree(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<main style=\"height:20px;overflow:scroll\">"
      "<div style=\"height:30px;overflow:scroll\">"
      "<section style=\"height:20px;background-color:red\"></section>"
      "<article style=\"height:20px;background-color:blue\"></article>"
      "</div></main>", &error);
  assert(document && !error);
  TaiNode *root = tai_document_root(document);
  TaiNode *outer = find_tag(root, "main");
  TaiNode *inner = find_tag(root, "div");
  TaiNode *blue = find_tag(root, "article");
  assert(outer && inner && blue);
  outer->scroll_y = 10;
  inner->scroll_y = 10;
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} main {display:block} "
      "div {display:block} section {display:block} article {display:block}",
      &error);
  assert(sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  TaiDisplayList *list = tai_display_list_create(layout, &error);
  assert(layout && list && !error);
  TaiDisplayHit hit = {0};
  assert(tai_display_list_hit_test(list, 150, 20, &hit));
  assert(hit.node_id == blue->id && hit.kind == TAI_DRAW_FILL_RECT);
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-nested-scroll.png",
                                    200, 60, &error));
  cairo_surface_t *surface =
      cairo_image_surface_create_from_png("/tmp/tai-ci-nested-scroll.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  unsigned char *pixel = cairo_image_surface_get_data(surface) +
                         20 * cairo_image_surface_get_stride(surface) + 150 * 4;
  assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
  cairo_surface_destroy(surface);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
}

int main(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<div style=\"display:block;background-color:#ff0000\">"
      "<span style=\"color:#0000ff\">Hello</span></div>", &error);
  assert(document && !error);
  TaiNode *root = tai_document_root(document);
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} div {display:block}",
      &error);
  assert(sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  assert(layout && !error);

  TaiDisplayList *list = tai_display_list_create(layout, &error);
  assert(list && !error);
  assert(tai_display_list_count(list) == 2);
  const TaiDisplayCommand *background = tai_display_list_command(list, 0);
  const TaiDisplayCommand *text = tai_display_list_command(list, 1);
  assert(background && background->kind == TAI_DRAW_FILL_RECT);
  assert(background->rgba == 0xff0000ffU);
  assert(text && text->kind == TAI_DRAW_TEXT);
  assert(text->rgba == 0x0000ffffU);
  assert(!strcmp(text->text, "Hello"));
  assert(text->font_size == 16.0);
  assert(tai_display_list_command(list, 2) == NULL);
  TaiDisplayHit precision_hit = {0};
  double skia_top = (double)(float)text->y;
  assert(tai_display_list_hit_test(list, text->x, skia_top, &precision_hit));
  assert(precision_hit.kind == TAI_DRAW_TEXT);
  assert(tai_display_list_hit_test(list, text->x,
                                   nextafter(skia_top, -INFINITY),
                                   &precision_hit));
  assert(precision_hit.kind == TAI_DRAW_FILL_RECT);

  /* The list is self-contained and remains usable after its source objects
   * have been released. */
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  layout = NULL;
  sheet = NULL;
  document = NULL;
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-render.png", 200, 80,
                                    &error));
  assert(!error);
  cairo_surface_t *surface =
      cairo_image_surface_create_from_png("/tmp/tai-ci-render.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  unsigned char *pixels = cairo_image_surface_get_data(surface);
  int stride = cairo_image_surface_get_stride(surface);
  unsigned char *background_pixel = pixels + 37 * stride + 14 * 4;
  assert(background_pixel[0] == 0 && background_pixel[1] == 0 &&
         background_pixel[2] == 255 && background_pixel[3] == 255);
  cairo_surface_destroy(surface);

  tai_display_list_destroy(list);
  free(error);
  test_scroll_clip();
  test_transparent_scroll_hit_region();
  test_nested_scroll_raster_and_hit_agree();
  return 0;
}
