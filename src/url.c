#include "tai/url.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

struct TaiUrl {
    char *scheme, *host, *path, *fragment, *key, *serialized, *origin, *port_text;
    long long port;
    bool view_source;
};

static char *join(const char *a, const char *b, const char *c) {
    size_t x = strlen(a), y = strlen(b), z = strlen(c);
    if (x > SIZE_MAX - y || x + y > SIZE_MAX - z - 1) return NULL;
    char *s = malloc(x + y + z + 1);
    if (s) { memcpy(s, a, x); memcpy(s + x, b, y); memcpy(s + x + y, c, z + 1); }
    return s;
}
static bool replace(char **dest, const char *src) {
    char *copy = tai_strdup(src);
    if (!copy) return false;
    free(*dest); *dest = copy; return true;
}
static bool whitespace(utf8proc_int32_t c) {
    return (c >= 9 && c <= 13) || (c >= 0x1c && c <= 0x20) || c == 0x85 ||
        c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
        c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}
static char *strip(const char *text) {
    size_t len = strlen(text), first = 0, last = 0;
    for (size_t i = 0; i < len;) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)text + i,
                                            (utf8proc_ssize_t)(len - i), &cp);
        if (n < 0) { n = 1; cp = (unsigned char)text[i]; }
        if (!whitespace(cp)) last = i + (size_t)n;
        else if (first == i) first += (size_t)n;
        i += (size_t)n;
    }
    return tai_strndup(text + first, last > first ? last - first : 0);
}
/* Python int accepts Unicode decimal digits, but rejects ASCII information
 * separators even though str.strip accepts them. Return -1 only for OOM. */
static int decimal_port(const char *text, char **result) {
    size_t len = strlen(text), used = 0;
    char *digits = malloc(len + 2);
    if (!digits) return -1;
    bool negative = false, started = false, trailing = false, previous_digit = false;
    for (size_t i = 0; i < len;) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)text + i, (utf8proc_ssize_t)(len-i), &cp);
        if (n < 0) goto invalid;
        i += (size_t)n;
        if (whitespace(cp) && !(cp >= 0x1c && cp <= 0x1f)) {
            if (started) trailing = true;
            continue;
        }
        if (trailing) goto invalid;
        if (!started && (cp == '+' || cp == '-')) { negative = cp == '-'; started = true; continue; }
        started = true;
        if (cp == '_') { if (!previous_digit) goto invalid; previous_digit = false; continue; }
        if (utf8proc_category(cp) != UTF8PROC_CATEGORY_ND) goto invalid;
        /* Nd blocks consist of contiguous groups of ten ordered digits. */
        utf8proc_int32_t first = cp;
        while (first > 0 && utf8proc_category(first-1) == UTF8PROC_CATEGORY_ND) first--;
        digits[used++] = (char)('0' + (cp-first)%10);
        previous_digit = true;
    }
    /* Match the reference interpreter's default decimal conversion guard. */
    if (!used || !previous_digit || used > 4300) goto invalid;
    size_t first = 0;
    while (first + 1 < used && digits[first] == '0') first++;
    size_t count = used-first;
    bool sign = negative && !(count == 1 && digits[first] == '0');
    memmove(digits + (sign ? 1 : 0), digits + first, count);
    if (sign) digits[0] = '-';
    digits[count + (sign ? 1 : 0)] = '\0';
    *result = digits;
    return 1;
invalid:
    free(digits); return 0;
}

