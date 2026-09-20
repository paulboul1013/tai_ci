#ifndef TAI_PRESENTATION_H
#define TAI_PRESENTATION_H

#include "tai/browser.h"

/* Blocks on the calling thread until the window is closed. The page is borrowed
 * for the entire call. A non-zero pixel-size event rebuilds its layout before
 * the adapter borrows the replacement immutable display list. All SDL and
 * Cairo resources are owned by this module. */
bool tai_present_window(TaiPage *page, int width, int height, char **error);

#endif
