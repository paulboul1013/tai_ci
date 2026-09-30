#define _POSIX_C_SOURCE 200809L
#include "tai/tabset.h"
#include "tai/bookmarks.h"

#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { LOAD_NAVIGATION, LOAD_HISTORY } LoadKind;

typedef struct Loader Loader;
typedef struct XhrJob XhrJob;
typedef struct AsyncJob AsyncJob;
typedef struct LoadTask LoadTask;
typedef struct TabSlot TabSlot;
typedef struct Completion Completion;

struct Completion {
    LoadTask *task;
    TaiPage *page;
    bool network_failure;
    char *error;
    Completion *next;
};

/* A task names its tab only by ID, never by TaiTabSet pointer: the tab set
 * may be destroyed while the shared loader still owns the task. */
struct LoadTask {
    uint64_t tab_id;
    uint64_t generation;
    char *url;
    char *body;
    char *referrer;
    char *internal_markup;
    LoadKind kind;
    size_t history_target;
    double width;
    double height;
    atomic_bool cancelled;
    bool completed;
    Loader *loader; /* set when the loader starts the task; loader-only */
    TaiPageLoad *page_load;
    Completion completion;
    LoadTask *queue_next;
    LoadTask *active_next;
};

struct TabSlot {
    uint64_t id;
    uint64_t generation;
    TaiSession *session;
    LoadTask *active_task;
};

/* Process-wide state shared by every window's tab set. The registry, tab IDs,
 * bookmarks and configuration belong to the SDL owner thread; the loader
 * thread owns the network and touches only the mutex-protected queues and
 * the immutable configuration strings. */
struct TaiBrowserApp {
    const char *default_css;
    char *home_url;
    char *ca_file;
    TaiBookmarks *bookmarks;
    bool rtl;
    uint64_t next_tab_id;
    TaiTabSet **tabsets;
    size_t tabset_count;
    size_t tabset_capacity;

    pthread_t loader_thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool mutex_ready;
    bool condition_ready;
    bool stopping;
    bool loader_ready;
    bool loader_started;
    bool loader_ok;
    LoadTask *queued_head;
    LoadTask *queued_tail;
    LoadTask *active_loads;
    Completion *completed_head;
    Completion *completed_tail;

    /* Synchronous XHR from JS on the SDL thread (event time): jobs queue
     * here for the loader, which owns the network; the SDL thread waits on
     * xhr_condition. The loader never waits for the SDL thread. */
    pthread_cond_t xhr_condition;
    bool xhr_condition_ready;
    bool xhr_closed; /* the loader no longer takes jobs */
    XhrJob *xhr_head;
    XhrJob *xhr_tail;
    /* Asynchronous XHR started by JS on the SDL thread, queued for the
     * loader (D5). Nobody waits for them; pages see the result later. */
    AsyncJob *async_head;
    AsyncJob *async_tail;
    /* Mirrors stopping for lock-free checks in the JS interrupt handler. */
    atomic_bool stopping_flag;
    /* Shared with every page and the loader's network; created before the
     * loader starts, destroyed after it is joined. */
    TaiCookieJar *cookies;
    TaiPageNet event_net;
};

/* One event-time XHR. It lives on the waiting SDL thread's stack, which
 * returns only after done, so the loader may use it until then. The request
 * inputs are borrowed from that thread and immutable while it waits. */
struct XhrJob {
    const TaiUrl *url;
    const TaiUrl *referrer;
    const char *payload;
    const char *origin;
    const char *policy;
    double deadline;        /* total limit, from queueing */
    TaiResponse *response;  /* written by the loader before done */
    const char *failure;    /* static message when response is NULL */
    bool done;              /* app->mutex */
    XhrJob *next;           /* app->mutex: the queue */
    Loader *loader;         /* loader-only from here on */
    TaiRequest *request;
    XhrJob *loader_next;    /* in-flight list */
};

/* One asynchronous XHR (D5), heap-allocated. It holds the owner's reference
 * to the page's fetch until tai_page_fetch_finish, which the loader calls
 * exactly once: on completion, deadline, abandonment, failure or exit. */
struct AsyncJob {
    TaiPageFetch *fetch;
    AsyncJob *next;         /* app->mutex queue, then the loader's list */
    TaiRequest *request;    /* loader-only */
    double deadline;
    Loader *loader;
};

/* Loader-thread state, on the loader's stack. */
struct Loader {
    TaiBrowserApp *app;
    TaiNetwork *network;
    bool network_failed;
    XhrJob *inflight;
    AsyncJob *async_inflight;
};

/* One window's tabs. It borrows its app unless it was created by one of the
 * single-window constructors, which own a private app. */
struct TaiTabSet {
    TaiBrowserApp *app;
    bool owns_app;
    double width;
    double height;
    bool started;
    TabSlot *slots;
    size_t count;
    size_t capacity;
    size_t active;
    /* Completions routed to this tab set by another tab set's pump. */
    Completion *inbox_head;
    Completion *inbox_tail;
};

static bool set_error(char **error, const char *message) {
    if (error && !*error) *error = tai_strdup(message);
    return false;
}

static void task_destroy(LoadTask *task) {
    if (!task) return;
    free(task->url);
    free(task->body);
    free(task->referrer);
    free(task->internal_markup);
    free(task);
}

static void completion_prepare(LoadTask *task, TaiPage *page,
                               bool network_failure, char *error) {
    Completion *completion = &task->completion;
    memset(completion, 0, sizeof(*completion));
    completion->task = task;
    completion->page = page;
    completion->network_failure = network_failure;
    completion->error = error;
    task->completed = true;
}

static void completion_publish(TaiBrowserApp *app, LoadTask *task) {
    Completion *completion = &task->completion;
    pthread_mutex_lock(&app->mutex);
    if (app->completed_tail) app->completed_tail->next = completion;
    else app->completed_head = completion;
    app->completed_tail = completion;
    pthread_mutex_unlock(&app->mutex);
}

static void completion_append(TaiBrowserApp *app, LoadTask *task, TaiPage *page,
                              bool network_failure, char *error) {
    completion_prepare(task, page, network_failure, error);
    completion_publish(app, task);
}

