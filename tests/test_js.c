#include "tai/js.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void invalidated(void *opaque) { (*(int *)opaque)++; }

static TaiNode *find_element(TaiNode *node, const char *tag) {
    if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
    for (size_t i = 0; i < node->child_count; i++) {
        TaiNode *found = find_element(node->children[i], tag);
        if (found) return found;
    }
    return NULL;
}

int main(void) {
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
