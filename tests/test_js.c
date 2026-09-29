#define _POSIX_C_SOURCE 200809L
#include "tai/js.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void invalidated(void *opaque) { (*(int *)opaque)++; }

static TaiNode *find_element(TaiNode *node, const char *tag) {
    if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
    for (size_t i = 0; i < node->child_count; i++) {
        TaiNode *found = find_element(node->children[i], tag);
        if (found) return found;
    }
    return NULL;
}

/* Captures what the JS layer writes to stderr between begin and end. */
typedef struct {
    int saved;
    FILE *file;
} StderrCapture;

static void capture_begin(StderrCapture *capture) {
    fflush(stderr);
    capture->file = tmpfile();
    assert(capture->file);
    capture->saved = dup(STDERR_FILENO);
    assert(capture->saved >= 0);
    assert(dup2(fileno(capture->file), STDERR_FILENO) >= 0);
}

static char *capture_end(StderrCapture *capture) {
    fflush(stderr);
    assert(dup2(capture->saved, STDERR_FILENO) >= 0);
    close(capture->saved);
    long size = ftell(capture->file);
    assert(size >= 0);
    char *text = calloc((size_t)size + 1, 1);
    assert(text);
    rewind(capture->file);
    assert(fread(text, 1, (size_t)size, capture->file) == (size_t)size);
    fclose(capture->file);
    return text;
}

static double seconds_now(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* D7: a throwing listener is reported and skipped; later listeners, bubbling
 * and an earlier preventDefault still apply. */
static void test_listener_errors(void) {
    char *error = NULL;
    TaiDocument *doc = tai_html_parse(
        "<div id=outer><button id=b>go</button></div>", &error);
    assert(doc && !error);
    TaiJsContext *js = tai_js_create(tai_document_root(doc), NULL, &error);
    assert(js && !error);
    assert(tai_js_eval(js, "listeners.js",
        "var seen=[];"
        "b.addEventListener('click',function(e){seen.push('first');"
        " e.preventDefault();});"
        "b.addEventListener('click',function(){throw Error('boom');});"
        "b.addEventListener('click',function(){seen.push('third');});"
        "outer.addEventListener('click',function(){seen.push('outer');"
        " outer.setAttribute('data-seen',seen.join(','));});"
        "b.addEventListener('keydown',function(){missing_function();});"
        "b.addEventListener('submit',function(){while(true){}});",
        &error));
    TaiNode *button = find_element(tai_document_root(doc), "button");
    TaiNode *outer = find_element(tai_document_root(doc), "div");

    StderrCapture capture;
    capture_begin(&capture);
    bool prevented = false;
    bool dispatched = tai_js_dispatch_event(js, "click", button, &prevented,
                                            &error);
    char *reported = capture_end(&capture);
    assert(dispatched && !error && prevented);
    assert(!strcmp(tai_map_get(&outer->attributes, "data-seen"),
                   "first,third,outer"));
    assert(strstr(reported, "Event click crashed Error: boom\n"));
    free(reported);

    capture_begin(&capture);
    prevented = true;
    dispatched = tai_js_dispatch_event(js, "keydown", button, &prevented,
                                       &error);
    reported = capture_end(&capture);
    assert(dispatched && !error && !prevented);
    assert(strstr(reported, "Event keydown crashed ReferenceError"));
    free(reported);

    /* The deadline interrupt cannot be caught per listener: it aborts the
     * dispatch, which reports and falls back to the default action. */
    capture_begin(&capture);
    prevented = true;
    double started = seconds_now();
    dispatched = tai_js_dispatch_event(js, "submit", button, &prevented,
                                       &error);
    double elapsed = seconds_now() - started;
    reported = capture_end(&capture);
    assert(dispatched && !error && !prevented);
    assert(elapsed >= 1.5 && elapsed < 10.0);
    assert(strstr(reported, "Event submit crashed"));
    free(reported);

    /* The context stays usable after an interrupted dispatch. */
    assert(tai_js_eval(js, "after.js", "seen.length", &error));
    tai_js_destroy(js);
    tai_document_destroy(doc);
}

typedef struct {
    TaiJsContext *js;
    bool shallow_ok;
    bool deep_ok;
    char *deep_error;
} ThreadRun;

static void *run_on_other_thread(void *opaque) {
    ThreadRun *run = opaque;
    char *error = NULL;
    run->shallow_ok = tai_js_eval(run->js, "shallow.js", "1 + 1", &error);
    free(error);
    error = NULL;
    run->deep_ok = tai_js_eval(run->js, "deep.js",
        "function down(n){return down(n+1)+1;} down(0)", &error);
    run->deep_error = error;
    return NULL;
}

/* The runtime is created on the loader thread and used on the SDL thread;
 * stack overflow detection must follow the thread that runs the script. */
static void test_other_thread_stack(void) {
    char *error = NULL;
    TaiDocument *doc = tai_html_parse("<p>x</p>", &error);
    assert(doc && !error);
    TaiJsContext *js = tai_js_create(tai_document_root(doc), NULL, &error);
    assert(js && !error);
    ThreadRun run = {.js = js};
    pthread_attr_t attributes;
    assert(pthread_attr_init(&attributes) == 0);
    assert(pthread_attr_setstacksize(&attributes, 8U * 1024U * 1024U) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, &attributes, run_on_other_thread, &run) == 0);
    assert(pthread_join(thread, NULL) == 0);
    pthread_attr_destroy(&attributes);
    assert(run.shallow_ok);
    assert(!run.deep_ok && run.deep_error);
    assert(strstr(run.deep_error, "stack"));
    free(run.deep_error);
    tai_js_destroy(js);
    tai_document_destroy(doc);
}

