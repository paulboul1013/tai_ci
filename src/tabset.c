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

typedef enum { LOAD_NAVIGATION, LOAD_HISTORY } LoadKind;

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

struct LoadTask {
    struct TaiTabSet *owner;
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

struct TaiTabSet {
    const char *default_css;
    char *home_url;
    TaiBookmarks *bookmarks;
    bool rtl;
    double width;
    double height;
    bool started;
    TabSlot *slots;
    size_t count;
    size_t capacity;
    size_t active;
    uint64_t next_tab_id;

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

static void completion_publish(TaiTabSet *tabs, LoadTask *task) {
    Completion *completion = &task->completion;
    pthread_mutex_lock(&tabs->mutex);
    if (tabs->completed_tail) tabs->completed_tail->next = completion;
    else tabs->completed_head = completion;
    tabs->completed_tail = completion;
    pthread_mutex_unlock(&tabs->mutex);
}

static void completion_append(TaiTabSet *tabs, LoadTask *task, TaiPage *page,
                              bool network_failure, char *error) {
    completion_prepare(task, page, network_failure, error);
    completion_publish(tabs, task);
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

static void active_remove(TaiTabSet *tabs, LoadTask **slot) {
    LoadTask *task = *slot;
    *slot = task->active_next;
    task->active_next = NULL;
    (void)tabs;
}

static LoadTask *queued_take(TaiTabSet *tabs) {
    LoadTask *task = tabs->queued_head;
    if (!task) return NULL;
    tabs->queued_head = task->queue_next;
    if (!tabs->queued_head) tabs->queued_tail = NULL;
    task->queue_next = NULL;
    return task;
}

static void queue_push(TaiTabSet *tabs, LoadTask *task) {
    pthread_mutex_lock(&tabs->mutex);
    if (tabs->queued_tail) tabs->queued_tail->queue_next = task;
    else tabs->queued_head = task;
    tabs->queued_tail = task;
    pthread_cond_signal(&tabs->condition);
    pthread_mutex_unlock(&tabs->mutex);
}

static void queue_push_locked(TaiTabSet *tabs, LoadTask *task) {
    if (tabs->queued_tail) tabs->queued_tail->queue_next = task;
    else tabs->queued_head = task;
    tabs->queued_tail = task;
    pthread_cond_signal(&tabs->condition);
}

static void load_task_start(TaiTabSet *tabs, LoadTask *task,
                            TaiNetwork *network, bool network_failed) {
    if (network_failed || atomic_load_explicit(&task->cancelled,
                                               memory_order_acquire)) {
        completion_append(tabs, task, NULL, false,
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
        completion_append(tabs, task, NULL, false,
                          tai_strdup("navigation URL allocation failed"));
        return;
    }
    char *error = NULL;
    TaiPageLoad *load = task->internal_markup
        ? tai_page_load_async_markup(network, url, task->internal_markup,
            tabs->default_css, task->width, task->height, tabs->rtl,
            page_load_done, task, &error)
        : tai_page_load_async(network, url, referrer, task->body,
            tabs->default_css, task->width, task->height, tabs->rtl,
            page_load_done, task, &error);
    tai_url_destroy(url);
    tai_url_destroy(referrer);
    if (task->completed) {
        completion_publish(tabs, task);
        free(error);
        return;
    }
    if (!load) {
        completion_append(tabs, task, NULL, false, error);
        return;
    }
    task->page_load = task->completed ? NULL : load;
    if (atomic_load_explicit(&task->cancelled, memory_order_acquire)) {
        task->page_load = NULL;
        tai_page_load_async_cancel(load);
        completion_append(tabs, task, NULL, false,
                          tai_strdup("navigation cancelled"));
        return;
    }
    task->active_next = tabs->active_loads;
    tabs->active_loads = task;
}

static void cancel_or_reap_loads(TaiTabSet *tabs, TaiNetwork *network,
                                 bool network_failed) {
    LoadTask **slot = &tabs->active_loads;
    while (*slot) {
        LoadTask *task = *slot;
        if (task->completed) {
            active_remove(tabs, slot);
            completion_publish(tabs, task);
            continue;
        }
        if (network_failed || atomic_load_explicit(&task->cancelled,
                                                    memory_order_acquire)) {
            TaiPageLoad *load = task->page_load;
            task->page_load = NULL;
            if (load) tai_page_load_async_cancel(load);
            active_remove(tabs, slot);
            completion_append(tabs, task, NULL, false,
                tai_strdup(network_failed ? "network polling failed"
                                          : "navigation cancelled"));
            continue;
        }
        slot = &task->active_next;
    }
    (void)network;
}

static void *loader_main(void *opaque) {
    TaiTabSet *tabs = opaque;
    TaiNetwork *network = tai_network_create();
    pthread_mutex_lock(&tabs->mutex);
    tabs->loader_ok = network != NULL;
    tabs->loader_ready = true;
    pthread_cond_broadcast(&tabs->condition);
    pthread_mutex_unlock(&tabs->mutex);
    if (!network) return NULL;

    bool network_failed = false;
    for (;;) {
        pthread_mutex_lock(&tabs->mutex);
        while (!tabs->stopping && !tabs->queued_head && !tabs->active_loads)
            pthread_cond_wait(&tabs->condition, &tabs->mutex);
        bool stopping = tabs->stopping;
        LoadTask *task = queued_take(tabs);
        pthread_mutex_unlock(&tabs->mutex);

        if (task) load_task_start(tabs, task, network, network_failed);
        if (tabs->active_loads) {
            if (!tai_network_poll(network, 16)) network_failed = true;
            cancel_or_reap_loads(tabs, network, network_failed || stopping);
        }

        pthread_mutex_lock(&tabs->mutex);
        bool done = tabs->stopping && !tabs->queued_head && !tabs->active_loads;
        pthread_mutex_unlock(&tabs->mutex);
        if (done) break;
    }
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
    task->owner = tabs;
    task->tab_id = slot->id;
    task->generation = slot->generation + 1;
    task->url = tai_strdup(url);
    if (!strcmp(url, "about:bookmarks"))
        task->internal_markup = bookmarks_markup(tabs->bookmarks);
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
    pthread_mutex_lock(&tabs->mutex);
    if (tabs->stopping) {
        pthread_mutex_unlock(&tabs->mutex);
        task_destroy(task);
        return set_error(error, "tab set is closing");
    }
    if (slot->active_task)
        atomic_store_explicit(&slot->active_task->cancelled, true,
                              memory_order_release);
    slot->generation = task->generation;
    slot->active_task = task;
    queue_push_locked(tabs, task);
    pthread_mutex_unlock(&tabs->mutex);
    return true;
}

static TabSlot *active_slot(TaiTabSet *tabs) {
    return tabs && tabs->count && tabs->active < tabs->count
        ? &tabs->slots[tabs->active] : NULL;
}

static TabSlot *slot_by_id(TaiTabSet *tabs, uint64_t id) {
    for (size_t index = 0; index < tabs->count; index++)
        if (tabs->slots[index].id == id) return &tabs->slots[index];
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

TaiTabSet *tai_tabset_create_with_home_url(const char *default_css, bool rtl,
                                           const char *home_url,
                                           char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!default_css || !home_url || !*home_url) {
        set_error(error, "default CSS and New Tab URL are required");
        return NULL;
    }
    TaiTabSet *tabs = calloc(1, sizeof(*tabs));
    if (!tabs) {
        set_error(error, "tab set allocation failed");
        return NULL;
    }
    tabs->default_css = default_css;
    tabs->home_url = tai_strdup(home_url);
    tabs->bookmarks = tai_bookmarks_create();
    tabs->rtl = rtl;
    tabs->next_tab_id = 1;
    if (!tabs->home_url || !tabs->bookmarks) {
        set_error(error, "tab set state allocation failed");
        tai_bookmarks_destroy(tabs->bookmarks);
        free(tabs->home_url);
        free(tabs);
        return NULL;
    }
    if (pthread_mutex_init(&tabs->mutex, NULL) != 0) {
        set_error(error, "tab set mutex initialization failed");
        tai_bookmarks_destroy(tabs->bookmarks);
        free(tabs->home_url);
        free(tabs);
        return NULL;
    }
    tabs->mutex_ready = true;
    if (pthread_cond_init(&tabs->condition, NULL) != 0) {
        set_error(error, "tab set condition initialization failed");
        pthread_mutex_destroy(&tabs->mutex);
        tai_bookmarks_destroy(tabs->bookmarks);
        free(tabs->home_url);
        free(tabs);
        return NULL;
    }
    tabs->condition_ready = true;
    if (pthread_create(&tabs->loader_thread, NULL, loader_main, tabs) != 0) {
        set_error(error, "page loader thread creation failed");
        pthread_cond_destroy(&tabs->condition);
        pthread_mutex_destroy(&tabs->mutex);
        tai_bookmarks_destroy(tabs->bookmarks);
        free(tabs->home_url);
        free(tabs);
        return NULL;
    }
    tabs->loader_started = true;
    pthread_mutex_lock(&tabs->mutex);
    while (!tabs->loader_ready)
        pthread_cond_wait(&tabs->condition, &tabs->mutex);
    bool ok = tabs->loader_ok;
    pthread_mutex_unlock(&tabs->mutex);
    if (!ok) {
        set_error(error, "page loader network initialization failed");
        tai_tabset_destroy(tabs);
        return NULL;
    }
    return tabs;
}

TaiTabSet *tai_tabset_create(const char *default_css, bool rtl, char **error) {
    TaiTabSet *tabs = tai_tabset_create_with_home_url(default_css, rtl,
        "https://browser.engineering/", error);
    if (!tabs) return NULL;
    /* An unusable bookmarks file must not keep the browser from starting.
     * The file is left untouched and this run keeps favorites in memory. */
    char *store_error = NULL;
    TaiBookmarks *bookmarks = tai_bookmarks_open_default(&store_error);
    if (bookmarks) {
        tai_bookmarks_destroy(tabs->bookmarks);
        tabs->bookmarks = bookmarks;
    } else {
        fprintf(stderr, "bookmarks will not be saved this session: %s\n",
                store_error ? store_error : "cannot open bookmarks file");
    }
    free(store_error);
    return tabs;
}

static bool append_new_slot(TaiTabSet *tabs, const char *url,
                            char **error) {
    if (tabs->count >= TAI_MAX_TABS)
        return set_error(error, "maximum of 25 tabs reached");
    if (!reserve_slot(tabs, error)) return false;
    TaiSession *session = tai_session_create_empty(tabs->default_css,
                                                   tabs->rtl);
    if (!session) return set_error(error, "tab session allocation failed");
    TabSlot slot = {.id = tabs->next_tab_id, .generation = 0,
                    .session = session};
    if (tabs->next_tab_id == UINT64_MAX) {
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
    tabs->next_tab_id++;
    stored->generation = task->generation;
    stored->active_task = task;
    queue_push(tabs, task);
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
    return append_new_slot(tabs, tabs->home_url, error);
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
    if (!tai_bookmarks_toggle(tabs->bookmarks, url, bookmarked))
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

bool tai_tabset_pump(TaiTabSet *tabs, bool *changed, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (changed) *changed = false;
    if (!tabs || !changed) return set_error(error, "invalid tab completion pump");
    pthread_mutex_lock(&tabs->mutex);
    Completion *items = tabs->completed_head;
    tabs->completed_head = NULL;
    tabs->completed_tail = NULL;
    pthread_mutex_unlock(&tabs->mutex);

    bool all_ok = true;
    while (items) {
        Completion *next = items->next;
        LoadTask *task = items->task;
        TabSlot *slot = slot_by_id(tabs, task->tab_id);
        if (slot && slot->generation == task->generation &&
            slot->active_task == task) {
            slot->active_task = NULL;
            *changed = true;
            TaiPage *candidate = items->page;
            items->page = NULL;
            bool initial_failure = items->network_failure &&
                tai_session_page(slot->session) == NULL;
            if (candidate && (!items->network_failure || initial_failure)) {
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
            tai_bookmarks_contains(tabs->bookmarks, committed_url),
    };
    return true;
}

void tai_tabset_destroy(TaiTabSet *tabs) {
    if (!tabs) return;
    if (tabs->loader_started) {
        pthread_mutex_lock(&tabs->mutex);
        tabs->stopping = true;
        for (size_t index = 0; index < tabs->count; index++) {
            LoadTask *task = tabs->slots[index].active_task;
            if (task)
                atomic_store_explicit(&task->cancelled, true,
                                      memory_order_release);
        }
        pthread_cond_broadcast(&tabs->condition);
        pthread_mutex_unlock(&tabs->mutex);
        pthread_join(tabs->loader_thread, NULL);
    }
    Completion *completion = tabs->completed_head;
    while (completion) {
        Completion *next = completion->next;
        tai_page_destroy(completion->page);
        free(completion->error);
        task_destroy(completion->task);
        completion = next;
    }
    LoadTask *queued = tabs->queued_head;
    while (queued) {
        LoadTask *next = queued->queue_next;
        task_destroy(queued);
        queued = next;
    }
    for (size_t index = 0; index < tabs->count; index++)
        tai_session_destroy(tabs->slots[index].session);
    free(tabs->slots);
    tai_bookmarks_destroy(tabs->bookmarks);
    free(tabs->home_url);
    if (tabs->condition_ready) pthread_cond_destroy(&tabs->condition);
    if (tabs->mutex_ready) pthread_mutex_destroy(&tabs->mutex);
    free(tabs);
}
