#include "tai/network.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static int calls;
static void done(void *user, TaiResponse *r) {
  (void)user;
  assert(r && !r->error);
  calls++;
  tai_response_destroy(r);
}
static TaiResponse *fetch_path(TaiNetwork *n, const char *base,
                               const char *path, const char *payload,
                               const TaiUrl *ref) {
  char address[512];
  snprintf(address, sizeof(address), "%s%s", base, path);
  TaiUrl *u = tai_url_parse(address);
  assert(u);
  TaiResponse *r = tai_network_request(n, u, ref, payload, NULL, NULL);
  tai_url_destroy(u);
  assert(r && !r->error);
  return r;
}
static void integration(TaiNetwork *n, const char *base) {
  TaiResponse *a = fetch_path(n, base, "/cache", NULL, NULL);
  TaiResponse *b = fetch_path(n, base, "/cache", NULL, NULL);
  assert(!strcmp(a->body, b->body));
  tai_response_destroy(a);
  tai_response_destroy(b);
  a = fetch_path(n, base, "/no-store", NULL, NULL);
  b = fetch_path(n, base, "/no-store", NULL, NULL);
  assert(strcmp(a->body, b->body));
  tai_response_destroy(a);
  tai_response_destroy(b);
  a = fetch_path(n, base, "/set-cookie", NULL, NULL);
  tai_response_destroy(a);
  char *cookie = tai_network_cookie_get(n, "127.0.0.1");
  assert(cookie && !*cookie);
  free(cookie);
  assert(tai_network_cookie_set(n, "127.0.0.1", "session=changed"));
  a = fetch_path(n, base, "/echo", NULL, NULL);
  assert(strstr(a->body, "session=secret"));
  tai_response_destroy(a);
  TaiUrl *other = tai_url_parse("http://other.invalid/");
  assert(other);
  a = fetch_path(n, base, "/echo", "post", other);
  assert(!strstr(a->body, "session=secret"));
  tai_response_destroy(a);
  tai_url_destroy(other);
  a = fetch_path(n, base, "/expire-cookie", NULL, NULL);
  tai_response_destroy(a);
  a = fetch_path(n, base, "/echo", NULL, NULL);
  assert(!strstr(a->body, "session=secret"));
  tai_response_destroy(a);
  TaiUrl *parent = tai_url_parse(base);
  assert(parent);
  TaiUrl *u = tai_url_resolve(parent, "/echo");
  tai_url_destroy(parent);
  assert(u);
  TaiRequest *cancel =
      tai_network_submit(n, u, NULL, NULL, NULL, NULL, done, NULL);
  assert(cancel);
  tai_network_cancel(n, cancel);
  for (int i = 0; i < 4; i++)
    assert(tai_network_submit(n, u, NULL, NULL, NULL, NULL, done, NULL));
  while (tai_network_pending(n))
    assert(tai_network_poll(n, 100));
  assert(calls == 4);
  tai_url_destroy(u);
}
int main(int argc, char **argv) {
  TaiNetwork *n = tai_network_create();
  assert(n);
  if (argc == 3 && !strcmp(argv[2], "--suite")) {
    integration(n, argv[1]);
    tai_network_destroy(n);
    return 0;
  }
  TaiUrl *u =
      tai_url_parse(argc > 1 ? argv[1] : "data:text/html,hello%20world");
  assert(u);
  if (argc > 1) {
    TaiUrl *ref = argc > 3 ? tai_url_parse(argv[3]) : NULL;
    TaiResponse *r = tai_network_request(
        n, u, ref, argc > 2 && strcmp(argv[2], "-") ? argv[2] : NULL, NULL,
        argc > 4 ? argv[4] : NULL);
    assert(r);
    printf("{\"status\":%ld,\"body\":", r->status);
    tai_json_string(stdout, r->body ? r->body : "");
    fputs(",\"error\":", stdout);
    tai_json_string(stdout, r->error ? r->error : "");
    fputs(",\"headers\":{", stdout);
    for (size_t i = 0; i < r->headers.count; i++) {
      if (i)
        putchar(',');
      tai_json_string(stdout, r->headers.items[i].key);
      putchar(':');
      tai_json_string(stdout, r->headers.items[i].value);
    }
    fputs("}}\n", stdout);
    tai_response_destroy(r);
    tai_url_destroy(ref);
  } else {
    TaiResponse *r = tai_network_request(n, u, NULL, NULL, NULL, NULL);
    assert(r && !r->error && !strcmp(r->body, "hello world"));
    tai_response_destroy(r);
    assert(tai_network_cookie_set(n, "a", "x=1; SameSite=Lax"));
    char *s = tai_network_cookie_get(n, "a");
    assert(s && !strcmp(s, "x=1; samesite=lax"));
    free(s);
    assert(tai_network_cookie_set(n, "a", "x=2; HttpOnly"));
    s = tai_network_cookie_get(n, "a");
    assert(s && !strcmp(s, "x=1; samesite=lax"));
    free(s);
    assert(tai_network_cookie_set(
        n, "a", "x=gone; Expires=Thu, 01 Jan 1970 00:00:00 GMT"));
    s = tai_network_cookie_get(n, "a");
    assert(s && !strcmp(s, ""));
    free(s);
    TaiRequest *q =
        tai_network_submit(n, u, NULL, NULL, NULL, NULL, done, NULL);
    assert(q);
    assert(calls == 0);
    tai_network_cancel(n, q);
    assert(tai_network_pending(n) == 0);
    assert(tai_network_submit(n, u, NULL, NULL, NULL, NULL, done, NULL));
    assert(tai_network_poll(n, 0));
    assert(calls == 1);
  }
  tai_url_destroy(u);
  tai_network_destroy(n);
  return 0;
}