static void page_load_done(void *opaque, TaiPage *page,
                           bool network_failure, char *error) {
    LoadTask *task = opaque;
    task->page_load = NULL;
    /* The loader removes this task from its active list before publishing it
     * to the UI thread. That prevents the UI from freeing a task the worker
     * still holds in its active list. */
    completion_prepare(task, page, network_failure, error);
}

static void active_remove(TaiBrowserApp *app, LoadTask **slot) {
    LoadTask *task = *slot;
    *slot = task->active_next;
    task->active_next = NULL;
    (void)app;
}

static LoadTask *queued_take(TaiBrowserApp *app) {
    LoadTask *task = app->queued_head;
    if (!task) return NULL;
    app->queued_head = task->queue_next;
    if (!app->queued_head) app->queued_tail = NULL;
    task->queue_next = NULL;
    return task;
}

static void queue_push(TaiBrowserApp *app, LoadTask *task) {
    pthread_mutex_lock(&app->mutex);
    if (app->queued_tail) app->queued_tail->queue_next = task;
    else app->queued_head = task;
    app->queued_tail = task;
    pthread_cond_signal(&app->condition);
    pthread_mutex_unlock(&app->mutex);
}

static void queue_push_locked(TaiBrowserApp *app, LoadTask *task) {
    if (app->queued_tail) app->queued_tail->queue_next = task;
    else app->queued_head = task;
    app->queued_tail = task;
    pthread_cond_signal(&app->condition);
}

