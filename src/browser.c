#define _POSIX_C_SOURCE 200809L
#include "tai/browser.h"
#include "tai/css.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <utf8proc.h>

typedef enum { RESOURCE_STYLE, RESOURCE_SCRIPT } ResourceKind;
static const double TAI_PAGE_VERTICAL_STEP = 18.0;
typedef struct {
    ResourceKind kind;
    TaiUrl *url;
    char *body;
    const char *source;
    TaiResponse *response;
} Resource;

typedef struct {
    TaiNode **nodes;
    double *scroll_y;
    size_t count;
} ScrollSnapshot;

struct TaiNavigationIntent {
    char *url;
    char *body;
};

struct TaiPage {
    TaiUrl *url;
    TaiDocument *document;
    TaiStylesheet *styles;
    TaiJsContext *javascript;
    TaiLayout *layout;
    TaiDisplayList *display;
    double viewport_width;
    double viewport_height;
    double scroll_y;
    bool rtl;
    bool secure;
    bool dirty;
    TaiNode *focused;
    TaiNavigationIntent *navigation;
    char *fragment_change;
    TaiUrl *fragment_previous_url;
    double fragment_previous_scroll;
};

static bool diagnostic(char **error, const char *message) {
    if (error && !*error) *error = tai_strdup(message);
    return false;
}

void tai_navigation_intent_destroy(TaiNavigationIntent *intent) {
    if (!intent) return;
    free(intent->url);
    free(intent->body);
    free(intent);
}

const char *tai_navigation_intent_url(const TaiNavigationIntent *intent) {
    return intent ? intent->url : NULL;
}

bool tai_navigation_intent_is_post(const TaiNavigationIntent *intent) {
    return intent && intent->body != NULL;
}

const char *tai_navigation_intent_body(const TaiNavigationIntent *intent) {
    return intent ? intent->body : NULL;
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
        tai_response_destroy(resources[i].response);
    }
    free(resources);
}

static bool node_count(const TaiNode *node, size_t *count) {
    if (*count == SIZE_MAX) return false;
    (*count)++;
    for (size_t index = 0; index < node->child_count; index++)
        if (!node_count(node->children[index], count)) return false;
    return true;
}

static void snapshot_scroll(TaiNode *node, ScrollSnapshot *snapshot,
                            size_t *index) {
    snapshot->nodes[*index] = node;
    snapshot->scroll_y[*index] = node->scroll_y;
    (*index)++;
    for (size_t child = 0; child < node->child_count; child++)
        snapshot_scroll(node->children[child], snapshot, index);
}

static void restore_scroll(const ScrollSnapshot *snapshot) {
    for (size_t index = 0; index < snapshot->count; index++)
        snapshot->nodes[index]->scroll_y = snapshot->scroll_y[index];
}

static void snapshot_destroy(ScrollSnapshot *snapshot) {
    free(snapshot->scroll_y);
    free(snapshot->nodes);
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

static bool scroll_to_fragment(TaiPage *page, const char *fragment,
                              char **error);

static TaiPage *page_create(const TaiUrl *url, double viewport_width,
                            double viewport_height, bool rtl, char **error) {
    if (!url || !isfinite(viewport_width) || !isfinite(viewport_height) ||
        viewport_width <= 0.0 || viewport_height <= 0.0 ||
        viewport_width > INT_MAX || viewport_height > INT_MAX) {
        diagnostic(error, "invalid page load input");
        return NULL;
    }
    TaiPage *page = calloc(1, sizeof(*page));
    if (!page) {
        diagnostic(error, "page allocation failed");
        return NULL;
    }
    page->viewport_width = viewport_width;
    page->viewport_height = viewport_height;
    page->rtl = rtl;
    page->url = tai_url_parse(tai_url_string(url));
    if (!page->url) {
        diagnostic(error, "page URL allocation failed");
        tai_page_destroy(page);
        return NULL;
    }
    return page;
}

static bool page_prepare_document(TaiPage *page, const TaiUrl *url,
                                  const TaiResponse *response,
                                  const char *default_css,
                                  Resource **resources,
                                  size_t *resource_count, char **error) {
    page->secure = !response->error &&
                   !strcmp(tai_url_scheme(url), "https");
    char *body = NULL;
    if (tai_url_view_source(url)) body = tai_view_source(response->body, error);
    page->document = tai_html_parse(body ? body : response->body, error);
    free(body);
    if (!page->document) return false;
    page->styles = tai_css_parse(default_css, error);
    if (!page->styles) return false;

    const char *csp = tai_map_get(&response->headers, "content-security-policy");
    TaiMap *origins = parse_csp(csp);
    if (csp && !origins && !strncmp(csp, "default-src", 11)) {
        diagnostic(error, "content security policy allocation failed");
        return false;
    }
    bool collected = collect_resources(tai_document_root(page->document), url,
                                       origins, resources, resource_count);
    if (origins) { tai_map_clear(origins); free(origins); }
    if (!collected) {
        diagnostic(error, "page resource allocation failed");
        return false;
    }
    page->javascript = tai_js_create(tai_document_root(page->document),
                                     invalidated, page, error);
    return page->javascript != NULL;
}

static bool page_apply_resources(TaiPage *page, Resource *resources,
                                 size_t resource_count, char **error) {
    for (size_t index = 0; index < resource_count; index++) {
        Resource *resource = &resources[index];
        const char *content = resource->body;
        if (resource->url) {
            if (!resource->response || resource->response->error) continue;
            content = resource->response->body;
        }
        if (resource->kind == RESOURCE_STYLE) {
            if (!tai_css_extend(page->styles, content ? content : "", error))
                return false;
        } else {
            char *script_error = NULL;
            tai_js_eval(page->javascript, resource->source,
                        content ? content : "", &script_error);
            free(script_error); /* Python reports and continues after script errors. */
        }
    }
    return true;
}

static bool page_finish_visual(TaiPage *page, char **error) {
    if (!tai_css_style(tai_document_root(page->document), page->styles, error))
        return false;
    page->layout = tai_layout_create(tai_document_root(page->document),
                                     page->viewport_width, page->rtl, error);
    if (!page->layout) return false;
    if (!scroll_to_fragment(page, tai_url_fragment(page->url), error))
        return false;
    page->display = tai_display_list_create(page->layout, error);
    return page->display != NULL;
}

TaiPage *tai_page_load_request(TaiNetwork *network, const TaiUrl *url,
                               const TaiUrl *referrer, const char *payload,
                               const char *default_css, double viewport_width,
                               double viewport_height, bool rtl, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!network || !url || !default_css) {
        diagnostic(error, "invalid page load input");
        return NULL;
    }
    TaiPage *page = page_create(url, viewport_width, viewport_height, rtl, error);
    TaiResponse *response = NULL;
    Resource *resources = NULL;
    size_t resource_count = 0;
    if (!page) goto fail;
    response = tai_network_request(network, url, referrer, payload, NULL, NULL);
    if (!response) {
        diagnostic(error, "network response allocation failed");
        goto fail;
    }
    if (response->error) {
        diagnostic(error, response->error);
        goto fail;
    }
    if (!page_prepare_document(page, url, response, default_css, &resources,
                               &resource_count, error)) goto fail;
    for (size_t i = 0; i < resource_count; i++) {
        Resource *resource = &resources[i];
        if (resource->url)
            resource->response = tai_network_request(
                network, resource->url, url, NULL, NULL, NULL);
    }
    if (!page_apply_resources(page, resources, resource_count, error) ||
        !page_finish_visual(page, error)) goto fail;
    resources_destroy(resources, resource_count);
    tai_response_destroy(response);
    return page;
fail:
    resources_destroy(resources, resource_count);
    tai_response_destroy(response);
    tai_page_destroy(page);
    return NULL;
}

