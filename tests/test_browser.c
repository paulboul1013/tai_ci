#include "tai/browser.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static TaiNode *find(TaiNode *node, const char *tag) {
    if (node->kind == TAI_ELEMENT && !strcmp(node->tag, tag)) return node;
    for (size_t i = 0; i < node->child_count; i++) {
        TaiNode *result = find(node->children[i], tag);
        if (result) return result;
    }
    return NULL;
}

int main(void) {
    char *error = NULL;
    TaiNetwork *network = tai_network_create();
    TaiUrl *url = tai_url_parse(
        "data:text/html,<style>p%20%7Bcolor:red%7D</style><p>Hello%20world</p>");
    assert(network && url);
    TaiPage *page = tai_page_load(network, url,
        "html {display:block} body {display:block} p {display:block}",
        800.0, false, &error);
    assert(page && !error);
    TaiNode *paragraph = find(tai_page_root(page), "p");
    assert(paragraph);
    assert(!strcmp(tai_map_get(&paragraph->style, "color"), "red"));
    assert(tai_layout_height(tai_page_layout(page)) > 0.0);
    assert(tai_page_display_list(page));
    assert(tai_display_list_count(tai_page_display_list(page)) == 2);
    TaiDisplayHit hit = {0};
    TaiNode *hit_node = tai_page_hit_test(page, 14.0, 21.0, &hit);
    assert(hit_node && hit_node->kind == TAI_TEXT);
    assert(hit_node->id == hit.node_id);
    assert(!tai_page_hit_test(page, 799.0, 21.0, &hit));

    TaiUrl *inline_url = tai_url_parse(
        "data:text/html,%3Cscript%3Edocument.querySelectorAll(%27p%27)%5B0%5D.setAttribute(%27data-state%27,%27first%27)%3B%3C%2Fscript%3E%3Cp%3EInline%3C%2Fp%3E%3Cscript%3Evar%20p%3Ddocument.querySelectorAll(%27p%27)%5B0%5D%3Bp.setAttribute(%27data-state%27%2Cp.getAttribute(%27data-state%27)%2B%27-second%27)%3B%3C%2Fscript%3E");
    TaiPage *inline_page = tai_page_load(network, inline_url,
        "html {display:block} body {display:block} p {display:block}",
        800.0, false, &error);
    assert(inline_page && !error);
    TaiNode *inline_paragraph = find(tai_page_root(inline_page), "p");
    assert(inline_paragraph);
    const char *state =
        tai_map_get(&inline_paragraph->attributes, "data-state");
    assert(state && !strcmp(state, "first-second"));
    tai_page_destroy(inline_page);
    tai_url_destroy(inline_url);

    TaiUrl *external_url = tai_url_parse(
        "data:text/html,%3Cscript%20src%3D%22data%3Atext%2Fjavascript%2Cdocument.querySelectorAll%2528%2522p%2522%2529%255B0%255D.setAttribute%2528%2522data-state%2522%252C%2522external%2522%2529%22%3E%3C%2Fscript%3E%3Cp%3EExternal%3C%2Fp%3E");
    TaiPage *external_page = tai_page_load(network, external_url,
        "html {display:block} body {display:block} p {display:block}",
        800.0, false, &error);
    assert(external_page && !error);
    TaiNode *external_paragraph = find(tai_page_root(external_page), "p");
    assert(external_paragraph);
    state = tai_map_get(&external_paragraph->attributes, "data-state");
    assert(state && !strcmp(state, "external"));
    tai_page_destroy(external_page);
    tai_url_destroy(external_url);
    tai_page_json(stdout, page);
    fputc('\n', stdout);
    tai_page_destroy(page);
    tai_url_destroy(url);
    tai_network_destroy(network);
    free(error);
    return 0;
}
