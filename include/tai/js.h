#ifndef TAI_JS_H
#define TAI_JS_H

#include "tai/dom.h"

typedef struct TaiJsContext TaiJsContext;

typedef enum {
    TAI_JS_REPORT_LOG,         /* log(x): text is display text, json the value */
    TAI_JS_REPORT_EVENT_ERROR  /* a listener or dispatch failed: event, text */
} TaiJsReportKind;

/* Borrowed for the duration of the report callback only. */
typedef struct {
    TaiJsReportKind kind;
    const char *event; /* event type for TAI_JS_REPORT_EVENT_ERROR */
    const char *text;  /* log display text, or the exception as a string */
    const char *json;  /* TAI_JS_REPORT_LOG: JSON of the value ("null" when
                          it has no JSON form); NULL otherwise */
} TaiJsReport;

/* Callbacks from the JS layer into its owner; every member may be NULL. They
 * run synchronously on the thread executing JS and must not re-enter it.
 *   invalidated   the DOM changed; the owner rebuilds style/layout later
 *   node_removed  node (and its subtree) left its parent; it may be
 *                 re-attached elsewhere by the same operation
 *   report        diagnostics; NULL writes them to stderr (D1) */
typedef struct {
    void (*invalidated)(void *userdata);
    void (*node_removed)(void *userdata, TaiNode *node);
    void (*report)(void *userdata, const TaiJsReport *report);
    void *userdata;
} TaiJsHost;

/* The context borrows the document root; the document must outlive it. host is
 * copied and may be NULL. All JS execution and DOM mutation occur on one
 * thread at a time; the owner may move the context between threads. */
TaiJsContext *tai_js_create(TaiNode *document_root, const TaiJsHost *host,
    char **error);
void tai_js_destroy(TaiJsContext *context);
/* Runs code as a classic script. *result, when non-NULL, receives the
 * completion value converted with String() (owned by the caller). */
bool tai_js_eval_value(TaiJsContext *context, const char *source_name,
    const char *code, char **result, char **error);
bool tai_js_eval(TaiJsContext *context, const char *source_name,
    const char *code, char **error);
/* Returns false only when the dispatch could not start (invalid input or
 * allocation failure); script failures are reported and count as not
 * prevented. */
bool tai_js_dispatch_event(TaiJsContext *context, const char *type,
    TaiNode *target, bool *default_prevented, char **error);

#endif
