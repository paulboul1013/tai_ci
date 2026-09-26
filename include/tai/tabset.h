#ifndef TAI_TABSET_H
#define TAI_TABSET_H

#include "tai/session.h"

typedef struct TaiTabSet TaiTabSet;

enum { TAI_MAX_TABS = 25 };

/* Borrowed snapshot. page and url remain valid until the next tab-set mutation
 * or tai_tabset_pump(). page is NULL while the active tab has no committed
 * document; url then names its pending navigation when one exists. */
typedef struct {
    TaiPage *page;
    const char *url;
    size_t tab_count;
    size_t active_index;
    /* Includes an in-flight navigation's provisional history entry. */
    size_t history_count;
    size_t history_index;
    bool loading;
    bool can_go_back;
    bool can_go_forward;
    bool bookmarkable;
    bool bookmarked;
    /* The committed page was requested over https and loaded without a
     * transport or certificate error. It changes only when a page commits,
     * so a pending navigation keeps the previous page's state. */
    bool secure;
} TaiTabSetView;

/* The caller owns the tab set and default_css must outlive it. Creation starts
 * one loader thread; that thread creates, exclusively uses, and destroys the
 * shared TaiNetwork. All public operations except destroy are called by the
 * SDL/window owner thread. Loaded pages transfer from the loader to sessions
 * only when tai_tabset_pump() commits a matching tab ID and generation.
 * Bookmarks persist in the per-user data file; if it cannot be opened or
 * parsed, a warning goes to stderr, the file is left untouched, and this tab
 * set keeps bookmarks in memory only. */
TaiTabSet *tai_tabset_create(const char *default_css, bool rtl, char **error);
/* Embedders and deterministic integration tests can supply the home URL used
 * by New Tab. The ordinary constructor uses browser.engineering. Bookmarks are
 * memory-only. */
TaiTabSet *tai_tabset_create_with_home_url(const char *default_css, bool rtl,
                                          const char *home_url,
                                          char **error);
/* Integration tests only: like tai_tabset_create_with_home_url, but the
 * loader's network trusts the PEM CA bundle at ca_file (copied). tai-browser
 * never calls this. */
TaiTabSet *tai_tabset_create_for_test(const char *default_css, bool rtl,
                                      const char *home_url,
                                      const char *ca_file, char **error);
/* Starts the initial tab and navigation. width/height are page viewport pixels,
 * not outer window dimensions. The window may be presented before this
 * navigation completes. */
bool tai_tabset_start(TaiTabSet *tabs, const char *url, double width,
                      double height,
                      char **error);
/* Call after the window owner has stopped issuing other operations. Destroy
 * cancels and joins the loader, then releases sessions and queued completions. */
void tai_tabset_destroy(TaiTabSet *tabs);

/* New Tab creates and selects the default-home tab before its load completes.
 * Returns false with an error once TAI_MAX_TABS tabs are open. */
bool tai_tabset_new_tab(TaiTabSet *tabs, char **error);
bool tai_tabset_select(TaiTabSet *tabs, size_t index);
/* Copies the intent's URL/body and targets the active tab at call time. */
bool tai_tabset_navigate(TaiTabSet *tabs,
                         const TaiNavigationIntent *intent, char **error);
bool tai_tabset_navigate_address(TaiTabSet *tabs, const char *text,
                                 char **error);
/* The active committed page alone may be toggled. Pending navigation disables
 * the control; all tabs share one collection owned by the tab set. */
bool tai_tabset_toggle_bookmark(TaiTabSet *tabs, bool *bookmarked,
                                char **error);
/* Opens the internal list in the active tab through normal navigation/history. */
bool tai_tabset_open_bookmarks(TaiTabSet *tabs, char **error);
/* direction is -1 for Back and 1 for Forward. Failed loads leave the current
 * page and history index unchanged. */
bool tai_tabset_history_available(const TaiTabSet *tabs, int direction);
bool tai_tabset_history(TaiTabSet *tabs, int direction, char **error);
bool tai_tabset_record_fragment(TaiTabSet *tabs, const char *url,
                                char **error);
/* Resizes every committed tab page; pending candidates use the latest size
 * when their completion is committed. */
bool tai_tabset_resize(TaiTabSet *tabs, double width, double height,
                       char **error);
/* Drains completed work on the window owner thread. changed reports whether a
 * page, URL/history, or loading state changed and needs repainting. */
bool tai_tabset_pump(TaiTabSet *tabs, bool *changed, char **error);
bool tai_tabset_view(const TaiTabSet *tabs, TaiTabSetView *view);

#endif
