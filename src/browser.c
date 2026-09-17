#define _POSIX_C_SOURCE 200809L
#include "tai/browser.h"
#include "tai/css.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef enum { RESOURCE_STYLE, RESOURCE_SCRIPT } ResourceKind;
typedef struct {
    ResourceKind kind;
    TaiUrl *url;
    char *body;
    const char *source;
} Resource;

struct TaiPage {
    TaiUrl *url;
    TaiDocument *document;
    TaiStylesheet *styles;
    TaiJsContext *javascript;
    TaiLayout *layout;
    TaiDisplayList *display;
    bool secure;
    bool dirty;
};

static bool diagnostic(char **error, const char *message) {
    if (error && !*error) *error = tai_strdup(message);
    return false;
}

static bool append(char **text, size_t *length, const char *part) {
    size_t count = strlen(part);
    if (*length > SIZE_MAX - count - 1) return false;
    char *next = realloc(*text, *length + count + 1);
    if (!next) return false;
    memcpy(next + *length, part, count + 1);
    *text = next;
    *length += count;
    return true;
}

static bool node_text(const TaiNode *node, char **text, size_t *length) {
    if (node->kind == TAI_TEXT) return append(text, length, node->text);
    for (size_t i = 0; i < node->child_count; i++)
        if (!node_text(node->children[i], text, length)) return false;
    return true;
}

static bool resource_add(Resource **items, size_t *count, Resource resource) {
    if (*count == SIZE_MAX / sizeof(**items)) return false;
    Resource *next = realloc(*items, (*count + 1) * sizeof(**items));
    if (!next) return false;
    *items = next;
    next[(*count)++] = resource;
    return true;
}

static bool allowed(const TaiMap *origins, const TaiUrl *url) {
    return !origins || tai_map_get(origins, tai_url_origin(url)) != NULL;
}

static bool collect_resources(TaiNode *node, const TaiUrl *base,
                              const TaiMap *origins, Resource **items,
                              size_t *count) {
    if (node->kind == TAI_ELEMENT) {
        Resource resource = {0};
        bool found = false;
        if (!strcmp(node->tag, "script")) {
            const char *source = tai_map_get(&node->attributes, "src");
            if (source) {
                resource.kind = RESOURCE_SCRIPT;
                resource.source = source;
                resource.url = tai_url_resolve(base, source);
                found = resource.url && allowed(origins, resource.url);
            } else {
                size_t length = 0;
                resource.kind = RESOURCE_SCRIPT;
                resource.source = "<inline-script>";
                if (!node_text(node, &resource.body, &length)) return false;
                if (!resource.body) resource.body = tai_strdup("");
                found = resource.body != NULL;
            }
        } else if (!strcmp(node->tag, "link") &&
                   tai_map_get(&node->attributes, "rel") &&
                   !strcmp(tai_map_get(&node->attributes, "rel"), "stylesheet")) {
            const char *source = tai_map_get(&node->attributes, "href");
            if (source) {
                resource.kind = RESOURCE_STYLE;
                resource.source = source;
                resource.url = tai_url_resolve(base, source);
                found = resource.url && allowed(origins, resource.url);
            }
        } else if (!strcmp(node->tag, "style")) {
            size_t length = 0;
            resource.kind = RESOURCE_STYLE;
            resource.source = "<style>";
            if (!node_text(node, &resource.body, &length)) return false;
            if (!resource.body) resource.body = tai_strdup("");
            found = resource.body != NULL;
        }
        if (found && !resource_add(items, count, resource)) {
            tai_url_destroy(resource.url);
            free(resource.body);
            return false;
        }
        if (!found) tai_url_destroy(resource.url);
    }
    for (size_t i = 0; i < node->child_count; i++)
        if (!collect_resources(node->children[i], base, origins, items, count))
            return false;
    return true;
}

static void resources_destroy(Resource *resources, size_t count) {
    for (size_t i = 0; i < count; i++) {
        tai_url_destroy(resources[i].url);
        free(resources[i].body);
    }
    free(resources);
}

static TaiMap *parse_csp(const char *header) {
    if (!header) return NULL;
    char *copy = tai_strdup(header);
    TaiMap *origins = calloc(1, sizeof(*origins));
    if (!copy || !origins) { free(copy); free(origins); return NULL; }
    char *save = NULL;
    char *word = strtok_r(copy, " \t\r\n", &save);
    if (!word || strcmp(word, "default-src")) {
        free(copy); free(origins); return NULL;
    }
    while ((word = strtok_r(NULL, " \t\r\n", &save))) {
        TaiUrl *url = tai_url_parse(word);
        if (!url || !tai_map_set(origins, tai_url_origin(url), "1", 0)) {
            tai_url_destroy(url);
            tai_map_clear(origins); free(origins); free(copy);
            return NULL;
        }
        tai_url_destroy(url);
    }
    free(copy);
    return origins;
}

