#include "tai/session.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

struct TaiSession {
    TaiNetwork *network;
    const char *default_css;
    TaiPage *page;
    char **urls;
    size_t count, index;
    bool rtl;
};

static bool failure(char **error, const char *message) {
    if (error && !*error) *error = tai_strdup(message);
    return false;
}

TaiSession *tai_session_create(TaiNetwork *network, TaiPage *initial_page,
                               const char *default_css, bool rtl) {
    if (!network || !initial_page || !default_css) return NULL;
    TaiSession *session = calloc(1, sizeof(*session));
    if (!session) return NULL;
    session->urls = calloc(1, sizeof(*session->urls));
    if (session->urls)
        session->urls[0] = tai_strdup(tai_url_string(tai_page_url(initial_page)));
    if (!session->urls || !session->urls[0]) {
        free(session->urls);
        free(session);
        return NULL;
    }
    session->network = network;
    session->default_css = default_css;
    session->page = initial_page;
    session->rtl = rtl;
    session->count = 1;
    return session;
}

TaiSession *tai_session_create_empty(const char *default_css, bool rtl) {
    if (!default_css) return NULL;
    TaiSession *session = calloc(1, sizeof(*session));
    if (!session) return NULL;
    session->default_css = default_css;
    session->rtl = rtl;
    return session;
}

void tai_session_destroy(TaiSession *session) {
    if (!session) return;
    tai_page_destroy(session->page);
    for (size_t i = 0; i < session->count; i++) free(session->urls[i]);
    free(session->urls);
    free(session);
}
TaiPage *tai_session_page(const TaiSession *session) { return session ? session->page : NULL; }
size_t tai_session_history_length(const TaiSession *session) { return session ? session->count : 0; }
size_t tai_session_history_index(const TaiSession *session) { return session ? session->index : 0; }
char *tai_session_history_url(const TaiSession *session, size_t index) {
    return session && index < session->count
        ? tai_strdup(session->urls[index]) : NULL;
}

static bool append_url(TaiSession *session, const char *url, TaiPage *candidate,
                       char **error) {
    size_t kept = session->count ? session->index + 1 : 0;
    if (kept >= SIZE_MAX / sizeof(char *))
        return failure(error, "history capacity exceeded");
    char *copy = tai_strdup(url);
    if (!copy) return failure(error, "history URL allocation failed");
    size_t next_count = kept + 1;
    char **next = malloc(next_count * sizeof(*next));
    if (!next) { free(copy); return failure(error, "history allocation failed"); }
    if (kept) memcpy(next, session->urls, kept * sizeof(*next));
    next[kept] = copy;
    for (size_t i = kept; i < session->count; i++) free(session->urls[i]);
    free(session->urls);
    session->urls = next;
    session->count = next_count;
    session->index = kept;
    if (candidate) {
        TaiPage *old = session->page;
        session->page = candidate;
        tai_page_destroy(old);
    }
    return true;
}

bool tai_session_navigate(TaiSession *session,
                          const TaiNavigationIntent *intent, char **error) {
    if (!session || !session->network || !session->page || !intent)
        return failure(error, "invalid session navigation");
    const char *text = tai_navigation_intent_url(intent);
    TaiUrl *url = text ? tai_url_parse(text) : NULL;
    if (!url) return failure(error, "navigation URL allocation failed");
    const char *body = tai_navigation_intent_body(intent);
    TaiPage *candidate = tai_page_load_request(
        session->network, url, tai_page_url(session->page), body,
        session->default_css, tai_page_viewport_width(session->page),
        tai_page_viewport_height(session->page), session->rtl, error);
    tai_url_destroy(url);
    if (!candidate) return false;
    bool ok = append_url(session, tai_url_string(tai_page_url(candidate)),
                         candidate, error);
    if (!ok) tai_page_destroy(candidate);
    return ok;
}

