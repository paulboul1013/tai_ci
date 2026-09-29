#define _POSIX_C_SOURCE 200809L
#include "tai/network.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
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
/* Two threads share one jar while a third uses it through a network: the
 * jar's mutex must keep every read consistent (ASan catches races that
 * corrupt the list; TSan is not in the toolchain). */
typedef struct {
  TaiCookieJar *jar;
  const char *host;
} JarWorker;
static void *jar_worker(void *opaque) {
  JarWorker *worker = opaque;
  for (int i = 0; i < 20000; i++) {
    char value[64];
    snprintf(value, sizeof(value), "n=%d; SameSite=Lax", i);
    assert(tai_cookie_jar_js_set(worker->jar, worker->host, value));
    char *read = tai_cookie_jar_js_get(worker->jar, worker->host);
    assert(read && !strncmp(read, "n=", 2) && strstr(read, "samesite=lax"));
    free(read);
    if (i % 100 == 0)
      assert(tai_cookie_jar_http_set(worker->jar, worker->host,
                                     "n=http; Expires=Thu, 01 Jan 1970 "
                                     "00:00:00 GMT"));
  }
  return NULL;
}
static void shared_jar(void) {
  TaiCookieJar *jar = tai_cookie_jar_create();
  assert(jar);
  TaiNetwork *n = tai_network_create_with_jar(jar);
  assert(n && tai_network_cookie_jar(n) == jar);
  JarWorker a = {jar, "a.test"}, b = {jar, "b.test"};
  pthread_t ta, tb;
  assert(!pthread_create(&ta, NULL, jar_worker, &a));
  assert(!pthread_create(&tb, NULL, jar_worker, &b));
  for (int i = 0; i < 20000; i++) {
    char *read = tai_network_cookie_get(n, i % 2 ? "a.test" : "b.test");
    assert(read);
    free(read);
  }
  assert(!pthread_join(ta, NULL) && !pthread_join(tb, NULL));
  /* The network borrows the jar: destroying it leaves the jar usable. */
  tai_network_destroy(n);
  assert(tai_cookie_jar_http_set(jar, "c.test", "sid=1; HttpOnly"));
  char *hidden = tai_cookie_jar_js_get(jar, "c.test");
  assert(hidden && !*hidden);
  free(hidden);
  assert(tai_cookie_jar_js_set(jar, "c.test", "sid=2"));
  tai_cookie_jar_destroy(jar);
}
static void count_done(void *user, TaiResponse *r) {
  (*(int *)user)++;
  tai_response_destroy(r);
}
/* A restricted poll leaves other completions for the outer poll unless
 * they were marked nested. */
static void restricted_poll(void) {
  TaiNetwork *n = tai_network_create();
  TaiUrl *u = tai_url_parse("data:text/plain,x");
  assert(n && u);
  int other = 0, nested = 0;
  assert(tai_network_submit(n, u, NULL, NULL, NULL, NULL, count_done, &other));
  TaiRequest *marked =
      tai_network_submit(n, u, NULL, NULL, NULL, NULL, count_done, &nested);
  assert(marked);
  tai_network_allow_nested(marked);
  TaiWaitStatus status = TAI_WAIT_FAILED;
  TaiResponse *r = tai_network_request_until(n, u, NULL, NULL, NULL, NULL, 0.0,
                                             NULL, NULL, &status);
  assert(r && status == TAI_WAIT_DONE && !strcmp(r->body, "x"));
  tai_response_destroy(r);
  assert(other == 0 && nested == 1 && tai_network_pending(n) == 1);
  assert(tai_network_poll_nested(n, 0) && other == 0);
  assert(tai_network_poll(n, 0) && other == 1 && !tai_network_pending(n));
  tai_url_destroy(u);
  tai_network_destroy(n);
}
/* A peer that accepts but never answers: only cancellation or the total
 * limit ends the wait. */
static int service_calls;
static bool give_up_after_three(void *user) {
  (void)user;
  return ++service_calls < 3;
}
static double now_seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void stalled_requests(void) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  struct sockaddr_in address = {.sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  socklen_t length = sizeof(address);
  assert(!bind(listener, (struct sockaddr *)&address, sizeof(address)) &&
         !listen(listener, 8) &&
         !getsockname(listener, (struct sockaddr *)&address, &length));
  char text[64];
  snprintf(text, sizeof(text), "http://127.0.0.1:%d/", ntohs(address.sin_port));
  TaiNetwork *n = tai_network_create();
  TaiUrl *u = tai_url_parse(text);
  assert(n && u);
  TaiWaitStatus status = TAI_WAIT_DONE;
  assert(!tai_network_request_until(n, u, NULL, NULL, NULL, NULL, 0.0,
                                    give_up_after_three, NULL, &status));
  assert(status == TAI_WAIT_CANCELLED && service_calls == 3 &&
         !tai_network_pending(n));
  double started = now_seconds();
  assert(!tai_network_request_until(n, u, NULL, NULL, NULL, NULL, 0.3, NULL,
                                    NULL, &status));
  double waited = now_seconds() - started;
  assert(status == TAI_WAIT_TIMED_OUT && waited >= 0.29 && waited < 2.0 &&
         !tai_network_pending(n));
  tai_url_destroy(u);
  tai_network_destroy(n);
  close(listener);
}
int main(int argc, char **argv) {
  TaiNetwork *n = tai_network_create();
  assert(n);
  /* --ca FILE trusts a test CA for the single-request mode below. */
  if (argc > 2 && !strcmp(argv[1], "--ca")) {
    assert(tai_network_set_ca_file(n, argv[2]));
    argv[2] = argv[0];
    argv += 2;
    argc -= 2;
  }
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
    printf(",\"certificate_error\":%s",
           r->certificate_error ? "true" : "false");
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
    shared_jar();
    restricted_poll();
    stalled_requests();
  }
  tai_url_destroy(u);
  tai_network_destroy(n);
  return 0;
}