typedef struct {
    size_t invalidations;
    size_t removed;
    TaiNode *last_removed;
} MutationLog;

static void count_invalidation(void *opaque) {
    ((MutationLog *)opaque)->invalidations++;
}

static void record_removal(void *opaque, TaiNode *node) {
    MutationLog *log = opaque;
    log->removed++;
    log->last_removed = node;
}

static void test_mutation_callbacks(void) {
    char *error = NULL;
    TaiDocument *doc = tai_html_parse(
        "<div id=one><b id=bold>b</b></div><div id=two></div>", &error);
    assert(doc && !error);
    MutationLog log = {0};
    TaiJsHost host = {.invalidated = count_invalidation,
                      .node_removed = record_removal, .userdata = &log};
    TaiJsContext *js = tai_js_create(tai_document_root(doc), &host, &error);
    assert(js && !error);
    TaiNode *bold = find_element(tai_document_root(doc), "b");
    /* Moving an attached node reports it; appending a new one does not. */
    assert(tai_js_eval(js, "move.js", "var keep = bold; two.appendChild(keep)", &error));
    assert(log.removed == 1 && log.last_removed == bold && log.invalidations == 1);
    assert(tai_js_eval(js, "add.js",
        "var n = document.createElement('i'); one.appendChild(n)", &error));
    assert(log.removed == 1 && log.invalidations == 2);
    assert(tai_js_eval(js, "remove.js", "two.removeChild(keep)", &error));
    assert(log.removed == 2 && log.last_removed == bold && !bold->parent);
    assert(log.invalidations == 3);
    /* insertBefore(x, x) is a no-op in Python: no callbacks at all. */
    assert(tai_js_eval(js, "noop.js", "one.insertBefore(n, n)", &error));
    assert(log.removed == 2 && log.invalidations == 3);
    assert(!tai_js_eval(js, "fail.js", "one.removeChild(keep)", &error));
    assert(error && strstr(error, "Node is not a child of this parent"));
    free(error);
    error = NULL;
    assert(log.invalidations == 3);
    tai_js_destroy(js);
    tai_document_destroy(doc);
}

/* innerHTML = s reports every former child as removed, invalidates once and
 * resyncs the ID globals; the getters change nothing. */
