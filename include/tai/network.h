#ifndef TAI_NETWORK_H
#define TAI_NETWORK_H
#include "tai/url.h"

typedef struct TaiNetwork TaiNetwork;
typedef struct TaiRequest TaiRequest;
/* The oracle's COOKIE_JAR: one cookie per host. Any thread may call the jar
 * functions; an internal mutex guards memory operations only. */
typedef struct TaiCookieJar TaiCookieJar;
typedef struct {
    TaiMap headers;
    char *body;
    size_t length;
    long status;
    char *error;
    bool certificate_error;
} TaiResponse;
/* Context is single-thread-owned. Submit copies all input strings. Poll drives
 * libcurl multi and invokes callbacks on the owner thread. Callback receives an
 * owned response; request handle expires immediately before callback execution.
 * Cancellation/destruction suppresses callbacks. Caller owns callback userdata.
 * A callback may submit or cancel requests, but must not destroy its TaiNetwork
 * from inside tai_network_poll(). */
typedef void (*TaiNetworkDone)(void *userdata, TaiResponse *response);
TaiNetwork *tai_network_create(void);
/* The network borrows jar, which must outlive it; tai_network_create makes
 * and owns a private one. */
TaiNetwork *tai_network_create_with_jar(TaiCookieJar *jar);
TaiCookieJar *tai_network_cookie_jar(TaiNetwork *network); /* borrowed */
void tai_network_destroy(TaiNetwork *network);
/* Test-only trust override: requests started afterwards (including redirect
 * hops) verify peers against the PEM bundle at path instead of libcurl's
 * default CA file; NULL restores the default. The path is copied. tai-browser
 * never calls this and no environment variable reaches it. */
bool tai_network_set_ca_file(TaiNetwork *network, const char *path);
TaiRequest *tai_network_submit(TaiNetwork *network, const TaiUrl *url,
    const TaiUrl *referrer, const char *payload, const char *origin,
    const char *referrer_policy, TaiNetworkDone done, void *userdata);
bool tai_network_poll(TaiNetwork *network, int wait_ms);
/* Marks a pending request as dispatchable by nested polls: the restricted
 * polls below drive every transfer but only call back their own request and
 * requests marked this way; other completions wait for tai_network_poll. */
void tai_network_allow_nested(TaiRequest *request);
bool tai_network_poll_nested(TaiNetwork *network, int wait_ms);
size_t tai_network_pending(const TaiNetwork *network);
void tai_network_cancel(TaiNetwork *network, TaiRequest *request);
typedef enum {
    TAI_WAIT_DONE,
    TAI_WAIT_CANCELLED, /* service returned false */
    TAI_WAIT_TIMED_OUT, /* timeout_seconds passed, across redirects */
    TAI_WAIT_FAILED     /* allocation or polling failure */
} TaiWaitStatus;
/* Submits one request and polls (restricted, see above) until it completes.
 * Between polls service(userdata), when non-NULL, may submit or cancel other
 * requests; returning false cancels this one. timeout_seconds <= 0 means no
 * total limit beyond libcurl's per-hop timeouts. NULL unless DONE; a
 * transport failure is a response with error set. May run inside a poll
 * callback of the same network. */
TaiResponse *tai_network_request_until(TaiNetwork *network, const TaiUrl *url,
    const TaiUrl *referrer, const char *payload, const char *origin,
    const char *referrer_policy, double timeout_seconds,
    bool (*service)(void *userdata), void *userdata, TaiWaitStatus *status);
TaiResponse *tai_network_request(TaiNetwork *network, const TaiUrl *url,
    const TaiUrl *referrer, const char *payload, const char *origin,
    const char *referrer_policy);
void tai_response_destroy(TaiResponse *response);
TaiCookieJar *tai_cookie_jar_create(void);
void tai_cookie_jar_destroy(TaiCookieJar *jar);
/* JS cookie surface follows the oracle's one-cookie-per-host policy. Getter
 * returns an owned string ("" without a host; NULL on allocation failure);
 * setter ignores invalid/HttpOnly input per Python and fails only on
 * allocation failure. http_set applies a Set-Cookie header value. */
char *tai_cookie_jar_js_get(TaiCookieJar *jar, const char *host);
bool tai_cookie_jar_js_set(TaiCookieJar *jar, const char *host,
                           const char *value);
bool tai_cookie_jar_http_set(TaiCookieJar *jar, const char *host,
                             const char *value);
/* The same surface on a network's jar. */
char *tai_network_cookie_get(TaiNetwork *network, const char *host);
bool tai_network_cookie_set(TaiNetwork *network, const char *host, const char *value);
#endif
