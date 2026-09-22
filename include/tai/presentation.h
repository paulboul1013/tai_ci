#ifndef TAI_PRESENTATION_H
#define TAI_PRESENTATION_H

#include "tai/browser.h"

/* Blocks on the calling thread until the window is closed. The page is borrowed
 * for the entire call. Resize events rebuild its layout; accepted wheel and
 * PageUp/PageDown events update its owned scroll state and repaint only when
 * that clamped state changes. All SDL and Cairo resources are owned here. */
bool tai_present_window(TaiPage *page, int width, int height, char **error);

#endif
