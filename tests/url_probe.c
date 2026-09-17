#include "tai/url.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 2;
    TaiUrl *base = tai_url_parse(argv[1]);
    if (!base) return 3;
    TaiUrl *value = argc == 3 ? tai_url_resolve(base, argv[2]) : base;
    /* Destroy base first: resolved URLs must own every observed string. */
    if (argc == 3) tai_url_destroy(base);
    if (value) tai_url_json(stdout, value); else fputs("null", stdout);
    putchar('\n');
    tai_url_destroy(value);
    return 0;
}
