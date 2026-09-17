#ifndef TAI_RENDER_H
#define TAI_RENDER_H

#include "tai/layout.h"
#include <stdint.h>

typedef struct TaiDisplayList TaiDisplayList;
typedef enum {
  TAI_DRAW_FILL_RECT,
  TAI_DRAW_TEXT,
  TAI_DRAW_HIT_TEST,
  TAI_PUSH_CLIP_SCROLL,
  TAI_POP_CLIP_SCROLL
} TaiDrawKind;

typedef struct {
  TaiDrawKind kind;
  double x, y, width, height;
  uint32_t rgba;
  const char *text;
  const char *font_family;
  double font_size;
  double ascent;
  double scroll_y;
  size_t node_id;
  bool bold, italic;
} TaiDisplayCommand;

typedef struct {
  size_t node_id;
  TaiDrawKind kind;
  double x, y, width, height;
} TaiDisplayHit;

/* The list owns all command strings and does not retain the DOM or layout. */
TaiDisplayList *tai_display_list_create(const TaiLayout *layout, char **error);
void tai_display_list_destroy(TaiDisplayList *list);
size_t tai_display_list_count(const TaiDisplayList *list);
const TaiDisplayCommand *tai_display_list_command(const TaiDisplayList *list,
                                                  size_t index);
/* Queries document-space coordinates in front-to-back paint order. On a miss
 * or invalid input, hit is cleared and false is returned. */
bool tai_display_list_hit_test(const TaiDisplayList *list, double x, double y,
                               TaiDisplayHit *hit);
void tai_display_list_json(FILE *out, const TaiDisplayList *list);

/* Renders a self-contained list to a Cairo PNG. The surface starts opaque
 * white; width and height are pixel dimensions and must be positive. */
bool tai_display_list_write_png(const TaiDisplayList *list, const char *path,
                                int width, int height, char **error);
/* Renders a viewport whose top-left corner is document_x/document_y. The
 * display list remains in document coordinates and is not mutated. */
bool tai_display_list_write_png_region(const TaiDisplayList *list,
                                       const char *path, int width, int height,
                                       double document_x, double document_y,
                                       char **error);

#endif
