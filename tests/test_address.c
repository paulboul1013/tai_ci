#include "tai/session.h"
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *css =
    "html {display:block} body {display:block} p {display:block}";

static void expect_normalized(const char *input, const char *expected) {
  char *actual = tai_session_normalize_address(input);
  assert(actual && !strcmp(actual, expected));
  free(actual);
}

static void test_normalization(void) {
  expect_normalized("  https://example.invalid/path?q=a+b  ",
                    "https://example.invalid/path?q=a+b");
  expect_normalized("\tdata:text/html,<p>typed</p>\n",
                    "data:text/html,<p>typed</p>");
  expect_normalized("about:blank", "about:blank");
  expect_normalized("about:blank?x", "about:blank?x");
  expect_normalized("about:blank/path", "about:blank/path");
  expect_normalized("view-source:about:blank#part",
                    "view-source:about:blank#part");
  expect_normalized("mailto:user@example.invalid",
                    "mailto:user@example.invalid");
  expect_normalized("example.com/a b",
                    "https://html.duckduckgo.com/html/?q=example.com%2Fa+b");
  expect_normalized("javascript:alert(1)",
                    "https://html.duckduckgo.com/html/?q=javascript%3Aalert%281%29");
  expect_normalized("", "https://html.duckduckgo.com/html/?q=");
  expect_normalized("\351\233\252 x",
                    "https://html.duckduckgo.com/html/?q=%E9%9B%AA+x");
  assert(tai_session_normalize_address("ftp://example.invalid/file") == NULL);
  assert(tai_session_normalize_address("http://example.invalid:bad/") == NULL);
}

static void test_data_navigation_and_failure(void) {
  char *error = NULL;
  TaiNetwork *network = tai_network_create();
  TaiUrl *initial_url = tai_url_parse("data:text/html,<p>initial</p>");
  assert(network && initial_url);
  TaiPage *initial_page = tai_page_load(network, initial_url, css,
                                        320, 160, false, &error);
  assert(initial_page && !error);
  TaiSession *session = tai_session_create(network, initial_page, css, false);
  assert(session);
  tai_url_destroy(initial_url);

  assert(tai_session_navigate_address(
      session, "  data:text/html,<p>typed</p>  ", &error));
  assert(!error && tai_session_history_length(session) == 2 &&
         tai_session_history_index(session) == 1);
  assert(!strcmp(tai_url_string(tai_page_url(tai_session_page(session))),
                 "data:text/html,<p>typed</p>"));
  assert(tai_session_back(session, &error) && !error);
  assert(tai_session_history_index(session) == 0);
  assert(!strcmp(tai_url_string(tai_page_url(tai_session_page(session))),
                 "data:text/html,<p>initial</p>"));
  assert(tai_session_forward(session, &error) && !error);
  assert(tai_session_history_index(session) == 1);

  TaiPage *stable_page = tai_session_page(session);
  assert(!tai_session_navigate_address(
      session, "ftp://example.invalid/file", &error));
  assert(error && tai_session_page(session) == stable_page &&
         tai_session_history_length(session) == 2 &&
         tai_session_history_index(session) == 1);
  free(error);
  error = NULL;
  assert(!tai_session_navigate_address(
      session, "mailto:user@example.invalid", &error));
  assert(error && tai_session_page(session) == stable_page &&
         tai_session_history_length(session) == 2 &&
         tai_session_history_index(session) == 1);
  free(error);
  error = NULL;
  assert(!tai_session_navigate_address(session,
      "http://127.0.0.1:1/address-failure", &error));
  assert(error && tai_session_page(session) == stable_page &&
         tai_session_history_length(session) == 2 &&
         tai_session_history_index(session) == 1);
  free(error);
  tai_session_destroy(session);
  tai_network_destroy(network);
}

typedef struct {
  int listener;
  unsigned short port;
  size_t request_count;
  bool failed;
  char paths[4][256];
  char referers[4][256];
} LoopbackFixture;

static bool send_all(int descriptor, const char *text, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    ssize_t count = send(descriptor, text + sent, length - sent, 0);
    if (count <= 0) return false;
    sent += (size_t)count;
  }
  return true;
}

static const char *referer_header(const char *request) {
  const char *line = request;
  while ((line = strstr(line, "\r\n")) != NULL) {
    line += 2;
    if (*line == '\r' || !*line) return NULL;
    if (!strncasecmp(line, "Referer: ", 9)) return line + 9;
  }
  return NULL;
}

