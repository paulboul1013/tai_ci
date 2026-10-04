#define _POSIX_C_SOURCE 200809L
#include "tai/js.h"
#include "tai/css.h"
#include "tai_js_sources.h"

#include <errno.h>
#include <math.h>
#include <quickjs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utf8proc.h>

/* The page runs the frozen tests/reference/runtime.js unchanged, followed by
 * src/js_prelude.js. Both reach native code only through call_python(op, ...),
 * mirroring the Python JSContext's exported functions.
 *
 * Handles are the only way JS names a node. As in Python's get_handle, they
 * are numbered in first-use order and never reused; only this file converts
 * between handles and nodes (handle_of / node_arg). Wrappers live in JS; C
 * holds no JSValue for a node. */
/* A pending setTimeout or setInterval; the callback stays in JS, keyed by
 * handle (js_scheduling.js). */
typedef struct {
    int64_t handle;
    bool interval;
    double due;    /* on the host's clock */
    double period; /* interval only, seconds */
    uint64_t order;
} Timer;

struct TaiJsContext {
    JSRuntime *runtime;
    JSContext *context;
    TaiNode *root;
    TaiJsHost host;
    double deadline;
    double extended; /* seconds of XHR blocking added to this deadline */
    unsigned depth; /* nested JS entries; only the outermost sets the deadline */
    TaiNode **handle_nodes;          /* handle -> node */
    size_t handle_count, handle_capacity;
    size_t *node_handles;            /* node id -> handle + 1; 0 means none */
    size_t node_handles_capacity;
    Timer *timers;                   /* unordered; see next_due_timer */
    size_t timer_count, timer_capacity;
    uint64_t timer_order;            /* arming order, breaks due-time ties */
};

