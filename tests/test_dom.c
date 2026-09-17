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
    char *source = tai_view_source("<p>&amp;</p><!--ignored-->", &error);
    assert(source && !strcmp(source, "<pre>&lt;p&gt;<b>&amp;amp;</b>&lt;/p&gt;</pre>"));
    free(source);
    return 0;
}