typedef struct TaiPageLoadRequest TaiPageLoadRequest;
struct TaiPageLoadRequest {
    TaiPageLoad *load;
    TaiRequest *request;
    size_t resource_index;
    bool document;
    TaiPageLoadRequest *next;
};

struct TaiPageLoad {
    TaiNetwork *network;
    TaiPage *page;
    TaiUrl *url;
    TaiUrl *referrer;
    char *payload;
    char *default_css;
    double viewport_width;
    double viewport_height;
    bool rtl;
    bool network_failure;
    Resource *resources;
    size_t resource_count;
    size_t pending_resources;
    TaiPageLoadRequest *requests;
    TaiPageLoadDone done;
    void *userdata;
};

static void page_load_destroy(TaiPageLoad *load) {
    if (!load) return;
    resources_destroy(load->resources, load->resource_count);
    tai_page_destroy(load->page);
    tai_url_destroy(load->url);
    tai_url_destroy(load->referrer);
    free(load->payload);
    free(load->default_css);
    free(load);
}

static void page_load_complete(TaiPageLoad *load, char *error) {
    TaiPageLoadDone done = load->done;
    void *userdata = load->userdata;
    TaiPage *page = load->page;
    bool network_failure = load->network_failure;
    load->page = NULL;
    page_load_destroy(load);
    done(userdata, page, network_failure, error);
}

static void page_load_fail(TaiPageLoad *load, const char *message) {
    char *error = message ? tai_strdup(message) : NULL;
    if (message && !error) error = tai_strdup("page load failed");
    tai_page_destroy(load->page);
    load->page = NULL;
    page_load_complete(load, error);
}

static bool append_html_escape(char **text, size_t *length,
                               const char *value) {
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        const char *replacement = NULL;
        switch (*p) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '\"': replacement = "&quot;"; break;
            case '\'': replacement = "&#x27;"; break;
            default: break;
        }
        char byte[2] = {(char)*p, '\0'};
        if (!append(text, length, replacement ? replacement : byte))
            return false;
    }
    return true;
}

static char *network_error_markup(const TaiUrl *url, const char *message,
                                  bool certificate_error) {
    char *body = NULL;
    size_t length = 0;
    const char *heading = certificate_error ? "Certificate Error" : "Network Error";
    if (!append(&body, &length, "<html><body><h1>") ||
        !append(&body, &length, heading) ||
        !append(&body, &length, "</h1><p>") ||
        !append_html_escape(&body, &length, tai_url_string(url)) ||
        !append(&body, &length, "</p><pre>") ||
        !append_html_escape(&body, &length, message ? message : "") ||
        !append(&body, &length, "</pre></body></html>")) {
        free(body);
        return NULL;
    }
    return body;
}

static void page_load_request_done(void *opaque, TaiResponse *response);

static bool page_load_submit_resource(TaiPageLoad *load, size_t index) {
    TaiPageLoadRequest *request = calloc(1, sizeof(*request));
    if (!request) return false;
    request->load = load;
    request->resource_index = index;
    Resource *resource = &load->resources[index];
    request->request = tai_network_submit(
        load->network, resource->url, load->url, NULL, NULL, NULL,
        page_load_request_done, request);
    if (!request->request) {
        free(request);
        return false;
    }
    request->next = load->requests;
    load->requests = request;
    load->pending_resources++;
    return true;
}

static void page_load_finish_resources(TaiPageLoad *load) {
    char *error = NULL;
    if (!page_apply_resources(load->page, load->resources,
                              load->resource_count, &error) ||
        !page_finish_visual(load->page, &error)) {
        if (!error) error = tai_strdup("page rendering failed");
        tai_page_destroy(load->page);
        load->page = NULL;
        page_load_complete(load, error);
        return;
    }
    page_load_complete(load, NULL);
}