static double seconds_now(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0.0;
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

double tai_js_clock(void) { return seconds_now(); }

static double timer_now(TaiJsContext *js) {
    return js->host.now ? js->host.now(js->host.userdata) : seconds_now();
}

static int interrupt_handler(JSRuntime *runtime, void *opaque) {
    (void)runtime;
    TaiJsContext *js = opaque;
    if (js->host.cancelled && js->host.cancelled(js->host.userdata)) return 1;
    return js->deadline > 0.0 && seconds_now() >= js->deadline;
}

/* Every entry from C into JS goes through enter/leave. QuickJS measures stack
 * depth from the thread that last updated the stack top; the runtime is created
 * on the loader thread and later used on the SDL thread, so the outermost entry
 * refreshes it. Nested entries (a bridge call re-entering JS) keep the outer
 * deadline instead of replacing or clearing it. */
static void enter_js(TaiJsContext *js) {
    if (js->depth++ == 0) {
        JS_UpdateStackTop(js->runtime);
        js->deadline = seconds_now() + 2.0;
        js->extended = 0.0;
    }
}

static void leave_js(TaiJsContext *js) {
    if (--js->depth == 0) js->deadline = 0.0;
}

static bool set_error(char **error, const char *message) {
    if (error && !*error) *error = tai_strdup(message ? message : "JavaScript error");
    return false;
}

/* Takes the pending exception and returns it as an owned string. */
static char *take_exception_text(JSContext *context) {
    JSValue exception = JS_GetException(context);
    const char *message = JS_ToCString(context, exception);
    if (!message) JS_FreeValue(context, JS_GetException(context));
    char *text = tai_strdup(message ? message : "(unprintable exception)");
    JS_FreeCString(context, message);
    JS_FreeValue(context, exception);
    return text;
}

static bool exception_error(TaiJsContext *js, char **error) {
    char *text = take_exception_text(js->context);
    bool result = set_error(error, text ? text : "JavaScript error");
    free(text);
    return result;
}

/* D1: without a host sink, diagnostics go to stderr because stdout carries
 * the headless JSON output. */
static void report(TaiJsContext *js, const TaiJsReport *entry) {
    if (js->host.report) {
        js->host.report(js->host.userdata, entry);
    } else if (entry->kind == TAI_JS_REPORT_LOG) {
        fprintf(stderr, "%s\n", entry->text);
    } else if (entry->kind == TAI_JS_REPORT_RAF_ERROR) {
        fprintf(stderr, "requestAnimationFrame callback crashed %s\n",
                entry->text);
    } else if (entry->kind == TAI_JS_REPORT_TASK_ERROR) {
        fprintf(stderr, "%s %s\n", entry->event, entry->text);
    } else {
        fprintf(stderr, "Event %s crashed %s\n", entry->event, entry->text);
    }
}

static void report_event_error(TaiJsContext *js, const char *type,
                               const char *text) {
    TaiJsReport entry = {.kind = TAI_JS_REPORT_EVENT_ERROR, .event = type,
                         .text = text ? text : "(unprintable exception)"};
    report(js, &entry);
}

static void report_raf_error(TaiJsContext *js, const char *text) {
    TaiJsReport entry = {.kind = TAI_JS_REPORT_RAF_ERROR,
                         .text = text ? text : "(unprintable exception)"};
    report(js, &entry);
}

static void report_task_error(TaiJsContext *js, const char *prefix,
                              const char *text) {
    TaiJsReport entry = {.kind = TAI_JS_REPORT_TASK_ERROR, .event = prefix,
                         .text = text ? text : "(unprintable exception)"};
    report(js, &entry);
}

static void invalidate(TaiJsContext *js) {
    if (js->host.invalidated) js->host.invalidated(js->host.userdata);
}

/* ---- handles ------------------------------------------------------------ */

static bool grow_array(void **items, size_t *capacity, size_t needed,
                       size_t width) {
    if (needed <= *capacity) return true;
    size_t next = *capacity ? *capacity : 64;
    while (next < needed) {
        if (next > SIZE_MAX / 2) return false;
        next *= 2;
    }
    if (next > SIZE_MAX / width) return false;
    void *grown = realloc(*items, next * width);
    if (!grown) return false;
    *items = grown;
    *capacity = next;
    return true;
}

/* Python JSContext.get_handle: the first use of a node assigns the next one. */
static bool handle_of(TaiJsContext *js, TaiNode *node, int64_t *handle) {
    size_t old_capacity = js->node_handles_capacity;
    if (node->id == SIZE_MAX ||
        !grow_array((void **)&js->node_handles, &js->node_handles_capacity,
                    node->id + 1, sizeof(*js->node_handles)))
        return false;
    memset(js->node_handles + old_capacity, 0,
           (js->node_handles_capacity - old_capacity) *
               sizeof(*js->node_handles));
    size_t stored = js->node_handles[node->id];
    if (!stored) {
        if (js->handle_count >= (size_t)INT64_MAX ||
            !grow_array((void **)&js->handle_nodes, &js->handle_capacity,
                        js->handle_count + 1, sizeof(*js->handle_nodes)))
            return false;
        js->handle_nodes[js->handle_count++] = node;
        stored = js->node_handles[node->id] = js->handle_count;
    }
    *handle = (int64_t)(stored - 1);
    return true;
}

static JSValue handle_value(TaiJsContext *js, TaiNode *node) {
    int64_t handle;
    if (!handle_of(js, node, &handle)) return JS_ThrowOutOfMemory(js->context);
    return JS_NewInt64(js->context, handle);
}

/* Python raises KeyError for a handle it never issued; any non-integer value
 * (undefined from a non-Node argument, strings, fractions) is one too. */
static bool node_arg(TaiJsContext *js, JSValueConst value, TaiNode **node) {
    double number;
    if (JS_IsNumber(value) && JS_ToFloat64(js->context, &number, value) == 0 &&
        number >= 0.0 && number < (double)js->handle_count &&
        floor(number) == number) {
        *node = js->handle_nodes[(size_t)number];
        return true;
    }
    JS_ThrowPlainError(js->context, "Unknown node handle");
    return false;
}

static bool element_arg(TaiJsContext *js, JSValueConst value, TaiNode **node,
                        const char *message) {
    if (!node_arg(js, value, node)) return false;
    if ((*node)->kind == TAI_ELEMENT) return true;
    JS_ThrowPlainError(js->context, "%s", message);
    return false;
}

static JSValue throw_dom_status(JSContext *context, TaiDomStatus status) {
    switch (status) {
    case TAI_DOM_NO_MEMORY: return JS_ThrowOutOfMemory(context);
    case TAI_DOM_NODE_LIMIT:
        return JS_ThrowPlainError(context, "Document node limit reached");
    case TAI_DOM_DEPTH_LIMIT:
        return JS_ThrowPlainError(context, "Document depth limit reached");
    case TAI_DOM_CYCLE:
        return JS_ThrowPlainError(context,
            "Cannot insert a node into itself or its descendant");
    case TAI_DOM_NOT_CHILD:
        return JS_ThrowPlainError(context, "Node is not a child of this parent");
    case TAI_DOM_REFERENCE_NOT_CHILD:
        return JS_ThrowPlainError(context,
            "Reference child is not a child of parent");
    case TAI_DOM_PARSE_ERROR:
        /* Python's HTMLParser raises IndexError: a catchable bridge error. */
        return JS_ThrowPlainError(context,
            "HTML parsing failed: invalid parser stack");
    case TAI_DOM_WRONG_DOCUMENT:
    case TAI_DOM_OK:
        break;
    }
    return JS_ThrowPlainError(context, "Node belongs to another document");
}

/* ---- ID globals ----------------------------------------------------------- */

/* Open-addressing set of borrowed id strings: a TaiMap lookup is linear,
 * which made the walk quadratic in the number of ids (Python uses a dict). */
typedef struct {
    const char **slots;
    size_t capacity, count;
} IdSet;

static size_t id_hash(const char *id) {
    uint64_t hash = 1469598103934665603u; /* FNV-1a */
    for (const unsigned char *c = (const unsigned char *)id; *c; c++)
        hash = (hash ^ *c) * 1099511628211u;
    return (size_t)hash;
}

/* 1 when added, 0 when already present, -1 on allocation failure. */
static int id_set_add(IdSet *set, const char *id) {
    if (set->count >= set->capacity / 2) {
        size_t capacity = set->capacity ? set->capacity * 2 : 64;
        if (capacity < set->capacity) return -1;
        const char **slots = calloc(capacity, sizeof(*slots));
        if (!slots) return -1;
        for (size_t i = 0; i < set->capacity; i++) {
            if (!set->slots[i]) continue;
            size_t slot = id_hash(set->slots[i]) & (capacity - 1);
            while (slots[slot]) slot = (slot + 1) & (capacity - 1);
            slots[slot] = set->slots[i];
        }
        free(set->slots);
        set->slots = slots;
        set->capacity = capacity;
    }
    size_t slot = id_hash(id) & (set->capacity - 1);
    for (; set->slots[slot]; slot = (slot + 1) & (set->capacity - 1))
        if (!strcmp(set->slots[slot], id)) return 0;
    set->slots[slot] = id;
    set->count++;
    return 1;
}

typedef struct {
    TaiJsContext *js;
    JSValue entries;
    uint32_t count;
    IdSet seen;
} IdEntries;

/* Python update_id_globals: the first element in document order for each
 * non-empty id, as [id, handle] pairs. */
static bool collect_ids(IdEntries *ids, TaiNode *node) {
    JSContext *context = ids->js->context;
    if (node->kind == TAI_ELEMENT) {
        const char *id = tai_map_get(&node->attributes, "id");
        int added = id && *id ? id_set_add(&ids->seen, id) : 0;
        if (added) {
            int64_t handle;
            if (added < 0 || !handle_of(ids->js, node, &handle)) {
                JS_ThrowOutOfMemory(context);
                return false;
            }
            JSValue entry = JS_NewArray(context);
            if (JS_IsException(entry) ||
                JS_SetPropertyUint32(context, ids->entries, ids->count++,
                                     entry) < 0 ||
                JS_SetPropertyUint32(context, entry, 0,
                                     JS_NewString(context, id)) < 0 ||
                JS_SetPropertyUint32(context, entry, 1,
                                     JS_NewInt64(context, handle)) < 0)
                return false;
        }
    }
    for (size_t index = 0; index < node->child_count; index++)
        if (!collect_ids(ids, node->children[index])) return false;
    return true;
}

/* Leaves a pending exception on failure. */
static bool sync_id_globals(TaiJsContext *js) {
    JSContext *context = js->context;
    IdEntries ids = {.js = js, .entries = JS_NewArray(context)};
    bool ok = !JS_IsException(ids.entries) && collect_ids(&ids, js->root);
    free(ids.seen.slots);
    if (ok) {
        JSValue global = JS_GetGlobalObject(context);
        JSValue function = JS_GetPropertyStr(context, global, "sync_id_globals");
        enter_js(js);
        JSValue result = JS_Call(context, function, global, 1,
                                 (JSValueConst *)&ids.entries);
        leave_js(js);
        ok = !JS_IsException(result);
        JS_FreeValue(context, result);
        JS_FreeValue(context, function);
        JS_FreeValue(context, global);
    }
    JS_FreeValue(context, ids.entries);
    return ok;
}

/* After a completed mutation: a failed resync is reported, not turned into an
 * error for the mutation that already happened. An uncatchable error (the
 * deadline interrupt) still propagates so the running script stops. */
static JSValue after_mutation(TaiJsContext *js, bool ids_may_change) {
    JSValue result = JS_UNDEFINED;
    if (ids_may_change && !sync_id_globals(js)) {
        JSContext *context = js->context;
        JSValue exception = JS_GetException(context);
        if (JS_IsUncatchableError(exception)) {
            result = JS_Throw(context, exception);
        } else {
            const char *text = JS_ToCString(context, exception);
            if (!text) JS_FreeValue(context, JS_GetException(context));
            fprintf(stderr, "ID global sync failed: %s\n",
                    text ? text : "(unprintable exception)");
            JS_FreeCString(context, text);
            JS_FreeValue(context, exception);
        }
    }
    invalidate(js);
    return result;
}

static void node_removed(TaiJsContext *js, TaiNode *node) {
    if (js->host.node_removed) js->host.node_removed(js->host.userdata, node);
}

/* ---- bridge operations ------------------------------------------------------ */

static char *casefold(const char *text) {
    utf8proc_uint8_t *mapped = NULL;
    utf8proc_ssize_t size = utf8proc_map((const utf8proc_uint8_t *)text, 0, &mapped,
        UTF8PROC_NULLTERM | UTF8PROC_STABLE | UTF8PROC_CASEFOLD);
    return size < 0 ? NULL : (char *)mapped;
}

/* Python str() of the value dukpy hands over: strings and numbers read the
 * same, but booleans and null/undefined print as True/False/None. */
static char *python_str(JSContext *context, JSValueConst value) {
    if (JS_IsBool(value))
        return tai_strdup(JS_ToBool(context, value) ? "True" : "False");
    if (JS_IsNull(value) || JS_IsUndefined(value)) return tai_strdup("None");
    const char *text = JS_ToCString(context, value);
    if (!text) return NULL;
    char *copy = tai_strdup(text);
    JS_FreeCString(context, text);
    if (!copy) JS_ThrowOutOfMemory(context);
    return copy;
}

typedef JSValue (*Operation)(TaiJsContext *js, int argc, JSValueConst *argv);

static JSValue op_log(TaiJsContext *js, int argc, JSValueConst *argv) {
    JSContext *context = js->context;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue json = JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) return JS_EXCEPTION;
    const char *json_text = JS_IsUndefined(json) ? NULL : JS_ToCString(context, json);
    const char *string = JS_IsString(value) ? JS_ToCString(context, value) : NULL;
    TaiJsReport entry = {.kind = TAI_JS_REPORT_LOG,
                         .json = json_text ? json_text : "null"};
    entry.text = string ? string : entry.json;
    bool ok = (!JS_IsString(value) || string) &&
              (JS_IsUndefined(json) || json_text);
    if (ok) report(js, &entry);
    JS_FreeCString(context, string);
    JS_FreeCString(context, json_text);
    JS_FreeValue(context, json);
    return ok ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue op_listener_error(TaiJsContext *js, int argc, JSValueConst *argv) {
    JSContext *context = js->context;
    const char *type = JS_ToCString(context, argv[0]);
    if (!type) JS_FreeValue(context, JS_GetException(context));
    const char *text = argc > 1 ? JS_ToCString(context, argv[1]) : NULL;
    if (argc > 1 && !text) JS_FreeValue(context, JS_GetException(context));
    report_event_error(js, type ? type : "?", text);
    JS_FreeCString(context, text);
    JS_FreeCString(context, type);
    return JS_UNDEFINED;
}

/* D10: js_prelude.js reports a throwing animation frame callback here and
 * runs the rest of the batch. */
static JSValue op_raf_error(TaiJsContext *js, int argc, JSValueConst *argv) {
    JSContext *context = js->context;
    const char *text = argc > 0 ? JS_ToCString(context, argv[0]) : NULL;
    if (argc > 0 && !text) JS_FreeValue(context, JS_GetException(context));
    report_raf_error(js, text);
    JS_FreeCString(context, text);
    return JS_UNDEFINED;
}

/* Python JSContext.requestAnimationFrame: the callback is already queued in
 * RAF_LISTENERS; only the owner learns that a frame is needed. */
static JSValue op_request_animation_frame(TaiJsContext *js, int argc,
                                          JSValueConst *argv) {
    (void)argc;
    (void)argv;
    if (js->host.animation_frame_requested)
        js->host.animation_frame_requested(js->host.userdata);
    return JS_UNDEFINED;
}

typedef struct {
    TaiJsContext *js;
    const TaiSelector *selector;
    JSValue array;
    uint32_t count;
} Matches;

static bool collect_matches(Matches *matches, TaiNode *node) {
    if (tai_selector_matches(matches->selector, node)) {
        JSValue handle = handle_value(matches->js, node);
        if (JS_IsException(handle) ||
            JS_SetPropertyUint32(matches->js->context, matches->array,
                                 matches->count++, handle) < 0)
            return false;
    }
    for (size_t index = 0; index < node->child_count; index++)
        if (!collect_matches(matches, node->children[index])) return false;
    return true;
}

static JSValue op_query_selector_all(TaiJsContext *js, int argc,
                                     JSValueConst *argv) {
    (void)argc;
    JSContext *context = js->context;
    const char *text = JS_ToCString(context, argv[0]);
    if (!text) return JS_EXCEPTION;
    char *error = NULL;
    TaiSelector *selector = tai_selector_parse(text, &error);
    JS_FreeCString(context, text);
    if (!selector) {
        JSValue thrown = JS_ThrowPlainError(context, "%s",
                                            error ? error : "invalid selector");
        free(error);
        return thrown;
    }
    Matches matches = {.js = js, .selector = selector,
                       .array = JS_NewArray(context)};
    bool ok = !JS_IsException(matches.array) && collect_matches(&matches, js->root);
    tai_selector_destroy(selector);
    if (ok) return matches.array;
    JS_FreeValue(context, matches.array);
    return JS_EXCEPTION;
}

static JSValue op_get_attribute(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    TaiNode *node;
    if (!element_arg(js, argv[0], &node, "getAttribute can only be used on an Element"))
        return JS_EXCEPTION;
    /* Python looks the raw value up in a str-keyed dict: a non-string name
     * never matches. Missing and empty values both read as "". */
    if (!JS_IsString(argv[1])) return JS_NewString(js->context, "");
    const char *name = JS_ToCString(js->context, argv[1]);
    if (!name) return JS_EXCEPTION;
    const char *value = tai_map_get(&node->attributes, name);
    JS_FreeCString(js->context, name);
    return JS_NewString(js->context, value ? value : "");
}

static JSValue op_set_attribute(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    JSContext *context = js->context;
    TaiNode *node;
    if (!element_arg(js, argv[0], &node, "setAttribute can only be used on an Element"))
        return JS_EXCEPTION;
    char *name = python_str(context, argv[1]);
    char *value = name ? python_str(context, argv[2]) : NULL;
    char *folded = value ? casefold(name) : NULL;
    JSValue result = JS_UNDEFINED;
    if (!value) {
        result = JS_EXCEPTION;
    } else if (!folded || !tai_map_set(&node->attributes, folded, value, 0)) {
        result = JS_ThrowOutOfMemory(context);
    } else {
        result = after_mutation(js, !strcmp(folded, "id"));
    }
    free(folded);
    free(value);
    free(name);
    return result;
}

static JSValue op_children(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    JSContext *context = js->context;
    TaiNode *node;
    if (!node_arg(js, argv[0], &node)) return JS_EXCEPTION;
    JSValue array = JS_NewArray(context);
    if (JS_IsException(array)) return array;
    uint32_t count = 0;
    for (size_t index = 0; index < node->child_count; index++) {
        TaiNode *child = node->children[index];
        if (child->kind != TAI_ELEMENT) continue;
        JSValue handle = handle_value(js, child);
        if (JS_IsException(handle) ||
            JS_SetPropertyUint32(context, array, count++, handle) < 0) {
            JS_FreeValue(context, array);
            return JS_EXCEPTION;
        }
    }
    return array;
}

static JSValue op_create_element(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    char *tag = python_str(js->context, argv[0]);
    if (!tag) return JS_EXCEPTION;
    char *folded = casefold(tag);
    free(tag);
    if (!folded) return JS_ThrowOutOfMemory(js->context);
    TaiDomStatus status;
    TaiNode *node = tai_document_create_element(js->root->document, folded, &status);
    free(folded);
    if (!node) return throw_dom_status(js->context, status);
    return handle_value(js, node);
}

static JSValue insert(TaiJsContext *js, TaiNode *parent, TaiNode *child,
                      TaiNode *reference) {
    bool was_attached = child->parent != NULL, changed = false;
    TaiDomStatus status = tai_node_insert_before(parent, child, reference, &changed);
    if (status != TAI_DOM_OK) return throw_dom_status(js->context, status);
    if (!changed) return JS_UNDEFINED; /* insertBefore(x, x): Python returns early */
    if (was_attached) node_removed(js, child);
    return after_mutation(js, true);
}

static JSValue op_append_child(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    TaiNode *parent, *child;
    if (!node_arg(js, argv[0], &parent) || !node_arg(js, argv[1], &child))
        return JS_EXCEPTION;
    return insert(js, parent, child, NULL);
}

static JSValue op_insert_before(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    TaiNode *parent, *child, *reference = NULL;
    if (!node_arg(js, argv[0], &parent) || !node_arg(js, argv[1], &child))
        return JS_EXCEPTION;
    /* runtime.js passes null for a null reference: Python appends. */
    if (!JS_IsNull(argv[2]) && !node_arg(js, argv[2], &reference))
        return JS_EXCEPTION;
    return insert(js, parent, child, reference);
}

static JSValue op_remove_child(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    TaiNode *parent, *child;
    if (!node_arg(js, argv[0], &parent) || !node_arg(js, argv[1], &child))
        return JS_EXCEPTION;
    TaiDomStatus status = tai_node_remove_child(parent, child);
    if (status != TAI_DOM_OK) return throw_dom_status(js->context, status);
    node_removed(js, child);
    return after_mutation(js, true);
}

static JSValue serialized(TaiJsContext *js, JSValueConst handle, bool outer) {
    TaiNode *node;
    if (!node_arg(js, handle, &node)) return JS_EXCEPTION;
    char *html = tai_node_serialize(node, outer);
    if (!html) return JS_ThrowOutOfMemory(js->context);
    JSValue result = JS_NewString(js->context, html);
    free(html);
    return result;
}

static JSValue op_inner_html_get(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    return serialized(js, argv[0], false);
}

static JSValue op_outer_html_get(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    return serialized(js, argv[0], true);
}

/* Python innerHTML_set. runtime.js has already called s.toString(), so a
 * null value threw its TypeError before reaching here. */
static JSValue op_inner_html_set(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    JSContext *context = js->context;
    TaiNode *node;
    if (!node_arg(js, argv[0], &node)) return JS_EXCEPTION;
    const char *html = JS_ToCString(context, argv[1]);
    if (!html) return JS_EXCEPTION;
    TaiNode **removed = NULL;
    size_t removed_count = 0;
    TaiDomStatus status =
        tai_node_set_inner_html(node, html, &removed, &removed_count);
    JS_FreeCString(context, html);
    if (status != TAI_DOM_OK) return throw_dom_status(context, status);
    for (size_t index = 0; index < removed_count; index++)
        node_removed(js, removed[index]);
    free(removed);
    return after_mutation(js, true);
}

static JSValue host_failure(JSContext *context, TaiJsHostStatus status,
                            char *message, const char *fallback) {
    JSValue thrown = status == TAI_JS_HOST_NO_MEMORY
        ? JS_ThrowOutOfMemory(context)
        : JS_ThrowPlainError(context, "%s", message ? message : fallback);
    free(message);
    return thrown;
}

/* Python document_cookie_get: the host applies the per-host jar rules. */
static JSValue op_cookie_get(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    (void)argv;
    if (!js->host.cookie_get) return JS_NewString(js->context, "");
    char *value = NULL;
    TaiJsHostStatus status = js->host.cookie_get(js->host.userdata, &value);
    if (status != TAI_JS_HOST_OK || !value) {
        free(value);
        return host_failure(js->context, status == TAI_JS_HOST_OK
                                ? TAI_JS_HOST_NO_MEMORY : status,
                            NULL, "document.cookie failed");
    }
    JSValue result = JS_NewString(js->context, value);
    free(value);
    return result;
}

/* runtime.js has already called value.toString(). */
static JSValue op_cookie_set(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    if (!js->host.cookie_set) return JS_UNDEFINED;
    const char *value = JS_ToCString(js->context, argv[0]);
    if (!value) return JS_EXCEPTION;
    TaiJsHostStatus status = js->host.cookie_set(js->host.userdata, value);
    JS_FreeCString(js->context, value);
    if (status != TAI_JS_HOST_OK)
        return host_failure(js->context, status, NULL, "document.cookie failed");
    return JS_UNDEFINED;
}

/* D5: the asynchronous branch of Python XMLHttpRequest_send. CSP (and any
 * other failure to start) throws at send(); the result arrives later through
 * tai_js_finish_xhr. */
static JSValue xhr_start(TaiJsContext *js, const char *url, const char *body,
                         JSValueConst handle_value) {
    JSContext *context = js->context;
    int64_t handle;
    if (!JS_IsNumber(handle_value) ||
        JS_ToInt64(context, &handle, handle_value) < 0 || handle < 0)
        return JS_ThrowPlainError(context, "XMLHttpRequest handle is invalid");
    if (!js->host.xhr_start)
        return JS_ThrowPlainError(context, "XMLHttpRequest is not available");
    char *message = NULL;
    TaiJsHostStatus status = js->host.xhr_start(js->host.userdata, url, body,
                                                (uint64_t)handle, &message);
    if (status == TAI_JS_HOST_OK) {
        free(message);
        return JS_UNDEFINED;
    }
    return host_failure(context, status, message, "XMLHttpRequest failed");
}

/* Python XMLHttpRequest_send(method, url, body): method is only a label. A
 * non-string URL (open() never called) or body fails in Python's request. */
static JSValue op_xhr_send(TaiJsContext *js, int argc, JSValueConst *argv) {
    JSContext *context = js->context;
    bool asynchronous = argc > 3 && JS_ToBool(context, argv[3]) > 0;
    if (!asynchronous && !js->host.xhr_send)
        return JS_ThrowPlainError(context, "XMLHttpRequest is not available");
    if (!JS_IsString(argv[1]))
        return JS_ThrowPlainError(context, "XMLHttpRequest URL must be a string");
    if (!JS_IsNull(argv[2]) && !JS_IsString(argv[2]))
        return JS_ThrowPlainError(context,
                                  "XMLHttpRequest body must be a string or null");
    const char *url = JS_ToCString(context, argv[1]);
    const char *body = url && JS_IsString(argv[2])
        ? JS_ToCString(context, argv[2]) : NULL;
    if (!url || (JS_IsString(argv[2]) && !body)) {
        JS_FreeCString(context, url);
        return JS_EXCEPTION;
    }
    if (asynchronous) {
        JSValue started = xhr_start(js, url, body, argv[4]);
        JS_FreeCString(context, body);
        JS_FreeCString(context, url);
        return started;
    }
    char *response = NULL, *message = NULL;
    double started = seconds_now();
    TaiJsHostStatus status = js->host.xhr_send(js->host.userdata, url, body,
                                               &response, &message);
    /* Blocking on the network does not count against the script limit, up
     * to 30 s per outermost entry so a retry loop still ends. */
    double blocked = seconds_now() - started;
    if (blocked > 30.0 - js->extended) blocked = 30.0 - js->extended;
    if (blocked > 0.0 && js->deadline > 0.0) {
        js->deadline += blocked;
        js->extended += blocked;
    }
    JS_FreeCString(context, body);
    JS_FreeCString(context, url);
    /* QuickJS polls the interrupt handler only every few thousand
     * operations, which a loop of slow requests may take minutes to reach:
     * check the deadline and cancellation now, as that interrupt would. */
    if (interrupt_handler(js->runtime, js)) {
        free(response);
        free(message);
        JS_ThrowInternalError(context, "interrupted");
        JSValue interrupted = JS_GetException(context);
        JS_SetUncatchableError(context, interrupted);
        return JS_Throw(context, interrupted);
    }
    if (status == TAI_JS_HOST_OK && response) {
        free(message);
        JSValue result = JS_NewString(context, response);
        free(response);
        return result;
    }
    free(response);
    return host_failure(context, status == TAI_JS_HOST_OK
                            ? TAI_JS_HOST_NO_MEMORY : status,
                        message, "XMLHttpRequest failed");
}

/* ---- timers (D5) ------------------------------------------------------------ */

/* dukpy hands Python a JSON round trip of each argument: NaN, the
 * infinities, functions and undefined arrive as None. Returns the parsed
 * primitive, JS_NULL for None, or JS_EXCEPTION (a cyclic object also throws
 * in dukpy). */
static JSValue python_argument(JSContext *context, JSValueConst value) {
    JSValue json = JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json) || JS_IsUndefined(json)) {
        return JS_IsException(json) ? json : JS_NULL;
    }
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, json);
    JS_FreeValue(context, json);
    if (!text) return JS_EXCEPTION;
    JSValue parsed = JS_ParseJSON(context, text, length, "<argument>");
    JS_FreeCString(context, text);
    return parsed;
}

