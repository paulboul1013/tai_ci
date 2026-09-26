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

/* Tabbed-chrome address field in physical pixels. A secure page reserves the
 * oracle's 30px lock slot [slot_x, slot_x + 30) before the field; the slot is
 * not a hit target. The field starts after it and, unlike Python, never
 * extends past the window so its bookmark star stays reachable. */
typedef struct {
  double slot_x;
  double x;
  double width;
} TaiAddressField;

enum { TAI_SECURITY_ICON_SLOT = 30, TAI_SECURITY_ICON_SIZE = 14 };

TaiAddressField tai_tabs_address_field(int width, bool secure);

#endif