static double seconds_now(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* ---- event-time XHR jobs (loader side) --------------------------------- */

/* Hands the result to the waiting SDL thread; the job is gone afterwards. */
static void xhr_finish(TaiBrowserApp *app, XhrJob *job, TaiResponse *response,
                       const char *failure) {
    pthread_mutex_lock(&app->mutex);
    job->response = response;
    job->failure = failure;
    job->done = true;
    pthread_cond_broadcast(&app->xhr_condition);
    pthread_mutex_unlock(&app->mutex);
}

static void xhr_unlink(Loader *loader, XhrJob *job) {
    XhrJob **slot = &loader->inflight;
    while (*slot && *slot != job) slot = &(*slot)->loader_next;
    if (*slot) *slot = job->loader_next;
}

static void xhr_done(void *opaque, TaiResponse *response) {
    XhrJob *job = opaque;
    Loader *loader = job->loader;
    xhr_unlink(loader, job);
    xhr_finish(loader->app, job, response,
               response ? NULL : "XMLHttpRequest failed");
}

/* ---- asynchronous XHR jobs (loader side, D5) ------------------------------ */

static void async_finish(AsyncJob *job, TaiResponse *response,
                         const char *failure) {
    tai_page_fetch_finish(job->fetch, response, failure);
    free(job);
}

static void async_done(void *opaque, TaiResponse *response) {
    AsyncJob *job = opaque;
    AsyncJob **slot = &job->loader->async_inflight;
    while (*slot && *slot != job) slot = &(*slot)->next;
    if (*slot) *slot = job->next;
    async_finish(job, response, "XMLHttpRequest failed");
}

/* Sends one job on the loader's network. Its callback is cheap and touches
 * no page, so nested polls may dispatch it. The total limit matches the
 * synchronous one. */
static void async_submit(Loader *loader, AsyncJob *job) {
    if (loader->network_failed) {
        async_finish(job, NULL, "network polling failed");
        return;
    }
    if (tai_page_fetch_abandoned(job->fetch)) {
        async_finish(job, NULL, "XMLHttpRequest cancelled");
        return;
    }
    const TaiPageFetchRequest *request = tai_page_fetch_request(job->fetch);
    job->loader = loader;
    job->request = tai_network_submit(loader->network, request->url,
        request->referrer, request->payload, request->origin,
        request->referrer_policy, async_done, job);
    if (!job->request) {
        async_finish(job, NULL, "XMLHttpRequest failed");
        return;
    }
    tai_network_allow_nested(job->request);
    job->deadline = seconds_now() + TAI_XHR_TIMEOUT_SECONDS;
    job->next = loader->async_inflight;
    loader->async_inflight = job;
}

/* Stops in-flight jobs past their deadline or whose page is gone, or all of
 * them when failure is set. */
static void async_expire(Loader *loader, const char *failure) {
    double now = failure ? 0.0 : seconds_now();
    AsyncJob **slot = &loader->async_inflight;
    while (*slot) {
        AsyncJob *job = *slot;
        bool abandoned = tai_page_fetch_abandoned(job->fetch);
        if (!failure && !abandoned && now < job->deadline) {
            slot = &job->next;
            continue;
        }
        *slot = job->next;
        tai_network_cancel(loader->network, job->request);
        async_finish(job, NULL, failure ? failure
            : abandoned ? "XMLHttpRequest cancelled"
                        : "XMLHttpRequest timed out");
    }
}

/* Submits queued jobs. Their requests may complete inside a nested poll
 * (a load-time XHR or a checkpoint between load-time scripts), so an
 * event-time XHR does not wait for a whole page load. */
static void loader_take_jobs(Loader *loader) {
    TaiBrowserApp *app = loader->app;
    pthread_mutex_lock(&app->mutex);
    XhrJob *jobs = app->xhr_head;
    app->xhr_head = app->xhr_tail = NULL;
    AsyncJob *async = app->async_head;
    app->async_head = app->async_tail = NULL;
    pthread_mutex_unlock(&app->mutex);
    while (async) {
        AsyncJob *job = async;
        async = job->next;
        async_submit(loader, job);
    }
    while (jobs) {
        XhrJob *job = jobs;
        jobs = job->next;
        job->loader = loader;
        if (loader->network_failed) {
            xhr_finish(app, job, NULL, "network polling failed");
            continue;
        }
        job->request = tai_network_submit(loader->network, job->url,
            job->referrer, job->payload, job->origin, job->policy, xhr_done,
            job);
        if (!job->request) {
            xhr_finish(app, job, NULL, "XMLHttpRequest failed");
            continue;
        }
        tai_network_allow_nested(job->request);
        job->loader_next = loader->inflight;
        loader->inflight = job;
    }
}

/* Cancels in-flight jobs past their deadline, or all of them. */
static void loader_expire_jobs(Loader *loader, const char *failure) {
    double now = failure ? 0.0 : seconds_now();
    XhrJob **slot = &loader->inflight;
    while (*slot) {
        XhrJob *job = *slot;
        if (!failure && now < job->deadline) {
            slot = &job->loader_next;
            continue;
        }
        *slot = job->loader_next;
        tai_network_cancel(loader->network, job->request);
        xhr_finish(loader->app, job, NULL,
                   failure ? failure : "XMLHttpRequest timed out");
    }
}

static void loader_service(Loader *loader) {
    loader_take_jobs(loader);
    loader_expire_jobs(loader, NULL);
    async_expire(loader, NULL);
}

/* ---- load-time page net (loader side) ---------------------------------- */

static bool task_cancelled(void *opaque) {
    LoadTask *task = opaque;
    return atomic_load_explicit(&task->cancelled, memory_order_acquire) ||
           atomic_load_explicit(&task->loader->app->stopping_flag,
                                memory_order_acquire);
}

/* Between polls of a load-time XHR: keep event-time jobs moving and stop
 * within one poll interval once the task is cancelled. */
static bool loader_request_service(void *opaque) {
    LoadTask *task = opaque;
    loader_service(task->loader);
    return !task_cancelled(task);
}

static TaiResponse *loader_request(void *opaque, const TaiUrl *url,
    const TaiUrl *referrer, const char *payload, const char *origin,
    const char *policy, char **message) {
    LoadTask *task = opaque;
    TaiWaitStatus status = TAI_WAIT_CANCELLED;
    TaiResponse *response = task_cancelled(task) ? NULL
        : tai_network_request_until(task->loader->network, url, referrer,
              payload, origin, policy, TAI_XHR_TIMEOUT_SECONDS,
              loader_request_service, task, &status);
    if (!response) *message = tai_page_net_wait_message(status);
    return response;
}

static bool loader_checkpoint(void *opaque) {
    LoadTask *task = opaque;
    Loader *loader = task->loader;
    loader_service(loader);
    if ((loader->inflight || loader->async_inflight) &&
        !tai_network_poll_nested(loader->network, 0))
        loader->network_failed = true;
    return !task_cancelled(task);
}

/* A load-time script's asynchronous XHR: this is already the loader. */
static bool loader_start(void *opaque, TaiPageFetch *fetch) {
    LoadTask *task = opaque;
    AsyncJob *job = calloc(1, sizeof(*job));
    if (!job) return false;
    job->fetch = fetch;
    async_submit(task->loader, job);
    return true;
}

/* ---- event-time page net (SDL side) ------------------------------------ */

static TaiResponse *app_xhr_request(void *opaque, const TaiUrl *url,
    const TaiUrl *referrer, const char *payload, const char *origin,
    const char *policy, char **message) {
    TaiBrowserApp *app = opaque;
    XhrJob job = {.url = url, .referrer = referrer, .payload = payload,
                  .origin = origin, .policy = policy};
    pthread_mutex_lock(&app->mutex);
    if (app->stopping || app->xhr_closed) {
        pthread_mutex_unlock(&app->mutex);
        *message = tai_strdup("XMLHttpRequest failed: browser is closing");
        return NULL;
    }
    job.deadline = seconds_now() + TAI_XHR_TIMEOUT_SECONDS;
    if (app->xhr_tail) app->xhr_tail->next = &job;
    else app->xhr_head = &job;
    app->xhr_tail = &job;
    pthread_cond_signal(&app->condition);
    /* D9: every window stops responding until the loader answers. The
     * loader finishes each job: on completion, deadline, failure or exit. */
    while (!job.done) pthread_cond_wait(&app->xhr_condition, &app->mutex);
    pthread_mutex_unlock(&app->mutex);
    if (!job.response) *message = tai_strdup(job.failure);
    return job.response;
}

/* An event-time asynchronous XHR: queue it for the loader and return. */
static bool app_start(void *opaque, TaiPageFetch *fetch) {
    TaiBrowserApp *app = opaque;
    AsyncJob *job = calloc(1, sizeof(*job));
    if (!job) return false;
    job->fetch = fetch;
    pthread_mutex_lock(&app->mutex);
    bool open = !app->stopping && !app->xhr_closed;
    if (open) {
        if (app->async_tail) app->async_tail->next = job;
        else app->async_head = job;
        app->async_tail = job;
        pthread_cond_signal(&app->condition);
    }
    pthread_mutex_unlock(&app->mutex);
    if (!open) free(job);
    return open;
}

static void load_task_start(Loader *loader, LoadTask *task) {
    TaiBrowserApp *app = loader->app;
    TaiNetwork *network = loader->network;
    bool network_failed = loader->network_failed;
    task->loader = loader;
    if (network_failed || atomic_load_explicit(&task->cancelled,
                                               memory_order_acquire)) {
        completion_append(app, task, NULL, false,
                          tai_strdup(network_failed ? "network owner failed"
                                                    : "navigation cancelled"));
        return;
    }
    TaiUrl *url = tai_url_parse(task->url);
    TaiUrl *referrer = task->referrer
        ? tai_url_parse(task->referrer) : NULL;
    if (!url || (task->referrer && !referrer)) {
        tai_url_destroy(url);
        tai_url_destroy(referrer);
        completion_append(app, task, NULL, false,
                          tai_strdup("navigation URL allocation failed"));
        return;
    }
    char *error = NULL;
    TaiPageNet net = {.request = loader_request,
                      .checkpoint = loader_checkpoint,
                      .cancelled = task_cancelled,
                      .start = loader_start,
                      .cookies = app->cookies, .userdata = task};
    TaiPageLoad *load = task->internal_markup
        ? tai_page_load_async_markup(network, url, task->internal_markup,
            app->default_css, task->width, task->height, app->rtl, &net,
            page_load_done, task, &error)
        : tai_page_load_async(network, url, referrer, task->body,
            app->default_css, task->width, task->height, app->rtl, &net,
            page_load_done, task, &error);
    tai_url_destroy(url);
    tai_url_destroy(referrer);
    if (task->completed) {
        completion_publish(app, task);
        free(error);
        return;
    }
    if (!load) {
        completion_append(app, task, NULL, false, error);
        return;
    }
    task->page_load = task->completed ? NULL : load;
    if (atomic_load_explicit(&task->cancelled, memory_order_acquire)) {
        task->page_load = NULL;
        tai_page_load_async_cancel(load);
        completion_append(app, task, NULL, false,
                          tai_strdup("navigation cancelled"));
        return;
    }
    task->active_next = app->active_loads;
    app->active_loads = task;
}

static void cancel_or_reap_loads(TaiBrowserApp *app, TaiNetwork *network,
                                 bool network_failed) {
    LoadTask **slot = &app->active_loads;
    while (*slot) {
        LoadTask *task = *slot;
        if (task->completed) {
            active_remove(app, slot);
            completion_publish(app, task);
            continue;
        }
        if (network_failed || atomic_load_explicit(&task->cancelled,
                                                    memory_order_acquire)) {
            TaiPageLoad *load = task->page_load;
            task->page_load = NULL;
            if (load) tai_page_load_async_cancel(load);
            active_remove(app, slot);
            completion_append(app, task, NULL, false,
                tai_strdup(network_failed ? "network polling failed"
                                          : "navigation cancelled"));
            continue;
        }
        slot = &task->active_next;
    }
    (void)network;
}

static void *loader_main(void *opaque) {
    TaiBrowserApp *app = opaque;
    TaiNetwork *network = tai_network_create_with_jar(app->cookies);
    if (network && app->ca_file &&
        !tai_network_set_ca_file(network, app->ca_file)) {
        tai_network_destroy(network);
        network = NULL;
    }
    pthread_mutex_lock(&app->mutex);
    app->loader_ok = network != NULL;
    app->loader_ready = true;
    pthread_cond_broadcast(&app->condition);
    pthread_mutex_unlock(&app->mutex);
    if (!network) return NULL;

    Loader loader = {.app = app, .network = network};
    for (;;) {
        pthread_mutex_lock(&app->mutex);
        while (!app->stopping && !app->queued_head && !app->active_loads &&
               !app->xhr_head && !loader.inflight && !app->async_head &&
               !loader.async_inflight)
            pthread_cond_wait(&app->condition, &app->mutex);
        bool stopping = app->stopping;
        LoadTask *task = queued_take(app);
        pthread_mutex_unlock(&app->mutex);

        if (task) load_task_start(&loader, task);
        loader_service(&loader);
        if (app->active_loads || loader.inflight || loader.async_inflight) {
            if (!tai_network_poll(network, 16)) loader.network_failed = true;
            if (loader.network_failed) {
                loader_expire_jobs(&loader, "network polling failed");
                async_expire(&loader, "network polling failed");
            }
            cancel_or_reap_loads(app, network,
                                 loader.network_failed || stopping);
        }

        pthread_mutex_lock(&app->mutex);
        bool done = app->stopping && !app->queued_head && !app->active_loads;
        pthread_mutex_unlock(&app->mutex);
        if (done) break;
    }
    /* No SDL thread can be waiting here (it is the one stopping the app),
     * but every accepted job must still finish. */
    pthread_mutex_lock(&app->mutex);
    app->xhr_closed = true;
    pthread_mutex_unlock(&app->mutex);
    loader.network_failed = true;
    loader_take_jobs(&loader);
    loader_expire_jobs(&loader, "browser is closing");
    async_expire(&loader, "browser is closing");
    tai_network_destroy(network);
    return NULL;
}

typedef struct {
    char *text;
    size_t length;
    size_t capacity;
} Markup;

static bool markup_append(Markup *markup, const char *text) {
    size_t length = strlen(text);
    if (length > SIZE_MAX - markup->length - 1) return false;
    size_t needed = markup->length + length + 1;
    if (needed > markup->capacity) {
        size_t capacity = markup->capacity ? markup->capacity : 128;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *next = realloc(markup->text, capacity);
        if (!next) return false;
        markup->text = next;
        markup->capacity = capacity;
    }
    memcpy(markup->text + markup->length, text, length + 1);
    markup->length += length;
    return true;
}

static bool markup_append_escaped(Markup *markup, const char *url) {
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        const char *escape = *p == '&' ? "&amp;" : *p == '<' ? "&lt;"
            : *p == '>' ? "&gt;" : *p == '"' ? "&quot;"
            : *p == '\'' ? "&#x27;" : NULL;
        if (escape) {
            if (!markup_append(markup, escape)) return false;
        } else {
            char byte[2] = {(char)*p, '\0'};
            if (!markup_append(markup, byte)) return false;
        }
    }
    return true;
}