/* Python str.strip() whitespace. */
static bool python_space(utf8proc_int32_t c) {
    if (c == ' ' || (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f) ||
        c == 0x85 || c == 0x2028 || c == 0x2029)
        return true;
    return c > 0x7f && utf8proc_category(c) == UTF8PROC_CATEGORY_ZS;
}

/* The text between Python whitespace, as [*start, *end). */
static void python_strip(const char *text, const char **start,
                         const char **end) {
    const utf8proc_uint8_t *bytes = (const utf8proc_uint8_t *)text;
    utf8proc_ssize_t length = (utf8proc_ssize_t)strlen(text), at = 0;
    *start = *end = text;
    bool seen = false;
    while (at < length) {
        utf8proc_int32_t c;
        utf8proc_ssize_t size = utf8proc_iterate(bytes + at, length - at, &c);
        if (size <= 0) size = 1, c = -1;
        if (c < 0 || !python_space(c)) {
            if (!seen) *start = text + at;
            seen = true;
            *end = text + at + size;
        }
        at += size;
    }
}

/* Digits with single underscores between them (Python's digitpart); appends
 * the digits to *out. */
static bool digit_part(const char **cursor, const char *end, char **out) {
    const char *at = *cursor;
    if (at >= end || *at < '0' || *at > '9') return false;
    while (at < end) {
        if (*at >= '0' && *at <= '9') {
            *(*out)++ = *at++;
        } else if (*at == '_' && at + 1 < end && at[1] >= '0' && at[1] <= '9') {
            at++;
        } else {
            break;
        }
    }
    *cursor = at;
    return true;
}

