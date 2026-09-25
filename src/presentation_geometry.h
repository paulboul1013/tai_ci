#ifndef TAI_PRESENTATION_GEOMETRY_H
#define TAI_PRESENTATION_GEOMETRY_H

#include <stdbool.h>

typedef struct {
  float x, y, w, h;
} TaiScrollbarRect;

bool tai_scrollbar_geometry(double width, double height, double scroll,
                            double max_scroll, TaiScrollbarRect *rect);

/* Map SDL window-logical pointer coordinates into the physical pixel space
 * used by presentation geometry and page hit testing. */
bool tai_presentation_pointer_to_pixels(double logical_x, double logical_y,
                                        int logical_width, int logical_height,
                                        int pixel_width, int pixel_height,
                                        double *pixel_x, double *pixel_y);

#endif
