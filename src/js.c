#define _POSIX_C_SOURCE 200809L
#include "tai/js.h"
#include "tai/css.h"

#include <quickjs.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utf8proc.h>

struct TaiJsContext {
    JSRuntime *runtime;
    JSContext *context;
    TaiNode *root;
    TaiJsInvalidated invalidated;
    void *userdata;
    double deadline;
};

static const char runtime_source[] =
    "var LISTENERS={};"
    "function Node(handle){this.handle=handle;}"
    "Node.prototype.getAttribute=function(name){return call_python('getAttribute',this.handle,name);};"
    "Node.prototype.setAttribute=function(name,value){call_python('setAttribute',this.handle,String(name),String(value));};"
    "Node.prototype.addEventListener=function(type,listener){var d=LISTENERS[this.handle]||(LISTENERS[this.handle]={});(d[type]||(d[type]=[])).push(listener);};"
    "function Event(type){this.type=type;this.do_default=true;this.propagation_stopped=false;this.target=null;this.currentTarget=null;}"
    "Event.prototype.preventDefault=function(){this.do_default=false;};"
    "Event.prototype.stopPropagation=function(){this.propagation_stopped=true;};"
    "Node.prototype.dispatchEvent=function(event){event.currentTarget=this;var d=LISTENERS[this.handle];var a=(d&&d[event.type])||[];for(var i=0;i<a.length;i++)a[i].call(this,event);return event.do_default;};"
    "function dispatch_event_path(type,handles){var e=new Event(type);if(!handles.length)return true;e.target=new Node(handles[0]);for(var i=0;i<handles.length;i++){new Node(handles[i]).dispatchEvent(e);if(e.propagation_stopped)break;}return e.do_default;}"
    "var document={querySelectorAll:function(s){return call_python('querySelectorAll',s).map(function(h){return new Node(h);});}};"
    "function __tai_set_id(name,handle){if(!(name in globalThis))globalThis[name]=new Node(handle);}";

