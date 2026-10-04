#ifndef TAI_JS_H
#define TAI_JS_H

#include "tai/dom.h"

#include <stdint.h>

typedef struct TaiJsContext TaiJsContext;

typedef enum {
    TAI_JS_REPORT_LOG,         /* log(x): text is display text, json the value */
    TAI_JS_REPORT_EVENT_ERROR, /* a listener or dispatch failed: event, text */
    TAI_JS_REPORT_RAF_ERROR,   /* an animation frame callback failed: text */
    TAI_JS_REPORT_TASK_ERROR   /* a timer or asynchronous XHR task failed:
                                  event is Python's message prefix, such as
                                  "setTimeout callback crashed", then text */
} TaiJsReportKind;

/* Borrowed for the duration of the report callback only. */
typedef struct {
    TaiJsReportKind kind;
    const char *event; /* event type for TAI_JS_REPORT_EVENT_ERROR; the
                          message prefix for TAI_JS_REPORT_TASK_ERROR */
    const char *text;  /* log display text, or the exception as a string */
    const char *json;  /* TAI_JS_REPORT_LOG: JSON of the value ("null" when
                          it has no JSON form); NULL otherwise */
} TaiJsReport;

typedef enum {
    TAI_JS_HOST_OK,
    TAI_JS_HOST_ERROR,    /* JS gets Error(message) */
    TAI_JS_HOST_NO_MEMORY /* JS gets an out-of-memory error */
} TaiJsHostStatus;

/* Callbacks from the JS layer into its owner; every member may be NULL. They
 * run synchronously on the thread executing JS and must not re-enter this
 * context.
 *   invalidated   the DOM changed; the owner rebuilds style/layout later
 *   node_removed  node (and its subtree) left its parent; it may be
 *                 re-attached elsewhere by the same operation
 *   report        diagnostics; NULL writes them to stderr (D1)
 *   cookie_get    document.cookie; *value is owned by the caller. NULL reads ""
 *   cookie_set    document.cookie = value. NULL ignores the write
 *   xhr_send      synchronous XMLHttpRequest.send: url as given to open(),
 *                 body NULL for null. On OK *response, otherwise *message
 *                 (may stay NULL), is owned by the caller. The time it blocks
 *                 does not count against the script time limit, up to 30 s
 *                 per outermost entry. NULL throws Error
 *   cancelled     polled by the interrupt handler: true stops the running
 *                 script uncatchably. Must be cheap and lock-free
 *   animation_frame_requested
 *                 requestAnimationFrame(cb) queued cb; the owner should run
 *                 tai_js_run_animation_frame at its next frame
 *   xhr_start     asynchronous XMLHttpRequest.send (D5): url as given to
 *                 open(), body NULL for null. OK means the owner will call
 *                 tai_js_finish_xhr(handle) exactly once, later, unless the
 *                 context is destroyed first; ERROR throws Error(*message)
 *                 (owned by the caller) at send(). NULL throws Error
 *   now           the timer clock in seconds; NULL uses tai_js_clock() */
typedef struct {
    void (*invalidated)(void *userdata);
    void (*node_removed)(void *userdata, TaiNode *node);
    void (*report)(void *userdata, const TaiJsReport *report);
    TaiJsHostStatus (*cookie_get)(void *userdata, char **value);
    TaiJsHostStatus (*cookie_set)(void *userdata, const char *value);
    TaiJsHostStatus (*xhr_send)(void *userdata, const char *url,
                                const char *body, char **response,
                                char **message);
    bool (*cancelled)(void *userdata);
    void (*animation_frame_requested)(void *userdata);
    TaiJsHostStatus (*xhr_start)(void *userdata, const char *url,
                                 const char *body, uint64_t handle,
                                 char **message);
    double (*now)(void *userdata);
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
/* One animation frame's callbacks: runRAFHandlers() takes the queued batch;
 * callbacks queued meanwhile wait for the next frame. A throwing callback is
 * reported and the rest of the batch still runs (D10); an error that escapes
 * the batch (the time limit, out of memory) is reported and ends it. Returns
 * false only for invalid input. */
bool tai_js_run_animation_frame(TaiJsContext *context, char **error);

/* Timers (D5; Python JSContext.setTimeout/setInterval). The context keeps
 * them; its owner runs them on the thread that runs the context's JS, and
 * destroying the context drops them (Python's discard). Delays follow
 * Python: a value float() rejects, a negative or a non-finite one is 0, and
 * an interval waits at least 1 ms. Intervals keep the ideal timeline
 * start + N * delay, so a late owner runs the missed ticks back to back. */
/* A monotonic clock in seconds, the default timer clock. */
double tai_js_clock(void);
/* When the earliest timer is due, on the host's clock; INFINITY without
 * timers. */
double tai_js_next_timer(const TaiJsContext *context);
/* Runs up to budget timer callbacks due at or before now, earliest first
 * (ties in arming order); timers the callbacks arm run too when due by now.
 * A throwing callback is reported as "setTimeout callback crashed" or
 * "setInterval callback crashed" and the rest still run. *ran (may be NULL)
 * counts the callbacks. Returns false only for invalid input. */
bool tai_js_run_timers(TaiJsContext *context, double now, size_t budget,
    size_t *ran, char **error);
/* Completes an asynchronous XMLHttpRequest started through xhr_start. With a
 * body, runXHROnload sets responseText and calls onload (a throw is reported
 * as "XMLHttpRequest onload crashed"); without one, "Async XMLHttpRequest
 * failed <message>" is reported and onload never runs. Returns false only
 * for invalid input. */
bool tai_js_finish_xhr(TaiJsContext *context, uint64_t handle,
    const char *body, const char *message, char **error);

/* Testing only: replaces the JS heap limit (0 = none) and returns the bytes
 * the JS heap already uses, so tests can fail JS allocations one by one. */
size_t tai_js_set_memory_limit_for_test(TaiJsContext *context, size_t limit);

#endif