static void page_load_request_done(void *opaque, TaiResponse *response) {
    TaiPageLoadRequest *request = opaque;
    TaiPageLoad *load = request->load;
    TaiPageLoadRequest **slot = &load->requests;
    while (*slot && *slot != request) slot = &(*slot)->next;
    if (*slot) *slot = request->next;
    bool document = request->document;
    size_t resource_index = request->resource_index;
    free(request);

    if (document) {
        if (!response) {
            page_load_fail(load, "network response allocation failed");
            return;
        }
        char *transport_error = response->error
            ? tai_strdup(response->error) : NULL;
        load->network_failure = response->error != NULL;
        if (response->error) {
            char *markup = network_error_markup(load->url, response->error,
                                                 response->certificate_error);
            if (!markup) {
                tai_response_destroy(response);
                free(transport_error);
                page_load_fail(load, "network error page allocation failed");
                return;
            }
            free(response->body);
            response->body = markup;
            response->length = strlen(markup);
        }
        char *setup_error = NULL;
        bool prepared = page_prepare_document(
            load->page, load->url, response, load->default_css,
            &load->resources, &load->resource_count, &setup_error);
        tai_response_destroy(response);
        if (!prepared) {
            free(transport_error);
            tai_page_destroy(load->page);
            load->page = NULL;
            page_load_complete(load, setup_error);
            return;
        }
        for (size_t index = 0; index < load->resource_count; index++) {
            if (load->resources[index].url)
                (void)page_load_submit_resource(load, index);
        }
        if (!load->pending_resources) {
            free(transport_error);
            page_load_finish_resources(load);
            return;
        }
        /* The error detail is intentionally not exposed as a window-fatal
         * condition; the first-load error page itself is the visible result. */
        free(transport_error);
        return;
    }

    Resource *resource = &load->resources[resource_index];
    resource->response = response;
    if (load->pending_resources) load->pending_resources--;
    if (!load->pending_resources) page_load_finish_resources(load);
}

TaiPageLoad *tai_page_load_async(TaiNetwork *network, const TaiUrl *url,
    const TaiUrl *referrer, const char *payload, const char *default_css,
    double viewport_width, double viewport_height, bool rtl,
    TaiPageLoadDone done, void *userdata, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!network || !url || !default_css || !done) {
        diagnostic(error, "invalid asynchronous page load input");
        return NULL;
    }
    TaiPageLoad *load = calloc(1, sizeof(*load));
    if (!load) {
        diagnostic(error, "page load allocation failed");
        return NULL;
    }
    load->network = network;
    load->url = tai_url_parse(tai_url_string(url));
    if (referrer) load->referrer = tai_url_parse(tai_url_string(referrer));
    if (payload) load->payload = tai_strdup(payload);
    load->default_css = tai_strdup(default_css);
    load->viewport_width = viewport_width;
    load->viewport_height = viewport_height;
    load->rtl = rtl;
    load->done = done;
    load->userdata = userdata;
    if (!load->url || (referrer && !load->referrer) ||
        (payload && !load->payload) || !load->default_css) {
        page_load_destroy(load);
        diagnostic(error, "page load input allocation failed");
        return NULL;
    }
    load->page = page_create(load->url, viewport_width, viewport_height,
                             rtl, error);
    if (!load->page) {
        page_load_destroy(load);
        return NULL;
    }
    TaiPageLoadRequest *request = calloc(1, sizeof(*request));
    if (!request) {
        page_load_destroy(load);
        diagnostic(error, "page request allocation failed");
        return NULL;
    }
    request->load = load;
    request->document = true;
    request->request = tai_network_submit(
        network, load->url, load->referrer, load->payload, NULL, NULL,
        page_load_request_done, request);
    if (!request->request) {
        free(request);
        page_load_destroy(load);
        diagnostic(error, "page request submission failed");
        return NULL;
    }
    load->requests = request;
    return load;
}

/* The general HTML parser preserves entities in attribute values. Internal
 * bookmark links are generated from exact serialized URLs; decode their href
 * after parsing so activation requests the saved URL rather than "&amp;". */
static char *decode_internal_href(const char *source) {
    size_t length = strlen(source);
    char *decoded = malloc(length + 1);
    if (!decoded) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < length;) {
        static const struct { const char *entity; char value; } entities[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
            {"&quot;", '"'}, {"&#x27;", '\''},
        };
        bool matched = false;
        for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); e++) {
            size_t entity_length = strlen(entities[e].entity);
            if (entity_length <= length - i &&
                !memcmp(source + i, entities[e].entity, entity_length)) {
                decoded[out++] = entities[e].value;
                i += entity_length;
                matched = true;
                break;
            }
        }
        if (!matched) decoded[out++] = source[i++];
    }
    decoded[out] = '\0';
    return decoded;
}

static bool decode_internal_links(TaiNode *node) {
    if (node->kind == TAI_ELEMENT && !strcmp(node->tag, "a")) {
        const char *href = tai_map_get(&node->attributes, "href");
        if (href) {
            char *decoded = decode_internal_href(href);
            if (!decoded) return false;
            int priority = tai_map_priority(&node->attributes, "href");
            bool ok = tai_map_set(&node->attributes, "href", decoded, priority);
            free(decoded);
            if (!ok) return false;
        }
    }
    for (size_t i = 0; i < node->child_count; i++)
        if (!decode_internal_links(node->children[i])) return false;
    return true;
}

TaiPageLoad *tai_page_load_async_markup(TaiNetwork *network, const TaiUrl *url,
    const char *markup, const char *default_css, double viewport_width,
    double viewport_height, bool rtl, TaiPageLoadDone done, void *userdata,
    char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!network || !url || strcmp(tai_url_scheme(url), "about") ||
        tai_url_view_source(url) || !markup || !default_css || !done) {
        diagnostic(error, "invalid internal page load input");
        return NULL;
    }
    TaiPage *page = page_create(url, viewport_width, viewport_height, rtl,
                                error);
    if (!page) return NULL;

    TaiResponse response = {.body = (char *)markup, .length = strlen(markup),
                            .status = 200};
    Resource *resources = NULL;
    size_t resource_count = 0;
    char *load_error = NULL;
    bool prepared = page_prepare_document(page, url, &response, default_css,
                                          &resources, &resource_count,
                                          &load_error);
    if (prepared && !decode_internal_links(tai_document_root(page->document))) {
        diagnostic(&load_error, "internal link allocation failed");
        prepared = false;
    }
    if (prepared)
        prepared = page_apply_resources(page, resources, resource_count,
                                        &load_error) &&
                   page_finish_visual(page, &load_error);
    resources_destroy(resources, resource_count);
    if (!prepared) {
        tai_page_destroy(page);
        page = NULL;
        if (!load_error) load_error = tai_strdup("internal page rendering failed");
    }
    done(userdata, page, false, load_error);
    return NULL;
}

