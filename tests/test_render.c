#define _POSIX_C_SOURCE 200809L
#include "tai/css.h"
#include "tai/layout.h"
#include "tai/render.h"
#include <cairo/cairo.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static TaiNode *find_tag(TaiNode *node, const char *tag) {
  if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
  for (size_t index = 0; index < node->child_count; index++) {
    TaiNode *found = find_tag(node->children[index], tag);
    if (found) return found;
  }
  return NULL;
}

static uint32_t pixel(cairo_surface_t *surface, int x, int y) {
  cairo_surface_flush(surface);
  unsigned char *data = cairo_image_surface_get_data(surface);
  int stride = cairo_image_surface_get_stride(surface);
  unsigned char *value = data + y * stride + x * 4;
  return ((uint32_t)value[2] << 24) | ((uint32_t)value[1] << 16) |
         ((uint32_t)value[0] << 8) | value[3];
}

static void test_image_ownership_raster_and_hit(void) {
  char original[4096];
  assert(getcwd(original, sizeof(original)));
  char temporary[] = "/tmp/tai-ci-image-test-XXXXXX";
  assert(mkdtemp(temporary));
  assert(chdir(temporary) == 0);
  assert(mkdir("openmoji", 0700) == 0 || access("openmoji", F_OK) == 0);
  static const char *const fixture_paths[] = {
      "openmoji/1F600_color.png", "openmoji/1F601_color.png",
      "openmoji/1F602_color.png", "openmoji/1F603_color.png",
      "openmoji/1F604_color.png", "openmoji/1F605_color.png"};
  for (size_t i = 0; i < sizeof(fixture_paths) / sizeof(fixture_paths[0]); i++) {
    cairo_surface_t *fixture =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 1);
    cairo_t *fixture_context = cairo_create(fixture);
    cairo_set_source_rgba(fixture_context, 1, 0, 0, 0.5);
    cairo_paint(fixture_context);
    assert(cairo_surface_write_to_png(fixture, fixture_paths[i]) ==
           CAIRO_STATUS_SUCCESS);
    cairo_destroy(fixture_context);
    cairo_surface_destroy(fixture);
  }

  char *error = NULL;
  TaiDocument *document = tai_html_parse("<span>😀 😁 😂 😃 😄 😅</span>", &error);
  assert(document && !error);
  TaiNode *root = tai_document_root(document);
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block}", &error);
  assert(sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 100.0, false, &error);
  TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
  assert(list && !error && tai_display_list_count(list) == 6);
  const TaiDisplayCommand *image = tai_display_list_command(list, 0);
  assert(image && image->kind == TAI_DRAW_IMAGE);
  assert(image->width == 22 && image->height == 6);
  TaiDisplayHit hit = {0};
  assert(!tai_display_list_hit_test(list, image->x + 1, image->y + 1, &hit));

  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-image.png", 100, 60,
                                    &error));
  cairo_surface_t *output =
      cairo_image_surface_create_from_png("/tmp/tai-ci-image.png");
  assert(cairo_surface_status(output) == CAIRO_STATUS_SUCCESS);
  uint32_t center = pixel(output, (int)image->x + 11, (int)image->y + 3);
  assert((center >> 24) == 255);
  assert(((center >> 16) & 255) >= 120 && ((center >> 16) & 255) <= 135);
  assert(((center >> 8) & 255) >= 120 && ((center >> 8) & 255) <= 135);
  cairo_surface_destroy(output);
  tai_display_list_destroy(list);
  free(error);

  error = NULL;
  document = tai_html_parse(
      "<main style='height:10px;overflow:scroll;border-radius:3px;"
      "filter:blur(0.5px);opacity:50%;mix-blend-mode:multiply'>"
      "<div>😀</div></main>", &error);
  assert(document && !error);
  root = tai_document_root(document);
  sheet = tai_css_parse(
      "html {display:block} body {display:block} main {display:block} "
      "div {display:block}", &error);
  assert(document && sheet && tai_css_style(root, sheet, &error));
  layout = tai_layout_create(root, 100.0, false, &error);
  list = layout ? tai_display_list_create(layout, &error) : NULL;
  assert(list && !error);
  const TaiDrawKind expected[] = {
      TAI_PUSH_BLEND, TAI_PUSH_CLIP, TAI_PUSH_BLUR,
      TAI_DRAW_HIT_TEST, TAI_PUSH_CLIP_SCROLL, TAI_DRAW_IMAGE,
      TAI_POP_CLIP_SCROLL, TAI_POP_BLUR, TAI_POP_CLIP, TAI_POP_BLEND};
  assert(tai_display_list_count(list) ==
         sizeof(expected) / sizeof(expected[0]));
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
    assert(tai_display_list_command(list, i)->kind == expected[i]);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-image-effects.png",
                                    100, 60, &error));
  tai_display_list_destroy(list);
  free(error);
  for (size_t i = 0; i < sizeof(fixture_paths) / sizeof(fixture_paths[0]); i++)
    assert(unlink(fixture_paths[i]) == 0);
  (void)rmdir("openmoji");
  assert(chdir(original) == 0);
  assert(rmdir(temporary) == 0);
}

