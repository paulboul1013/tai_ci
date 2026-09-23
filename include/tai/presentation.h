#ifndef TAI_PRESENTATION_H
#define TAI_PRESENTATION_H

#include "tai/browser.h"

/* The caller owns *page and may replace it after loading a candidate page. The
 * intent is borrowed for the callback; failed loads should return true with the
 * old page intact so the window remains usable. */
typedef bool (*TaiPresentNavigate)(void *userdata, TaiPage **page,
                                  const TaiNavigationIntent *intent,
                                  char **error);

/* Compatibility display entry point. It blocks on the calling thread until the
 * window is closed and borrows page for the entire call. Resize events rebuild
 * its layout; accepted scroll events update its state. It has no navigation
 * callback, so callers that need link/form navigation must use the variant
 * below. All SDL and Cairo resources are owned here. */
bool tai_present_window(TaiPage *page, int width, int height, char **error);
/* Variant with a caller-owned page slot and same-window navigation adapter.
 * Only the callback changes page/session ownership; SDL/texture ownership
 * stays in presentation. */
bool tai_present_window_with_navigation(TaiPage **page, int width, int height,
                                        TaiPresentNavigate navigate,
                                        void *userdata, char **error);

#endif
