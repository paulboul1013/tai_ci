#define _POSIX_C_SOURCE 200809L
#include "tai/trace.h"
#include "tai/core.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const span_names[TAI_SPAN_COUNT] = {
    [TAI_SPAN_JAVASCRIPT] = "javascript",
    [TAI_SPAN_RENDER] = "render",
    [TAI_SPAN_TASK_LOAD] = "Task: Tab.load",
    [TAI_SPAN_TASK_FINISH_DOCUMENT] = "Task: Tab._finish_document_load",
    [TAI_SPAN_TASK_FINISH_RESOURCES] = "Task: Tab._finish_page_resources",
    [TAI_SPAN_TASK_SCRIPT] = "Task: JSContext.run",
    [TAI_SPAN_TASK_ANIMATION_FRAME] = "Task: Tab.run_animation_frame",
    [TAI_SPAN_TASK_CLICK] = "Task: Tab.click",
    [TAI_SPAN_TASK_SET_TIMEOUT] = "Task: JSContext.dispatch_settimeout",
    [TAI_SPAN_TASK_SET_INTERVAL] = "Task: JSContext.dispatch_setinterval",
    [TAI_SPAN_TASK_XHR_ONLOAD] = "Task: JSContext.dispatch_xhr_onload",
    [TAI_SPAN_SYNC_XHR] = "Network I/O: synchronous XHR",
    [TAI_SPAN_RASTER_AND_DRAW] = "raster_and_draw",
    [TAI_SPAN_SDL_PRESENT] = "sdl_present",
    [TAI_SPAN_COMMIT] = "commit",
};
static const char *const fetch_formats[TAI_FETCH_COUNT] = {
    [TAI_FETCH_DOCUMENT] = "Network I/O: document",
    [TAI_FETCH_ASYNC_XHR] = "Network I/O: asynchronous XHR",
    [TAI_FETCH_SCRIPT] = "resource_fetch[%zu]: script",
    [TAI_FETCH_STYLE] = "resource_fetch[%zu]: style",
};
static const char *const instant_names[TAI_INSTANT_COUNT] = {
    [TAI_INSTANT_STALE_COMPLETION_DROPPED] = "stale_completion_dropped",
};

/* One output array is the shared invariant Perfetto needs, so writers are
 * serialized rather than buffered per thread. */
static struct {
    pthread_mutex_t mutex;
    FILE *file;
    uint64_t generation; /* bumped per open; scopes thread naming */
    uint64_t next_fetch_id;
} trace = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static atomic_bool enabled;
static atomic_uint next_tid = 1;
static _Thread_local unsigned tls_tid;
static _Thread_local uint64_t tls_named_generation;

const char *tai_trace_span_name(TaiTraceSpan span) {
    return (unsigned)span < TAI_SPAN_COUNT ? span_names[span] : NULL;
}
const char *tai_trace_fetch_format(TaiTraceFetchKind kind) {
    return (unsigned)kind < TAI_FETCH_COUNT ? fetch_formats[kind] : NULL;
}
const char *tai_trace_instant_name(TaiTraceInstant instant) {
    return (unsigned)instant < TAI_INSTANT_COUNT ? instant_names[instant] : NULL;
}

static bool tracing(void) {
    return atomic_load_explicit(&enabled, memory_order_relaxed);
}
static uint64_t now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}
static unsigned self_tid(void) {
    if (!tls_tid) tls_tid = atomic_fetch_add(&next_tid, 1);
    return tls_tid;
}

/* Caller holds the mutex and the file is open. */
static void name_thread_locked(const char *name) {
    if (tls_named_generation == trace.generation) return;
    tls_named_generation = trace.generation;
    unsigned tid = self_tid();
    fprintf(trace.file, ",{\"name\":\"thread_name\",\"ph\":\"M\",\"ts\":%" PRIu64
        ",\"pid\":1,\"tid\":%u,\"cat\":\"__metadata\",\"args\":{\"name\":",
        now_us(), tid);
    if (name) tai_json_string(trace.file, name);
    else fprintf(trace.file, "\"Thread %u\"", tid);
    fputs("}}", trace.file);
    fflush(trace.file);
}

/* Caller holds the mutex and the file is open. Names and arg keys come from
 * static strings, which hold no JSON metacharacters. */
