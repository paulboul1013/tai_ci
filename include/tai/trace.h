#ifndef TAI_TRACE_H
#define TAI_TRACE_H
/* Process-wide Chrome/Perfetto JSON trace, the native MeasureTime
 * (tests/reference/browser.py:665). It depends only on libc, pthreads and
 * tai_json_string, so any layer calls it without a handle.
 *
 * Disabled is the default: every emit is one relaxed atomic load and a return
 * until tai_trace_open succeeds. Only main.c (--window) opens and closes it.
 * Each event is written and flushed under one mutex as it happens, so a crash
 * leaves a loadable prefix; the file is valid JSON only after tai_trace_close. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Creates/truncates path and writes the header and process_name metadata.
 * Fails if a trace is already open or the file cannot be created. */
bool tai_trace_open(const char *path, char **error);
/* Writes "]}" and closes. Idempotent. Emits afterwards are ignored. Call only
 * once no other thread can emit (the loader has been joined). */
void tai_trace_close(void);

/* Emits one thread_name metadata event for the calling thread per open trace;
 * later calls are no-ops. A thread that emits first is named "Thread <tid>". */
void tai_trace_thread_name(const char *name);

/* Closed vocabulary: one name table in trace.c, each name meaning the same
 * work as the Python event of that name. */
typedef enum {
    TAI_SPAN_JAVASCRIPT,
    TAI_SPAN_RENDER,
    TAI_SPAN_TASK_ANIMATION_FRAME,
    TAI_SPAN_TASK_SET_TIMEOUT,
    TAI_SPAN_TASK_SET_INTERVAL,
    TAI_SPAN_TASK_XHR_ONLOAD,
    TAI_SPAN_TASK_FINISH_DOCUMENT,
    TAI_SPAN_TASK_FINISH_RESOURCES,
    TAI_SPAN_SYNC_XHR,
    TAI_SPAN_RASTER_AND_DRAW,
    TAI_SPAN_SDL_PRESENT,
    TAI_SPAN_COUNT
} TaiTraceSpan;

/* ph "B"/"E" on the calling thread. Pair them in a thin wrapper around an
 * untraced body, never across early returns, so spans always nest. */
void tai_trace_begin(TaiTraceSpan span);
void tai_trace_end(TaiTraceSpan span);

/* Requests multiplexed on the loader's one libcurl multi overlap on one
 * thread, so they are async ph "b"/"e" slices keyed by id instead of B/E. */
typedef enum {
    TAI_FETCH_DOCUMENT,
    TAI_FETCH_ASYNC_XHR,
    TAI_FETCH_SCRIPT,
    TAI_FETCH_STYLE,
    TAI_FETCH_COUNT
} TaiTraceFetchKind;

/* Plain data owned by the request's owner. A zeroed token means tracing was
 * off at begin, or the slice has already ended. */
typedef struct {
    uint64_t id;
    TaiTraceFetchKind kind;
    size_t index;
} TaiTraceFetch;

/* index is the Python resource index for script/style and ignored otherwise. */
TaiTraceFetch tai_trace_fetch_begin(TaiTraceFetchKind kind, size_t index);
/* Emits the matching "e" and zeroes *fetch, so a second call emits nothing. */
void tai_trace_fetch_end(TaiTraceFetch *fetch);

/* The vocabulary, for tests: span names, and fetch names where script/style
 * carry a "%zu" for the resource index. */
const char *tai_trace_span_name(TaiTraceSpan span);
const char *tai_trace_fetch_format(TaiTraceFetchKind kind);

#endif