static bool ascii_equal_nocase(const char *text, size_t length,
                               const char *word) {
    if (strlen(word) != length) return false;
    for (size_t index = 0; index < length; index++) {
        char c = text[index];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != word[index]) return false;
    }
    return true;
}

/* Python float(str); false for a ValueError. Non-decimal digits that
 * Python also accepts are not supported. */
static bool python_float_text(const char *text, double *out) {
    const char *at, *end;
    python_strip(text, &at, &end);
    size_t length = (size_t)(end - at);
    char *copy = malloc(length + 2), *write = copy;
    if (!copy) return false;
    const char *body = at;
    if (body < end && (*body == '+' || *body == '-')) *write++ = *body++;
    bool ok;
    if (ascii_equal_nocase(body, (size_t)(end - body), "inf") ||
        ascii_equal_nocase(body, (size_t)(end - body), "infinity") ||
        ascii_equal_nocase(body, (size_t)(end - body), "nan")) {
        *out = ascii_equal_nocase(body, (size_t)(end - body), "nan")
            ? NAN : (copy[0] == '-' && write > copy ? -INFINITY : INFINITY);
        free(copy);
        return true;
    }
    bool whole = digit_part(&body, end, &write);
    bool fraction = false;
    if (body < end && *body == '.') {
        *write++ = *body++;
        fraction = digit_part(&body, end, &write);
    }
    ok = whole || fraction;
    if (ok && body < end && (*body == 'e' || *body == 'E')) {
        *write++ = *body++;
        if (body < end && (*body == '+' || *body == '-')) *write++ = *body++;
        ok = digit_part(&body, end, &write);
    }
    ok = ok && body == end;
    *write = '\0';
    if (ok) *out = strtod(copy, NULL);
    free(copy);
    return ok;
}

