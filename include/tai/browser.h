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
    const char *default_css, double viewport_width, bool rtl, char **error);
void tai_page_destroy(TaiPage *page);
TaiNode *tai_page_root(const TaiPage *page);
const TaiLayout *tai_page_layout(const TaiPage *page);
const TaiDisplayList *tai_page_display_list(const TaiPage *page);
const TaiUrl *tai_page_url(const TaiPage *page);
void tai_page_json(FILE *out, const TaiPage *page);

#endif
