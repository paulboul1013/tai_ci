#ifndef TAI_LAYOUT_H
#define TAI_LAYOUT_H
#include "tai/dom.h"
typedef struct TaiLayout TaiLayout;
typedef enum {
  TAI_LAYOUT_DOCUMENT,
  TAI_LAYOUT_BLOCK,
  TAI_LAYOUT_LINE,
  TAI_LAYOUT_TEXT
} TaiLayoutItemKind;
typedef struct {
  TaiLayoutItemKind kind;
  const TaiNode *node;
  const char *word;
  double x, y, width, height, ascent, descent, space, font_size;
  bool bold, italic;
} TaiLayoutItem;
typedef bool (*TaiLayoutVisitor)(const TaiLayoutItem *item, void *opaque);
/* Layout owns its tree/fonts/words; DOM is borrowed and must outlive layout.
 * Construction never mutates DOM. One owner thread; no concurrent font access.
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
#endif
