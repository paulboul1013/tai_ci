/* Prints native tabbed-chrome row geometry for tests/tab_strip_differential.py:
 * one JSON object per line with tabs, active, width, chrome_bottom, address_y
 * and back_y, for every tab count/active/width given on the command line as
 * TABS:ACTIVE:WIDTH triples. */
#include "../src/presentation_internal.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    unsigned tabs = 0, active = 0;
    int width = 0;
    if (sscanf(argv[i], "%u:%u:%d", &tabs, &active, &width) != 3 ||
        !tabs || active >= tabs || tabs > TAI_MAX_TABS || width <= 0)
      return EXIT_FAILURE;
    TaiTabSetView view = {.tab_count = tabs, .active_index = active};
    bool wraps = tai_pres_tab_row_wraps(&view, width);
    printf("{\"tabs\":%u,\"active\":%u,\"width\":%d,\"wraps\":%s,"
           "\"chrome_bottom\":%.3f,\"address_y\":%.3f,\"back_y\":%.3f}\n",
           tabs, active, width, wraps ? "true" : "false",
           tabs_chrome_bottom(width, wraps), tabs_address_y(width, wraps),
           tabs_back_button_y(wraps));
  }
  return EXIT_SUCCESS;
}
