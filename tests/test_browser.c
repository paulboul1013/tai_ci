#include "tai/browser.h"
#include <cairo/cairo.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
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

static uint32_t pixel(cairo_surface_t *surface, int x, int y) {
    cairo_surface_flush(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char *value = data + y * stride + x * 4;
    return ((uint32_t)value[2] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[0] << 8) | value[3];
}

int main(void) {
    char *error = NULL;
    TaiNetwork *network = tai_network_create();
    TaiUrl *url = tai_url_parse(
        "data:text/html,<style>p%20%7Bcolor:red%7D</style><p>Hello%20world</p>");
    assert(network && url);
    TaiPage *page = tai_page_load(network, url,
        "html {display:block} body {display:block} p {display:block}",
        800.0, 600.0, false, &error);
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

    TaiUrl *scroll_url = tai_url_parse(
        "data:text/html,%3Cdiv%20style%3D%22height%3A30px%3Bbackground-color%3Ared%22%3E%3C%2Fdiv%3E%3Csection%20style%3D%22height%3A30px%3Bbackground-color%3Ablue%22%3E%3C%2Fsection%3E");
    TaiPage *scroll_page = tai_page_load(network, scroll_url,
        "html {display:block} body {display:block} div {display:block} "
        "section {display:block}", 200.0, 30.0, false, &error);
    assert(scroll_page && !error);
    TaiNode *section = find(tai_page_root(scroll_page), "section");
    assert(section);
    assert(tai_page_scroll_y(scroll_page) == 0.0);
    assert(tai_page_set_scroll_y(scroll_page, -10.0));
    assert(tai_page_scroll_y(scroll_page) == 0.0);
    assert(tai_page_set_scroll_y(scroll_page, 1000.0));
    double maximum = tai_page_max_scroll_y(scroll_page);
    assert(maximum > 0.0 && tai_page_scroll_y(scroll_page) == maximum);
    assert(!tai_page_set_scroll_y(scroll_page, NAN));
    assert(tai_page_scroll_y(scroll_page) == maximum);
    assert(!tai_page_set_scroll_y(scroll_page, INFINITY));
    assert(tai_page_scroll_y(scroll_page) == maximum);
    assert(!tai_page_set_scroll_y(scroll_page, -INFINITY));
    assert(tai_page_scroll_y(scroll_page) == maximum);
    assert(tai_page_set_scroll_y(scroll_page, 30.0));
    assert(tai_page_hit_test(scroll_page, 150.0, 50.0, &hit) == section);
    assert(tai_page_viewport_hit_test(scroll_page, 150.0, 20.0, &hit) ==
           section);
    assert(tai_page_viewport_hit_test(scroll_page, 187.0, 20.0, &hit) == NULL);
    hit = (TaiDisplayHit){.node_id = 123, .width = 123};
    assert(tai_page_viewport_hit_test(scroll_page, NAN, 20.0, &hit) == NULL);
    assert(hit.node_id == 0 && hit.width == 0);
    hit = (TaiDisplayHit){.node_id = 123, .width = 123};
    assert(tai_page_viewport_hit_test(scroll_page, 150.0, INFINITY, &hit) ==
           NULL);
    assert(hit.node_id == 0 && hit.width == 0);
    assert(tai_page_write_viewport_png(scroll_page,
        "/tmp/tai-ci-page-scroll.png", &error));
    cairo_surface_t *surface =
        cairo_image_surface_create_from_png("/tmp/tai-ci-page-scroll.png");
    assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
    assert(cairo_image_surface_get_width(surface) == 200);
    assert(cairo_image_surface_get_height(surface) == 30);
    assert(pixel(surface, 150, 20) == 0x0000ffffU);
    cairo_surface_destroy(surface);
    tai_page_destroy(scroll_page);
    tai_url_destroy(scroll_url);

    TaiUrl *nested_url = tai_url_parse(
        "data:text/html,%3Cmain%20style%3D%22height%3A20px%3Boverflow%3Ascroll%22%3E%3Cdiv%20style%3D%22height%3A30px%3Boverflow%3Ascroll%22%3E%3Csection%20style%3D%22height%3A20px%3Bbackground-color%3Ared%22%3E%3C%2Fsection%3E%3Carticle%20style%3D%22height%3A20px%3Bbackground-color%3Ablue%22%3E%3C%2Farticle%3E%3C%2Fdiv%3E%3C%2Fmain%3E");
    TaiPage *nested_page = tai_page_load(network, nested_url,
        "html {display:block} body {display:block} main {display:block} "
        "div {display:block} section {display:block} article {display:block}",
        200.0, 30.0, false, &error);
    assert(nested_page && !error);
    TaiNode *article = find(tai_page_root(nested_page), "article");
    assert(article);
    /* Inject the two already-clamped element offsets into this self-contained
     * test fixture, then exercise page scroll only through TaiPage's interface. */
    const TaiDisplayList *nested_display = tai_page_display_list(nested_page);
    size_t injected = 0;
    for (size_t index = 0; index < tai_display_list_count(nested_display);
         index++) {
        const TaiDisplayCommand *command =
            tai_display_list_command(nested_display, index);
        if (command->kind == TAI_PUSH_CLIP_SCROLL) {
            ((TaiDisplayCommand *)command)->scroll_y = 10.0;
            injected++;
        }
    }
    assert(injected == 2);
    assert(tai_page_set_scroll_y(nested_page, 10.0));
    assert(tai_page_viewport_hit_test(nested_page, 150.0, 10.0, &hit) ==
           article);
    assert(tai_page_write_viewport_png(
        nested_page, "/tmp/tai-ci-page-nested-scroll.png", &error));
    surface = cairo_image_surface_create_from_png(
        "/tmp/tai-ci-page-nested-scroll.png");
    assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
    assert(pixel(surface, 150, 10) == 0x0000ffffU);
    cairo_surface_destroy(surface);
    tai_page_destroy(nested_page);
    tai_url_destroy(nested_url);

    TaiUrl *inline_url = tai_url_parse(
        "data:text/html,%3Cscript%3Edocument.querySelectorAll(%27p%27)%5B0%5D.setAttribute(%27data-state%27,%27first%27)%3B%3C%2Fscript%3E%3Cp%3EInline%3C%2Fp%3E%3Cscript%3Evar%20p%3Ddocument.querySelectorAll(%27p%27)%5B0%5D%3Bp.setAttribute(%27data-state%27%2Cp.getAttribute(%27data-state%27)%2B%27-second%27)%3B%3C%2Fscript%3E");
    TaiPage *inline_page = tai_page_load(network, inline_url,
        "html {display:block} body {display:block} p {display:block}",
        800.0, 600.0, false, &error);
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
        800.0, 600.0, false, &error);
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