static void invalidated(void *opaque) {
    ((TaiPage *)opaque)->dirty = true;
}

TaiPage *tai_page_load(TaiNetwork *network, const TaiUrl *url,
                       const char *default_css, double viewport_width, bool rtl,
                       char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!network || !url || !default_css || viewport_width <= 0.0) {
        diagnostic(error, "invalid page load input");
        return NULL;
    }
    TaiPage *page = calloc(1, sizeof(*page));
    TaiResponse *response = NULL;
    TaiMap *origins = NULL;
    Resource *resources = NULL;
    size_t resource_count = 0;
    if (!page) goto oom;
    page->url = tai_url_parse(tai_url_string(url));
    if (!page->url) goto oom;
    response = tai_network_request(network, url, NULL, NULL, NULL, NULL);
    if (!response) goto oom;
    if (response->error) {
        diagnostic(error, response->error);
        goto fail;
    }
    page->secure = !strcmp(tai_url_scheme(url), "https");
    char *body = NULL;
    if (tai_url_view_source(url)) body = tai_view_source(response->body, error);
    page->document = tai_html_parse(body ? body : response->body, error);
    free(body);
    if (!page->document) goto fail;
    page->styles = tai_css_parse(default_css, error);
    if (!page->styles) goto fail;
    const char *csp = tai_map_get(&response->headers, "content-security-policy");
    origins = parse_csp(csp);
    if (csp && !origins && !strncmp(csp, "default-src", 11)) goto oom;
    if (!collect_resources(tai_document_root(page->document), url, origins,
                           &resources, &resource_count)) goto oom;
    page->javascript = tai_js_create(tai_document_root(page->document),
                                     invalidated, page, error);
    if (!page->javascript) goto fail;
    for (size_t i = 0; i < resource_count; i++) {
        Resource *resource = &resources[i];
        TaiResponse *loaded = NULL;
        const char *content = resource->body;
        if (resource->url) {
            loaded = tai_network_request(network, resource->url, url, NULL, NULL,
                                         NULL);
            if (!loaded || loaded->error) {
                tai_response_destroy(loaded);
                continue;
            }
            content = loaded->body;
        }
        if (resource->kind == RESOURCE_STYLE) {
            if (!tai_css_extend(page->styles, content ? content : "", error)) {
                tai_response_destroy(loaded);
                goto fail;
            }
        } else {
            char *script_error = NULL;
            tai_js_eval(page->javascript, resource->source, content ? content : "",
                        &script_error);
            free(script_error); /* Python reports and continues after script errors. */
        }
        tai_response_destroy(loaded);
    }
    if (!tai_css_style(tai_document_root(page->document), page->styles, error))
        goto fail;
    page->layout = tai_layout_create(tai_document_root(page->document),
                                     viewport_width, rtl, error);
    if (!page->layout) goto fail;
    page->display = tai_display_list_create(page->layout, error);
    if (!page->display) goto fail;
    resources_destroy(resources, resource_count);
    if (origins) { tai_map_clear(origins); free(origins); }
    tai_response_destroy(response);
    return page;
oom:
    diagnostic(error, "page allocation failed");
fail:
    resources_destroy(resources, resource_count);
    if (origins) { tai_map_clear(origins); free(origins); }
    tai_response_destroy(response);
    tai_page_destroy(page);
    return NULL;
}

void tai_page_destroy(TaiPage *page) {
    if (!page) return;
    tai_display_list_destroy(page->display);
    tai_layout_destroy(page->layout);
    tai_js_destroy(page->javascript);
    tai_css_destroy(page->styles);
    tai_document_destroy(page->document);
    tai_url_destroy(page->url);
    free(page);
}

TaiNode *tai_page_root(const TaiPage *page) {
    return page ? tai_document_root(page->document) : NULL;
}
const TaiLayout *tai_page_layout(const TaiPage *page) { return page ? page->layout : NULL; }
const TaiDisplayList *tai_page_display_list(const TaiPage *page) {
    return page ? page->display : NULL;
}
const TaiUrl *tai_page_url(const TaiPage *page) { return page ? page->url : NULL; }
void tai_page_json(FILE *out, const TaiPage *page) {
    fputs("{\"url\":", out);
    tai_json_string(out, tai_url_string(page->url));
    fprintf(out, ",\"secure\":%s,\"dom\":", page->secure ? "true" : "false");
    tai_dom_json(out, tai_document_root(page->document), true);
    fputs(",\"layout\":", out);
    tai_layout_json(out, page->layout);
    fputs(",\"display\":", out);
    tai_display_list_json(out, page->display);
    fputc('}', out);
}