void tai_url_destroy(TaiUrl *u) {
    if (!u) return;
    free(u->scheme); free(u->host); free(u->path); free(u->fragment);
    free(u->port_text); free(u->key); free(u->serialized); free(u->origin); free(u);
}
bool tai_url_is_http(const TaiUrl *u) {
    return u && (!strcmp(u->scheme, "http") || !strcmp(u->scheme, "https"));
}
static bool supported(const char *s) {
    return !strcmp(s, "http") || !strcmp(s, "https") || !strcmp(s, "file") ||
        !strcmp(s, "data") || !strcmp(s, "about") || !strcmp(s, "mailto");
}
static bool finalize(TaiUrl *u) {
    char *port = join(":", u->port_text, "");
    if (!port) return false;
    char *prefix = join(u->scheme, "://", u->host);
    if (!prefix) { free(port); return false; }
    u->origin = join(prefix, port, "");
    if (!u->origin) { free(port); free(prefix); return false; }
    char *base = NULL;
    if (!strcmp(u->scheme, "mailto")) base = join("mailto:", u->path, "");
    else if (u->view_source) base = join("view-source:", u->key, "");
    else if (!strcmp(u->scheme, "about")) base = join("about:", u->path, "");
    else if (!strcmp(u->scheme, "data")) base = join("data:", u->path, "");
    else if (!strcmp(u->scheme, "file")) base = join("file://", u->path, "");
    else {
        if ((!strcmp(u->scheme, "http") && u->port == 80) ||
            (!strcmp(u->scheme, "https") && u->port == 443)) port[0] = '\0';
        base = join(prefix, port, u->path);
    }
    free(port); free(prefix);
    if (!base) return false;
    u->serialized = join(base, *u->fragment && strcmp(u->scheme, "mailto") ? "#" : "",
                         strcmp(u->scheme, "mailto") ? u->fragment : "");
    free(base);
    return u->serialized != NULL;
}
TaiUrl *tai_url_parse(const char *text) {
    if (!text) return NULL;
    TaiUrl *u = calloc(1, sizeof(*u));
    char *input = tai_strdup(text);
    if (!u || !input) { free(u); free(input); return NULL; }
    u->scheme = tai_strdup(""); u->host = tai_strdup("");
    u->path = tai_strdup(""); u->fragment = tai_strdup("");
    u->key = tai_strdup(text); u->port_text = tai_strdup("0");
    if (!u->scheme || !u->host || !u->path || !u->fragment || !u->key || !u->port_text) goto oom;
    char *s = input;
    if (!strncmp(s, "view-source:", 12)) { u->view_source = true; s += 12; }
    char *fragment = strchr(s, '#');
    if (fragment) {
        *fragment++ = '\0';
        if (!replace(&u->fragment, fragment)) goto oom;
    }
    if (!strncmp(s, "about:", 6) || !strncmp(s, "mailto:", 7)) {
        char *colon = strchr(s, ':');
        if (!replace(&u->key, s)) goto oom;
        *colon++ = '\0';
        if (!replace(&u->scheme, s) || !replace(&u->path, colon)) goto oom;
        goto done;
    }
    if (!strncmp(s, "data:", 5)) {
        if (!replace(&u->scheme, "data") || !replace(&u->path, s + 5) ||
            !replace(&u->key, s)) goto oom;
        goto done;
    }
    char *delimiter = strstr(s, "://");
    if (!delimiter) goto fallback;
    *delimiter = '\0';
    if (!replace(&u->scheme, s)) goto oom;
    s = delimiter + 3;
    if (!supported(u->scheme)) goto fallback;
    if (!strcmp(u->scheme, "http")) u->port = 80;
    else if (!strcmp(u->scheme, "https")) u->port = 443;
    if (tai_url_is_http(u)) {
        if (!replace(&u->port_text, u->port == 80 ? "80" : "443")) goto oom;
        char *slash = strchr(s, '/');
        if (slash) {
            if (!replace(&u->path, slash)) goto oom;
            *slash = '\0';
        } else if (!replace(&u->path, "/")) goto oom;
        if (!replace(&u->host, s)) goto oom;
        char *colon = strchr(u->host, ':');
        if (colon) {
            *colon++ = '\0';
            char *number = NULL;
            int status = decimal_port(colon, &number);
            if (status < 0) goto oom;
            if (!status) goto fallback;
            free(u->port_text); u->port_text = number;
            errno = 0;
            u->port = strtoll(number, NULL, 10);
            if (errno == ERANGE) u->port = LLONG_MIN;
        }
        char *port = join(":", u->port_text, "");
        if (!port) goto oom;
        char *origin = join(u->scheme, "://", u->host);
        if (!origin) { free(port); goto oom; }
        char *with_port = join(origin, port, u->path);
        free(origin); free(port);
        if (!with_port) goto oom;
        free(u->key); u->key = with_port;
    } else {
        if (!strcmp(u->scheme, "file") && !replace(&u->path, s)) goto oom;
        if (!replace(&u->key, s)) goto oom;
    }
    goto done;
fallback:
    /* Oracle retains parsed host/port/fragment even when it falls back. */
    if (!replace(&u->scheme, "about") || !replace(&u->path, "blank") ||
        !replace(&u->key, "about:blank")) goto oom;
done:
    free(input);
    if (finalize(u)) return u;
    tai_url_destroy(u); return NULL;
oom:
    free(input); tai_url_destroy(u); return NULL;
}