static char *bookmarks_markup(const TaiBookmarks *bookmarks) {
    TaiBookmarkSnapshot snapshot = {0};
    if (!tai_bookmarks_snapshot(bookmarks, &snapshot)) return NULL;
    Markup markup = {0};
    bool ok = markup_append(&markup,
        "<html><head><title>Bookmarks</title></head><body><h1>Bookmarks</h1>");
    if (ok && !snapshot.count)
        ok = markup_append(&markup, "<p>No bookmarks yet.</p>");
    if (ok && snapshot.count) ok = markup_append(&markup, "<ul>");
    for (size_t i = 0; ok && i < snapshot.count; i++) {
        ok = markup_append(&markup, "<li><a href=\"") &&
             markup_append_escaped(&markup, snapshot.urls[i]) &&
             markup_append(&markup, "\">") &&
             markup_append_escaped(&markup, snapshot.urls[i]) &&
             markup_append(&markup, "</a></li>");
    }
    if (ok && snapshot.count) ok = markup_append(&markup, "</ul>");
    if (ok) ok = markup_append(&markup, "</body></html>");
    tai_bookmark_snapshot_destroy(&snapshot);
    if (!ok) { free(markup.text); return NULL; }
    return markup.text;
}

static LoadTask *task_create(TaiTabSet *tabs, TabSlot *slot, const char *url,
                             const char *body, LoadKind kind,
                             size_t history_target, char **error) {
    if (!slot || !url || slot->generation == UINT64_MAX) {
        set_error(error, "invalid or exhausted tab navigation");
        return NULL;
    }
    TaiUrl *validated_url = tai_url_parse(url);
    if (!validated_url) {
        set_error(error, "navigation URL is malformed");
        return NULL;
    }
    tai_url_destroy(validated_url);
    LoadTask *task = calloc(1, sizeof(*task));
    if (!task) {
        set_error(error, "navigation task allocation failed");
        return NULL;
    }
    task->tab_id = slot->id;
    task->generation = slot->generation + 1;
    task->url = tai_strdup(url);
    if (!strcmp(url, "about:bookmarks"))
        task->internal_markup = bookmarks_markup(tabs->app->bookmarks);
    if (body) task->body = tai_strdup(body);
    const TaiPage *page = tai_session_page(slot->session);
    /* Python captures the tab's current URL before replacing it. While a
     * previous navigation is pending, that provisional URL is the referrer
     * for a superseding request. */
    const char *referrer = slot->active_task ? slot->active_task->url
        : page ? tai_url_string(tai_page_url(page)) : NULL;
    if (referrer) task->referrer = tai_strdup(referrer);
    task->kind = kind;
    task->history_target = history_target;
    task->width = tabs->width;
    task->height = tabs->height;
    atomic_init(&task->cancelled, false);
    if (!task->url || (!strcmp(url, "about:bookmarks") &&
                       !task->internal_markup) || (body && !task->body) ||
        (referrer && !task->referrer)) {
        task_destroy(task);
        set_error(error, "navigation task input allocation failed");
        return NULL;
    }
    return task;
}