void tai_page_load_async_cancel(TaiPageLoad *load) {
    if (!load) return;
    TaiPageLoadRequest *request = load->requests;
    load->requests = NULL;
    while (request) {
        TaiPageLoadRequest *next = request->next;
        tai_network_cancel(load->network, request->request);
        free(request);
        request = next;
    }
    page_load_destroy(load);
}

TaiPage *tai_page_load(TaiNetwork *network, const TaiUrl *url,
                       const char *default_css, double viewport_width,
                       double viewport_height, bool rtl, char **error) {
    return tai_page_load_request(network, url, NULL, NULL, default_css,
                                 viewport_width, viewport_height, rtl, error);
}

bool tai_page_replace_from_intent(TaiNetwork *network, TaiPage **page,
                                  const TaiNavigationIntent *intent,
                                  const char *default_css, bool rtl,
                                  char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!network || !page || !*page || !intent || !intent->url ||
        !default_css)
        return diagnostic(error, "invalid page navigation input");
    TaiUrl *url = tai_url_parse(intent->url);
    if (!url) return diagnostic(error, "navigation URL allocation failed");
    TaiPage *candidate = tai_page_load_request(
        network, url, tai_page_url(*page), intent->body, default_css,
        tai_page_viewport_width(*page), tai_page_viewport_height(*page), rtl,
        error);
    tai_url_destroy(url);
    if (!candidate) return false;
    TaiPage *old_page = *page;
    *page = candidate;
    tai_page_destroy(old_page);
    return true;
}

void tai_page_destroy(TaiPage *page) {
    if (!page) return;
    tai_navigation_intent_destroy(page->navigation);
    free(page->fragment_change);
    tai_url_destroy(page->fragment_previous_url);
    tai_display_list_destroy(page->display);
    tai_layout_destroy(page->layout);
    tai_js_destroy(page->javascript);
    tai_css_destroy(page->styles);
    tai_document_destroy(page->document);
    tai_url_destroy(page->url);
    free(page);
}

bool tai_page_take_navigation_intent(TaiPage *page,
                                     TaiNavigationIntent **intent) {
    if (!page || !intent) return false;
    *intent = page->navigation;
    page->navigation = NULL;
    return true;
}

bool tai_page_take_fragment_change(TaiPage *page, char **url) {
    if (!page || !url) return false;
    *url = page->fragment_change;
    page->fragment_change = NULL;
    return true;
}

bool tai_page_fragment_url_changed(const TaiPage *page) {
    return page && page->fragment_previous_url &&
        strcmp(tai_url_string(page->fragment_previous_url),
               tai_url_string(page->url)) != 0;
}

void tai_page_finish_fragment_change(TaiPage *page, bool committed) {
    if (!page || !page->fragment_previous_url) return;
    if (!committed) {
        tai_url_destroy(page->url);
        page->url = page->fragment_previous_url;
        page->scroll_y = page->fragment_previous_scroll;
    } else {
        tai_url_destroy(page->fragment_previous_url);
    }
    page->fragment_previous_url = NULL;
}

static bool set_navigation_intent(TaiPage *page, const TaiUrl *url,
                                  const char *body, char **error) {
    if (!page || !url) return true;
    TaiNavigationIntent *intent = calloc(1, sizeof(*intent));
    if (!intent) return diagnostic(error, "navigation intent allocation failed");
    intent->url = tai_strdup(tai_url_string(url));
    if (body) intent->body = tai_strdup(body);
    if (!intent->url || (body && !intent->body)) {
        tai_navigation_intent_destroy(intent);
        return diagnostic(error, "navigation intent allocation failed");
    }
    tai_navigation_intent_destroy(page->navigation);
    page->navigation = intent;
    return true;
}

