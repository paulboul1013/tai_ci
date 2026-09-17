#ifndef TAI_JS_H
#define TAI_JS_H

#include "tai/dom.h"

typedef struct TaiJsContext TaiJsContext;
typedef void (*TaiJsInvalidated)(void *userdata);

/* The context borrows the document root; the document must outlive it. All JS
 * execution and DOM mutation occur on one Tab thread. */
TaiJsContext *tai_js_create(TaiNode *document_root,
    TaiJsInvalidated invalidated, void *userdata, char **error);
void tai_js_destroy(TaiJsContext *context);
bool tai_js_eval(TaiJsContext *context, const char *source_name,
    const char *code, char **error);
bool tai_js_dispatch_event(TaiJsContext *context, const char *type,
    TaiNode *target, bool *default_prevented, char **error);

#endif