static bool task_publish(TaiTabSet *tabs, TabSlot *slot, LoadTask *task,
                         char **error) {
    TaiBrowserApp *app = tabs->app;
    pthread_mutex_lock(&app->mutex);
    if (app->stopping) {
        pthread_mutex_unlock(&app->mutex);
        task_destroy(task);
        return set_error(error, "tab set is closing");
    }
    if (slot->active_task)
        atomic_store_explicit(&slot->active_task->cancelled, true,
                              memory_order_release);
    slot->generation = task->generation;
    slot->active_task = task;
    queue_push_locked(app, task);
    pthread_mutex_unlock(&app->mutex);
    return true;
}

static TabSlot *active_slot(TaiTabSet *tabs) {
    return tabs && tabs->count && tabs->active < tabs->count
        ? &tabs->slots[tabs->active] : NULL;
}

static TabSlot *slot_by_id(const TaiTabSet *tabs, uint64_t id) {
    for (size_t index = 0; index < tabs->count; index++)
        if (tabs->slots[index].id == id) return (TabSlot *)&tabs->slots[index];
    return NULL;
}

static bool reserve_slot(TaiTabSet *tabs, char **error) {
    if (tabs->count < tabs->capacity) return true;
    if (tabs->capacity > SIZE_MAX / 2 / sizeof(*tabs->slots))
        return set_error(error, "tab capacity exceeded");
    size_t capacity = tabs->capacity ? tabs->capacity * 2 : 4;
    TabSlot *next = realloc(tabs->slots, capacity * sizeof(*next));
    if (!next) return set_error(error, "tab allocation failed");
    tabs->slots = next;
    tabs->capacity = capacity;
    return true;
}

static void completions_free(Completion *completion) {
    while (completion) {
        Completion *next = completion->next;
        tai_page_destroy(completion->page);
        free(completion->error);
        task_destroy(completion->task);
        completion = next;
    }
}

static void app_free_state(TaiBrowserApp *app) {
    tai_bookmarks_destroy(app->bookmarks);
    free(app->home_url);
    free(app->ca_file);
    free(app->tabsets);
    free(app);
}

static TaiBrowserApp *app_create(const char *default_css, bool rtl,
                                 const char *home_url, const char *ca_file,
                                 char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!default_css || !home_url || !*home_url) {
        set_error(error, "default CSS and New Tab URL are required");
        return NULL;
    }
    TaiBrowserApp *app = calloc(1, sizeof(*app));
    if (!app) {
        set_error(error, "browser allocation failed");
        return NULL;
    }
    app->default_css = default_css;
    app->home_url = tai_strdup(home_url);
    app->ca_file = ca_file ? tai_strdup(ca_file) : NULL;
    app->bookmarks = tai_bookmarks_create();
    app->rtl = rtl;
    app->next_tab_id = 1;
    if (!app->home_url || !app->bookmarks || (ca_file && !app->ca_file)) {
        set_error(error, "browser state allocation failed");
        app_free_state(app);
        return NULL;
    }
    if (pthread_mutex_init(&app->mutex, NULL) != 0) {
        set_error(error, "browser mutex initialization failed");
        app_free_state(app);
        return NULL;
    }
    app->mutex_ready = true;
    if (pthread_cond_init(&app->condition, NULL) != 0) {
        set_error(error, "browser condition initialization failed");
        pthread_mutex_destroy(&app->mutex);
        app_free_state(app);
        return NULL;
    }
    app->condition_ready = true;
    atomic_init(&app->stopping_flag, false);
    if (pthread_cond_init(&app->xhr_condition, NULL) != 0) {
        set_error(error, "browser condition initialization failed");
        pthread_cond_destroy(&app->condition);
        pthread_mutex_destroy(&app->mutex);
        app_free_state(app);
        return NULL;
    }
    app->xhr_condition_ready = true;
    app->cookies = tai_cookie_jar_create();
    app->event_net = (TaiPageNet){.request = app_xhr_request,
                                  .start = app_start,
                                  .cookies = app->cookies, .userdata = app};
    if (!app->cookies ||
        pthread_create(&app->loader_thread, NULL, loader_main, app) != 0) {
        set_error(error, app->cookies ? "page loader thread creation failed"
                                      : "cookie jar allocation failed");
        tai_cookie_jar_destroy(app->cookies);
        pthread_cond_destroy(&app->xhr_condition);
        pthread_cond_destroy(&app->condition);
        pthread_mutex_destroy(&app->mutex);
        app_free_state(app);
        return NULL;
    }
    app->loader_started = true;
    pthread_mutex_lock(&app->mutex);
    while (!app->loader_ready)
        pthread_cond_wait(&app->condition, &app->mutex);
    bool ok = app->loader_ok;
    pthread_mutex_unlock(&app->mutex);
    if (!ok) {
        set_error(error, "page loader network initialization failed");
        tai_browser_app_destroy(app);
        return NULL;
    }
    return app;
}

TaiBrowserApp *tai_browser_app_create_with_home_url(const char *default_css,
                                                    bool rtl,
                                                    const char *home_url,
                                                    char **error) {
    return app_create(default_css, rtl, home_url, NULL, error);
}

