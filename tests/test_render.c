#include "tai/css.h"
#include "tai/layout.h"
#include "tai/render.h"
#include <cairo/cairo.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  return 0;
}