/* Python JSContext._timer_delay_seconds of a dukpy argument. Leaves a
 * pending exception and returns false only when the argument cannot be
 * converted at all. */
static bool timer_delay(JSContext *context, JSValueConst value, double *seconds) {
    JSValue argument = python_argument(context, value);
    if (JS_IsException(argument)) return false;
    double milliseconds = 0.0;
    if (JS_IsBool(argument)) {
        milliseconds = JS_ToBool(context, argument) ? 1.0 : 0.0;
    } else if (JS_IsNumber(argument)) {
        if (JS_ToFloat64(context, &milliseconds, argument) < 0)
            milliseconds = 0.0;
    } else if (JS_IsString(argument)) {
        const char *text = JS_ToCString(context, argument);
        if (!text) {
            JS_FreeValue(context, argument);
            return false;
        }
        if (!python_float_text(text, &milliseconds)) milliseconds = 0.0;
        JS_FreeCString(context, text);
    }
    JS_FreeValue(context, argument);
    if (!isfinite(milliseconds) || milliseconds < 0.0) milliseconds = 0.0;
    *seconds = milliseconds / 1000.0;
    return true;
}

/* Python int() of a dukpy argument, for clearInterval: false (and no
 * exception) for None or a TypeError/ValueError, and for a value outside
 * int64_t, which names no timer. */
