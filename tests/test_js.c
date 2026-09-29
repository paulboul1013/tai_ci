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
    TaiJsContext *js = tai_js_create(tai_document_root(doc), NULL, NULL, &error);
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
    TaiJsContext *js = tai_js_create(tai_document_root(doc), NULL, NULL, &error);
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

int main(void) {
    test_listener_errors();
    test_other_thread_stack();
    char *error = NULL;
    TaiDocument *doc = tai_html_parse(
        "<div id='box'><p class='item'>old</p></div>", &error);
    assert(doc && !error);
    int invalidations = 0;
    TaiJsContext *js = tai_js_create(tai_document_root(doc), invalidated,
                                     &invalidations, &error);
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