static TaiUrl *clone_url(const TaiUrl *base) {
    TaiUrl *copy = calloc(1, sizeof(*copy));
    if (!copy) return NULL;
    copy->port = base->port; copy->view_source = base->view_source;
    char **dest[] = {&copy->scheme,&copy->host,&copy->path,&copy->fragment,&copy->key,&copy->serialized,&copy->origin,&copy->port_text};
    const char *src[] = {base->scheme,base->host,base->path,base->fragment,base->key,base->serialized,base->origin,base->port_text};
    for (size_t i=0;i<8;i++) {
        *dest[i] = tai_strdup(src[i]);
        if (!*dest[i]) { tai_url_destroy(copy); return NULL; }
    }
    return copy;
}

TaiUrl *tai_url_resolve(const TaiUrl *base, const char *relative) {
    if (!base || !relative) return NULL;
    char *s = strip(relative), *text = NULL;
    if (!s) return NULL;
    if (!*s) { free(s); return clone_url(base); }
    if (*s == '#') {
        const char *end = strchr(base->serialized, '#');
        char *prefix = end ? tai_strndup(base->serialized, (size_t)(end - base->serialized)) : tai_strdup(base->serialized);
        if (prefix) text = join(prefix, s[1] ? s : "", "");
        free(prefix);
    } else if (!strncmp(s, "//", 2)) text = join(base->scheme, ":", s);
    else {
        char *colon = strchr(s, ':'), *slash = strchr(s, '/');
        if (colon && (!slash || colon < slash)) {
            char *scheme = tai_strndup(s, (size_t)(colon - s));
            if (!scheme) { free(s); return NULL; }
            utf8proc_uint8_t *folded = NULL;
            if (utf8proc_map((const utf8proc_uint8_t *)scheme, 0, &folded,
                             UTF8PROC_NULLTERM | UTF8PROC_CASEFOLD | UTF8PROC_STABLE) < 0) {
                free(scheme); free(s); return NULL;
            }
            free(scheme); scheme = (char *)folded;
            if (supported(scheme) || !strcmp(scheme, "view-source")) text = tai_strdup(s);
            free(scheme);
        } else if (*s == '/') {
            if (tai_url_is_http(base)) text = join(base->origin, s, "");
            else if (!strcmp(base->scheme, "file")) text = join("file://", s, "");
        } else {
            const char *last = strrchr(base->path, '/');
            if (last) {
                char *dir = tai_strndup(base->path, (size_t)(last - base->path));
                const char *tail = s;
                if (dir) {
                    while (!strncmp(tail, "../", 3)) {
                        tail += 3;
                        char *parent = strrchr(dir, '/');
                        if (parent) *parent = '\0';
                    }
                    char *path = join(dir, "/", tail);
                    if (path) {
                        char *prefix = tai_url_is_http(base) ? tai_strdup(base->origin) : join(base->scheme, "://", base->host);
                        if (prefix) text = join(prefix, path, "");
                        free(prefix);
                    }
                    free(path); free(dir);
                }
            }
        }
    }
    free(s);
    TaiUrl *result = text ? tai_url_parse(text) : NULL;
    free(text); return result;
}
const char *tai_url_scheme(const TaiUrl *u) { return u->scheme; }
const char *tai_url_host(const TaiUrl *u) { return u->host; }
const char *tai_url_path(const TaiUrl *u) { return u->path; }
const char *tai_url_fragment(const TaiUrl *u) { return u->fragment; }
const char *tai_url_key(const TaiUrl *u) { return u->key; }
const char *tai_url_string(const TaiUrl *u) { return u->serialized; }
const char *tai_url_origin(const TaiUrl *u) { return u->origin; }
long long tai_url_port(const TaiUrl *u) { return u->port; }
bool tai_url_view_source(const TaiUrl *u) { return u->view_source; }
void tai_url_json(FILE *out, const TaiUrl *u) {
    const char *names[] = {"scheme", "host", "path", "fragment", "url_string", "serialized", "origin"};
    const char *values[] = {u->scheme, u->host, u->path, u->fragment, u->key, u->serialized, u->origin};
    fputc('{', out);
    for (size_t i = 0; i < 7; i++) {
        if (i) fputc(',', out);
        tai_json_string(out, names[i]); fputc(':', out); tai_json_string(out, values[i]);
    }
    fprintf(out, ",\"port\":%s,\"view_source\":%s}", u->port_text, u->view_source ? "true" : "false");
}
