#include "tai/dom.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
/* Linked with --wrap so allocations can fail deterministically; a negative
 * budget (the default) never fails. */
static long allocation_budget = -1;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
static bool refuse_allocation(void) {
    if (allocation_budget < 0) return false;
    if (allocation_budget == 0) return true;
    allocation_budget--;
    return false;
}
void *__wrap_malloc(size_t size) {
    return refuse_allocation() ? NULL : __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
    return refuse_allocation() ? NULL : __real_calloc(count, size);
}
void *__wrap_realloc(void *ptr, size_t size) {
    return refuse_allocation() ? NULL : __real_realloc(ptr, size);
}
static size_t node_count(const TaiDocument *doc) {
    size_t count = 0;
    while (tai_document_node(doc, count)) count++;
    return count;
}
static void assert_serialized(const TaiNode *node, bool outer, const char *want) {
    char *html = tai_node_serialize(node, outer);
    assert(html);
    if (strcmp(html, want)) {
        fprintf(stderr, "serialize: expected %s\n          got %s\n", want, html);
        assert(0);
    }
    free(html);
}
/* Slice 3: Python serialize_node and innerHTML_set. */
static void serialization(void) {
    char *error = NULL;
    TaiDocument *doc = tai_html_parse(
        "<div id=d b=1 A='x' b=\"2\" q=\"a&quot;'<>&\" disabled>"
        "t &amp; &lt;u&gt; \"'<br>x<input value=1>"
        "<script>a < b && c</script></div>", &error);
    assert(doc && !error);
    TaiNode *body = tai_document_root(doc)->children[0];
    TaiNode *div = body->children[0];
    /* Attribute order is first occurrence; the later duplicate wins. Values
     * escape " & < > and ' (as &#x27;); text and script escape only & < >;
     * void elements have no end tag. */
    const char *inner =
        "t &amp; &lt;u&gt; \"'<br>x<input value=\"1\">"
        "<script>a &lt; b &amp;&amp; c</script>";
    assert_serialized(div, false, inner);
    char want[512];
    snprintf(want, sizeof want,
             "<div id=\"d\" b=\"2\" a=\"x\" q=\"a&amp;quot;&#x27;&lt;&gt;&amp;\""
             " disabled=\"\">%s</div>", inner);
    assert_serialized(div, true, want);
    TaiNode *text = div->children[0], *br = div->children[1];
    assert(text->kind == TAI_TEXT);
    assert_serialized(text, true, "t &amp; &lt;u&gt; \"'");
    assert_serialized(text, false, "");
    assert_serialized(br, true, "<br>");
    /* A void element given children by a script still hides them in
     * outerHTML but shows them in its own innerHTML, as in Python. */
    TaiDomStatus status;
    TaiNode *child = tai_document_create_element(doc, "i", &status);
    assert(child && tai_node_insert_before(br, child, NULL, NULL) == TAI_DOM_OK);
    assert_serialized(br, true, "<br>");
    assert_serialized(br, false, "<i></i>");

    /* innerHTML adopts the children of the LAST body (find_body keeps going);
     * old children stay alive, detached, and are handed back. */
    TaiNode **removed = NULL;
    size_t removed_count = 0, before = node_count(doc);
    assert(tai_node_set_inner_html(div, "a<body>b</body>c<body><i>last</i></body>",
                                   &removed, &removed_count) == TAI_DOM_OK);
    assert(removed_count == 5 && removed[0] == text && removed[1] == br);
    for (size_t i = 0; i < removed_count; i++) assert(!removed[i]->parent);
    free(removed);
    assert_serialized(div, false, "<i>last</i>");
    assert(div->children[0]->parent == div && div->children[0]->document == doc);
    assert(node_count(doc) > before);
    assert(tai_node_set_inner_html(div, "", &removed, &removed_count) == TAI_DOM_OK);
    assert(removed_count == 1 && removed);
    free(removed);
    assert(tai_node_set_inner_html(div, "", &removed, &removed_count) == TAI_DOM_OK);
    assert(!removed && !removed_count && !div->child_count);

    /* Markup where Python's HTMLParser raises (IndexError on an empty stack)
     * is a parse error, not an allocation failure, and changes nothing. */
    assert(tai_node_set_inner_html(div, "<p>keep</p>", NULL, NULL) == TAI_DOM_OK);
    before = node_count(doc);
    assert(tai_node_set_inner_html(div,
               "<i></div><b> </i><head></i></p><i><b><i><ul><i><i></i>",
               &removed, &removed_count) == TAI_DOM_PARSE_ERROR);
    assert(!removed && !removed_count && node_count(doc) == before);
    assert_serialized(div, false, "<p>keep</p>");

    /* Allocation failure at every point leaves the tree and the document
     * unchanged; serialization either succeeds or returns NULL. */
    char *snapshot = tai_node_serialize(tai_document_root(doc), true);
    assert(snapshot);
    before = node_count(doc);
    bool done = false;
    for (long limit = 0; !done; limit++) {
        assert(limit < 100000);
        removed = NULL;
        allocation_budget = limit;
        status = tai_node_set_inner_html(div, "<b id=n>x<i>y</b>z</i>", &removed,
                                         &removed_count);
        allocation_budget = -1;
        if (status == TAI_DOM_OK) {
            assert(removed_count == 1 && removed[0]->parent == NULL);
            free(removed);
            done = true;
        } else {
            assert(status == TAI_DOM_NO_MEMORY && !removed && node_count(doc) == before);
            assert_serialized(tai_document_root(doc), true, snapshot);
        }
    }
    free(snapshot);
    snapshot = tai_node_serialize(tai_document_root(doc), true);
    assert(snapshot);
    for (long limit = 0;; limit++) {
        allocation_budget = limit;
        char *html = tai_node_serialize(tai_document_root(doc), true);
        allocation_budget = -1;
        if (html) {
            assert(!strcmp(html, snapshot));
            free(html);
            break;
        }
    }
    free(snapshot);
    tai_document_destroy(doc);

    /* D2: innerHTML counts every parsed node toward the limit (the wrapper
     * html/head/body included) and fails without changing anything. */
    doc = tai_html_parse("<p>old</p>", &error);
    assert(doc);
    body = tai_document_root(doc)->children[0];
    while (tai_document_create_element(doc, "b", &status)) {}
    assert(status == TAI_DOM_NODE_LIMIT);
    assert(tai_node_set_inner_html(body, "", NULL, NULL) == TAI_DOM_NODE_LIMIT);
    assert_serialized(body, false, "<p>old</p>");
    tai_document_destroy(doc);
    doc = tai_html_parse("", &error);
    assert(doc);
    body = tai_document_root(doc)->children[0];
    for (size_t count = node_count(doc); count < TAI_DOCUMENT_SCRIPT_NODE_LIMIT - 2;
         count++)
        assert(tai_document_create_element(doc, "b", &status));
    /* The wrapped "<i></i>" parses to html, body and i: one too many. */
    assert(tai_node_set_inner_html(body, "<i></i>", NULL, NULL) == TAI_DOM_NODE_LIMIT);
    assert(!body->child_count && node_count(doc) == TAI_DOCUMENT_SCRIPT_NODE_LIMIT - 2);
    /* An empty string adds just html and body, reaching the limit exactly. */
    assert(tai_node_set_inner_html(body, "", NULL, NULL) == TAI_DOM_OK);
    assert(node_count(doc) == TAI_DOCUMENT_SCRIPT_NODE_LIMIT);
    tai_document_destroy(doc);
    assert(!tai_node_serialize(NULL, true));
    assert(tai_node_set_inner_html(NULL, "", NULL, NULL) == TAI_DOM_WRONG_DOCUMENT);
}
int main(int argc, char **argv) {
    char *error = NULL;
    if (argc == 3 && !strcmp(argv[1], "--source")) {
        char *input = tai_read_file(argv[2], NULL);
        if (!input) return 2;
        char *source = tai_view_source(input, &error);
        free(input);
        if (!source) { free(error); return 3; }
        fputs(source, stdout); free(source); return 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "--serialize") ||
                      !strcmp(argv[1], "--serialize-inner"))) {
        char *input = tai_read_file(argv[2], NULL);
        if (!input) return 2;
        TaiDocument *doc = tai_html_parse(input, &error);
        free(input);
        if (!doc) { free(error); return 3; }
        char *html = tai_node_serialize(tai_document_root(doc),
                                        !strcmp(argv[1], "--serialize"));
        tai_document_destroy(doc);
        if (!html) return 4;
        fputs(html, stdout); free(html); return 0;
    }
    if (argc == 2) {
        char *input = tai_read_file(argv[1], NULL);
        if (!input) return 2;
        TaiDocument *doc = tai_html_parse(input, &error);
        free(input);
        if (!doc) { fprintf(stderr, "%s\n", error ? error : "parse error"); free(error); return 3; }
        tai_dom_json(stdout, tai_document_root(doc), false);
        tai_document_destroy(doc);
        return 0;
    }
    TaiDocument *doc = tai_html_parse("<p id='old'>hello</p>", &error);
    assert(doc && !error);
    TaiNode *root = tai_document_root(doc), *body = root->children[0];
    TaiNode *old = body->children[0];
    size_t old_id = old->id;
    assert(tai_node_set_inner_html(body, "<b>new &amp; safe</b>", NULL, NULL) == TAI_DOM_OK);
    assert(!old->parent && tai_document_node(doc, old_id) == old);
    assert(body->child_count == 1 && !strcmp(body->children[0]->tag, "b"));
    assert(!strcmp(body->children[0]->children[0]->text, "new & safe"));
    assert(!tai_node_append(body, root)); /* true ancestor cycle */
    assert(tai_node_append(body, old));
    assert(old->parent == body);
    assert(!tai_node_append(old, body));
    assert(tai_node_append(body, old));
    assert(body->child_count == 2);
    TaiDocument *other = tai_html_parse("other", &error);
    assert(other && !tai_node_append(body, tai_document_root(other)));
    tai_document_destroy(other);
    for (int i = 0; i < 100; i++)
        assert(tai_node_set_inner_html(body, "<input checked><b><i>x</b>y</i>", NULL,
                                       NULL) == TAI_DOM_OK);
    assert(tai_document_node(doc, old_id) == old && !old->parent);
    tai_document_destroy(doc);

    /* Script mutation API: statuses follow Python's check order, and a
     * failed call leaves the tree unchanged. */
    doc = tai_html_parse("<ol><li>1</li><li>2</li></ol><p>x</p>", &error);
    assert(doc && !error);
    body = tai_document_root(doc)->children[0];
    TaiNode *list = body->children[0], *first = list->children[0];
    TaiNode *second = list->children[1], *para = body->children[1];
    TaiDomStatus status = TAI_DOM_NO_MEMORY;
    TaiNode *made = tai_document_create_element(doc, "Span", &status);
    assert(made && status == TAI_DOM_OK && !strcmp(made->tag, "Span"));
    assert(!made->parent && tai_document_node(doc, made->id) == made);
    bool changed = true;
    assert(tai_node_insert_before(list, made, first, &changed) == TAI_DOM_OK);
    assert(changed && list->child_count == 3 && list->children[0] == made);
    assert(tai_node_insert_before(list, second, second, &changed) == TAI_DOM_OK);
    assert(!changed && list->children[2] == second);
    assert(tai_node_insert_before(list, para, NULL, &changed) == TAI_DOM_OK);
    assert(changed && list->children[3] == para && body->child_count == 1);
    assert(tai_node_insert_before(list, first, made, &changed) == TAI_DOM_OK);
    assert(list->children[0] == first && list->children[1] == made);
    assert(tai_node_insert_before(first, list, NULL, &changed) == TAI_DOM_CYCLE);
    assert(!changed && list->parent == body);
    assert(tai_node_insert_before(list, list, NULL, NULL) == TAI_DOM_CYCLE);
    assert(tai_node_insert_before(body, made, first, NULL) ==
           TAI_DOM_REFERENCE_NOT_CHILD);
    assert(tai_node_insert_before(list, list, first, NULL) == TAI_DOM_CYCLE);
    assert(tai_node_remove_child(body, made) == TAI_DOM_NOT_CHILD);
    assert(tai_node_remove_child(list, made) == TAI_DOM_OK);
    assert(!made->parent && list->child_count == 3);
    assert(tai_node_remove_child(list, made) == TAI_DOM_NOT_CHILD);
    other = tai_html_parse("<i>o</i>", &error);
    assert(other);
    assert(tai_node_insert_before(list, tai_document_root(other), NULL, NULL) ==
           TAI_DOM_WRONG_DOCUMENT);
    tai_document_destroy(other);
    tai_document_destroy(doc);

    /* D2: script-created nodes stop at the document node limit. */
    doc = tai_html_parse("", &error);
    assert(doc);
    size_t created = 0;
    while (tai_document_create_element(doc, "b", &status)) created++;
    assert(status == TAI_DOM_NODE_LIMIT);
    assert(tai_document_node(doc, TAI_DOCUMENT_SCRIPT_NODE_LIMIT - 1));
    assert(!tai_document_node(doc, TAI_DOCUMENT_SCRIPT_NODE_LIMIT));
    assert(created > 0);
    tai_document_destroy(doc);

    serialization();

    char *source = tai_view_source("<p>&amp;</p><!--ignored-->", &error);
    assert(source && !strcmp(source, "<pre>&lt;p&gt;<b>&amp;amp;</b>&lt;/p&gt;</pre>"));
    free(source);
    return 0;
}