static void test_scroll_clip(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<main style=\"height:20px;overflow:scroll;background-color:#ff0000;"
      "opacity:50%\">"
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

  assert(tai_display_list_count(list) == 12);
  const TaiDisplayCommand *blend_push = tai_display_list_command(list, 0);
  const TaiDisplayCommand *outer_push = tai_display_list_command(list, 1);
  const TaiDisplayCommand *hit_only = tai_display_list_command(list, 3);
  const TaiDisplayCommand *push = tai_display_list_command(list, 4);
  const TaiDisplayCommand *pop = tai_display_list_command(list, 9);
  const TaiDisplayCommand *outer_pop = tai_display_list_command(list, 10);
  const TaiDisplayCommand *blend_pop = tai_display_list_command(list, 11);
  assert(blend_push && blend_push->kind == TAI_PUSH_BLEND);
  assert(outer_push && outer_push->kind == TAI_PUSH_CLIP);
  assert(hit_only && hit_only->kind == TAI_DRAW_HIT_TEST);
  assert(hit_only->node_id == scroller->id);
  assert(push && push->kind == TAI_PUSH_CLIP_SCROLL);
  assert(push->x == 13 && push->y == 18 && push->width == 174);
  assert(push->height == 20 && push->scroll_y == 20);
  assert(pop && pop->kind == TAI_POP_CLIP_SCROLL);
  assert(outer_pop && outer_pop->kind == TAI_POP_CLIP);
  assert(blend_pop && blend_pop->kind == TAI_POP_BLEND);

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
  assert(inside[0] == 127 && inside[1] == 255 && inside[2] == 127 &&
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

  /* A page adapter converts viewport y=10 to document y=20 once. The same
   * document origin passed to raster must expose the same nested-scroll leaf. */
  assert(tai_display_list_hit_test(list, 150, 10 + 10, &hit));
  assert(hit.node_id == blue->id);
  assert(tai_display_list_write_png_region(
      list, "/tmp/tai-ci-page-and-element-scroll.png", 200, 40, 0, 10,
      &error));
  surface = cairo_image_surface_create_from_png(
      "/tmp/tai-ci-page-and-element-scroll.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  pixel = cairo_image_surface_get_data(surface) +
          10 * cairo_image_surface_get_stride(surface) + 150 * 4;
  assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 &&
         pixel[3] == 255);
  cairo_surface_destroy(surface);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
}

static void test_rounded_overflow_clip_raster_and_hit(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<main style=\"height:20px;overflow:clip;border-radius:10px;"
      "background-color:red;opacity:50%\"><div style=\"height:40px;"
      "background-color:blue\"></div></main>", &error);
  assert(document && !error);
  TaiNode *root = tai_document_root(document);
  TaiNode *child = find_tag(root, "div");
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} main {display:block} "
      "div {display:block}", &error);
  assert(child && sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
  assert(layout && list && !error);
  bool found_clip = false;
  size_t blend_push = SIZE_MAX, clip_push = SIZE_MAX;
  size_t clip_pop = SIZE_MAX, blend_pop = SIZE_MAX;
  for (size_t index = 0; index < tai_display_list_count(list); index++) {
    const TaiDisplayCommand *command = tai_display_list_command(list, index);
    if (command->kind == TAI_PUSH_BLEND) blend_push = index;
    if (command->kind == TAI_PUSH_CLIP) {
      assert(command->radius == 10.0 && command->scroll_y == 0.0);
      found_clip = true;
      clip_push = index;
    }
    if (command->kind == TAI_POP_CLIP) clip_pop = index;
    if (command->kind == TAI_POP_BLEND) blend_pop = index;
  }
  assert(found_clip);
  assert(blend_push < clip_push && clip_push < clip_pop &&
         clip_pop < blend_pop);

  TaiDisplayHit hit = {0};
  /* Python's Blend overflow mask only affects raster; a descendant leaf can
   * still win point hit testing at this rounded-off corner. */
  assert(tai_display_list_hit_test(list, 13, 18, &hit));
  assert(hit.node_id == child->id);
  assert(tai_display_list_hit_test(list, 100, 18, &hit));
  assert(hit.node_id == child->id);
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-rounded-clip.png",
                                    200, 80, &error));
  cairo_surface_t *surface =
      cairo_image_surface_create_from_png("/tmp/tai-ci-rounded-clip.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  assert(pixel(surface, 13, 18) == 0xffffffffU);
  /* The isolated destination-in mask removes the parent's red fill at the
   * antialiased edge instead of blending it into the child blue. */
  assert(pixel(surface, 19, 18) == 0xffffffffU);
  assert(pixel(surface, 20, 18) == 0x7f7fffffU);
  assert(pixel(surface, 100, 18) == 0x7f7fffffU);
  cairo_surface_destroy(surface);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
}

