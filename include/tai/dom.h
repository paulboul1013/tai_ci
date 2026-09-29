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
bool tai_node_append(TaiNode *parent, TaiNode *child);
TaiNode *tai_document_node(const TaiDocument *doc, size_t id);

/* Script-driven mutation. Nodes created here are owned by the document like
 * parsed ones and stay alive (possibly detached) until it is destroyed. */
#define TAI_DOCUMENT_SCRIPT_NODE_LIMIT 1000000u
typedef enum {
    TAI_DOM_OK,
    TAI_DOM_NO_MEMORY,
    TAI_DOM_NODE_LIMIT,       /* document already holds the script limit */
    TAI_DOM_WRONG_DOCUMENT,
    TAI_DOM_CYCLE,            /* child is the parent or one of its ancestors */
    TAI_DOM_NOT_CHILD,        /* removal target is not a child of parent */
    TAI_DOM_REFERENCE_NOT_CHILD,
    TAI_DOM_PARSE_ERROR       /* markup where Python's HTMLParser raises */
} TaiDomStatus;
/* The tag is stored as given; callers fold case when their API requires it. */
TaiNode *tai_document_create_element(TaiDocument *doc, const char *tag,
    TaiDomStatus *status);
/* Moves child (detaching it from any old parent) before reference, or to the
 * end when reference is NULL. Python order: reference check, reference ==
 * child is a successful no-op (*changed false), then the cycle check. Nothing
 * changes unless TAI_DOM_OK is returned. */
TaiDomStatus tai_node_insert_before(TaiNode *parent, TaiNode *child,
    TaiNode *reference, bool *changed);
TaiDomStatus tai_node_remove_child(TaiNode *parent, TaiNode *child);
/* Python innerHTML_set: parses html into the same document (every parsed node
 * counts toward the limit) and replaces node's children with those of the
 * last <body> of the fragment; the old children stay alive, detached. On
 * TAI_DOM_OK with removed non-NULL, *removed receives an owned array (free it)
 * of the former children, or NULL when there were none. Nothing changes
 * unless TAI_DOM_OK is returned. */
TaiDomStatus tai_node_set_inner_html(TaiNode *node, const char *html,
    TaiNode ***removed, size_t *removed_count);
/* Python JSContext.serialize_node: outerHTML when outer, else innerHTML (the
 * children only). Returns an owned string, or NULL on allocation failure. */
char *tai_node_serialize(const TaiNode *node, bool outer);
void tai_dom_json(FILE *out, const TaiNode *node, bool include_style);
char *tai_view_source(const char *html, char **error);
#endif