static void write_args_locked(const TaiTraceArg *args, size_t count) {
    fputs(",\"args\":{", trace.file);
    for (size_t i = 0; i < count; i++) {
        fprintf(trace.file, "%s\"%s\":", i ? "," : "", args[i].key);
        if (args[i].kind == TAI_TRACE_UINT)
            fprintf(trace.file, "%" PRIu64, args[i].number);
        else if (args[i].kind == TAI_TRACE_STRING && args[i].string)
            tai_json_string(trace.file, args[i].string);
        else
            fputs("null", trace.file);
    }
    fputc('}', trace.file);
}
static void write_event_locked(const char *ph, const char *name, uint64_t id,
                               const TaiTraceArg *args, size_t count) {
    name_thread_locked(NULL);
    fprintf(trace.file, ",{\"ph\":\"%s\",\"cat\":\"_\",\"name\":\"%s\"", ph, name);
    if (id) fprintf(trace.file, ",\"id\":%" PRIu64, id);
    if (!strcmp(ph, "i")) fputs(",\"s\":\"t\"", trace.file);
    if (count) write_args_locked(args, count);
    fprintf(trace.file, ",\"ts\":%" PRIu64 ",\"pid\":1,\"tid\":%u}", now_us(),
        self_tid());
    fflush(trace.file);
}
static void write_event(const char *ph, const char *name, uint64_t id,
                        const TaiTraceArg *args, size_t count) {
    pthread_mutex_lock(&trace.mutex);
    if (trace.file) write_event_locked(ph, name, id, args, count);
    pthread_mutex_unlock(&trace.mutex);
}

bool tai_trace_open(const char *path, char **error) {
    if (error) *error = NULL;
    pthread_mutex_lock(&trace.mutex);
    const char *problem = NULL;
    if (trace.file) problem = "a trace is already open";
    else if (!(trace.file = fopen(path, "w"))) problem = strerror(errno);
    if (problem) {
        pthread_mutex_unlock(&trace.mutex);
        if (error) {
            size_t length = strlen(path) + strlen(problem) + 3;
            *error = malloc(length);
            if (*error) snprintf(*error, length, "%s: %s", path, problem);
        }
        return false;
    }
    trace.generation++;
    fprintf(trace.file, "{\"traceEvents\":[{\"name\":\"process_name\",\"ph\":\"M\","
        "\"ts\":%" PRIu64 ",\"pid\":1,\"cat\":\"__metadata\","
        "\"args\":{\"name\":\"Browser\"}}", now_us());
    fflush(trace.file);
    atomic_store(&enabled, true);
    pthread_mutex_unlock(&trace.mutex);
    return true;
}

void tai_trace_close(void) {
    pthread_mutex_lock(&trace.mutex);
    if (trace.file) {
        atomic_store(&enabled, false);
        fputs("]}", trace.file);
        fclose(trace.file);
        trace.file = NULL;
    }
    pthread_mutex_unlock(&trace.mutex);
}

void tai_trace_thread_name(const char *name) {
    if (!tracing()) return;
    pthread_mutex_lock(&trace.mutex);
    if (trace.file) name_thread_locked(name);
    pthread_mutex_unlock(&trace.mutex);
}

void tai_trace_begin(TaiTraceSpan span) {
    if (tracing()) write_event("B", span_names[span], 0, NULL, 0);
}
void tai_trace_begin_args(TaiTraceSpan span, const TaiTraceArg *args,
                          size_t count) {
    if (tracing()) write_event("B", span_names[span], 0, args, count);
}
void tai_trace_end(TaiTraceSpan span) {
    if (tracing()) write_event("E", span_names[span], 0, NULL, 0);
}
void tai_trace_instant(TaiTraceInstant instant, const TaiTraceArg *args,
                       size_t count) {
    if (tracing()) write_event("i", instant_names[instant], 0, args, count);
}

static void fetch_name(const TaiTraceFetch *fetch, char *buffer, size_t size) {
    snprintf(buffer, size, fetch_formats[fetch->kind], fetch->index);
}

TaiTraceFetch tai_trace_fetch_begin(TaiTraceFetchKind kind, size_t index,
                                    const char *url) {
    TaiTraceFetch fetch = {0};
    if (!tracing()) return fetch;
    fetch.kind = kind;
    fetch.index = kind == TAI_FETCH_SCRIPT || kind == TAI_FETCH_STYLE ? index : 0;
    char name[64];
    fetch_name(&fetch, name, sizeof name);
    pthread_mutex_lock(&trace.mutex);
    if (trace.file) {
        fetch.id = ++trace.next_fetch_id;
        TaiTraceArg arg = {"url", TAI_TRACE_STRING, url, 0};
        write_event_locked("b", name, fetch.id, &arg, url ? 1 : 0);
    }
    pthread_mutex_unlock(&trace.mutex);
    return fetch;
}

void tai_trace_fetch_end(TaiTraceFetch *fetch) {
    if (!fetch || !fetch->id) return;
    char name[64];
    fetch_name(fetch, name, sizeof name);
    write_event("e", name, fetch->id, NULL, 0);
    *fetch = (TaiTraceFetch){0};
}