static void test_rounded_fill_raster_and_hit(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<div style=\"height:20px;border-radius:10px;"
      "background-color:red\"></div>", &error);
  assert(document && !error);
  TaiNode *root = tai_document_root(document);
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} div {display:block}", &error);
  assert(sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
  assert(layout && list && !error);
  TaiDisplayHit hit = {0};
  assert(!tai_display_list_hit_test(list, 13, 18, &hit));
  assert(tai_display_list_hit_test(list, 100, 18, &hit));
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-rounded-fill.png",
                                    200, 80, &error));
  cairo_surface_t *surface =
      cairo_image_surface_create_from_png("/tmp/tai-ci-rounded-fill.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  assert(pixel(surface, 13, 18) == 0xffffffffU);
  assert(pixel(surface, 100, 18) == 0xff0000ffU);
  cairo_surface_destroy(surface);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
}

static void test_subtree_compositing(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<div style='display:block;height:40px;background:#ff0000'>"
      "<div style='display:block;height:20px;background:#0000ff;opacity:50%'>"
      "</div>"
      "<div style='display:block;height:20px;background:#0000ff;"
      "mix-blend-mode:difference'></div></div>", &error);
  assert(document && !error);
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} div {display:block}", &error);
  TaiNode *root = tai_document_root(document);
  assert(sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
  assert(layout && list && !error);

  size_t pushes = 0, pops = 0;
  for (size_t index = 0; index < tai_display_list_count(list); index++) {
    const TaiDisplayCommand *command = tai_display_list_command(list, index);
    if (command->kind == TAI_PUSH_BLEND) pushes++;
    if (command->kind == TAI_POP_BLEND) pops++;
  }
  assert(pushes == 2 && pops == 2);
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-compositing.png",
                                    200, 80, &error));
  cairo_surface_t *surface =
      cairo_image_surface_create_from_png("/tmp/tai-ci-compositing.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  assert(pixel(surface, 20, 20) == 0x7f0080ffU);
  assert(pixel(surface, 20, 45) == 0xff00ffffU);
  cairo_surface_destroy(surface);

  TaiDisplayHit hit = {0};
  assert(tai_display_list_hit_test(list, 20, 20, &hit));
  assert(hit.kind == TAI_DRAW_FILL_RECT);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
}

static void test_opacity_applies_once_to_complete_subtree(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<div style='display:block;height:20px;background-color:#ff0000;"
      "opacity:50%'><div style='height:20px;background-color:#0000ff'>"
      "</div></div>", &error);
  assert(document && !error);
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} div {display:block}", &error);
  TaiNode *root = tai_document_root(document);
  assert(sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
  assert(layout && list && !error);
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-subtree-opacity.png",
                                    200, 80, &error));
  cairo_surface_t *surface =
      cairo_image_surface_create_from_png("/tmp/tai-ci-subtree-opacity.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  /* The blue child first replaces the red parent inside one group. Applying
   * 50% once to that group yields this value; per-leaf alpha would retain red. */
  assert(pixel(surface, 20, 20) == 0x7f7fffffU);
  cairo_surface_destroy(surface);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
}

