#ifndef TAI_BROWSER_H
#define TAI_BROWSER_H

#include "tai/js.h"
#include "tai/layout.h"
#include "tai/network.h"
#include "tai/render.h"

typedef struct TaiPage TaiPage;
typedef struct TaiNavigationIntent TaiNavigationIntent;
typedef enum {
  TAI_PAGE_KEY_BACKSPACE,
  TAI_PAGE_KEY_LEFT,
  TAI_PAGE_KEY_RIGHT,
  TAI_PAGE_KEY_RETURN
} TaiPageKey;

/* Loads a stable page state synchronously for headless use. Network ownership
 * remains with the caller. default_css is copied through the parsed stylesheet.
 * The page owns URL, response-derived DOM, CSS, JS and layout in destruction
 * order. External resources are discovered once in source order. */
TaiPage *tai_page_load(TaiNetwork *network, const TaiUrl *url,
    const char *default_css, double viewport_width, double viewport_height,
    bool rtl, char **error);
/* Loads a navigation request. A NULL payload is GET; a non-NULL payload is
 * POST, including an empty string. referrer is borrowed for the call. */
TaiPage *tai_page_load_request(TaiNetwork *network, const TaiUrl *url,
    const TaiUrl *referrer, const char *payload, const char *default_css,
    double viewport_width, double viewport_height, bool rtl, char **error);
/* A page owns an intent until take transfers it. Intent strings are owned by
 * the intent; accessors borrow them. GET has a NULL body, POST has a body. */
bool tai_page_take_navigation_intent(TaiPage *page,
                                    TaiNavigationIntent **intent);
const char *tai_navigation_intent_url(const TaiNavigationIntent *intent);
bool tai_navigation_intent_is_post(const TaiNavigationIntent *intent);
const char *tai_navigation_intent_body(const TaiNavigationIntent *intent);
void tai_navigation_intent_destroy(TaiNavigationIntent *intent);
/* Applies an owned intent in the outer page/session owner. A candidate loads
 * with the current page URL as referrer and current viewport size. On failure
 * *page is untouched; on success the old page is destroyed and replaced. */
bool tai_page_replace_from_intent(TaiNetwork *network, TaiPage **page,
    const TaiNavigationIntent *intent, const char *default_css, bool rtl,
    char **error);
void tai_page_destroy(TaiPage *page);
TaiNode *tai_page_root(const TaiPage *page);
const TaiLayout *tai_page_layout(const TaiPage *page);
const TaiDisplayList *tai_page_display_list(const TaiPage *page);
/* Rebuilds the layout and self-contained display list for a finite positive
 * viewport. On failure, every observable page field remains unchanged. */
bool tai_page_resize(TaiPage *page, double viewport_width,
                     double viewport_height, char **error);
double tai_page_viewport_width(const TaiPage *page);
double tai_page_viewport_height(const TaiPage *page);
/* Resolves a document-space display hit while the page owns its document. */
TaiNode *tai_page_hit_test(const TaiPage *page, double x, double y,
                           TaiDisplayHit *hit);
/* Page scroll is clamped to the document overflow. Non-finite values fail
 * without changing state. */
double tai_page_scroll_y(const TaiPage *page);
double tai_page_max_scroll_y(const TaiPage *page);
bool tai_page_set_scroll_y(TaiPage *page, double scroll_y);
/* Converts viewport coordinates to document coordinates exactly once, then
 * resolves the hit while the page owns its document. */
TaiNode *tai_page_viewport_hit_test(const TaiPage *page, double x, double y,
                                    TaiDisplayHit *hit);
/* Dispatches a primary viewport activation through the page's DOM/JS seam.
 * A finite miss, non-finite coordinate, or prevented event without invalidation
 * succeeds with *changed false. On a JS invalidation, this rebuilds the styled
 * layout and immutable display list before reporting *changed true. */
bool tai_page_activate_viewport(TaiPage *page, double x, double y,
                                bool *changed, char **error);
/* Inserts sanitized UTF-8 text into the focused text/password control. Cursor
 * positions are Unicode code-point indexes, never byte indexes. */
bool tai_page_text_input(TaiPage *page, const char *text, bool *changed,
                         char **error);
/* Applies the focused-control default for one supported special key. */
bool tai_page_key(TaiPage *page, TaiPageKey key, bool *changed, char **error);
bool tai_page_text_input_active(const TaiPage *page);
/* Writes the current page viewport with page scroll applied. */
bool tai_page_write_viewport_png(const TaiPage *page, const char *path,
                                 char **error);
const TaiUrl *tai_page_url(const TaiPage *page);
void tai_page_json(FILE *out, const TaiPage *page);

#endif