static bool address_space(utf8proc_int32_t codepoint) {
    return (codepoint >= 9 && codepoint <= 13) ||
           (codepoint >= 0x1c && codepoint <= 0x20) || codepoint == 0x85 ||
           codepoint == 0xa0 || codepoint == 0x1680 ||
           (codepoint >= 0x2000 && codepoint <= 0x200a) ||
           codepoint == 0x2028 || codepoint == 0x2029 ||
           codepoint == 0x202f || codepoint == 0x205f || codepoint == 0x3000;
}

static char *trim_address(const char *text) {
    size_t length = strlen(text), first = 0, last = 0;
    for (size_t index = 0; index < length;) {
        utf8proc_int32_t codepoint = 0;
        utf8proc_ssize_t width = utf8proc_iterate(
            (const utf8proc_uint8_t *)text + index,
            (utf8proc_ssize_t)(length - index), &codepoint);
        if (width < 0) {
            width = 1;
            codepoint = (unsigned char)text[index];
        }
        if (!address_space(codepoint)) last = index + (size_t)width;
        else if (first == index) first += (size_t)width;
        index += (size_t)width;
    }
    size_t result_length = last > first ? last - first : 0;
    char *result = malloc(result_length + 1);
    if (!result) return NULL;
    memcpy(result, text + first, result_length);
    result[result_length] = '\0';
    return result;
}

static bool is_address_url(const char *text) {
    return strstr(text, "://") != NULL ||
           !strncmp(text, "about:", 6) || !strncmp(text, "data:", 5) ||
           !strncmp(text, "file:", 5) ||
           !strncmp(text, "view-source:", 12) ||
           !strncmp(text, "mailto:", 7);
}

static bool query_byte(unsigned char value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '.' ||
           value == '-' || value == '~';
}

static bool explicit_about_blank(const char *text) {
    if (!strncmp(text, "view-source:", 12)) text += 12;
    return !strncmp(text, "about:blank", 11) &&
           (text[11] == '\0' || text[11] == '#' || text[11] == '?' ||
            text[11] == '/');
}

static char *search_address(const char *text) {
    static const char prefix[] = "https://html.duckduckgo.com/html/?q=";
    size_t input_length = strlen(text);
    if (input_length > (SIZE_MAX - sizeof(prefix)) / 3) return NULL;
    size_t capacity = sizeof(prefix) - 1 + input_length * 3 + 1;
    char *result = malloc(capacity);
    if (!result) return NULL;
    size_t output = sizeof(prefix) - 1;
    memcpy(result, prefix, output);
    static const char hex[] = "0123456789ABCDEF";
    for (size_t index = 0; index < input_length; index++) {
        unsigned char value = (unsigned char)text[index];
        if (query_byte(value)) {
            result[output++] = (char)value;
        } else if (value == ' ') {
            result[output++] = '+';
        } else {
            result[output++] = '%';
            result[output++] = hex[value >> 4];
            result[output++] = hex[value & 0x0f];
        }
    }
    result[output] = '\0';
    return result;
}

char *tai_session_normalize_address(const char *text) {
    if (!text) return NULL;
    char *trimmed = trim_address(text);
    if (!trimmed) return NULL;
    bool allows_blank = explicit_about_blank(trimmed);
    char *candidate = is_address_url(trimmed) ? tai_strdup(trimmed)
                                              : search_address(trimmed);
    free(trimmed);
    if (!candidate) return NULL;
    TaiUrl *url = tai_url_parse(candidate);
    free(candidate);
    if (!url) return NULL;
    /* tai_url_parse intentionally falls back to about:blank for unsupported
     * and malformed syntax. An address submission rejects that fallback so
     * the user's text can never silently replace the current page with blank. */
    if (!strcmp(tai_url_scheme(url), "about") &&
        !strcmp(tai_url_path(url), "blank") && !allows_blank) {
        tai_url_destroy(url);
        return NULL;
    }
    char *normalized = tai_strdup(tai_url_string(url));
    tai_url_destroy(url);
    return normalized;
}