static void test_blend_modes_and_sibling_isolation(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<div style='display:block;height:60px;background-color:#ff0000'>"
      "<div style='height:20px;background-color:#0000ff;mix-blend-mode:multiply'>"
      "</div>"
      "<div style='height:20px;background-color:#0000ff;mix-blend-mode:difference'>"
      "</div>"
      "<div style='height:20px;background-color:#0000ff;"
      "mix-blend-mode:unknown'></div></div>", &error);
  assert(document && !error);
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} div {display:block}", &error);
  TaiNode *root = tai_document_root(document);
  assert(sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
  assert(layout && list && !error);
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-blend-modes.png",
                                    200, 120, &error));
  cairo_surface_t *surface =
      cairo_image_surface_create_from_png("/tmp/tai-ci-blend-modes.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  assert(pixel(surface, 20, 20) == 0x000000ffU);
  assert(pixel(surface, 20, 40) == 0xff00ffffU);
  assert(pixel(surface, 20, 60) == 0x0000ffffU);
  cairo_surface_destroy(surface);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
}

static void test_destination_in_compositing(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<div style='display:block;height:40px;background-color:#ff0000'>"
      "<div style='height:20px;background-color:#0000ff;"
      "mix-blend-mode:destination-in'></div></div>", &error);
  assert(document && !error);
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} div {display:block}", &error);
  TaiNode *root = tai_document_root(document);
  assert(sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 200.0, false, &error);
  TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
  assert(layout && list && !error);
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-destination-in.png",
                                    200, 80, &error));
  cairo_surface_t *surface =
      cairo_image_surface_create_from_png("/tmp/tai-ci-destination-in.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  assert(pixel(surface, 20, 20) == 0xff0000ffU);
  assert(pixel(surface, 20, 60) == 0x00000000U);
  cairo_surface_destroy(surface);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
}

static void test_blur_structure_order_raster_and_hit(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<div style='display:block;height:40px;background:white'>"
      "<div style='display:block;height:20px;width:20px;background:black;"
      "filter: BLUR( 2PX ) ;overflow:clip;border-radius:4px;opacity:50%'>"
      "<div style='height:10px;background:red'></div></div>"
      "<div style='height:20px;background:blue'></div></div>", &error);
  assert(document && !error);
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} div {display:block}", &error);
  TaiNode *root = tai_document_root(document);
  assert(sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 100.0, false, &error);
  TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
  assert(layout && list && !error);

  size_t blend_push = SIZE_MAX, clip_push = SIZE_MAX, blur_push = SIZE_MAX;
  size_t blur_pop = SIZE_MAX, clip_pop = SIZE_MAX, blend_pop = SIZE_MAX;
  for (size_t index = 0; index < tai_display_list_count(list); index++) {
    const TaiDisplayCommand *command = tai_display_list_command(list, index);
    if (command->kind == TAI_PUSH_BLEND) blend_push = index;
    if (command->kind == TAI_PUSH_CLIP) clip_push = index;
    if (command->kind == TAI_PUSH_BLUR) {
      blur_push = index;
      assert(command->sigma == 2.0);
    }
    if (command->kind == TAI_POP_BLUR) blur_pop = index;
    if (command->kind == TAI_POP_CLIP) clip_pop = index;
    if (command->kind == TAI_POP_BLEND) blend_pop = index;
  }
  assert(blend_push < clip_push && clip_push < blur_push &&
         blur_push < blur_pop && blur_pop < clip_pop && clip_pop < blend_pop);

  TaiDisplayHit hit = {0};
  assert(tai_display_list_hit_test(list, 15, 20, &hit));
  assert(!tai_display_list_hit_test(list, 10, 20, &hit));
  assert(tai_display_list_write_png(list, "/tmp/tai-ci-blur.png", 100, 80,
                                    &error));
  cairo_surface_t *surface =
      cairo_image_surface_create_from_png("/tmp/tai-ci-blur.png");
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  /* Blur moves black pixels toward the white interior, while overflow clips
   * every expanded pixel at the rounded border and leaves the sibling blue. */
  uint32_t blurred = pixel(surface, 30, 25);
  assert(blurred != 0x7f7fffffU && blurred != 0xffffffffU);
  assert(pixel(surface, 20, 45) == 0x0000ffffU);
  assert(pixel(surface, 12, 18) == 0xffffffffU);
  cairo_surface_destroy(surface);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
}