TaiBrowserApp *tai_browser_app_create_for_test(const char *default_css,
                                               bool rtl, const char *home_url,
                                               const char *ca_file,
                                               char **error) {
    return app_create(default_css, rtl, home_url, ca_file, error);
}

TaiBrowserApp *tai_browser_app_create(const char *default_css, bool rtl,
                                      char **error) {
    TaiBrowserApp *app = tai_browser_app_create_with_home_url(default_css, rtl,
        "https://browser.engineering/", error);
    if (!app) return NULL;
    /* An unusable bookmarks file must not keep the browser from starting.
     * The file is left untouched and this run keeps favorites in memory. */
    char *store_error = NULL;
    TaiBookmarks *bookmarks = tai_bookmarks_open_default(&store_error);
    if (bookmarks) {
        tai_bookmarks_destroy(app->bookmarks);
        app->bookmarks = bookmarks;
    } else {
        fprintf(stderr, "bookmarks will not be saved this session: %s\n",
                store_error ? store_error : "cannot open bookmarks file");
    }
    free(store_error);
    return app;
}

void tai_browser_app_destroy(TaiBrowserApp *app) {
    if (!app) return;
    if (app->loader_started) {
        pthread_mutex_lock(&app->mutex);
        app->stopping = true;
        atomic_store_explicit(&app->stopping_flag, true, memory_order_release);
        /* Every tab set is gone, so nothing may start another load. */
        for (LoadTask *task = app->queued_head; task; task = task->queue_next)
            atomic_store_explicit(&task->cancelled, true,
                                  memory_order_release);
        pthread_cond_broadcast(&app->condition);
        pthread_mutex_unlock(&app->mutex);
        pthread_join(app->loader_thread, NULL);
    }
    completions_free(app->completed_head);
    LoadTask *queued = app->queued_head;
    while (queued) {
        LoadTask *next = queued->queue_next;
        task_destroy(queued);
        queued = next;
    }
    /* The loader and every page that borrowed the jar are gone. */
    tai_cookie_jar_destroy(app->cookies);
    if (app->xhr_condition_ready) pthread_cond_destroy(&app->xhr_condition);
    if (app->condition_ready) pthread_cond_destroy(&app->condition);
    if (app->mutex_ready) pthread_mutex_destroy(&app->mutex);
    app_free_state(app);
}

const char *tai_browser_app_home_url(const TaiBrowserApp *app) {
    return app ? app->home_url : NULL;
}

TaiTabSet *tai_tabset_create_in_app(TaiBrowserApp *app, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!app) {
        set_error(error, "browser is required");
        return NULL;
    }
    if (app->tabset_count == app->tabset_capacity) {
        if (app->tabset_capacity > SIZE_MAX / 2 / sizeof(*app->tabsets)) {
            set_error(error, "window capacity exceeded");
            return NULL;
        }
        size_t capacity = app->tabset_capacity ? app->tabset_capacity * 2 : 4;
        TaiTabSet **next = realloc(app->tabsets, capacity * sizeof(*next));
        if (!next) {
            set_error(error, "window registry allocation failed");
            return NULL;
        }
        app->tabsets = next;
        app->tabset_capacity = capacity;
    }
    TaiTabSet *tabs = calloc(1, sizeof(*tabs));
    if (!tabs) {
        set_error(error, "tab set allocation failed");
        return NULL;
    }
    tabs->app = app;
    app->tabsets[app->tabset_count++] = tabs;
    return tabs;
}

static TaiTabSet *tabset_create_owning(TaiBrowserApp *app, char **error) {
    if (!app) return NULL;
    TaiTabSet *tabs = tai_tabset_create_in_app(app, error);
    if (!tabs) {
        tai_browser_app_destroy(app);
        return NULL;
    }
    tabs->owns_app = true;
    return tabs;
}

TaiTabSet *tai_tabset_create_with_home_url(const char *default_css, bool rtl,
                                           const char *home_url,
                                           char **error) {
    return tabset_create_owning(tai_browser_app_create_with_home_url(
        default_css, rtl, home_url, error), error);
}

TaiTabSet *tai_tabset_create_for_test(const char *default_css, bool rtl,
                                      const char *home_url,
                                      const char *ca_file, char **error) {
    return tabset_create_owning(tai_browser_app_create_for_test(
        default_css, rtl, home_url, ca_file, error), error);
}

TaiTabSet *tai_tabset_create(const char *default_css, bool rtl, char **error) {
    return tabset_create_owning(tai_browser_app_create(default_css, rtl,
                                                       error), error);
}

static bool append_new_slot(TaiTabSet *tabs, const char *url,
                            char **error) {
    if (tabs->count >= TAI_MAX_TABS)
        return set_error(error, "maximum of 25 tabs reached");
    if (!reserve_slot(tabs, error)) return false;
    TaiBrowserApp *app = tabs->app;
    TaiSession *session = tai_session_create_empty(app->default_css,
                                                   app->rtl);
    if (!session) return set_error(error, "tab session allocation failed");
    TabSlot slot = {.id = app->next_tab_id, .generation = 0,
                    .session = session};
    if (app->next_tab_id == UINT64_MAX) {
        tai_session_destroy(session);
        return set_error(error, "tab identity exhausted");
    }
    LoadTask *task = task_create(tabs, &slot, url, NULL, LOAD_NAVIGATION, 0,
                                 error);
    if (!task) {
        tai_session_destroy(session);
        return false;
    }
    tabs->slots[tabs->count] = slot;
    TabSlot *stored = &tabs->slots[tabs->count];
    tabs->count++;
    app->next_tab_id++;
    stored->generation = task->generation;
    stored->active_task = task;
    queue_push(app, task);
    tabs->active = tabs->count - 1;
    return true;
}

bool tai_tabset_start(TaiTabSet *tabs, const char *url, double width,
                      double height,
                      char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!tabs || tabs->started || !url || !isfinite(width) ||
        !isfinite(height) || width <= 0 || height <= 0)
        return set_error(error, "invalid initial tab state");
    tabs->width = width;
    tabs->height = height;
    tabs->started = true;
    if (!append_new_slot(tabs, url, error)) {
        tabs->started = false;
        return false;
    }
    return true;
}

bool tai_tabset_new_tab(TaiTabSet *tabs, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!tabs || !tabs->started)
        return set_error(error, "tab set has not started");
    return append_new_slot(tabs, tabs->app->home_url, error);
}

