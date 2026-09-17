#ifndef TAI_BROWSER_H
#define TAI_BROWSER_H

#include "tai/js.h"
#include "tai/layout.h"
#include "tai/network.h"
#include "tai/render.h"

typedef struct TaiPage TaiPage;

/* Loads a stable page state synchronously for headless use. Network ownership
 * remains with the caller. default_css is copied through the parsed stylesheet.
 * The page owns URL, response-derived DOM, CSS, JS and layout in destruction
 * order. External resources are discovered once in source order. */
TaiPage *tai_page_load(TaiNetwork *network, const TaiUrl *url,
    const char *default_css, double viewport_width, double viewport_height,
    bool rtl, char **error);
void tai_page_destroy(TaiPage *page);
TaiNode *tai_page_root(const TaiPage *page);
const TaiLayout *tai_page_layout(const TaiPage *page);
const TaiDisplayList *tai_page_display_list(const TaiPage *page);
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
/* Writes the current page viewport with page scroll applied. */
bool tai_page_write_viewport_png(const TaiPage *page, const char *path,
                                 char **error);
const TaiUrl *tai_page_url(const TaiPage *page);
void tai_page_json(FILE *out, const TaiPage *page);

#endif