static bool python_int(JSContext *context, JSValueConst value, int64_t *out,
                       bool *failed) {
    *failed = false;
    JSValue argument = python_argument(context, value);
    if (JS_IsException(argument)) {
        *failed = true;
        return false;
    }
    bool ok = false;
    if (JS_IsBool(argument)) {
        *out = JS_ToBool(context, argument);
        ok = true;
    } else if (JS_IsNumber(argument)) {
        double number = 0.0;
        ok = JS_ToFloat64(context, &number, argument) == 0 && isfinite(number) &&
             fabs(number) < 9.2e18;
        if (ok) *out = (int64_t)trunc(number);
    } else if (JS_IsString(argument)) {
        const char *text = JS_ToCString(context, argument);
        if (!text) {
            *failed = true;
        } else {
            const char *at, *end;
            python_strip(text, &at, &end);
            size_t length = (size_t)(end - at);
            char *digits = malloc(length + 2), *write = digits;
            if (!digits) {
                JS_ThrowOutOfMemory(context);
                *failed = true;
            } else {
                if (at < end && (*at == '+' || *at == '-')) *write++ = *at++;
                ok = digit_part(&at, end, &write) && at == end;
                *write = '\0';
                if (ok) {
                    errno = 0;
                    long long parsed = strtoll(digits, NULL, 10);
                    ok = errno == 0;
                    *out = parsed;
                }
                free(digits);
            }
            JS_FreeCString(context, text);
        }
    }
    JS_FreeValue(context, argument);
    return ok;
}

static JSValue arm_timer(TaiJsContext *js, JSValueConst handle_value,
                         JSValueConst delay_value, bool interval) {
    JSContext *context = js->context;
    int64_t handle;
    if (!JS_IsNumber(handle_value) ||
        JS_ToInt64(context, &handle, handle_value) < 0)
        return JS_ThrowPlainError(context, "timer handle is invalid");
    double delay;
    if (!timer_delay(context, delay_value, &delay)) return JS_EXCEPTION;
    /* Python: a 0 ms repeating worker would spin, so intervals wait 1 ms. */
    if (interval && delay < 0.001) delay = 0.001;
    if (!grow_array((void **)&js->timers, &js->timer_capacity,
                    js->timer_count + 1, sizeof(*js->timers)))
        return JS_ThrowOutOfMemory(context);
    /* Python replaces (and stops) an interval armed again under its handle. */
    for (size_t index = 0; interval && index < js->timer_count; index++)
        if (js->timers[index].interval && js->timers[index].handle == handle) {
            js->timers[index] = js->timers[--js->timer_count];
            break;
        }
    js->timers[js->timer_count++] = (Timer){
        .handle = handle, .interval = interval,
        .due = timer_now(js) + delay, .period = delay,
        .order = js->timer_order++};
    return JS_UNDEFINED;
}

static JSValue op_set_timeout(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    return arm_timer(js, argv[0], argv[1], false);
}

static JSValue op_set_interval(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    return arm_timer(js, argv[0], argv[1], true);
}

/* Python clearInterval: int() of the handle stops future ticks; a value
 * int() rejects is ignored. js_scheduling.js has dropped the callback. */
