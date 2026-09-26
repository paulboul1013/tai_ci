#ifndef TAI_NETWORK_H
#define TAI_NETWORK_H
#include "tai/url.h"

typedef struct TaiNetwork TaiNetwork;
typedef struct TaiRequest TaiRequest;
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
size_t tai_network_pending(const TaiNetwork *network);
void tai_network_cancel(TaiNetwork *network, TaiRequest *request);
TaiResponse *tai_network_request(TaiNetwork *network, const TaiUrl *url,
    const TaiUrl *referrer, const char *payload, const char *origin,
    const char *referrer_policy);
void tai_response_destroy(TaiResponse *response);
/* JS cookie surface follows the oracle's one-cookie-per-host policy. Getter
 * returns owned string; setter ignores invalid/HttpOnly input per Python. */
char *tai_network_cookie_get(TaiNetwork *network, const char *host);
bool tai_network_cookie_set(TaiNetwork *network, const char *host, const char *value);
#endif
