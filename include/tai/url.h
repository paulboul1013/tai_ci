#ifndef TAI_URL_H
#define TAI_URL_H
#include "tai/core.h"

typedef struct TaiUrl TaiUrl;
/* URL owns every string. Invalid syntax follows the oracle's about:blank fallback;
 * NULL denotes allocation failure. Accessors borrow strings for the URL lifetime. */
TaiUrl *tai_url_parse(const char *text);
TaiUrl *tai_url_resolve(const TaiUrl *base, const char *relative);
void tai_url_destroy(TaiUrl *url);
const char *tai_url_scheme(const TaiUrl *url);
const char *tai_url_host(const TaiUrl *url);
const char *tai_url_path(const TaiUrl *url);
const char *tai_url_fragment(const TaiUrl *url);
const char *tai_url_key(const TaiUrl *url);
const char *tai_url_string(const TaiUrl *url);
const char *tai_url_origin(const TaiUrl *url);
/* Arbitrary decimal ports remain exact in JSON/key/string/origin.
 * Out-of-range long long values return LLONG_MIN; transport must reject
 * every port outside 0..65535. Resolve returns owned deep copies, including
 * empty relatives; reference resolve exceptions are represented by NULL. */
long long tai_url_port(const TaiUrl *url);
bool tai_url_view_source(const TaiUrl *url);
bool tai_url_is_http(const TaiUrl *url);
void tai_url_json(FILE *out, const TaiUrl *url);
#endif