static JSValue op_clear_interval(TaiJsContext *js, int argc, JSValueConst *argv) {
    (void)argc;
    int64_t handle;
    bool failed;
    if (!python_int(js->context, argv[0], &handle, &failed))
        return failed ? JS_EXCEPTION : JS_UNDEFINED;
    for (size_t index = 0; index < js->timer_count; index++)
        if (js->timers[index].interval && js->timers[index].handle == handle) {
            js->timers[index] = js->timers[--js->timer_count];
            break;
        }
    return JS_UNDEFINED;
}

static const struct {
    const char *name;
    int arguments; /* after the operation name */
    Operation run;
} operations[] = {
    {"log", 0, op_log},
    {"listener_error", 1, op_listener_error},
    {"raf_error", 1, op_raf_error},
    {"requestAnimationFrame", 0, op_request_animation_frame},
    {"querySelectorAll", 1, op_query_selector_all},
    {"getAttribute", 2, op_get_attribute},
    {"setAttribute", 3, op_set_attribute},
    {"children", 1, op_children},
    {"createElement", 1, op_create_element},
    {"appendChild", 2, op_append_child},
    {"insertBefore", 3, op_insert_before},
    {"removeChild", 2, op_remove_child},
    {"innerHTML_get", 1, op_inner_html_get},
    {"innerHTML_set", 2, op_inner_html_set},
    {"outerHTML_get", 1, op_outer_html_get},
    {"document_cookie_get", 0, op_cookie_get},
    {"document_cookie_set", 1, op_cookie_set},
    {"XMLHttpRequest_send", 5, op_xhr_send},
    {"setTimeout", 2, op_set_timeout},
    {"setInterval", 2, op_set_interval},
    {"clearInterval", 1, op_clear_interval},
};

static JSValue call_python(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv) {
    (void)this_value;
    TaiJsContext *js = JS_GetContextOpaque(context);
    if (!js || argc < 1) return JS_ThrowTypeError(context, "missing bridge operation");
    const char *name = JS_ToCString(context, argv[0]);
    if (!name) return JS_EXCEPTION;
    JSValue result = JS_UNDEFINED;
    bool found = false;
    for (size_t index = 0; index < sizeof(operations) / sizeof(*operations); index++) {
        if (strcmp(name, operations[index].name)) continue;
        found = true;
        /* Missing arguments read as undefined, as in dukpy. */
        JSValueConst arguments[5] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED,
                                     JS_UNDEFINED, JS_UNDEFINED};
        for (int i = 1; i < argc && i <= 5; i++) arguments[i - 1] = argv[i];
        result = operations[index].run(js, argc - 1, arguments);
        break;
    }
    if (!found)
        result = JS_ThrowPlainError(context, "%s is not supported yet", name);
    JS_FreeCString(context, name);
    return result;
}

/* ---- context lifecycle -------------------------------------------------------- */

static bool evaluate(TaiJsContext *js, const char *name, const char *code,
                     char **result, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (result) *result = NULL;
    enter_js(js);
    JSValue value = JS_Eval(js->context, code, strlen(code), name, JS_EVAL_TYPE_GLOBAL);
    leave_js(js);
    if (JS_IsException(value)) return exception_error(js, error);
    bool ok = true;
    if (result) {
        const char *text = JS_ToCString(js->context, value);
        *result = text ? tai_strdup(text) : NULL;
        JS_FreeCString(js->context, text);
        if (!*result) ok = text ? set_error(error, "JavaScript result allocation failed")
                                : exception_error(js, error);
    }
    JS_FreeValue(js->context, value);
    return ok;
}

TaiJsContext *tai_js_create(TaiNode *root, const TaiJsHost *host, char **error) {
    if (!root || !root->document) { set_error(error, "missing JavaScript document"); return NULL; }
    TaiJsContext *js = calloc(1, sizeof(*js));
    if (!js) { set_error(error, "JavaScript allocation failed"); return NULL; }
    js->root = root;
    if (host) js->host = *host;
    js->runtime = JS_NewRuntime();
    if (!js->runtime) goto fail;
    JS_SetMemoryLimit(js->runtime, 64U * 1024U * 1024U);
    JS_SetMaxStackSize(js->runtime, 512U * 1024U);
    JS_SetInterruptHandler(js->runtime, interrupt_handler, js);
    js->context = JS_NewContext(js->runtime);
    if (!js->context) goto fail;
    JS_SetContextOpaque(js->context, js);
    JSValue global = JS_GetGlobalObject(js->context);
    bool bound = JS_SetPropertyStr(js->context, global, "call_python",
        JS_NewCFunction(js->context, call_python, "call_python", 1)) >= 0;
    JS_FreeValue(js->context, global);
    if (!bound) goto fail;
    if (!evaluate(js, "runtime.js", (const char *)tai_runtime_js, NULL, error) ||
        !evaluate(js, "js_prelude.js", (const char *)tai_prelude_js, NULL, error) ||
        !evaluate(js, "js_scheduling.js", (const char *)tai_scheduling_js, NULL,
                  error))
        goto destroy;
    enter_js(js);
    bool synced = sync_id_globals(js);
    leave_js(js);
    if (!synced) {
        exception_error(js, error);
        goto destroy;
    }
    return js;
fail:
    set_error(error, "JavaScript allocation failed");
destroy:
    tai_js_destroy(js);
    return NULL;
}

void tai_js_destroy(TaiJsContext *js) {
    if (!js) return;
    if (js->context) JS_FreeContext(js->context);
    if (js->runtime) JS_FreeRuntime(js->runtime);
    free(js->handle_nodes);
    free(js->node_handles);
    free(js->timers);
    free(js);
}

bool tai_js_eval_value(TaiJsContext *js, const char *name, const char *code,
                       char **result, char **error) {
    if (result) *result = NULL;
    if (!js || !code) return set_error(error, "missing JavaScript input");
    return evaluate(js, name ? name : "script", code, result, error);
}

bool tai_js_eval(TaiJsContext *js, const char *name, const char *code,
                 char **error) {
    return tai_js_eval_value(js, name, code, NULL, error);
}

