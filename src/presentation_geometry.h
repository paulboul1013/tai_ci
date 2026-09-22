#ifndef TAI_PRESENTATION_GEOMETRY_H
#define TAI_PRESENTATION_GEOMETRY_H

#include <stdbool.h>

typedef struct {
  float x, y, w, h;
} TaiScrollbarRect;

bool tai_scrollbar_geometry(double width, double height, double scroll,
                            double max_scroll, TaiScrollbarRect *rect);

#endif