TaiNode *tai_page_root(const TaiPage *page) {
    return page ? tai_document_root(page->document) : NULL;
}
const TaiLayout *tai_page_layout(const TaiPage *page) { return page ? page->layout : NULL; }
const TaiDisplayList *tai_page_display_list(const TaiPage *page) {
    return page ? page->display : NULL;
}
bool tai_page_resize(TaiPage *page, double viewport_width,
                     double viewport_height, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (!page || !page->document || !page->styles ||
        !isfinite(viewport_width) || !isfinite(viewport_height) ||
        viewport_width <= 0.0 || viewport_height <= 0.0 ||
        viewport_width > INT_MAX || viewport_height > INT_MAX)
        return diagnostic(error, "invalid page resize input");
    if (page->viewport_width == viewport_width &&
        page->viewport_height == viewport_height)
        return true;

    ScrollSnapshot snapshot = {0};
    if (!node_count(tai_document_root(page->document), &snapshot.count) ||
        snapshot.count > SIZE_MAX / sizeof(*snapshot.nodes) ||
        snapshot.count > SIZE_MAX / sizeof(*snapshot.scroll_y) ||
        !(snapshot.nodes = malloc(snapshot.count * sizeof(*snapshot.nodes))) ||
        !(snapshot.scroll_y = malloc(snapshot.count * sizeof(*snapshot.scroll_y)))) {
        snapshot_destroy(&snapshot);
        return diagnostic(error, "page resize allocation failed");
    }
    size_t snapshot_index = 0;
    snapshot_scroll(tai_document_root(page->document), &snapshot, &snapshot_index);

    TaiLayout *replacement_layout = tai_layout_create(
        tai_document_root(page->document), viewport_width, page->rtl, error);
    if (!replacement_layout) {
        restore_scroll(&snapshot);
        snapshot_destroy(&snapshot);
        return false;
    }
    TaiDisplayList *replacement_display = tai_display_list_create(
        replacement_layout, error);
    if (!replacement_display) {
        tai_layout_destroy(replacement_layout);
        restore_scroll(&snapshot);
        snapshot_destroy(&snapshot);
        return false;
    }

    TaiDisplayList *old_display = page->display;
    TaiLayout *old_layout = page->layout;
    page->layout = replacement_layout;
    page->display = replacement_display;
    page->viewport_width = viewport_width;
    page->viewport_height = viewport_height;
    tai_display_list_destroy(old_display);
    tai_layout_destroy(old_layout);
    snapshot_destroy(&snapshot);
    return true;
}
double tai_page_viewport_width(const TaiPage *page) {
    return page ? page->viewport_width : 0.0;
}
double tai_page_viewport_height(const TaiPage *page) {
    return page ? page->viewport_height : 0.0;
}
TaiNode *tai_page_hit_test(const TaiPage *page, double x, double y,
                           TaiDisplayHit *hit) {
    TaiDisplayHit found = {0};
    if (hit) *hit = found;
    if (!page || !page->document ||
        !tai_display_list_hit_test(page->display, x, y, &found))
        return NULL;
    TaiNode *node = tai_document_node(page->document, found.node_id);
    if (!node) return NULL;
    if (hit) *hit = found;
    return node;
}
double tai_page_scroll_y(const TaiPage *page) {
    return page ? page->scroll_y : 0.0;
}
double tai_page_max_scroll_y(const TaiPage *page) {
    if (!page || !page->layout) return 0.0;
    double document_height = tai_layout_height(page->layout) +
                             2.0 * TAI_PAGE_VERTICAL_STEP;
    return fmax(0.0, document_height - page->viewport_height);
}
bool tai_page_set_scroll_y(TaiPage *page, double scroll_y) {
    if (!page || !isfinite(scroll_y)) return false;
    page->scroll_y = fmax(0.0, fmin(scroll_y, tai_page_max_scroll_y(page)));
    return true;
}

/* A DOM mutation cannot be rolled back, but a frame replacement can: keep the
 * old layout/display pair alive until a self-contained replacement exists.
 * Styling can also fail after changing individual computed-style maps, so the
 * dirty bit remains set until the complete visual frame is available. */
static bool rebuild_dirty_page(TaiPage *page, char **error) {
    ScrollSnapshot snapshot = {0};
    TaiNode *root = tai_document_root(page->document);
    if (!node_count(root, &snapshot.count) ||
        snapshot.count > SIZE_MAX / sizeof(*snapshot.nodes) ||
        snapshot.count > SIZE_MAX / sizeof(*snapshot.scroll_y) ||
        !(snapshot.nodes = malloc(snapshot.count * sizeof(*snapshot.nodes))) ||
        !(snapshot.scroll_y = malloc(snapshot.count * sizeof(*snapshot.scroll_y)))) {
        snapshot_destroy(&snapshot);
        return diagnostic(error, "page frame rebuild allocation failed");
    }
    size_t snapshot_index = 0;
    snapshot_scroll(root, &snapshot, &snapshot_index);

    if (!tai_css_style(root, page->styles, error)) {
        snapshot_destroy(&snapshot);
        return false;
    }
    TaiLayout *replacement_layout = tai_layout_create(
        root, page->viewport_width, page->rtl, error);
    if (!replacement_layout) {
        restore_scroll(&snapshot);
        snapshot_destroy(&snapshot);
        return false;
    }
    TaiDisplayList *replacement_display = tai_display_list_create(
        replacement_layout, error);
    if (!replacement_display) {
        tai_layout_destroy(replacement_layout);
        restore_scroll(&snapshot);
        snapshot_destroy(&snapshot);
        return false;
    }

    TaiDisplayList *old_display = page->display;
    TaiLayout *old_layout = page->layout;
    page->layout = replacement_layout;
    page->display = replacement_display;
    page->scroll_y = fmax(0.0, fmin(page->scroll_y, tai_page_max_scroll_y(page)));
    page->dirty = false;
    tai_display_list_destroy(old_display);
    tai_layout_destroy(old_layout);
    snapshot_destroy(&snapshot);
    return true;
}

static bool ascii_case_equal(const char *left, const char *right) {
    if (!left || !right) return false;
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++, b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return !*left && !*right;
}

static bool text_input_node(const TaiNode *node) {
    if (!node || node->kind != TAI_ELEMENT || strcmp(node->tag, "input"))
        return false;
    const char *type = tai_map_get(&node->attributes, "type");
    return !type || (!ascii_case_equal(type, "hidden") &&
                     strcmp(type, "checkbox"));
}

static size_t utf8_count(const char *text) {
    size_t count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        utf8proc_int32_t codepoint;
        utf8proc_ssize_t used = utf8proc_iterate(p, -1, &codepoint);
        p += used > 0 ? (size_t)used : 1;
        if (count == SIZE_MAX) return SIZE_MAX;
        count++;
    }
    return count;
}

static size_t utf8_offset(const char *text, size_t index) {
    size_t offset = 0;
    while (text[offset] && index) {
        utf8proc_int32_t codepoint;
        utf8proc_ssize_t used = utf8proc_iterate(
            (const utf8proc_uint8_t *)text + offset, -1, &codepoint);
        offset += used > 0 ? (size_t)used : 1;
        index--;
    }
    return offset;
}

/* SDL text is untrusted UTF-8. Mirror the oracle's errors=ignore plus control
 * filtering, but cap the public string boundary before allocation. */
