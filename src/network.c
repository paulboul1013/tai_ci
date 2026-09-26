#include "tai/network.h"
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utf8proc.h>

#define BODY_LIMIT (32u * 1024u * 1024u)
#define HEADER_LIMIT (128u * 1024u)

typedef struct Cookie {
  char *host, *value;
  TaiMap params;
  time_t expires;
  bool has_expiry;
  struct Cookie *next;
} Cookie;
typedef struct Cache {
  char *key;
  TaiResponse *response;
  time_t expires;
  struct Cache *next;
} Cache;
struct TaiRequest {
  TaiNetwork *network;
  TaiUrl *url, *referrer;
  char *payload, *origin, *policy;
  TaiNetworkDone done;
  void *userdata;
  CURL *easy;
  struct curl_slist *headers;
  TaiResponse *response;
  size_t capacity, header_bytes;
  unsigned redirects;
  bool ready, active;
  char curl_error[CURL_ERROR_SIZE];
  struct TaiRequest *next;
};
struct TaiNetwork {
  CURLM *multi;
  TaiRequest *requests;
  Cookie *cookies;
  Cache *cache;
  char *ca_file;
  size_t pending;
};

static char *trim_copy(const char *s, size_t n) {
  while (n && isspace((unsigned char)*s)) {
    s++;
    n--;
  }
  while (n && isspace((unsigned char)s[n - 1]))
    n--;
  return tai_strndup(s, n);
}
static void lower(char *s) {
  for (; *s; s++)
    *s = (char)tolower((unsigned char)*s);
}
static bool bad_header(const char *s) {
  return s && (strchr(s, '\r') || strchr(s, '\n'));
}
static TaiResponse *response_new(void) {
  return calloc(1, sizeof(TaiResponse));
}
void tai_response_destroy(TaiResponse *r) {
  if (!r)
    return;
  tai_map_clear(&r->headers);
  free(r->body);
  free(r->error);
  free(r);
}
static void fail(TaiRequest *q, const char *why) {
  if (!q->response->error)
    q->response->error = tai_strdup(why);
  q->ready = true;
}
static TaiResponse *response_copy(const TaiResponse *r) {
  TaiResponse *c = response_new();
  if (!c)
    return NULL;
  c->status = r->status;
  c->length = r->length;
  c->body = tai_strndup(r->body ? r->body : "", r->length);
  if (!c->body || !tai_map_copy(&c->headers, &r->headers)) {
    tai_response_destroy(c);
    return NULL;
  }
  return c;
}
static void cookie_free(Cookie *c) {
  free(c->host);
  free(c->value);
  tai_map_clear(&c->params);
  free(c);
}
static Cookie **cookie_slot(TaiNetwork *n, const char *host) {
  Cookie **p = &n->cookies;
  while (*p && strcmp((*p)->host, host))
    p = &(*p)->next;
  return p;
}
static Cookie *cookie_valid(TaiNetwork *n, const char *host) {
  Cookie **p = cookie_slot(n, host);
  if (*p && (*p)->has_expiry && (*p)->expires <= time(NULL)) {
    Cookie *c = *p;
    *p = c->next;
    cookie_free(c);
  }
  return *p;
}
static bool cookie_store(TaiNetwork *n, const char *host, const char *value,
                         bool js) {
  if (!host || !*host)
    return true;
  Cookie *old = cookie_valid(n, host);
  if (js && old && tai_map_get(&old->params, "httponly"))
    return true;
  Cookie *c = calloc(1, sizeof(*c));
  if (!c)
    return false;
  c->host = tai_strdup(host);
  const char *semi = strchr(value, ';');
  c->value = trim_copy(value, semi ? (size_t)(semi - value) : strlen(value));
  if (!c->host || !c->value) {
    cookie_free(c);
    return false;
  }
  const char *p = semi ? semi + 1 : "";
  while (*p) {
    const char *end = strchr(p, ';');
    if (!end)
      end = p + strlen(p);
    char *part = trim_copy(p, (size_t)(end - p));
    if (!part) {
      cookie_free(c);
      return false;
    }
    if (*part) {
      char *eq = strchr(part, '=');
      char *key, *val;
      if (eq) {
        *eq = '\0';
        key = trim_copy(part, strlen(part));
        val = trim_copy(eq + 1, strlen(eq + 1));
      } else {
        key = tai_strdup(part);
        val = tai_strdup("true");
      }
      if (!key || !val) {
        free(key);
        free(val);
        free(part);
        cookie_free(c);
        return false;
      }
      lower(key);
      if (!strcmp(key, "samesite"))
        lower(val);
      bool ok = tai_map_set(&c->params, key, val, 0);
      free(key);
      free(val);
      if (!ok) {
        free(part);
        cookie_free(c);
        return false;
      }
    }
    free(part);
    p = *end ? end + 1 : end;
  }
  if (js && (!*c->value || !strchr(c->value, '=') ||
             tai_map_get(&c->params, "httponly"))) {
    cookie_free(c);
    return true;
  }
  const char *expires = tai_map_get(&c->params, "expires");
  if (expires) {
    c->expires = curl_getdate(expires, NULL);
    c->has_expiry = c->expires != (time_t)-1;
  }
  Cookie **slot = cookie_slot(n, host);
  old = *slot;
  if (c->has_expiry && c->expires <= time(NULL)) {
    if (old) {
      *slot = old->next;
      cookie_free(old);
    }
    cookie_free(c);
    return true;
  }
  c->next = old ? old->next : NULL;
  *slot = c;
  if (old)
    cookie_free(old);
  return true;
}
bool tai_network_cookie_set(TaiNetwork *n, const char *host,
                            const char *value) {
  return n && value && cookie_store(n, host, value, true);
}
char *tai_network_cookie_get(TaiNetwork *n, const char *host) {
  if (!n || !host || !*host)
    return tai_strdup("");
  Cookie *c = cookie_valid(n, host);
  if (!c || tai_map_get(&c->params, "httponly"))
    return tai_strdup("");
  size_t len = strlen(c->value);
  for (size_t i = 0; i < c->params.count; i++)
    len +=
        3 + strlen(c->params.items[i].key) + strlen(c->params.items[i].value);
  char *s = malloc(len + 1);
  if (!s)
    return NULL;
  strcpy(s, c->value);
  for (size_t i = 0; i < c->params.count; i++) {
    strcat(s, "; ");
    strcat(s, c->params.items[i].key);
    if (strcmp(c->params.items[i].value, "true")) {
      strcat(s, "=");
      strcat(s, c->params.items[i].value);
    }
  }
  return s;
}
/* Keep valid UTF-8 unchanged; replace malformed input as Python
 * decode(errors='replace'). */
