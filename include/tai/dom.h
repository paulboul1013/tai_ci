#ifndef TAI_DOM_H
#define TAI_DOM_H
#include "tai/core.h"
typedef struct TaiDocument TaiDocument;
typedef struct TaiNode TaiNode;
typedef enum { TAI_ELEMENT, TAI_TEXT } TaiNodeKind;
/* Document owns nodes, strings, maps and child arrays. All node links are borrowed.
 * Detached nodes remain alive until document destruction, keeping JS handles valid.
 * Nodes may be mutated only on the owning Tab thread. */
struct TaiNode {
    TaiNodeKind kind;
    size_t id;
    TaiDocument *document;
    char *tag;
    char *text;
    TaiMap attributes;
    TaiMap style;
    TaiNode *parent;
    TaiNode **children;
    size_t child_count, child_capacity;
    bool focused, checked, visited;
    size_t cursor_index;
    double scroll_y;
};
/* On error NULL with owned diagnostic in *error when non-NULL. */
TaiDocument *tai_html_parse(const char *html, char **error);
TaiNode *tai_document_root(const TaiDocument *doc);
void tai_document_destroy(TaiDocument *doc);
/* Parse fragment into same owner; replaces children, retains detached nodes. */
bool tai_node_set_inner_html(TaiNode *node, const char *html, char **error);
bool tai_node_append(TaiNode *parent, TaiNode *child);
TaiNode *tai_document_node(const TaiDocument *doc, size_t id);
void tai_dom_json(FILE *out, const TaiNode *node, bool include_style);
char *tai_view_source(const char *html, char **error);
#endif