static char *filtered_input(const char *text, size_t *codepoints, char **error) {
    enum { TAI_TEXT_INPUT_MAX = 4096 };
    if (codepoints) *codepoints = 0;
    if (!text) return tai_strdup("");
    size_t length = 0;
    while (text[length]) {
        if (length == TAI_TEXT_INPUT_MAX) {
            diagnostic(error, "text input exceeds 4096 bytes");
            return NULL;
        }
        length++;
    }
    char *result = malloc(length + 1);
    if (!result) {
        diagnostic(error, "text input allocation failed");
        return NULL;
    }
    size_t source = 0, output = 0, count = 0;
    while (source < length) {
        utf8proc_int32_t codepoint;
        utf8proc_ssize_t used = utf8proc_iterate(
            (const utf8proc_uint8_t *)text + source,
            (utf8proc_ssize_t)(length - source), &codepoint);
        if (used < 1) { source++; continue; }
        if (codepoint >= 0x20) {
            memcpy(result + output, text + source, (size_t)used);
            output += (size_t)used;
            count++;
        }
        source += (size_t)used;
    }
    result[output] = '\0';
    if (codepoints) *codepoints = count;
    return result;
}

static bool replace_input_value(TaiNode *node, size_t start, size_t end,
                                const char *replacement, size_t replacement_count,
                                char **error) {
    const char *value = tai_map_get(&node->attributes, "value");
    if (!value) value = "";
    size_t count = utf8_count(value);
    if (count == SIZE_MAX) return diagnostic(error, "invalid input value");
    if (start > count) start = count;
    if (end < start) end = start;
    if (end > count) end = count;
    size_t left = utf8_offset(value, start);
    size_t right = utf8_offset(value, end);
    size_t replacement_length = strlen(replacement);
    size_t value_length = strlen(value);
    if (left > SIZE_MAX - replacement_length ||
        left + replacement_length > SIZE_MAX - (value_length - right) - 1)
        return diagnostic(error, "input value too large");
    if (start > SIZE_MAX - replacement_count)
        return diagnostic(error, "input cursor overflow");
    size_t length = left + replacement_length + value_length - right;
    char *next = malloc(length + 1);
    if (!next) return diagnostic(error, "input value allocation failed");
    memcpy(next, value, left);
    memcpy(next + left, replacement, replacement_length);
    memcpy(next + left + replacement_length, value + right,
           value_length - right + 1);
    bool ok = tai_map_set(&node->attributes, "value", next, 0);
    free(next);
    if (!ok) return diagnostic(error, "input value allocation failed");
    node->cursor_index = start + replacement_count;
    return true;
}

static bool rebuild_if_changed(TaiPage *page, bool local_changed,
                               bool *changed, char **error) {
    if (!page->dirty && !local_changed) return true;
    if (!rebuild_dirty_page(page, error)) return false;
    if (changed) *changed = true;
    return true;
}

enum { TAI_FORM_DATA_MAX = 32 * 1024 * 1024 };

static bool form_safe_byte(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' ||
           byte == '-' || byte == '~';
}

static char *quote_plus(const char *value, char **error) {
    static const char hex[] = "0123456789ABCDEF";
    if (!value) value = "";
    size_t length = strlen(value);
    if (length > (SIZE_MAX - 1) / 3) {
        diagnostic(error, "form field is too large");
        return NULL;
    }
    char *encoded = malloc(length * 3 + 1);
    if (!encoded) {
        diagnostic(error, "form field allocation failed");
        return NULL;
    }
    size_t output = 0;
    for (size_t index = 0; index < length; index++) {
        unsigned char byte = (unsigned char)value[index];
        if (form_safe_byte(byte)) encoded[output++] = (char)byte;
        else if (byte == ' ') encoded[output++] = '+';
        else {
            encoded[output++] = '%';
            encoded[output++] = hex[byte >> 4];
            encoded[output++] = hex[byte & 15];
        }
    }
    encoded[output] = '\0';
    return encoded;
}

static bool form_append(char **body, size_t *length, const char *part,
                        char **error) {
    size_t part_length = strlen(part);
    if (part_length > TAI_FORM_DATA_MAX - *length ||
        *length + part_length == TAI_FORM_DATA_MAX) {
        return diagnostic(error, "encoded form data exceeds 32 MiB");
    }
    char *next = realloc(*body, *length + part_length + 1);
    if (!next) return diagnostic(error, "form data allocation failed");
    memcpy(next + *length, part, part_length + 1);
    *body = next;
    *length += part_length;
    return true;
}

static bool encode_form_inputs(const TaiNode *node, char **body,
                               size_t *length, char **error) {
    if (node->kind == TAI_ELEMENT && !strcmp(node->tag, "input")) {
        const char *name = tai_map_get(&node->attributes, "name");
        if (name) {
            const char *type = tai_map_get(&node->attributes, "type");
            bool checkbox = type && !strcmp(type, "checkbox");
            if (!checkbox || node->checked) {
                const char *value = tai_map_get(&node->attributes, "value");
                if (checkbox && !value) value = "on";
                if (!value) value = "";
                char *encoded_name = quote_plus(name, error);
                char *encoded_value = quote_plus(value, error);
                if (!encoded_name || !encoded_value) {
                    free(encoded_name);
                    free(encoded_value);
                    return false;
                }
                bool ok = (!*length || form_append(body, length, "&", error)) &&
                          form_append(body, length, encoded_name, error) &&
                          form_append(body, length, "=", error) &&
                          form_append(body, length, encoded_value, error);
                free(encoded_name);
                free(encoded_value);
                if (!ok) return false;
            }
        }
    }
    for (size_t index = 0; index < node->child_count; index++)
        if (!encode_form_inputs(node->children[index], body, length, error))
            return false;
    return true;
}

static char *encode_form_data(const TaiNode *form, char **error) {
    char *body = tai_strdup("");
    size_t length = 0;
    if (!body) {
        diagnostic(error, "form data allocation failed");
        return NULL;
    }
    if (!encode_form_inputs(form, &body, &length, error)) {
        free(body);
        return NULL;
    }
    return body;
}

static TaiNode *action_form_ancestor(TaiNode *node) {
    for (; node; node = node->parent)
        if (node->kind == TAI_ELEMENT && !strcmp(node->tag, "form") &&
            tai_map_get(&node->attributes, "action"))
            return node;
    return NULL;
}