bool tai_session_navigate_address(TaiSession *session, const char *text,
                                  char **error) {
    if (!session || !session->network || !session->page || !text)
        return failure(error, "invalid address navigation");
    char *normalized = tai_session_normalize_address(text);
    if (!normalized)
        return failure(error, "address URL is malformed or unsupported");
    TaiUrl *url = tai_url_parse(normalized);
    free(normalized);
    if (!url) return failure(error, "address URL allocation failed");
    if (!strcmp(tai_url_scheme(url), "mailto")) {
        tai_url_destroy(url);
        return failure(error, "mailto requires an external application");
    }
    TaiPage *candidate = tai_page_load_request(
        session->network, url, tai_page_url(session->page), NULL,
        session->default_css,
        tai_page_viewport_width(session->page),
        tai_page_viewport_height(session->page), session->rtl, error);
    tai_url_destroy(url);
    if (!candidate) return false;
    bool ok = append_url(session, tai_url_string(tai_page_url(candidate)),
                         candidate, error);
    if (!ok) tai_page_destroy(candidate);
    return ok;
}

bool tai_session_record_fragment(TaiSession *session, const char *url,
                                  char **error) {
    if (!session || !session->page || !url ||
        strcmp(url, tai_url_string(tai_page_url(session->page))))
        return failure(error, "invalid fragment history URL");
    return append_url(session, url, NULL, error);
}

bool tai_session_history_target(const TaiSession *session, int direction,
                                char **url, size_t *target_index) {
    if (url) *url = NULL;
    if (!session || !url || !target_index || !session->count ||
        (direction != -1 && direction != 1))
        return false;
    size_t target = direction < 0
        ? (session->index ? session->index - 1 : session->index)
        : session->index + 1;
    if ((direction < 0 && session->index == 0) || target >= session->count)
        return false;
    *url = tai_strdup(session->urls[target]);
    if (!*url) return false;
    *target_index = target;
    return true;
}

bool tai_session_commit_navigation(TaiSession *session, TaiPage *candidate,
                                   char **error) {
    const TaiUrl *url = candidate ? tai_page_url(candidate) : NULL;
    if (!session || !url)
        return failure(error, "invalid loaded-page commit");
    return append_url(session, tai_url_string(url), candidate, error);
}

bool tai_session_commit_history(TaiSession *session, TaiPage *candidate,
                                size_t target_index, char **error) {
    const TaiUrl *url = candidate ? tai_page_url(candidate) : NULL;
    if (!session || !session->count || !url || target_index >= session->count ||
        strcmp(tai_url_string(url), session->urls[target_index]))
        return failure(error, "invalid history-page commit");
    TaiPage *old = session->page;
    session->page = candidate;
    session->index = target_index;
    tai_page_destroy(old);
    return true;
}

static bool traverse(TaiSession *session, size_t target, char **error) {
    if (!session->network || !session->page)
        return failure(error, "session has no loaded page");
    TaiUrl *url = tai_url_parse(session->urls[target]);
    if (!url) return failure(error, "history URL allocation failed");
    TaiPage *candidate = tai_page_load_request(
        session->network, url, tai_page_url(session->page), NULL,
        session->default_css, tai_page_viewport_width(session->page),
        tai_page_viewport_height(session->page), session->rtl, error);
    tai_url_destroy(url);
    if (!candidate) return false;
    TaiPage *old = session->page;
    session->page = candidate;
    session->index = target;
    tai_page_destroy(old);
    return true;
}
bool tai_session_back(TaiSession *session, char **error) {
    if (!session) return failure(error, "invalid session");
    return session->index ? traverse(session, session->index - 1, error) : true;
}
bool tai_session_forward(TaiSession *session, char **error) {
    if (!session) return failure(error, "invalid session");
    return session->index + 1 < session->count
        ? traverse(session, session->index + 1, error) : true;
}