static char *decode_utf8(const char *s, size_t len, size_t *outlen) {
  if (len > SIZE_MAX / 3 - 1)
    return NULL;
  char *out = malloc(len * 3 + 1);
  if (!out)
    return NULL;
  size_t i = 0, j = 0;
  while (i < len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t count = utf8proc_iterate((const utf8proc_uint8_t *)s + i,
                                              (utf8proc_ssize_t)(len - i), &cp);
    if (count > 0) {
      memcpy(out + j, s + i, (size_t)count);
      j += (size_t)count;
      i += (size_t)count;
    } else {
      unsigned char lead = (unsigned char)s[i];
      size_t expected = lead >= 0xc2 && lead <= 0xdf   ? 2
                        : lead >= 0xe0 && lead <= 0xef ? 3
                        : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                       : 1;
      size_t used = 1;
      while (used < expected && i + used < len) {
        unsigned char c = (unsigned char)s[i + used];
        if (c < 0x80 || c > 0xbf)
          break;
        if (used == 1 &&
            ((lead == 0xe0 && c < 0xa0) || (lead == 0xed && c > 0x9f) ||
             (lead == 0xf0 && c < 0x90) || (lead == 0xf4 && c > 0x8f)))
          break;
        used++;
      }
      memcpy(out + j, "\xef\xbf\xbd", 3);
      j += 3;
      i += used;
    }
  }
  out[j] = '\0';
  *outlen = j;
  return out;
}
static size_t body_cb(char *data, size_t size, size_t count, void *user) {
  TaiRequest *q = user;
  if (size && count > SIZE_MAX / size)
    return 0;
  size_t bytes = size * count;
  if (bytes > BODY_LIMIT - q->response->length) {
    fail(q, "response body exceeds 32 MiB limit");
    return 0;
  }
  size_t need = q->response->length + bytes + 1;
  if (need > q->capacity) {
    size_t cap = q->capacity ? q->capacity : 4096;
    while (cap < need)
      cap *= 2;
    char *p = realloc(q->response->body, cap);
    if (!p) {
      fail(q, "out of memory");
      return 0;
    }
    q->response->body = p;
    q->capacity = cap;
  }
  memcpy(q->response->body + q->response->length, data, bytes);
  q->response->length += bytes;
  q->response->body[q->response->length] = '\0';
  return bytes;
}
static size_t header_cb(char *data, size_t size, size_t count, void *user) {
  TaiRequest *q = user;
  if (size && count > SIZE_MAX / size)
    return 0;
  size_t bytes = size * count;
  if (bytes > HEADER_LIMIT - q->header_bytes) {
    fail(q, "response headers exceed 128 KiB limit");
    return 0;
  }
  q->header_bytes += bytes;
  if (bytes >= 5 && !memcmp(data, "HTTP/", 5)) {
    tai_map_clear(&q->response->headers);
    return bytes;
  }
  const char *colon = memchr(data, ':', bytes);
  if (!colon)
    return bytes;
  char *key = trim_copy(data, (size_t)(colon - data));
  char *value = trim_copy(colon + 1, bytes - (size_t)(colon + 1 - data));
  if (!key || !value) {
    free(key);
    free(value);
    fail(q, "out of memory");
    return 0;
  }
  lower(key);
  bool ok = tai_map_set(&q->response->headers, key, value, 0);
  free(key);
  free(value);
  if (!ok) {
    fail(q, "out of memory");
    return 0;
  }
  return bytes;
}
static bool add_header(TaiRequest *q, const char *key, const char *value) {
  if (bad_header(value))
    return false;
  size_t len = strlen(key) + strlen(value) + 3;
  char *s = malloc(len);
  if (!s)
    return false;
  snprintf(s, len, "%s: %s", key, value);
  struct curl_slist *h = curl_slist_append(q->headers, s);
  free(s);
  if (!h)
    return false;
  q->headers = h;
  return true;
}
static void easy_clear(TaiRequest *q) {
  if (q->easy) {
    if (q->active)
      curl_multi_remove_handle(q->network->multi, q->easy);
    curl_easy_cleanup(q->easy);
    q->easy = NULL;
  }
  q->active = false;
  curl_slist_free_all(q->headers);
  q->headers = NULL;
}
static void request_free(TaiRequest *q) {
  easy_clear(q);
  tai_url_destroy(q->url);
  tai_url_destroy(q->referrer);
  free(q->payload);
  free(q->origin);
  free(q->policy);
  tai_response_destroy(q->response);
  free(q);
}
static Cache *cache_find(TaiNetwork *n, const char *key) {
  for (Cache *c = n->cache; c; c = c->next)
    if (!strcmp(c->key, key) && c->expires > time(NULL))
      return c;
  return NULL;
}
static bool start(TaiRequest *q) {
  TaiResponse *r = q->response;
  const char *scheme = tai_url_scheme(q->url);
  q->ready = false;
  if (!strcmp(scheme, "about")) {
    r->body = tai_strdup("");
    q->ready = true;
    return r->body != NULL;
  }
  if (!strcmp(scheme, "data")) {
    const char *p = strchr(tai_url_path(q->url), ',');
    p = p ? p + 1 : "";
    size_t len = strlen(p);
    if (len > BODY_LIMIT) {
      fail(q, "data URL exceeds 32 MiB limit");
      return true;
    }
    char *raw = malloc(len + 1);
    if (!raw)
      return false;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
      if (p[i] == '%' && i + 2 < len && isxdigit((unsigned char)p[i + 1]) &&
          isxdigit((unsigned char)p[i + 2])) {
        char hex[3] = {p[i + 1], p[i + 2], 0};
        raw[j++] = (char)strtoul(hex, NULL, 16);
        i += 2;
      } else
        raw[j++] = p[i];
    }
    r->body = decode_utf8(raw, j, &r->length);
    free(raw);
    q->ready = true;
    return r->body != NULL;
  }
  if (!strcmp(scheme, "mailto")) {
    r->body =
        tai_strdup("\n            <html>\n            <body>\n                "
                   "<h1>External mail link</h1>\n                <p>This link "
                   "should be opened by your mail application.</p>\n           "
                   " </body>\n            </html>\n            ");
    if (r->body)
      r->length = strlen(r->body);
    q->ready = true;
    return r->body != NULL;
  }
  if (!strcmp(scheme, "file")) {
    FILE *f = fopen(tai_url_path(q->url), "rb");
    if (!f) {
      int err = errno;
      const char *path = tai_url_path(q->url);
      size_t cap = strlen(path) * 2 + strlen(strerror(err)) + 256;
      r->body = malloc(cap);
      if (!r->body)
        return false;
      snprintf(
          r->body, cap,
          "\n                <html>\n                <body>\n                  "
          "  <h1>File not found</h1>\n                    <p>%s</p>\n          "
          "          <pre>[Errno %d] %s: '%s'</pre>\n                </body>\n "
          "               </html>\n                ",
          path, err, strerror(err), path);
      r->length = strlen(r->body);
    } else {
      char buf[8192];
      size_t count;
      while ((count = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (body_cb(buf, 1, count, q) != count)
          break;
      }
      if (ferror(f))
        fail(q, "file read failed");
      fclose(f);
      if (!r->body)
        r->body = tai_strdup("");
      if (r->body) {
        size_t offset = 0;
        while (offset < r->length) {
          utf8proc_int32_t cp;
          utf8proc_ssize_t step =
              utf8proc_iterate((const utf8proc_uint8_t *)r->body + offset,
                               (utf8proc_ssize_t)(r->length - offset), &cp);
          if (step <= 0) {
            fail(q, "file is not valid UTF-8");
            r->body[0] = '\0';
            r->length = 0;
            break;
          }
          offset += (size_t)step;
        }
      }
      /* Python text files perform universal newline conversion. */
      if (r->body) {
        size_t j = 0;
        for (size_t i = 0; i < r->length; i++) {
          char c = r->body[i];
          if (c == '\r') {
            if (i + 1 < r->length && r->body[i + 1] == '\n')
              i++;
            c = '\n';
          }
          r->body[j++] = c;
        }
        r->body[j] = '\0';
        r->length = j;
      }
    }
    q->ready = true;
    return r->body != NULL;
  }
  if (!tai_url_is_http(q->url)) {
    fail(q, "unsupported request scheme");
    return true;
  }
  if (!q->payload && !q->origin) {
    Cache *c = cache_find(q->network, tai_url_key(q->url));
    if (c) {
      TaiResponse *copy = response_copy(c->response);
      if (!copy)
        return false;
      tai_response_destroy(r);
      q->response = copy;
      q->ready = true;
      return true;
    }
  }
  q->easy = curl_easy_init();
  if (!q->easy)
    return false;
  if (!add_header(q, "Host", tai_url_host(q->url)) ||
      !add_header(q, "Connection", q->payload ? "close" : "keep-alive") ||
      !add_header(q, "User-Agent", "Tai_Gar/1.0"))
    return false;
  if (q->origin && !add_header(q, "Origin", q->origin))
    return false;
  if (q->referrer && tai_url_is_http(q->referrer) &&
      (!q->policy || strcmp(q->policy, "no-referrer")) &&
      (!q->policy || strcmp(q->policy, "same-origin") ||
       !strcmp(tai_url_origin(q->referrer), tai_url_origin(q->url)))) {
    const char *s = tai_url_string(q->referrer), *hash = strchr(s, '#');
    char *ref = tai_strndup(s, hash ? (size_t)(hash - s) : strlen(s));
    if (!ref)
      return false;
    bool ok = add_header(q, "Referer", ref);
    free(ref);
    if (!ok)
      return false;
  }
  Cookie *cookie = cookie_valid(q->network, tai_url_host(q->url));
  if (cookie) {
    const char *site = tai_map_get(&cookie->params, "samesite");
    bool allow = !(q->payload && q->referrer && site && !strcmp(site, "lax") &&
                   strcmp(tai_url_host(q->referrer), tai_url_host(q->url)));
    if (allow && !add_header(q, "Cookie", cookie->value))
      return false;
  }
  /* Disable libcurl's extra Accept and POST content type to retain oracle
   * headers. */
  if (!add_header(q, "Accept", "") || !add_header(q, "Expect", "") ||
      !add_header(q, "Content-Type", ""))
    return false;
#define SET(option, value)                                                     \
  do {                                                                         \
    if (curl_easy_setopt(q->easy, option, value) != CURLE_OK)                  \
      return false;                                                            \
  } while (0)
  SET(CURLOPT_URL, tai_url_key(q->url));
  SET(CURLOPT_PATH_AS_IS, 1L);
  SET(CURLOPT_HTTPHEADER, q->headers);
  SET(CURLOPT_ACCEPT_ENCODING, "gzip");
  SET(CURLOPT_FOLLOWLOCATION, 0L);
  SET(CURLOPT_PROTOCOLS_STR, "http,https");
  SET(CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
  SET(CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
  SET(CURLOPT_WRITEFUNCTION, body_cb);
  SET(CURLOPT_WRITEDATA, q);
  SET(CURLOPT_HEADERFUNCTION, header_cb);
  SET(CURLOPT_HEADERDATA, q);
  SET(CURLOPT_PRIVATE, q);
  SET(CURLOPT_ERRORBUFFER, q->curl_error);
  SET(CURLOPT_NOSIGNAL, 1L);
  SET(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  SET(CURLOPT_TIMEOUT_MS, 30000L);
  SET(CURLOPT_PROXY, "");
  if (q->network->ca_file)
    SET(CURLOPT_CAINFO, q->network->ca_file);
  if (q->payload) {
    SET(CURLOPT_POST, 1L);
    SET(CURLOPT_POSTFIELDS, q->payload);
    SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(q->payload));
  }
#undef SET
  if (curl_multi_add_handle(q->network->multi, q->easy) != CURLM_OK)
    return false;
  q->active = true;
  return true;
}
static void cache_store(TaiRequest *q) {
  if (q->payload || q->origin || q->response->status != 200)
    return;
  const char *cc = tai_map_get(&q->response->headers, "cache-control");
  if (!cc)
    return;
  char *copy = tai_strdup(cc);
  if (!copy)
    return;
  lower(copy);
  if (strstr(copy, "no-store")) {
    free(copy);
    return;
  }
  char *p = copy;
  long age = -1;
  while (*p) {
    char *end = strchr(p, ',');
    if (end)
      *end = '\0';
    while (isspace((unsigned char)*p))
      p++;
    if (!strncmp(p, "max-age=", 8)) {
      char *num = p + 8, *last = NULL;
      errno = 0;
      long parsed = strtol(num, &last, 10);
      while (last && isspace((unsigned char)*last))
        last++;
      if (isdigit((unsigned char)*num) && last && !*last && !errno &&
          parsed >= 0)
        age = parsed;
      break;
    }
    if (!end)
      break;
    p = end + 1;
  }
  free(copy);
  if (age < 0)
    return;
  Cache *entry = calloc(1, sizeof(*entry));
  if (!entry)
    return;
  entry->key = tai_strdup(tai_url_key(q->url));
  entry->response = response_copy(q->response);
  if (!entry->key || !entry->response) {
    free(entry->key);
    tai_response_destroy(entry->response);
    free(entry);
    return;
  }
  /* Bound expiry arithmetic; very large ages have the same practical lifetime.
   */
  if (age > 315360000L)
    age = 315360000L;
  entry->expires = time(NULL) + (time_t)age;
  size_t bytes = entry->response->length, count = 1;
  for (Cache *c = q->network->cache; c; c = c->next) {
    bytes += c->response->length;
    count++;
  }
  if (bytes > 64u * 1024u * 1024u || count > 64u) {
    while (q->network->cache) {
      Cache *old_entry = q->network->cache;
      q->network->cache = old_entry->next;
      free(old_entry->key);
      tai_response_destroy(old_entry->response);
      free(old_entry);
    }
  }
  Cache **slot = &q->network->cache;
  while (*slot && strcmp((*slot)->key, entry->key))
    slot = &(*slot)->next;
  Cache *old = *slot;
  entry->next = old ? old->next : NULL;
  *slot = entry;
  if (old) {
    free(old->key);
    tai_response_destroy(old->response);
    free(old);
  }
}
static void complete_transfer(TaiRequest *q, CURLcode code) {
  curl_easy_getinfo(q->easy, CURLINFO_RESPONSE_CODE, &q->response->status);
  if (code != CURLE_OK) {
    q->response->certificate_error = code == CURLE_PEER_FAILED_VERIFICATION ||
                                     code == CURLE_SSL_CACERT_BADFILE;
    fail(q, *q->curl_error ? q->curl_error : curl_easy_strerror(code));
    easy_clear(q);
    return;
  }
  easy_clear(q);
  const char *cookie = tai_map_get(&q->response->headers, "set-cookie");
  if (cookie &&
      !cookie_store(q->network, tai_url_host(q->url), cookie, false)) {
    fail(q, "out of memory");
    return;
  }
  const char *location = tai_map_get(&q->response->headers, "location");
  if (q->response->status >= 300 && q->response->status < 400 && location) {
    if (++q->redirects >= 10) {
      fail(q, "Redirect loop detected!");
      return;
    }
    char *dest = NULL;
    if (*location == '/') {
      size_t len = strlen(tai_url_scheme(q->url)) +
                   strlen(tai_url_host(q->url)) + strlen(location) + 4;
      dest = malloc(len);
      if (dest)
        snprintf(dest, len, "%s://%s%s", tai_url_scheme(q->url),
                 tai_url_host(q->url), location);
    } else
      dest = tai_strdup(location);
    TaiUrl *next = dest ? tai_url_parse(dest) : NULL;
    free(dest);
    if (!next) {
      fail(q, "out of memory");
      return;
    }
    if (!tai_url_is_http(next)) {
      tai_url_destroy(next);
      fail(q, "redirect target is not HTTP(S)");
      return;
    }
    TaiResponse *r = response_new();
    if (!r) {
      tai_url_destroy(next);
      fail(q, "out of memory");
      return;
    }
    tai_url_destroy(q->url);
    q->url = next;
    tai_response_destroy(q->response);
    q->response = r;
    q->capacity = 0;
    q->header_bytes = 0;
    q->curl_error[0] = '\0';
    if (!start(q))
      fail(q, "could not initialize redirected request");
    return;
  }
  size_t len = 0;
  char *text = decode_utf8(q->response->body ? q->response->body : "",
                           q->response->length, &len);
  if (!text) {
    fail(q, "out of memory");
    return;
  }
  free(q->response->body);
  q->response->body = text;
  q->response->length = len;
  cache_store(q);
  q->ready = true;
}
TaiNetwork *tai_network_create(void) {
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    return NULL;
  TaiNetwork *n = calloc(1, sizeof(*n));
  if (!n) {
    curl_global_cleanup();
    return NULL;
  }
  n->multi = curl_multi_init();
  if (!n->multi) {
    free(n);
    curl_global_cleanup();
    return NULL;
  }
  return n;
}
void tai_network_destroy(TaiNetwork *n) {
  if (!n)
    return;
  while (n->requests) {
    TaiRequest *q = n->requests;
    n->requests = q->next;
    request_free(q);
  }
  while (n->cookies) {
    Cookie *c = n->cookies;
    n->cookies = c->next;
    cookie_free(c);
  }
  while (n->cache) {
    Cache *c = n->cache;
    n->cache = c->next;
    free(c->key);
    tai_response_destroy(c->response);
    free(c);
  }
  curl_multi_cleanup(n->multi);
  free(n->ca_file);
  free(n);
  curl_global_cleanup();
}
bool tai_network_set_ca_file(TaiNetwork *n, const char *path) {
  if (!n)
    return false;
  char *copy = NULL;
  if (path && !(copy = tai_strdup(path)))
    return false;
  free(n->ca_file);
  n->ca_file = copy;
  return true;
}
TaiRequest *tai_network_submit(TaiNetwork *n, const TaiUrl *url,
                               const TaiUrl *referrer, const char *payload,
                               const char *origin, const char *policy,
                               TaiNetworkDone done, void *userdata) {
  if (!n || !url)
    return NULL;
  TaiRequest *q = calloc(1, sizeof(*q));
  if (!q)
    return NULL;
  q->network = n;
  q->done = done;
  q->userdata = userdata;
  q->response = response_new();
  q->url = tai_url_parse(tai_url_string(url));
  if (referrer)
    q->referrer = tai_url_parse(tai_url_string(referrer));
  if (payload)
    q->payload = tai_strdup(payload);
  if (origin)
    q->origin = tai_strdup(origin);
  if (policy)
    q->policy = tai_strdup(policy);
  if (!q->response || !q->url || (referrer && !q->referrer) ||
      (payload && !q->payload) || (origin && !q->origin) ||
      (policy && !q->policy)) {
    request_free(q);
    return NULL;
  }
  if (!start(q)) {
    request_free(q);
    return NULL;
  }
  q->next = n->requests;
  n->requests = q;
  n->pending++;
  return q;
}
void tai_network_cancel(TaiNetwork *n, TaiRequest *q) {
  if (!n || !q)
    return;
  TaiRequest **slot = &n->requests;
  while (*slot && *slot != q)
    slot = &(*slot)->next;
  if (*slot) {
    *slot = q->next;
    n->pending--;
    request_free(q);
  }
}
size_t tai_network_pending(const TaiNetwork *n) { return n ? n->pending : 0; }
bool tai_network_poll(TaiNetwork *n, int wait_ms) {
  if (!n)
    return false;
  int active = 0;
  if (curl_multi_perform(n->multi, &active) != CURLM_OK)
    return false;
  bool ready = false;
  for (TaiRequest *q = n->requests; q; q = q->next)
    if (q->ready)
      ready = true;
  if (active && !ready && wait_ms > 0) {
    if (curl_multi_poll(n->multi, NULL, 0, wait_ms, NULL) != CURLM_OK)
      return false;
    if (curl_multi_perform(n->multi, &active) != CURLM_OK)
      return false;
  }
  int remaining = 0;
  CURLMsg *message;
  while ((message = curl_multi_info_read(n->multi, &remaining))) {
    if (message->msg != CURLMSG_DONE)
      continue;
    TaiRequest *q = NULL;
    curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &q);
    if (q)
      complete_transfer(q, message->data.result);
  }
  /* Detach before calling application code, so callbacks can submit/cancel. */
  for (;;) {
    TaiRequest **slot = &n->requests;
    while (*slot && !(*slot)->ready)
      slot = &(*slot)->next;
    if (!*slot)
      break;
    TaiRequest *q = *slot;
    *slot = q->next;
    n->pending--;
    TaiResponse *r = q->response;
    q->response = NULL;
    TaiNetworkDone done = q->done;
    void *user = q->userdata;
    request_free(q);
    if (done)
      done(user, r);
    else
      tai_response_destroy(r);
  }
  return true;
}
static void sync_done(void *user, TaiResponse *response) {
  *(TaiResponse **)user = response;
}
TaiResponse *tai_network_request(TaiNetwork *n, const TaiUrl *url,
                                 const TaiUrl *referrer, const char *payload,
                                 const char *origin, const char *policy) {
  TaiResponse *response = NULL;
  TaiRequest *q = tai_network_submit(n, url, referrer, payload, origin, policy,
                                     sync_done, &response);
  if (!q)
    return NULL;
  while (!response) {
    if (!tai_network_poll(n, 100)) {
      tai_network_cancel(n, q);
      return NULL;
    }
  }
  return response;
}