static void *serve_loopback(void *opaque) {
  LoopbackFixture *fixture = opaque;
  static const char body[] = "<html><body><p>loopback</p></body></html>";
  for (size_t index = 0; index < 4; index++) {
    int client = accept(fixture->listener, NULL, NULL);
    if (client < 0) {
      fixture->failed = true;
      return NULL;
    }
    char request[8192];
    size_t used = 0;
    while (used + 1 < sizeof(request)) {
      ssize_t count = recv(client, request + used,
                           sizeof(request) - used - 1, 0);
      if (count <= 0) break;
      used += (size_t)count;
      request[used] = '\0';
      if (strstr(request, "\r\n\r\n")) break;
    }
    request[used] = '\0';
    if (sscanf(request, "%*s %255s", fixture->paths[index]) != 1) {
      fixture->failed = true;
    }
    const char *referer = referer_header(request);
    if (referer) {
      const char *end = strstr(referer, "\r\n");
      size_t length = end ? (size_t)(end - referer) : 0;
      if (!end || length >= sizeof(fixture->referers[index])) {
        fixture->failed = true;
      } else {
        memcpy(fixture->referers[index], referer, length);
        fixture->referers[index][length] = '\0';
      }
    }
    char response[512];
    int response_length = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        sizeof(body) - 1, body);
    if (response_length <= 0 || (size_t)response_length >= sizeof(response) ||
        !send_all(client, response, (size_t)response_length))
      fixture->failed = true;
    shutdown(client, SHUT_RDWR);
    close(client);
    fixture->request_count++;
  }
  return NULL;
}

static int loopback_listener(unsigned short *port) {
  int descriptor = socket(AF_INET, SOCK_STREAM, 0);
  assert(descriptor >= 0);
  struct sockaddr_in address = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
      .sin_port = 0,
  };
  assert(bind(descriptor, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(listen(descriptor, 4) == 0);
  socklen_t address_length = sizeof(address);
  assert(getsockname(descriptor, (struct sockaddr *)&address,
                     &address_length) == 0);
  *port = ntohs(address.sin_port);
  return descriptor;
}

static void test_loopback_query_history_and_referrer(void) {
  LoopbackFixture fixture = {0};
  fixture.listener = loopback_listener(&fixture.port);
  pthread_t server;
  assert(pthread_create(&server, NULL, serve_loopback, &fixture) == 0);

  char source[256], address[256], source_referer[256], target_referer[256];
  assert(snprintf(source, sizeof(source), "http://127.0.0.1:%u/start",
                  fixture.port) > 0);
  assert(snprintf(address, sizeof(address),
                  "  http://127.0.0.1:%u/address?q=a+b  ",
                  fixture.port) > 0);
  assert(snprintf(source_referer, sizeof(source_referer),
                  "http://127.0.0.1:%u/start", fixture.port) > 0);
  assert(snprintf(target_referer, sizeof(target_referer),
                  "http://127.0.0.1:%u/address?q=a+b", fixture.port) > 0);

  char *error = NULL;
  TaiNetwork *network = tai_network_create();
  TaiUrl *url = tai_url_parse(source);
  assert(network && url);
  TaiPage *page = tai_page_load(network, url, css, 320, 160, false, &error);
  assert(page && !error);
  TaiSession *session = tai_session_create(network, page, css, false);
  assert(session);
  tai_url_destroy(url);

  assert(tai_session_navigate_address(session, address, &error) && !error);
  assert(!strcmp(tai_url_string(tai_page_url(tai_session_page(session))),
                 target_referer));
  assert(tai_session_history_length(session) == 2 &&
         tai_session_history_index(session) == 1);
  assert(tai_session_back(session, &error) && !error);
  assert(tai_session_history_index(session) == 0);
  assert(tai_session_forward(session, &error) && !error);
  assert(tai_session_history_index(session) == 1);

  tai_session_destroy(session);
  tai_network_destroy(network);
  assert(pthread_join(server, NULL) == 0);
  close(fixture.listener);
  assert(!fixture.failed && fixture.request_count == 4);
  assert(!strcmp(fixture.paths[0], "/start"));
  assert(!strcmp(fixture.paths[1], "/address?q=a+b"));
  assert(!strcmp(fixture.paths[2], "/start"));
  assert(!strcmp(fixture.paths[3], "/address?q=a+b"));
  assert(!fixture.referers[0][0]);
  assert(!strcmp(fixture.referers[1], source_referer));
  assert(!strcmp(fixture.referers[2], target_referer));
  assert(!strcmp(fixture.referers[3], source_referer));
}

int main(void) {
  test_normalization();
  test_data_navigation_and_failure();
  test_loopback_query_history_and_referrer();
  puts("address normalization, submit, and history passed");
  return 0;
}