static char *join_url_and_query(const TaiUrl *url, const char *body,
                                char **error) {
    const char *url_text = tai_url_string(url);
    const char *path = tai_url_path(url);
    const char *separator = strchr(path, '?') ? "&" : "?";
    size_t url_length = strlen(url_text);
    size_t separator_length = strlen(separator);
    size_t body_length = strlen(body);
    if (url_length > TAI_FORM_DATA_MAX ||
        separator_length > TAI_FORM_DATA_MAX - url_length ||
        body_length > TAI_FORM_DATA_MAX - url_length - separator_length) {
        diagnostic(error, "form navigation URL exceeds 32 MiB");
        return NULL;
    }
    size_t length = url_length + separator_length + body_length;
    char *text = malloc(length + 1);
    if (!text) {
        diagnostic(error, "form navigation URL allocation failed");
        return NULL;
    }
    memcpy(text, url_text, url_length);
    memcpy(text + url_length, separator, separator_length);
    memcpy(text + url_length + separator_length, body, body_length);
    text[length] = '\0';
    return text;
}

static bool submit_form(TaiPage *page, TaiNode *form, char **error) {
    bool prevented = false;
    if (!tai_js_dispatch_event(page->javascript, "submit", form, &prevented,
                              error))
        return false;
    if (prevented) return true;

    char *body = encode_form_data(form, error);
    if (!body) return false;
    const char *action = tai_map_get(&form->attributes, "action");
    TaiUrl *target = tai_url_resolve(page->url, action ? action : "");
    if (!target) {
        free(body);
        return true;
    }
    const char *method = tai_map_get(&form->attributes, "method");
    bool post = method && ascii_case_equal(method, "post");
    bool ok;
    if (post) {
        ok = set_navigation_intent(page, target, body, error);
    } else {
        char *get_text = join_url_and_query(target, body, error);
        TaiUrl *get_target = get_text ? tai_url_parse(get_text) : NULL;
        free(get_text);
        if (!get_target) {
            free(body);
            tai_url_destroy(target);
            return diagnostic(error, "form GET URL allocation failed");
        }
        ok = set_navigation_intent(page, get_target, NULL, error);
        tai_url_destroy(get_target);
    }
    free(body);
    tai_url_destroy(target);
    return ok;
}

static TaiNode *find_id_node(TaiNode *node, const char *id) {
    if (node->kind == TAI_ELEMENT) {
        const char *node_id = tai_map_get(&node->attributes, "id");
        if (node_id && !strcmp(node_id, id)) return node;
    }
    for (size_t index = 0; index < node->child_count; index++) {
        TaiNode *found = find_id_node(node->children[index], id);
        if (found) return found;
    }
    return NULL;
}

typedef struct {
    const TaiNode *target;
    double y;
    bool found;
} FragmentPosition;

static bool find_fragment_position(const TaiLayoutItem *item, void *opaque) {
    FragmentPosition *position = opaque;
    for (const TaiNode *node = item->node; node; node = node->parent) {
        if (node == position->target) {
            if (!position->found || item->y < position->y)
                position->y = item->y;
            position->found = true;
            break;
        }
    }
    return true;
}

static bool scroll_to_fragment(TaiPage *page, const char *fragment,
                              char **error) {
    if (!page || !fragment || !*fragment) return true;
    TaiNode *node = find_id_node(tai_document_root(page->document), fragment);
    if (!node) return true;
    FragmentPosition position = {.target = node};
    if (!tai_layout_visit(page->layout, find_fragment_position, &position,
                          error))
        return false;
    if (position.found) tai_page_set_scroll_y(page, position.y);
    return true;
}

static bool apply_fragment_url(TaiPage *page, TaiUrl *target,
                               bool *changed, char **error) {
    if (page->fragment_previous_url) {
        tai_url_destroy(target);
        return diagnostic(error, "pending fragment change must be consumed");
    }
    char *signal = tai_strdup(tai_url_string(target));
    if (!signal) {
        tai_url_destroy(target);
        return diagnostic(error, "fragment URL allocation failed");
    }
    double previous_scroll = page->scroll_y;
    const char *fragment = tai_url_fragment(target);
    if (!scroll_to_fragment(page, fragment, error)) {
        free(signal);
        tai_url_destroy(target);
        return false;
    }
    tai_url_destroy(page->fragment_previous_url);
    page->fragment_previous_url = page->url;
    page->fragment_previous_scroll = previous_scroll;
    free(page->fragment_change);
    page->fragment_change = signal;
    page->url = target;
    if (changed) *changed = true;
    return true;
}

TaiNode *tai_page_viewport_hit_test(const TaiPage *page, double x, double y,
                                    TaiDisplayHit *hit) {
    if (!page || !isfinite(y)) {
        if (hit) *hit = (TaiDisplayHit){0};
        return NULL;
    }
    return tai_page_hit_test(page, x, y + page->scroll_y, hit);
}