bool tai_tabset_select(TaiTabSet *tabs, size_t index) {
    if (!tabs || index >= tabs->count) return false;
    tabs->active = index;
    return true;
}

static void virtual_history(const TaiSession *session, const LoadTask *pending,
                            size_t *count, size_t *index) {
    *count = tai_session_history_length(session);
    *index = tai_session_history_index(session);
    if (!pending) return;
    if (pending->kind == LOAD_NAVIGATION) {
        *count = *count ? *index + 2 : 1;
        if (*count) *index = *count - 1;
    } else {
        *index = pending->history_target;
    }
}

static bool navigate_url(TaiTabSet *tabs, const char *url, const char *body,
                         char **error) {
    TabSlot *slot = active_slot(tabs);
    if (!slot || !url) return set_error(error, "invalid tab navigation");
    LoadTask *task = task_create(tabs, slot, url, body, LOAD_NAVIGATION, 0,
                                 error);
    if (!task) return false;
    return task_publish(tabs, slot, task, error);
}

bool tai_tabset_navigate(TaiTabSet *tabs,
                         const TaiNavigationIntent *intent, char **error) {
    if (error) { free(*error); *error = NULL; }
    return navigate_url(tabs, tai_navigation_intent_url(intent),
                        tai_navigation_intent_body(intent), error);
}

bool tai_tabset_navigate_address(TaiTabSet *tabs, const char *text,
                                 char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!tabs || !text) return set_error(error, "invalid tab address");
    char *url = tai_session_normalize_address(text);
    if (!url) return set_error(error, "address URL is malformed or unsupported");
    TaiUrl *parsed = tai_url_parse(url);
    bool mailto = parsed && !strcmp(tai_url_scheme(parsed), "mailto");
    tai_url_destroy(parsed);
    if (mailto) {
        free(url);
        return set_error(error, "mailto requires an external application");
    }
    bool ok = navigate_url(tabs, url, NULL, error);
    free(url);
    return ok;
}

bool tai_tabset_toggle_bookmark(TaiTabSet *tabs, bool *bookmarked,
                                char **error) {
    if (error) { free(*error); *error = NULL; }
    TabSlot *slot = active_slot(tabs);
    if (!slot || slot->active_task || !tai_session_page(slot->session))
        return set_error(error, "no committed bookmarkable page");
    const char *url = tai_url_string(tai_page_url(
        tai_session_page(slot->session)));
    if (!tai_bookmarks_toggle(tabs->app->bookmarks, url, bookmarked))
        return set_error(error, "bookmark toggle failed");
    return true;
}

bool tai_tabset_open_bookmarks(TaiTabSet *tabs, char **error) {
    if (error) { free(*error); *error = NULL; }
    return navigate_url(tabs, "about:bookmarks", NULL, error);
}

bool tai_tabset_history_available(const TaiTabSet *tabs, int direction) {
    if (!tabs || (direction != -1 && direction != 1) ||
        !tabs->count || tabs->active >= tabs->count)
        return false;
    const TaiSession *session = tabs->slots[tabs->active].session;
    const LoadTask *pending = tabs->slots[tabs->active].active_task;
    size_t index = 0, count = 0;
    virtual_history(session, pending, &count, &index);
    return direction < 0 ? index > 0 : index + 1 < count;
}

bool tai_tabset_history(TaiTabSet *tabs, int direction, char **error) {
    if (error) { free(*error); *error = NULL; }
    TabSlot *slot = active_slot(tabs);
    if (!slot || (direction != -1 && direction != 1))
        return set_error(error, "invalid tab history traversal");
    size_t virtual_index = 0, virtual_count = 0;
    virtual_history(slot->session, slot->active_task, &virtual_count,
                    &virtual_index);
    if ((direction < 0 && virtual_index == 0) ||
        (direction > 0 && virtual_index + 1 >= virtual_count)) return true;
    size_t target = direction < 0 ? virtual_index - 1 : virtual_index + 1;
    char *url = tai_session_history_url(slot->session, target);
    if (!url) return set_error(error, "history URL unavailable");
    LoadTask *task = task_create(tabs, slot, url, NULL, LOAD_HISTORY, target,
                                 error);
    free(url);
    if (!task) return false;
    return task_publish(tabs, slot, task, error);
}

bool tai_tabset_record_fragment(TaiTabSet *tabs, const char *url,
                                char **error) {
    if (error) { free(*error); *error = NULL; }
    TabSlot *slot = active_slot(tabs);
    if (!slot) return set_error(error, "no active tab");
    return tai_session_record_fragment(slot->session, url, error);
}

bool tai_tabset_resize(TaiTabSet *tabs, double width, double height,
                       char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!tabs || !isfinite(width) || !isfinite(height) || width <= 0 ||
        height <= 0)
        return set_error(error, "invalid tab viewport size");
    tabs->width = width;
    tabs->height = height;
    for (size_t index = 0; index < tabs->count; index++) {
        TaiPage *page = tai_session_page(tabs->slots[index].session);
        if (page && !tai_page_resize(page, width, height, error)) return false;
    }
    return true;
}

static TaiTabSet *tabset_owning(const TaiBrowserApp *app, uint64_t tab_id) {
    for (size_t index = 0; index < app->tabset_count; index++)
        if (slot_by_id(app->tabsets[index], tab_id))
            return app->tabsets[index];
    return NULL;
}

/* Moves every published completion to the inbox of the tab set that owns its
 * tab. A completion whose tab or window is already gone is released here. */
static void route_completions(TaiBrowserApp *app) {
    pthread_mutex_lock(&app->mutex);
    Completion *items = app->completed_head;
    app->completed_head = NULL;
    app->completed_tail = NULL;
    pthread_mutex_unlock(&app->mutex);
    while (items) {
        Completion *next = items->next;
        items->next = NULL;
        TaiTabSet *owner = tabset_owning(app, items->task->tab_id);
        if (owner) {
            if (owner->inbox_tail) owner->inbox_tail->next = items;
            else owner->inbox_head = items;
            owner->inbox_tail = items;
        } else {
            completions_free(items);
        }
        items = next;
    }
}

