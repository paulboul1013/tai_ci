/* Prints tai_page_title() for each markup file argument as one JSON array.
 * tests/title_integration.py compares the result with the Python oracle. */
#include "tai/browser.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool called;
    TaiPage *page;
    char *error;
} MarkupResult;

static void markup_done(void *opaque, TaiPage *page, bool network_failure,
                        char *error) {
    MarkupResult *result = opaque;
    (void)network_failure;
    result->called = true;
    result->page = page;
    result->error = error;
}

static char *title_of(TaiNetwork *network, const TaiUrl *url,
                      const char *markup) {
    MarkupResult result = {0};
    char *error = NULL;
    TaiPageLoad *load = tai_page_load_async_markup(
        network, url, markup, "", 320.0, 160.0, false, NULL, markup_done, &result,
        &error);
    if (load || error || !result.called || !result.page || result.error) {
        fprintf(stderr, "markup load failed: %s\n",
                error ? error : result.error ? result.error : "unknown");
        exit(1);
    }
    char *title = tai_page_title(result.page);
    assert(title);
    tai_page_destroy(result.page);
    return title;
}

int main(int argc, char **argv) {
    TaiNetwork *network = tai_network_create();
    TaiUrl *url = tai_url_parse("about:title");
    assert(network && url);

    /* No page: the caller supplies the fallback name. */
    char *none = tai_page_title(NULL);
    assert(none && !strcmp(none, ""));
    free(none);

    /* Invalid UTF-8 bytes are never whitespace and never overrun the strip. */
    char *invalid = title_of(network, url,
                             "<title> \xff x \xc3</title><p>y</p>");
    assert(!strcmp(invalid, "\xff x \xc3"));
    free(invalid);

    putchar('[');
    for (int index = 1; index < argc; index++) {
        char *markup = tai_read_file(argv[index], NULL);
        if (!markup) {
            fprintf(stderr, "cannot read %s\n", argv[index]);
            return 1;
        }
        char *title = title_of(network, url, markup);
        if (index > 1) putchar(',');
        tai_json_string(stdout, title);
        free(title);
        free(markup);
    }
    puts("]");
    tai_url_destroy(url);
    tai_network_destroy(network);
    return 0;
}