static void test_blur_noop_elision(void) {
  static const char *const filters[] = {
      "blur()", "blur(0)", "blur(+0)", "blur(-0.0)", "blur(0px)",
      "blur(-2px)", "blur(nanpx)", "blur(infpx)", "blur(2)",
      "blur(2px) extra", "none"};
  for (size_t case_index = 0;
       case_index < sizeof(filters) / sizeof(filters[0]); case_index++) {
    char html[256];
    snprintf(html, sizeof(html),
             "<div style='height:20px;background:red;filter:%s'></div>",
             filters[case_index]);
    char *error = NULL;
    TaiDocument *document = tai_html_parse(html, &error);
    TaiStylesheet *sheet = tai_css_parse(
        "html {display:block} body {display:block} div {display:block}", &error);
    TaiNode *root = document ? tai_document_root(document) : NULL;
    assert(document && sheet && tai_css_style(root, sheet, &error));
    TaiLayout *layout = tai_layout_create(root, 100.0, false, &error);
    TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
    assert(list && !error);
    for (size_t index = 0; index < tai_display_list_count(list); index++)
      assert(tai_display_list_command(list, index)->kind != TAI_PUSH_BLUR &&
             tai_display_list_command(list, index)->kind != TAI_POP_BLUR);
    tai_display_list_destroy(list);
    tai_layout_destroy(layout);
    tai_css_destroy(sheet);
    tai_document_destroy(document);
    free(error);
  }
}

static void test_blur_work_budget_fails_safely(void) {
  char *error = NULL;
  TaiDocument *document = tai_html_parse(
      "<div style='height:20px;background:red;filter:blur(100px)'></div>",
      &error);
  TaiStylesheet *sheet = tai_css_parse(
      "html {display:block} body {display:block} div {display:block}", &error);
  TaiNode *root = document ? tai_document_root(document) : NULL;
  assert(document && sheet && tai_css_style(root, sheet, &error));
  TaiLayout *layout = tai_layout_create(root, 800.0, false, &error);
  TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
  assert(list && !error);
  assert(!tai_display_list_write_png(list, "/tmp/tai-ci-huge-blur.png",
                                     800, 532, &error));
  assert(error && strstr(error, "blur"));
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
  unsigned char *frame = NULL;
  int frame_stride = 0;
  assert(tai_display_list_raster_region(list, 200, 80, 0, 0,
                                         &frame, &frame_stride, &error));
  assert(frame && frame_stride >= 200 * 4);
  unsigned char *frame_pixel = frame + 37 * frame_stride + 14 * 4;
  assert(frame_pixel[0] == 0 && frame_pixel[1] == 0 &&
         frame_pixel[2] == 255 && frame_pixel[3] == 255);
  free(frame);
  frame = NULL;
  assert(tai_display_list_raster_region(list, 100, 60, 0, 0,
                                         &frame, &frame_stride, &error));
  assert(frame && frame_stride >= 100 * 4);
  free(frame);
  frame = NULL;
  assert(!tai_display_list_raster_region(list, 0, 80, 0, 0,
                                          &frame, &frame_stride, &error));
  assert(!frame && error);

  tai_display_list_destroy(list);
  free(error);
  test_scroll_clip();
  test_transparent_scroll_hit_region();
  test_nested_scroll_raster_and_hit_agree();
  test_rounded_overflow_clip_raster_and_hit();
  test_rounded_fill_raster_and_hit();
  test_subtree_compositing();
  test_opacity_applies_once_to_complete_subtree();
  test_blend_modes_and_sibling_isolation();
  test_destination_in_compositing();
  test_blur_structure_order_raster_and_hit();
  test_blur_noop_elision();
  test_blur_work_budget_fails_safely();
  test_image_ownership_raster_and_hit();
  return 0;
}