bool tai_tabset_pump(TaiTabSet *tabs, bool *changed, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (changed) *changed = false;
    if (!tabs || !changed) return set_error(error, "invalid tab completion pump");
    route_completions(tabs->app);
    Completion *items = tabs->inbox_head;
    tabs->inbox_head = NULL;
    tabs->inbox_tail = NULL;

    bool all_ok = true;
    while (items) {
        Completion *next = items->next;
        LoadTask *task = items->task;
        TabSlot *slot = slot_by_id(tabs, task->tab_id);
        if (slot && slot->generation == task->generation &&
            slot->active_task == task) {
            slot->active_task = NULL;
            *changed = true;
            /* A transport or certificate failure still yields an error page;
             * Python shows it at the requested URL and history position. */
            TaiPage *candidate = items->page;
            items->page = NULL;
            /* The page's load-time net names the task freed below; from now
             * on its JS runs on this thread and queues XHR for the loader. */
            if (candidate) tai_page_set_net(candidate, &tabs->app->event_net);
            if (candidate) {
                if ((tai_page_viewport_width(candidate) != tabs->width ||
                     tai_page_viewport_height(candidate) != tabs->height) &&
                    !tai_page_resize(candidate, tabs->width, tabs->height,
                                     error)) {
                    tai_page_destroy(candidate);
                    candidate = NULL;
                    all_ok = false;
                    if (!error || !*error)
                        set_error(error, "completed page resize failed");
                }
                if (candidate) {
                    bool committed = task->kind == LOAD_HISTORY
                        ? tai_session_commit_history(slot->session, candidate,
                            task->history_target, error)
                        : tai_session_commit_navigation(slot->session,
                                                        candidate, error);
                    if (committed) candidate = NULL;
                    else {
                        all_ok = false;
                        if (!error || !*error)
                            set_error(error, "tab navigation commit failed");
                    }
                }
            }
            tai_page_destroy(candidate);
            if (items->error && !items->network_failure)
                fprintf(stderr, "tab navigation failed: %s\n", items->error);
        }
        tai_page_destroy(items->page);
        free(items->error);
        task_destroy(task);
        items = next;
    }
    return all_ok;
}

bool tai_tabset_run_tasks(TaiTabSet *tabs, double now, size_t budget,
                          bool *active_changed, double *next, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (active_changed) *active_changed = false;
    if (next) *next = INFINITY;
    if (!tabs || isnan(now)) return set_error(error, "invalid page task input");
    bool ok = true;
    for (size_t index = 0; index < tabs->count; index++) {
        TaiPage *page = tai_session_page(tabs->slots[index].session);
        if (!page) continue;
        if (tai_page_next_task(page) <= now) {
            bool changed = false;
            char *page_error = NULL;
            if (!tai_page_run_tasks(page, now, budget, &changed, &page_error)) {
                if (ok) set_error(error, page_error ? page_error
                                                    : "page task failed");
                ok = false;
            }
            free(page_error);
            if (changed && index == tabs->active && active_changed)
                *active_changed = true;
        }
        double due = tai_page_next_task(page);
        if (next && due < *next) *next = due;
    }
    return ok;
}

char *tai_tabset_history_url(const TaiTabSet *tabs, size_t index) {
    if (!tabs || !tabs->count || tabs->active >= tabs->count) return NULL;
    const TabSlot *slot = &tabs->slots[tabs->active];
    size_t count = 0, current = 0;
    virtual_history(slot->session, slot->active_task, &count, &current);
    if (index >= count) return NULL;
    if (slot->active_task && slot->active_task->kind == LOAD_NAVIGATION &&
        index == count - 1)
        return tai_strdup(slot->active_task->url);
    return tai_session_history_url(slot->session, index);
}

bool tai_tabset_view(const TaiTabSet *tabs, TaiTabSetView *view) {
    if (!tabs || !view || !tabs->count || tabs->active >= tabs->count)
        return false;
    const TabSlot *slot = &tabs->slots[tabs->active];
    TaiPage *page = tai_session_page(slot->session);
    const char *committed_url = page ? tai_url_string(tai_page_url(page)) : NULL;
    bool bookmarkable = !slot->active_task && committed_url &&
        *committed_url && strcmp(committed_url, "about:blank") &&
        strcmp(committed_url, "about:bookmarks");
    size_t history_count = 0, history_index = 0;
    virtual_history(slot->session, slot->active_task, &history_count,
                    &history_index);
    *view = (TaiTabSetView){
        .page = page,
        .url = slot->active_task ? slot->active_task->url
            : page ? tai_url_string(tai_page_url(page)) : "",
        .tab_count = tabs->count,
        .active_index = tabs->active,
        .history_count = history_count,
        .history_index = history_index,
        .loading = slot->active_task != NULL,
        .can_go_back = tai_tabset_history_available(tabs, -1),
        .can_go_forward = tai_tabset_history_available(tabs, 1),
        .bookmarkable = bookmarkable,
        .bookmarked = bookmarkable &&
            tai_bookmarks_contains(tabs->app->bookmarks, committed_url),
        .secure = page && tai_page_secure(page),
    };
    return true;
}

void tai_tabset_destroy(TaiTabSet *tabs) {
    if (!tabs) return;
    TaiBrowserApp *app = tabs->app;
    /* The shared loader may still own this window's tasks; cancelled ones
     * complete there and route_completions() or app destroy releases them. */
    pthread_mutex_lock(&app->mutex);
    for (size_t index = 0; index < tabs->count; index++) {
        LoadTask *task = tabs->slots[index].active_task;
        if (task)
            atomic_store_explicit(&task->cancelled, true,
                                  memory_order_release);
    }
    pthread_cond_broadcast(&app->condition);
    pthread_mutex_unlock(&app->mutex);
    for (size_t index = 0; index < app->tabset_count; index++) {
        if (app->tabsets[index] != tabs) continue;
        memmove(&app->tabsets[index], &app->tabsets[index + 1],
                (app->tabset_count - index - 1) * sizeof(*app->tabsets));
        app->tabset_count--;
        break;
    }
    completions_free(tabs->inbox_head);
    for (size_t index = 0; index < tabs->count; index++)
        tai_session_destroy(tabs->slots[index].session);
    free(tabs->slots);
    bool owns_app = tabs->owns_app;
    free(tabs);
    if (owns_app) tai_browser_app_destroy(app);
}