static void test_inner_html(void) {
    char *error = NULL;
    TaiDocument *doc = tai_html_parse(
        "<div id=box><b id=old>b</b>text<i>i</i></div>", &error);
    assert(doc && !error);
    MutationLog log = {0};
    TaiJsHost host = {.invalidated = count_invalidation,
                      .node_removed = record_removal, .userdata = &log};
    TaiJsContext *js = tai_js_create(tai_document_root(doc), &host, &error);
    assert(js && !error);
    TaiNode *box = find_element(tai_document_root(doc), "div");
    TaiNode *italic = find_element(box, "i");
    char *result = NULL;
    assert(tai_js_eval_value(js, "get.js", "box.innerHTML + '|' + box.outerHTML",
                             &result, &error));
    assert(!strcmp(result, "<b id=\"old\">b</b>text<i>i</i>|"
                           "<div id=\"box\"><b id=\"old\">b</b>text<i>i</i></div>"));
    free(result);
    assert(log.invalidations == 0 && log.removed == 0);
    assert(tai_js_eval_value(js, "set.js",
        "var kept = old; box.innerHTML = '<p id=fresh>n</p>';"
        " [typeof old, fresh.outerHTML, kept.outerHTML].join('|')",
        &result, &error));
    assert(!strcmp(result, "undefined|<p id=\"fresh\">n</p>|<b id=\"old\">b</b>"));
    free(result);
    assert(log.removed == 3 && log.last_removed == italic && !italic->parent);
    assert(log.invalidations == 1);
    assert(box->child_count == 1 && !strcmp(box->children[0]->tag, "p"));
    /* An emptied element reports nothing more to remove. */
    assert(tai_js_eval(js, "clear.js", "box.innerHTML = ''; box.innerHTML = ''",
                       &error));
    assert(log.removed == 4 && log.invalidations == 3 && !box->child_count);
    /* runtime.js calls s.toString(): null throws before reaching C. */
    assert(!tai_js_eval(js, "null.js", "box.innerHTML = null", &error));
    assert(error && strstr(error, "TypeError"));
    free(error);
    error = NULL;
    assert(log.invalidations == 3);
    /* Python's parser raises on this markup: a catchable plain Error. */
    assert(tai_js_eval_value(js, "parse.js",
        "try { box.innerHTML = '<i></div><b> </i><head></i></p><i><b><i><ul><i><i></i>';"
        " 'no error' } catch (e) { e.name + ': ' + e.message }", &result, &error));
    assert(!strcmp(result, "Error: HTML parsing failed: invalid parser stack"));
    free(result);
    assert(log.invalidations == 3 && log.removed == 4);
    assert(!tai_js_eval(js, "unknown.js", "new Node(999).innerHTML", &error));
    assert(error && strstr(error, "Unknown node handle"));
    free(error);
    error = NULL;

    /* D2: the parsed fragment counts toward the node limit; failing leaves
     * the element and the callbacks untouched. */
    assert(tai_js_eval(js, "fill.js", "box.innerHTML = '<u>keep</u>'", &error));
    size_t removed = log.removed, invalidations = log.invalidations;
    TaiDomStatus status = TAI_DOM_OK;
    while (tai_document_create_element(doc, "b", &status)) {}
    assert(status == TAI_DOM_NODE_LIMIT);
    assert(!tai_js_eval(js, "over.js", "box.innerHTML = 'x'", &error));
    assert(error && strstr(error, "Document node limit reached"));
    free(error);
    assert(log.removed == removed && log.invalidations == invalidations);
    assert(box->child_count == 1 && !strcmp(box->children[0]->tag, "u"));
    tai_js_destroy(js);
    tai_document_destroy(doc);
}

/* A bridge call that re-enters JS (the ID global resync) must not reset the
 * outer script's deadline. */
static void test_nested_deadline(void) {
    char *error = NULL;
    TaiDocument *doc = tai_html_parse("<p id=a>x</p>", &error);
    assert(doc && !error);
    TaiJsContext *js = tai_js_create(tai_document_root(doc), NULL, &error);
    assert(js && !error);
    StderrCapture capture;
    capture_begin(&capture);
    double started = seconds_now();
    bool ran = tai_js_eval(js, "loop.js",
        "var el = a; while (true) el.setAttribute('id', 'x');", &error);
    double elapsed = seconds_now() - started;
    char *reported = capture_end(&capture);
    assert(!ran && error && strstr(error, "interrupted"));
    assert(elapsed >= 1.5 && elapsed < 10.0);
    /* An interrupt landing inside the resync propagates to the script
     * instead of being reported and swallowed. */
    assert(!strstr(reported, "ID global sync failed"));
    free(reported);
    free(error);
    tai_js_destroy(js);
    tai_document_destroy(doc);
}

