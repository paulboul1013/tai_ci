#include "presentation_internal.h"
#include <math.h>

TaiAddressField tai_tabs_address_field(int width, bool secure) {
  double slot = secure ? TAI_SECURITY_ICON_SLOT : 0.0;
  /* The lock slot is added after inline layout, so it does not change where
   * the field wraps; only the field's start and its natural width move. */
  double natural = width >= 232 ? fmax(100.0, width - 150.0 - slot) : 100.0;
  TaiAddressField field = {.slot_x = address_x(width)};
  field.x = field.slot_x + slot;
  field.width = fmax(0.0, fmin(natural, (double)width - field.x));
  return field;
}

bool tai_scrollbar_geometry(double width, double height, double scroll,
                            double max_scroll, TaiScrollbarRect *rect) {
  if (!rect || !isfinite(width) || !isfinite(height) || !isfinite(scroll) ||
      !isfinite(max_scroll) || width <= 0 || height <= 0 ||
      max_scroll <= 0 || scroll < 0 || scroll > max_scroll)
    return false;
  double document_height = height + max_scroll;
  if (!isfinite(document_height)) return false;
  double bar_height = fmin(height, fmax(20.0, height * height / document_height));
  double bar_y = (height - bar_height) * scroll / max_scroll;
  rect->x = (float)fmax(0.0, width - 12.0);
  rect->y = (float)bar_y;
  rect->w = (float)fmin(width, 12.0);
  rect->h = (float)bar_height;
  return true;
}
