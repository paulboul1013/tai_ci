#ifndef TAI_PRESENTATION_H
#define TAI_PRESENTATION_H

#include "tai/browser.h"

typedef struct TaiTabSet TaiTabSet;

/* The caller owns *page and may replace it after loading a candidate page. The
 * intent is borrowed for the callback; failed loads should return true with the
 * old page intact so the window remains usable. */
typedef bool (*TaiPresentNavigate)(void *userdata, TaiPage **page,
                                  const TaiNavigationIntent *intent,
                                  char **error);
/* History/address callbacks abort the presentation loop if they return false.
 * Recoverable navigation failures should retain the current page and return
 * true; a successful replacement must leave *page nonnull. */
typedef bool (*TaiPresentHistory)(void *userdata, TaiPage **page,
                                  int direction, char **error);
typedef bool (*TaiPresentFragment)(void *userdata, const char *url,
                                   char **error);
typedef bool (*TaiPresentAddress)(void *userdata, TaiPage **page,
                                  const char *text, char **error);
typedef bool (*TaiPresentHistoryAvailable)(void *userdata, int direction);

typedef struct {
  TaiPresentNavigate navigate;
  TaiPresentHistory history;
  TaiPresentFragment fragment;
  TaiPresentAddress address;
  TaiPresentHistoryAvailable history_available;
  void *userdata;
} TaiPresentWindowCallbacks;

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
/* direction -1/1 handles Alt+Left/Alt+Right; fragment URL is borrowed. */
bool tai_present_window_with_history(TaiPage **page, int width, int height,
                                     TaiPresentNavigate navigate,
                                     TaiPresentHistory history,
                                     TaiPresentFragment fragment,
                                     void *userdata, char **error);
/* Chrome-enabled window. The window dimensions include the toolbar; the page
 * viewport excludes it. All callbacks and userdata are borrowed for the call. */
bool tai_present_window_with_chrome(
    TaiPage **page, int width, int height,
    const TaiPresentWindowCallbacks *callbacks, char **error);
/* Starts an asynchronous, tabbed window after the native window exists. The
 * tab set remains owned by the caller and is pumped on the SDL thread. */
bool tai_present_window_with_tabs(TaiTabSet *tabs, const char *initial_url,
                                  int width, int height, char **error);

#endif