/* ID globals resync after every mutation by walking the whole document, as in
 * Python. 10,000 appends must fit in half of the 2 s script budget in the
 * Debug build; sanitizer builds run fewer and only check correctness. */
#if defined(__SANITIZE_ADDRESS__)
#define APPEND_COUNT "2000"
#define APPEND_TOTAL 2000
#define APPEND_LIMIT 2.0
#else
#define APPEND_COUNT "10000"
#define APPEND_TOTAL 10000
#define APPEND_LIMIT 1.0
#endif
static void test_append_cost(void) {
    char *error = NULL;
    TaiDocument *doc = tai_html_parse("<div id=root></div>", &error);
    assert(doc && !error);
    TaiJsContext *js = tai_js_create(tai_document_root(doc), NULL, &error);
    assert(js && !error);
    double started = seconds_now();
    assert(tai_js_eval(js, "append.js",
        "for (var i = 0; i < " APPEND_COUNT "; i++)"
        " root.appendChild(document.createElement('span'));", &error));
    double elapsed = seconds_now() - started;
    fprintf(stderr, APPEND_COUNT " appends: %.3f s\n", elapsed);
    assert(elapsed < APPEND_LIMIT);
    assert(find_element(tai_document_root(doc), "div")->child_count ==
           APPEND_TOTAL);
    tai_js_destroy(js);
    tai_document_destroy(doc);
}

/* D2: createElement fails with a JS error at the document node limit. */
static void test_node_limit(void) {
    char *error = NULL;
    TaiDocument *doc = tai_html_parse("<p>x</p>", &error);
    assert(doc && !error);
    TaiJsContext *js = tai_js_create(tai_document_root(doc), NULL, &error);
    assert(js && !error);
    TaiDomStatus status = TAI_DOM_OK;
    while (tai_document_node(doc, TAI_DOCUMENT_SCRIPT_NODE_LIMIT - 2) == NULL)
        assert(tai_document_create_element(doc, "b", &status));
    assert(tai_js_eval(js, "last.js", "var last = document.createElement('i')",
                       &error));
    assert(!tai_js_eval(js, "over.js", "document.createElement('i')", &error));
    assert(error && strstr(error, "Document node limit reached"));
    free(error);
    tai_js_destroy(js);
    tai_document_destroy(doc);
}

int main(void) {
    test_mutation_callbacks();
    test_inner_html();
    test_nested_deadline();
    test_append_cost();
    test_node_limit();
    test_listener_errors();
    test_other_thread_stack();
    char *error = NULL;
    TaiDocument *doc = tai_html_parse(
        "<div id='box'><p class='item'>old</p></div>", &error);
    assert(doc && !error);
    int invalidations = 0;
    TaiJsHost host = {.invalidated = invalidated, .userdata = &invalidations};
    TaiJsContext *js = tai_js_create(tai_document_root(doc), &host, &error);
    assert(js && !error);
    assert(tai_js_eval(js, "test.js",
        "var p=document.querySelectorAll('.item')[0];"
        "p.setAttribute('data-state','ready');"
        "box.setAttribute('aria-label','container');", &error));
    TaiNode *p = find_element(tai_document_root(doc), "p");
    TaiNode *div = find_element(tai_document_root(doc), "div");
    assert(!strcmp(tai_map_get(&p->attributes, "data-state"), "ready"));
    assert(!strcmp(tai_map_get(&div->attributes, "aria-label"), "container"));
    assert(invalidations == 2);
    bool prevented = false;
    assert(tai_js_eval(js, "listener.js",
        "p.addEventListener('click',function(e){"
        " this.setAttribute('clicked','yes'); e.preventDefault(); });", &error));
    assert(tai_js_dispatch_event(js, "click", p, &prevented, &error));
    assert(prevented);
    assert(!strcmp(tai_map_get(&p->attributes, "clicked"), "yes"));
    assert(!tai_js_eval(js, "bad.js", "throw Error('boom')", &error));
    assert(error && strstr(error, "boom"));
    free(error);
    tai_js_destroy(js);
    tai_document_destroy(doc);
    return 0;
}
