#ifndef TAI_LAYOUT_H
#define TAI_LAYOUT_H
#include "tai/dom.h"
typedef struct TaiLayout TaiLayout;
typedef enum {
  TAI_LAYOUT_DOCUMENT,
  TAI_LAYOUT_BLOCK,
  TAI_LAYOUT_LINE,
  TAI_LAYOUT_TEXT,
  TAI_LAYOUT_IMAGE,
  TAI_LAYOUT_INPUT,
  TAI_LAYOUT_BUTTON
} TaiLayoutItemKind;
typedef enum {
  TAI_CONTROL_NONE,
  TAI_CONTROL_TEXT,
  TAI_CONTROL_PASSWORD,
  TAI_CONTROL_CHECKBOX,
  TAI_CONTROL_BUTTON
} TaiControlKind;
typedef struct {
  TaiLayoutItemKind kind;
  TaiControlKind control;
  const TaiNode *node;
  const char *word;
  double x, y, width, height, ascent, descent, space, font_size, caret_x;
  double content_height, scroll_y;
  bool bold, italic, scrollable;
  /* Borrowed immutable premultiplied Cairo ARGB32 pixels, valid only during
   * the callback. image_stride is bytes per row. */
  const unsigned char *image_pixels;
  int image_width, image_height, image_stride;
} TaiLayoutItem;
typedef bool (*TaiLayoutVisitor)(const TaiLayoutItem *item, void *opaque);
typedef enum { TAI_LAYOUT_ENTER, TAI_LAYOUT_LEAVE } TaiLayoutVisitEvent;
typedef bool (*TaiLayoutTreeVisitor)(const TaiLayoutItem *item,
                                    TaiLayoutVisitEvent event, void *opaque);
/* Layout owns its tree/fonts/words; DOM is borrowed and must outlive layout.
 * Construction only clamps fixed overflow:scroll nodes' scroll_y state to the
 * computed range. One owner thread; no concurrent DOM or font access.
 */
TaiLayout *tai_layout_create(TaiNode *root, double viewport_width, bool rtl,
                             char **error);
void tai_layout_destroy(TaiLayout *layout);
void tai_layout_json(FILE *out, const TaiLayout *layout);
double tai_layout_height(const TaiLayout *layout);
/* Visits layout items in paint order. Item strings and node pointers are
 * borrowed for the duration of the callback; the visitor must not retain them
 * or mutate the DOM. */
bool tai_layout_visit(const TaiLayout *layout, TaiLayoutVisitor visitor,
                      void *opaque, char **error);
/* Walks the layout tree in paint nesting order. ENTER geometry is in document
 * coordinates; LEAVE closes any state opened for that same item. */
bool tai_layout_walk(const TaiLayout *layout, TaiLayoutTreeVisitor visitor,
                     void *opaque, char **error);
/* Resolves the Python strict-midpoint caret rule for a current text/password
 * control. node_id is stable while its owning document lives. */
bool tai_layout_control_caret_index(const TaiLayout *layout, size_t node_id,
                                    double document_x, size_t *index);
#endif
