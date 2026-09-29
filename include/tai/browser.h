#ifndef TAI_BROWSER_H
#define TAI_BROWSER_H

#include "tai/js.h"
#include "tai/layout.h"
#include "tai/network.h"
#include "tai/render.h"

/* The browser's name: the window title before a page commits and for pages
 * without a usable <title>. The Python oracle uses its own name. */
#define TAI_BROWSER_NAME "Tai Ci"

typedef struct TaiPage TaiPage;
typedef struct TaiPageLoad TaiPageLoad;
typedef struct TaiNavigationIntent TaiNavigationIntent;
typedef enum {
  TAI_PAGE_KEY_BACKSPACE,
  TAI_PAGE_KEY_LEFT,
  TAI_PAGE_KEY_RIGHT,
  TAI_PAGE_KEY_RETURN
} TaiPageKey;

/* How a page's scripts reach the network: document.cookie and synchronous
 * XMLHttpRequest. The owner that runs the page's JS injects it; every
 * pointer is borrowed and must outlive the page's use of it.
 *   request     sends one request and blocks until it completes, on the
 *               thread running the page's JS. Returns an owned response (a
 *               transport failure sets response->error), or NULL with an
 *               owned *message (NULL means out of memory)
 *   checkpoint  called between load-time scripts; may service other work.
 *               false means the load was cancelled: the remaining scripts
 *               are skipped and the load fails. May be NULL
 *   cancelled   lock-free; true stops the running script. May be NULL
 *   cookies     the shared jar; NULL makes document.cookie read "" */
typedef struct {
    TaiResponse *(*request)(void *userdata, const TaiUrl *url,
                            const TaiUrl *referrer, const char *payload,
                            const char *origin, const char *referrer_policy,
                            char **message);
    bool (*checkpoint)(void *userdata);
    bool (*cancelled)(void *userdata);
    TaiCookieJar *cookies;
    void *userdata;
} TaiPageNet;
/* Total time limit of one synchronous XMLHttpRequest, across redirects. */
#define TAI_XHR_TIMEOUT_SECONDS 30.0
/* The owned JS error message for a request that did not complete, or NULL
 * when allocation fails. */
char *tai_page_net_wait_message(TaiWaitStatus status);
/* A TaiPageNet.request for userdata = a TaiNetwork used directly on its
 * owner thread (headless and single-page paths). */
TaiResponse *tai_page_net_direct_request(void *network, const TaiUrl *url,
    const TaiUrl *referrer, const char *payload, const char *origin,
    const char *referrer_policy, char **message);
/* Replaces the page's net (copied; the pointed-to objects are not). Only the
 * thread that currently owns the page may call it, never while its JS runs. */
void tai_page_set_net(TaiPage *page, const TaiPageNet *net);

/* Async page construction is driven only by the TaiNetwork owner thread. The
 * load borrows network until completion or cancellation; the network must
 * outlive that operation. net (may be NULL: no XHR, no cookies) is copied
 * into the page for its load-time scripts; the owner replaces it with
 * tai_page_set_net before the page runs JS on another thread.
 * Completion transfers page and owned error to done. A non-NULL page with
 * network_failure=true is the Python-compatible Network or Certificate Error
 * document for the requested URL.
 * The load handle expires immediately before completion. */
typedef void (*TaiPageLoadDone)(void *userdata, TaiPage *page,
                                bool network_failure, char *error);
TaiPageLoad *tai_page_load_async(TaiNetwork *network, const TaiUrl *url,
    const TaiUrl *referrer, const char *payload, const char *default_css,
    double viewport_width, double viewport_height, bool rtl,
    const TaiPageNet *net, TaiPageLoadDone done, void *userdata, char **error);
/* Builds generated HTML at an about: URL through the normal document, inline
 * resource, style, layout and display paths. The markup is borrowed only for
 * this call. External resource references are not fetched. Valid input always
 * invokes done synchronously and returns NULL (the handle has expired);
 * ownership of page and error transfers to done. Invalid input or failure to
 * create the page invokes no callback and sets *error. */
TaiPageLoad *tai_page_load_async_markup(TaiNetwork *network, const TaiUrl *url,
    const char *markup, const char *default_css, double viewport_width,
    double viewport_height, bool rtl, const TaiPageNet *net,
    TaiPageLoadDone done, void *userdata, char **error);
/* Cancels all outstanding document/subresource requests and destroys partial
 * construction. Must run on the same thread that started the load, while the
 * borrowed network is still alive. */
void tai_page_load_async_cancel(TaiPageLoad *load);

/* Loads a stable page state synchronously for headless use. Network ownership
 * remains with the caller. The page's scripts use network and its cookie jar
 * directly on the calling thread, so the page must be destroyed before the
 * network and its JS must run on the network's owner thread. default_css is copied through the parsed stylesheet.
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
/* Transfers the URL of a same-document fragment activation, if any. A caller
 * must consume and finish it before the next fragment activation. */
bool tai_page_take_fragment_change(TaiPage *page, char **url);
/* True while a taken fragment change remains pending and changes URL text. */
bool tai_page_fragment_url_changed(const TaiPage *page);
/* Resolves the pending fragment transaction after session history recording.
 * On failure restores the old URL and scroll without allocation. */
void tai_page_finish_fragment_change(TaiPage *page, bool committed);
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
/* Explicit page scroll changes clamp to document overflow. Resize preserves
 * the existing top-level offset, which may then exceed the new maximum;
 * non-finite explicit values fail without changing state. */
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
/* Clear the currently focused page control, rebuilding its display list when
 * focus styling changes. */
bool tai_page_blur_input(TaiPage *page, bool *changed, char **error);
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
/* True when the page's requested URL is https and its document response had
 * no transport or certificate error (the oracle's Tab.secure). Redirects do
 * not change it; internal and error pages are never secure. */
bool tai_page_secure(const TaiPage *page);
/* The oracle's Tab.get_title() computed from the current DOM: the first
 * <title> in document order whose direct Text children, joined and stripped
 * like Python str.strip(), are non-empty. Returns an owned string, "" when no
 * such title exists (the caller picks the fallback name), or NULL on
 * allocation failure. */
char *tai_page_title(const TaiPage *page);
void tai_page_json(FILE *out, const TaiPage *page);

#endif