bool tai_page_activate_viewport(TaiPage *page, double x, double y,
                                bool *changed, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (changed) *changed = false;
    if (!page || !page->document || !page->javascript || !page->styles ||
        !page->layout || !page->display)
        return diagnostic(error, "invalid page activation input");
    if (!isfinite(x) || !isfinite(y)) return true;

    /* Python blurs before hit testing or dispatch, so a prevented activation
     * still visibly clears an already focused control. */
    bool frame_changed = false;
    if (page->focused) {
        page->focused->focused = false;
        page->focused = NULL;
        frame_changed = true;
    }

    TaiNode *target = tai_page_viewport_hit_test(page, x, y, NULL);
    while (target && target->kind != TAI_ELEMENT) target = target->parent;
    if (!target) {
        if (frame_changed && !rebuild_dirty_page(page, error)) return false;
        if (changed) *changed = frame_changed;
        return true;
    }

    bool prevented = false;
    if (!tai_js_dispatch_event(page->javascript, "click", target, &prevented,
                               error))
        return false;
    if (!prevented) {
        TaiNode *button = NULL;
        for (TaiNode *node = target; node; node = node->parent)
            if (node->kind == TAI_ELEMENT && !strcmp(node->tag, "button")) {
                button = node;
                break;
            }
        if (button) {
            TaiNode *form = action_form_ancestor(button);
            if (form && !submit_form(page, form, error)) return false;
        } else {
            bool handled_input = false;
            for (TaiNode *node = target; node; node = node->parent) {
                if (node->kind != TAI_ELEMENT || strcmp(node->tag, "input"))
                    continue;
                handled_input = true;
                const char *type = tai_map_get(&node->attributes, "type");
                if (type && !strcmp(type, "checkbox")) {
                    node->checked = !node->checked;
                } else {
                    page->focused = node;
                    node->focused = true;
                    const char *value =
                        tai_map_get(&node->attributes, "value");
                    size_t caret = utf8_count(value ? value : "");
                    if (caret == SIZE_MAX)
                        return diagnostic(error, "input cursor overflow");
                    (void)tai_layout_control_caret_index(page->layout, node->id,
                                                         x, &caret);
                    node->cursor_index = caret;
                }
                frame_changed = true;
                break;
            }
            if (!handled_input) {
                for (TaiNode *node = target; node; node = node->parent) {
                    if (node->kind != TAI_ELEMENT || strcmp(node->tag, "a"))
                        continue;
                    const char *href = tai_map_get(&node->attributes, "href");
                    if (href) {
                        TaiUrl *resolved = tai_url_resolve(page->url, href);
                        if (resolved && href[0] == '#') {
                            if (!apply_fragment_url(page, resolved,
                                                    &frame_changed, error))
                                return false;
                            resolved = NULL;
                        }
                        else if (resolved &&
                                 strcmp(tai_url_scheme(resolved), "mailto") &&
                                 !set_navigation_intent(page, resolved, NULL,
                                                       error)) {
                            tai_url_destroy(resolved);
                            return false;
                        }
                        tai_url_destroy(resolved);
                    }
                    break;
                }
            }
        }
    }
    if (!page->dirty && !frame_changed) return true;
    if (!rebuild_dirty_page(page, error)) return false;
    if (changed) *changed = true;
    return true;
}

bool tai_page_blur_input(TaiPage *page, bool *changed, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (changed) *changed = false;
    if (!page || !page->document || !page->display)
        return diagnostic(error, "invalid page blur input");
    if (!page->focused) return true;
    TaiNode *previously_focused = page->focused;
    previously_focused->focused = false;
    page->focused = NULL;
    if (!rebuild_dirty_page(page, error)) {
        previously_focused->focused = true;
        page->focused = previously_focused;
        return false;
    }
    if (changed) *changed = true;
    return true;
}

bool tai_page_text_input(TaiPage *page, const char *text, bool *changed,
                         char **error) {
    if (error) { free(*error); *error = NULL; }
    if (changed) *changed = false;
    if (!page || !page->document || !page->javascript || !page->layout ||
        !page->display || !page->styles)
        return diagnostic(error, "invalid page text input");
    if (!text_input_node(page->focused)) return true;

    size_t inserted = 0;
    char *filtered = filtered_input(text, &inserted, error);
    if (!filtered) return false;
    if (!*filtered) { free(filtered); return true; }
    bool prevented = false;
    if (!tai_js_dispatch_event(page->javascript, "keydown", page->focused,
                               &prevented, error)) {
        free(filtered);
        return false;
    }
    bool local_changed = false;
    if (!prevented) {
        const char *value = tai_map_get(&page->focused->attributes, "value");
        size_t cursor = utf8_count(value ? value : "");
        if (page->focused->cursor_index < cursor)
            cursor = page->focused->cursor_index;
        if (!replace_input_value(page->focused, cursor, cursor, filtered,
                                 inserted, error)) {
            free(filtered);
            return false;
        }
        local_changed = true;
    }
    free(filtered);
    return rebuild_if_changed(page, local_changed, changed, error);
}

bool tai_page_key(TaiPage *page, TaiPageKey key, bool *changed, char **error) {
    if (error) { free(*error); *error = NULL; }
    if (changed) *changed = false;
    if (!page || !page->document || !page->layout || !page->display ||
        !page->styles)
        return diagnostic(error, "invalid page key input");
    if (!text_input_node(page->focused)) return true;
    const char *value = tai_map_get(&page->focused->attributes, "value");
    size_t count = utf8_count(value ? value : "");
    if (count == SIZE_MAX) return diagnostic(error, "invalid input value");
    size_t cursor = page->focused->cursor_index;
    if (cursor > count) cursor = count;
    bool local_changed = false;
    if (key == TAI_PAGE_KEY_RETURN) {
        if (page->focused->kind == TAI_ELEMENT &&
            !strcmp(page->focused->tag, "input")) {
            TaiNode *form = action_form_ancestor(page->focused->parent);
            if (form && !submit_form(page, form, error)) return false;
        }
    } else if (key == TAI_PAGE_KEY_BACKSPACE) {
        if (cursor && !replace_input_value(page->focused, cursor - 1, cursor,
                                           "", 0, error))
            return false;
        local_changed = cursor != 0;
    } else if (key == TAI_PAGE_KEY_LEFT) {
        page->focused->cursor_index = cursor ? cursor - 1 : 0;
        /* The oracle requests a frame even for a clamped arrow key. */
        local_changed = true;
    } else if (key == TAI_PAGE_KEY_RIGHT) {
        page->focused->cursor_index = cursor < count ? cursor + 1 : count;
        local_changed = true;
    } else {
        return true;
    }
    return rebuild_if_changed(page, local_changed, changed, error);
}

bool tai_page_text_input_active(const TaiPage *page) {
    return page && text_input_node(page->focused);
}

bool tai_page_write_viewport_png(const TaiPage *page, const char *path,
                                 char **error) {
    if (!page) {
        if (error) {
            free(*error);
            *error = tai_strdup("invalid page PNG output input");
        }
        return false;
    }
    return tai_display_list_write_png_region(
        page->display, path, (int)ceil(page->viewport_width),
        (int)ceil(page->viewport_height), 0.0, page->scroll_y, error);
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
