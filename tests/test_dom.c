#include "tai/dom.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
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
    assert(tai_node_set_inner_html(body, "<b>new &amp; safe</b>", &error));
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
        assert(tai_node_set_inner_html(body, "<input checked><b><i>x</b>y</i>", &error));
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

    char *source = tai_view_source("<p>&amp;</p><!--ignored-->", &error);
    assert(source && !strcmp(source, "<pre>&lt;p&gt;<b>&amp;amp;</b>&lt;/p&gt;</pre>"));
    free(source);
    return 0;
}
