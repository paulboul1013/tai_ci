/* JS DOM bridge under allocation failure (slice 7).
 *
 * Linked with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc, so QuickJS and
 * the project code fail allocations deterministically. Each run creates the
 * document and context unarmed, then fails allocation N while running a
 * scenario that reaches every bridge operation: createElement, insertBefore,
 * appendChild, removeChild, setAttribute (ID-global sync), innerHTML and
 * outerHTML, document.cookie, synchronous and asynchronous XHR, an event
 * listener, requestAnimationFrame and timers. N counts up until a run injects
 * nothing. Three modes: every malloc from N on fails; only malloc N fails;
 * or the JS heap limit sits N bytes above its use, failing QuickJS's own
 * allocations one by one (outside ASan its arena hands out small blocks
 * without calling malloc, so the first two modes reach only the C side and
 * arena refills). The process must not crash, the tree must stay consistent,
 * and under ASan/LSan nothing may leak. */
#include "tai/dom.h"
#include "tai/js.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);

static long budget = -1; /* <0 disarmed; allocations left before failing */
static bool one_shot, injected;

static bool refuse(void) {
    if (budget < 0) return false;
    if (budget == 0) {
        injected = true;
        if (one_shot) budget = -1;
        return true;
    }
    budget--;
    return false;
}
void *__wrap_malloc(size_t size) {
    return refuse() ? NULL : __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
    return refuse() ? NULL : __real_calloc(count, size);
}
void *__wrap_realloc(void *ptr, size_t size) {
    return refuse() ? NULL : __real_realloc(ptr, size);
}

typedef enum { PERSISTENT, ONE_SHOT, JS_HEAP } Mode;

typedef struct {
    int problems; /* reports other than log() */
    char *cookie;
    uint64_t xhr_handle;
    bool xhr_started;
} Host;

static void quiet(void *userdata, const TaiJsReport *report) {
    Host *host = userdata;
    if (report->kind != TAI_JS_REPORT_LOG) host->problems++;
}
static TaiJsHostStatus cookie_get(void *userdata, char **value) {
    Host *host = userdata;
    const char *text = host->cookie ? host->cookie : "";
    *value = malloc(strlen(text) + 1);
    if (!*value) return TAI_JS_HOST_NO_MEMORY;
    strcpy(*value, text);
    return TAI_JS_HOST_OK;
}
static TaiJsHostStatus cookie_set(void *userdata, const char *value) {
    Host *host = userdata;
    char *copy = malloc(strlen(value) + 1);
    if (!copy) return TAI_JS_HOST_NO_MEMORY;
    strcpy(copy, value);
    free(host->cookie);
    host->cookie = copy;
    return TAI_JS_HOST_OK;
}
static TaiJsHostStatus xhr_send(void *userdata, const char *url,
                                const char *body, char **response,
                                char **message) {
    (void)userdata;
    (void)body;
    (void)message;
    *response = malloc(strlen(url) + 1);
    if (!*response) return TAI_JS_HOST_NO_MEMORY;
    strcpy(*response, url);
    return TAI_JS_HOST_OK;
}
static TaiJsHostStatus xhr_start(void *userdata, const char *url,
                                 const char *body, uint64_t handle,
                                 char **message) {
    (void)url;
    (void)body;
    (void)message;
    Host *host = userdata;
    host->xhr_handle = handle;
    host->xhr_started = true;
    return TAI_JS_HOST_OK;
}
static double clock_now(void *userdata) {
    (void)userdata;
    return 0.0;
}

static const char *const SCENARIO =
    "var box = document.querySelectorAll('div')[0];"
    "var made = document.createElement('section');"
    "made.setAttribute('id', 'made');"
    "box.appendChild(made);"
    "box.insertBefore(document.createElement('b'), made);"
    "made.innerHTML = '<p id=inner class=c>t<i>x</i></p><ul><li>1<li>2</ul>';"
    "var html = box.outerHTML + made.innerHTML;"
    "box.removeChild(made);"
    "box.appendChild(made);"
    "inner.setAttribute('id', 'renamed');"
    "document.cookie = 'a=1';"
    "var cookie = document.cookie;"
    "var sync = new XMLHttpRequest(); sync.open('GET', '/sync', false);"
    "sync.send(); var text = sync.responseText;"
    "var later = new XMLHttpRequest(); later.open('GET', '/async', true);"
    "later.onload = function () {"
    " made.setAttribute('loaded', later.responseText); };"
    "later.send();"
    "box.addEventListener('click', function (e) {"
    " made.innerHTML = '<u id=clicked>c</u>'; e.preventDefault(); });"
    "requestAnimationFrame(function () { made.setAttribute('frame', '1'); });"
    "setTimeout(function () {"
    " made.appendChild(document.createElement('em')); }, 0);"
    "var tick = setInterval(function () { clearInterval(tick); log('tick'); }, 0);"
    "log(html.length, cookie, text, typeof renamed);";