bool tai_js_dispatch_event(TaiJsContext *js, const char *type, TaiNode *target,
                           bool *default_prevented, char **error) {
    if (default_prevented) *default_prevented = false;
    if (!js || !type || !target) return set_error(error, "missing event input");
    JSContext *context = js->context;
    JSValue global = JS_GetGlobalObject(context);
    JSValue function = JS_GetPropertyStr(context, global, "dispatch_event_path");
    JSValue arguments[2] = {JS_NewString(context, type), JS_NewArray(context)};
    bool built = !JS_IsException(function) && !JS_IsException(arguments[0]) &&
                 !JS_IsException(arguments[1]);
    /* Python collects the element ancestors (assigning handles) up front, so
     * nodes detached by a listener still receive the bubbling event. */
    uint32_t index = 0;
    for (TaiNode *node = target; built && node; node = node->parent) {
        if (node->kind != TAI_ELEMENT) continue;
        JSValue handle = handle_value(js, node);
        if (JS_IsException(handle) ||
            JS_SetPropertyUint32(context, arguments[1], index++, handle) < 0)
            built = false;
    }
    JSValue result = JS_UNDEFINED;
    if (built) {
        enter_js(js);
        result = JS_Call(context, function, global, 2, arguments);
        leave_js(js);
    }
    JS_FreeValue(context, arguments[0]);
    JS_FreeValue(context, arguments[1]);
    JS_FreeValue(context, function);
    JS_FreeValue(context, global);
    if (!built) return exception_error(js, error);

    /* Listener exceptions are caught per listener in JS. What still escapes
     * (the deadline interrupt, out of memory) aborts this dispatch; Python
     * reports "Event <type> crashed" and runs the default action. */
    int do_default = 1;
    if (!JS_IsException(result)) do_default = JS_ToBool(context, result);
    if (JS_IsException(result) || do_default < 0) {
        char *text = take_exception_text(context);
        report_event_error(js, type, text);
        free(text);
        do_default = 1;
    }
    JS_FreeValue(context, result);
    if (default_prevented) *default_prevented = !do_default;
    return true;
}

bool tai_js_run_animation_frame(TaiJsContext *js, char **error) {
    if (!js) return set_error(error, "missing animation frame input");
    /* Python Tab.run_animation_frame evaluates RAF_JS and reports whatever
     * escapes it as "requestAnimationFrame callback crashed". */
    static const char code[] = "runRAFHandlers()";
    enter_js(js);
    JSValue value = JS_Eval(js->context, code, sizeof(code) - 1, "raf.js",
                            JS_EVAL_TYPE_GLOBAL);
    leave_js(js);
    if (JS_IsException(value)) {
        char *text = take_exception_text(js->context);
        report_raf_error(js, text);
        free(text);
    }
    JS_FreeValue(js->context, value);
    return true;
}

/* Calls the global function name(arguments...) as one outermost task and
 * reports what escapes it as "<prefix> <error>", like Python's task
 * wrappers. The arguments are freed. */
static void run_task(TaiJsContext *js, const char *name, const char *prefix,
                     int argc, JSValue *argv) {
    JSContext *context = js->context;
    JSValue global = JS_GetGlobalObject(context);
    JSValue function = JS_GetPropertyStr(context, global, name);
    bool built = !JS_IsException(function);
    for (int index = 0; index < argc; index++)
        built = built && !JS_IsException(argv[index]);
    JSValue result = JS_EXCEPTION;
    if (built) {
        enter_js(js);
        result = JS_Call(context, function, global, argc, (JSValueConst *)argv);
        leave_js(js);
    }
    if (JS_IsException(result)) {
        char *text = take_exception_text(context);
        report_task_error(js, prefix, text);
        free(text);
    }
    JS_FreeValue(context, result);
    for (int index = 0; index < argc; index++) JS_FreeValue(context, argv[index]);
    JS_FreeValue(context, function);
    JS_FreeValue(context, global);
}

static bool timer_before(const Timer *left, const Timer *right) {
    return left->due < right->due ||
           (left->due == right->due && left->order < right->order);
}

/* The earliest timer, or SIZE_MAX without timers. */
static size_t next_due_timer(const TaiJsContext *js) {
    size_t best = SIZE_MAX;
    for (size_t index = 0; index < js->timer_count; index++)
        if (best == SIZE_MAX || timer_before(&js->timers[index], &js->timers[best]))
            best = index;
    return best;
}

double tai_js_next_timer(const TaiJsContext *js) {
    size_t index = js ? next_due_timer(js) : SIZE_MAX;
    return index == SIZE_MAX ? INFINITY : js->timers[index].due;
}

bool tai_js_run_timers(TaiJsContext *js, double now, size_t budget,
                       size_t *ran, char **error) {
    if (ran) *ran = 0;
    if (!js || isnan(now)) return set_error(error, "missing timer input");
    for (size_t count = 0; count < budget; count++) {
        size_t index = next_due_timer(js);
        if (index == SIZE_MAX || js->timers[index].due > now) break;
        Timer *timer = &js->timers[index];
        int64_t handle = timer->handle;
        bool interval = timer->interval;
        if (interval) {
            /* Python's worker re-arms on the ideal timeline before the
             * queued tick runs. */
            timer->due += timer->period;
            timer->order = js->timer_order++;
        } else {
            *timer = js->timers[--js->timer_count];
        }
        JSValue argument = JS_NewInt64(js->context, handle);
        if (interval)
            run_task(js, "runSetInterval", "setInterval callback crashed", 1,
                     &argument);
        else
            run_task(js, "runSetTimeout", "setTimeout callback crashed", 1,
                     &argument);
        if (ran) (*ran)++;
    }
    return true;
}

bool tai_js_finish_xhr(TaiJsContext *js, uint64_t handle, const char *body,
                       const char *message, char **error) {
    if (!js || handle > (uint64_t)INT64_MAX)
        return set_error(error, "missing XMLHttpRequest input");
    JSContext *context = js->context;
    if (!body) {
        /* Python prints the failure from the network thread; the JS object
         * never hears of it. */
        report_task_error(js, "Async XMLHttpRequest failed",
                          message ? message : "XMLHttpRequest failed");
        JSValue argument = JS_NewInt64(context, (int64_t)handle);
        run_task(js, "dropXHR", "XMLHttpRequest onload crashed", 1, &argument);
        return true;
    }
    JSValue arguments[2] = {JS_NewString(context, body),
                            JS_NewInt64(context, (int64_t)handle)};
    run_task(js, "runXHROnload", "XMLHttpRequest onload crashed", 2, arguments);
    return true;
}

size_t tai_js_set_memory_limit_for_test(TaiJsContext *js, size_t limit) {
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(js->runtime, &usage);
    JS_SetMemoryLimit(js->runtime, limit);
    return (size_t)usage.malloc_size;
}