static double seconds_now(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0.0;
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

static int interrupt_handler(JSRuntime *runtime, void *opaque) {
    (void)runtime;
    TaiJsContext *js = opaque;
    return js->deadline > 0.0 && seconds_now() >= js->deadline;
}

static bool set_error(char **error, const char *message) {
    if (error && !*error) *error = tai_strdup(message ? message : "JavaScript error");
    return false;
}

static bool exception_error(TaiJsContext *js, char **error) {
    JSValue exception = JS_GetException(js->context);
    const char *message = JS_ToCString(js->context, exception);
    bool result = set_error(error, message);
    JS_FreeCString(js->context, message);
    JS_FreeValue(js->context, exception);
    return result;
}

static TaiNode *node_from_value(TaiJsContext *js, JSValueConst value) {
    int64_t handle;
    if (JS_ToInt64(js->context, &handle, value) < 0 || handle < 0) return NULL;
    return tai_document_node(js->root->document, (size_t)handle);
}

static void collect_matches(TaiNode *node, const TaiSelector *selector,
                            JSContext *context, JSValue array, uint32_t *index) {
    if (tai_selector_matches(selector, node)) {
        JS_SetPropertyUint32(context, array, (*index)++, JS_NewInt64(context, (int64_t)node->id));
    }
    for (size_t i = 0; i < node->child_count; i++)
        collect_matches(node->children[i], selector, context, array, index);
}

static char *casefold(const char *text) {
    utf8proc_uint8_t *mapped = NULL;
    utf8proc_ssize_t size = utf8proc_map((const utf8proc_uint8_t *)text, 0, &mapped,
        UTF8PROC_NULLTERM | UTF8PROC_STABLE | UTF8PROC_CASEFOLD);
    return size < 0 ? NULL : (char *)mapped;
}

static JSValue call_python(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv) {
    (void)this_value;
    TaiJsContext *js = JS_GetContextOpaque(context);
    if (!js || argc < 1) return JS_ThrowTypeError(context, "missing bridge operation");
    const char *operation = JS_ToCString(context, argv[0]);
    if (!operation) return JS_EXCEPTION;
    JSValue result = JS_UNDEFINED;
    if (!strcmp(operation, "querySelectorAll") && argc >= 2) {
        const char *text = JS_ToCString(context, argv[1]);
        char *error = NULL;
        TaiSelector *selector = text ? tai_selector_parse(text, &error) : NULL;
        JS_FreeCString(context, text);
        if (!selector) {
            result = JS_ThrowTypeError(context, "%s", error ? error : "invalid selector");
            free(error);
        } else {
            result = JS_NewArray(context);
            uint32_t index = 0;
            collect_matches(js->root, selector, context, result, &index);
            tai_selector_destroy(selector);
        }
    } else if (!strcmp(operation, "getAttribute") && argc >= 3) {
        TaiNode *node = node_from_value(js, argv[1]);
        const char *name = JS_ToCString(context, argv[2]);
        const char *value = node && name ? tai_map_get(&node->attributes, name) : NULL;
        result = JS_NewString(context, value ? value : "");
        JS_FreeCString(context, name);
    } else if (!strcmp(operation, "setAttribute") && argc >= 4) {
        TaiNode *node = node_from_value(js, argv[1]);
        const char *name = JS_ToCString(context, argv[2]);
        const char *value = JS_ToCString(context, argv[3]);
        char *folded = name ? casefold(name) : NULL;
        if (!node || node->kind != TAI_ELEMENT || !folded || !value ||
            !tai_map_set(&node->attributes, folded, value, 0))
            result = JS_ThrowTypeError(context, "setAttribute failed");
        else if (js->invalidated) js->invalidated(js->userdata);
        free(folded);
        JS_FreeCString(context, name);
        JS_FreeCString(context, value);
    } else {
        result = JS_ThrowTypeError(context, "unsupported bridge operation: %s", operation);
    }
    JS_FreeCString(context, operation);
    return result;
}

static bool evaluate(TaiJsContext *js, const char *name, const char *code,
                     char **error) {
    if (error) { free(*error); *error = NULL; }
    js->deadline = seconds_now() + 2.0;
    JSValue value = JS_Eval(js->context, code, strlen(code), name, JS_EVAL_TYPE_GLOBAL);
    js->deadline = 0.0;
    if (JS_IsException(value)) {
        JS_FreeValue(js->context, value);
        return exception_error(js, error);
    }
    JS_FreeValue(js->context, value);
    return true;
}

static bool sync_ids(TaiJsContext *js, TaiNode *node, char **error) {
    if (node->kind == TAI_ELEMENT) {
        const char *id = tai_map_get(&node->attributes, "id");
        if (id && *id) {
            JSValue global = JS_GetGlobalObject(js->context);
            JSValue function = JS_GetPropertyStr(js->context, global, "__tai_set_id");
            JSValue arguments[2] = {JS_NewString(js->context, id),
                                    JS_NewInt64(js->context, (int64_t)node->id)};
            JSValue result = JS_Call(js->context, function, global, 2, arguments);
            JS_FreeValue(js->context, arguments[0]);
            JS_FreeValue(js->context, arguments[1]);
            JS_FreeValue(js->context, function);
            JS_FreeValue(js->context, global);
            if (JS_IsException(result)) {
                JS_FreeValue(js->context, result);
                return exception_error(js, error);
            }
            JS_FreeValue(js->context, result);
        }
    }
    for (size_t i = 0; i < node->child_count; i++)
        if (!sync_ids(js, node->children[i], error)) return false;
    return true;
}

TaiJsContext *tai_js_create(TaiNode *root, TaiJsInvalidated invalidated,
                            void *userdata, char **error) {
    if (!root || !root->document) { set_error(error, "missing JavaScript document"); return NULL; }
    TaiJsContext *js = calloc(1, sizeof(*js));
    if (!js) { set_error(error, "JavaScript allocation failed"); return NULL; }
    js->root = root;
    js->invalidated = invalidated;
    js->userdata = userdata;
    js->runtime = JS_NewRuntime();
    if (!js->runtime) goto fail;
    JS_SetMemoryLimit(js->runtime, 64U * 1024U * 1024U);
    JS_SetMaxStackSize(js->runtime, 512U * 1024U);
    JS_SetInterruptHandler(js->runtime, interrupt_handler, js);
    js->context = JS_NewContext(js->runtime);
    if (!js->context) goto fail;
    JS_SetContextOpaque(js->context, js);
    JSValue global = JS_GetGlobalObject(js->context);
    JS_SetPropertyStr(js->context, global, "call_python",
        JS_NewCFunction(js->context, call_python, "call_python", 1));
    JS_FreeValue(js->context, global);
    if (!evaluate(js, "runtime.js", runtime_source, error) || !sync_ids(js, root, error)) {
        tai_js_destroy(js);
        return NULL;
    }
    return js;
fail:
    set_error(error, "JavaScript allocation failed");
    tai_js_destroy(js);
    return NULL;
}

void tai_js_destroy(TaiJsContext *js) {
    if (!js) return;
    if (js->context) JS_FreeContext(js->context);
    if (js->runtime) JS_FreeRuntime(js->runtime);
    free(js);
}

bool tai_js_eval(TaiJsContext *js, const char *name, const char *code,
                 char **error) {
    if (!js || !code) return set_error(error, "missing JavaScript input");
    return evaluate(js, name ? name : "script", code, error);
}

bool tai_js_dispatch_event(TaiJsContext *js, const char *type, TaiNode *target,
                           bool *default_prevented, char **error) {
    if (!js || !type || !target) return set_error(error, "missing event input");
    JSValue handles = JS_NewArray(js->context);
    uint32_t index = 0;
    for (TaiNode *node = target; node; node = node->parent)
        if (node->kind == TAI_ELEMENT)
            JS_SetPropertyUint32(js->context, handles, index++, JS_NewInt64(js->context, (int64_t)node->id));
    JSValue global = JS_GetGlobalObject(js->context);
    JSValue function = JS_GetPropertyStr(js->context, global, "dispatch_event_path");
    JSValue arguments[2] = {JS_NewString(js->context, type), handles};
    js->deadline = seconds_now() + 2.0;
    JSValue result = JS_Call(js->context, function, global, 2, arguments);
    js->deadline = 0.0;
    JS_FreeValue(js->context, arguments[0]);
    JS_FreeValue(js->context, handles);
    JS_FreeValue(js->context, function);
    JS_FreeValue(js->context, global);
    if (JS_IsException(result)) {
        JS_FreeValue(js->context, result);
        return exception_error(js, error);
    }
    int do_default = JS_ToBool(js->context, result);
    JS_FreeValue(js->context, result);
    if (do_default < 0) return exception_error(js, error);
    if (default_prevented) *default_prevented = !do_default;
    return true;
}