/* Every child points back at its parent, and no node is past the D13 cap. */
static void check_tree(const TaiNode *root) {
    typedef struct {
        const TaiNode *node;
        size_t depth;
    } Item;
    size_t capacity = 64, count = 0;
    Item *stack = __real_malloc(capacity * sizeof(*stack));
    assert(stack);
    stack[count++] = (Item){root, 0};
    while (count) {
        Item item = stack[--count];
        assert(item.depth <= TAI_DOM_MAX_DEPTH);
        for (size_t i = 0; i < item.node->child_count; i++) {
            const TaiNode *child = item.node->children[i];
            assert(child->parent == item.node);
            if (count == capacity) {
                capacity *= 2;
                stack = __real_realloc(stack, capacity * sizeof(*stack));
                assert(stack);
            }
            stack[count++] = (Item){child, item.depth + 1};
        }
    }
    free(stack);
}

static void drop(char **error, int *failures) {
    free(*error);
    *error = NULL;
    (*failures)++;
}

#define EXPECTED "1,done,1,a=1,undefined,3"

/* One run failing at point (never when point < 0); true if anything failed. */
static bool run(Mode mode, long point) {
    char *error = NULL;
    TaiDocument *doc = tai_html_parse("<div id=box><p>old</p></div>", &error);
    assert(doc && !error);
    Host state = {0};
    TaiJsHost host = {
        .report = quiet, .cookie_get = cookie_get, .cookie_set = cookie_set,
        .xhr_send = xhr_send, .xhr_start = xhr_start, .now = clock_now,
        .userdata = &state};
    TaiJsContext *js = tai_js_create(tai_document_root(doc), &host, &error);
    assert(js && !error);
    TaiNode *box = tai_document_root(doc)->children[0]->children[0];

    int failures = 0;
    injected = false;
    if (point >= 0 && mode == JS_HEAP) {
        size_t used = tai_js_set_memory_limit_for_test(js, 0);
        tai_js_set_memory_limit_for_test(js, used + (size_t)point);
    } else if (point >= 0) {
        one_shot = mode == ONE_SHOT;
        budget = point;
    }
    if (!tai_js_eval(js, "scenario.js", SCENARIO, &error))
        drop(&error, &failures);
    bool prevented = false;
    if (!tai_js_dispatch_event(js, "click", box, &prevented, &error))
        drop(&error, &failures);
    if (!tai_js_run_animation_frame(js, &error)) drop(&error, &failures);
    if (state.xhr_started &&
        !tai_js_finish_xhr(js, state.xhr_handle, "done", NULL, &error))
        drop(&error, &failures);
    if (!tai_js_run_timers(js, 1.0, 32, NULL, &error))
        drop(&error, &failures);
    budget = -1;
    tai_js_set_memory_limit_for_test(js, 0);
    char *result = NULL;
    if (!tai_js_eval_value(js, "after.js",
                           "[document.querySelectorAll('u').length,"
                           " made.getAttribute('loaded'),"
                           " made.getAttribute('frame'), document.cookie,"
                           " typeof renamed, box.children.length].join()",
                           &result, &error))
        drop(&error, &failures);
    bool hit = mode == JS_HEAP
                   ? failures || state.problems || !result ||
                         strcmp(result, EXPECTED)
                   : injected;
    /* The unarmed run reaches every step. */
    if (point < 0) {
        if (!result || strcmp(result, EXPECTED) || failures || state.problems)
            fprintf(stderr, "unarmed run: %s\n", result ? result : "(error)");
        assert(result && !strcmp(result, EXPECTED) && !failures &&
               !state.problems);
    }
    free(result);

    check_tree(tai_document_root(doc));
    tai_js_destroy(js);
    tai_document_destroy(doc);
    free(state.cookie);
    return hit;
}

int main(int argc, char **argv) {
    /* JS heap headroom grows by this many bytes per run (default 16, the
     * QuickJS allocation granularity). */
    long step = argc > 1 ? strtol(argv[1], NULL, 10) : 16;
    if (step < 1) step = 1;
    assert(!run(PERSISTENT, -1));
    static const char *const names[] = {"persistent", "one-shot", "js-heap"};
    for (Mode mode = PERSISTENT; mode <= JS_HEAP; mode++) {
        long point = 0, clean = 0, runs = 0, last_hit = 0;
        /* A JS heap run can fail without a visible symptom, so stop only
         * after 64 clean runs in a row. */
        long needed = mode == JS_HEAP ? 64 : 1;
        while (clean < needed) {
            if (run(mode, point)) {
                clean = 0;
                last_hit = point;
            } else
                clean++;
            runs++;
            point += mode == JS_HEAP ? step : 1;
        }
        fprintf(stderr, "%s: %ld runs, last failing point %ld\n", names[mode],
                runs, last_hit);
        assert(last_hit > 100);
    }
    return 0;
}
