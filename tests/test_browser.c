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

static const TaiDisplayCommand *text_command(const TaiDisplayList *list,
                                              const char *text) {
    for (size_t index = 0; index < tai_display_list_count(list); index++) {
        const TaiDisplayCommand *command =
            tai_display_list_command(list, index);
        if (command->kind == TAI_DRAW_TEXT && !strcmp(command->text, text))
            return command;
    }
    return NULL;
}

typedef struct {
    const TaiNode *target;
    double x, y;
    bool found;
} ControlPoint;

static bool find_control_point(const TaiLayoutItem *item, void *opaque) {
    ControlPoint *point = opaque;
    if (item->node == point->target &&
        (item->kind == TAI_LAYOUT_INPUT || item->kind == TAI_LAYOUT_BUTTON)) {
        point->x = item->x + item->width / 2.0;
        point->y = item->y + item->height / 2.0;
        point->found = true;
    }
    return true;
}

static bool activate_control(TaiPage *page, const TaiNode *target,
                             bool *changed, char **error) {
    ControlPoint point = {.target = target};
    if (!tai_layout_visit(tai_page_layout(page), find_control_point, &point,
                          error) || !point.found)
        return false;
    return tai_page_activate_viewport(page, point.x, point.y, changed, error);
}

typedef struct {
    bool called;
    bool network_failure;
    TaiPage *page;
    char *error;
} MarkupResult;

static void markup_done(void *opaque, TaiPage *page, bool network_failure,
                        char *error) {
    MarkupResult *result = opaque;
    result->called = true;
    result->network_failure = network_failure;
    result->page = page;
    result->error = error;
}

int main(void) {
    char *error = NULL;
    TaiNetwork *network = tai_network_create();
    TaiUrl *internal_url = tai_url_parse("about:bookmarks");
    assert(network && internal_url);
    char internal_markup[] = "<style>p {color:red}</style><p>Saved page</p>"
                             "<link rel=stylesheet href=http://example.invalid/x.css>";
    MarkupResult internal = {0};
    TaiPageLoad *internal_load = tai_page_load_async_markup(
        network, internal_url, internal_markup,
        "html {display:block} body {display:block} p {display:block}",
        320.0, 160.0, false, markup_done, &internal, &error);
    assert(!internal_load && !error && internal.called && internal.page &&
           !internal.error && !internal.network_failure);
    memset(internal_markup, 'X', sizeof(internal_markup) - 1);
    assert(!strcmp(tai_url_string(tai_page_url(internal.page)),
                   "about:bookmarks"));
    assert(text_command(tai_page_display_list(internal.page), "Saved"));
    assert(text_command(tai_page_display_list(internal.page), "page"));
    TaiNode *internal_paragraph = find(tai_page_root(internal.page), "p");
    assert(internal_paragraph &&
           !strcmp(tai_map_get(&internal_paragraph->style, "color"), "red"));
    assert(tai_network_pending(network) == 0);
    tai_page_destroy(internal.page);
    tai_url_destroy(internal_url);

    TaiUrl *ordinary_url = tai_url_parse("https://example.invalid/");
    MarkupResult rejected = {0};
    assert(ordinary_url);
    assert(!tai_page_load_async_markup(network, ordinary_url,
        "<p>wrong scheme</p>", "", 320.0, 160.0, false,
        markup_done, &rejected, &error));
    assert(error && !rejected.called);
    free(error);
    error = NULL;
    tai_url_destroy(ordinary_url);

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

    TaiUrl *resize_url = tai_url_parse(
        "data:text/html,<p>one%20two%20three%20four%20five%20six</p>");
    TaiPage *resize_page = tai_page_load(network, resize_url,
        "html {display:block} body {display:block} p {display:block}",
        300.0, 40.0, false, &error);
    assert(resize_page && !error);
    const TaiDisplayList *wide_display = tai_page_display_list(resize_page);
    const TaiDisplayCommand *wide_six = text_command(wide_display, "six");
    assert(wide_six);
    double wide_six_y = wide_six->y;
    assert(tai_page_set_scroll_y(resize_page, 1000.0));
    double old_scroll = tai_page_scroll_y(resize_page);
    assert(tai_page_resize(resize_page, 80.0, 40.0, &error));
    assert(!error && tai_page_viewport_width(resize_page) == 80.0 &&
           tai_page_viewport_height(resize_page) == 40.0);
    const TaiDisplayList *narrow_display = tai_page_display_list(resize_page);
    const TaiDisplayCommand *narrow_six = text_command(narrow_display, "six");
    assert(narrow_six && narrow_six->y > wide_six_y);
    assert(tai_page_scroll_y(resize_page) <= tai_page_max_scroll_y(resize_page));
    assert(tai_page_scroll_y(resize_page) <= old_scroll);
    assert(old_scroll > 0.0);
    assert(tai_page_resize(resize_page, 80.0, 10000.0, &error));
    assert(!error && tai_page_max_scroll_y(resize_page) == 0.0 &&
           tai_page_scroll_y(resize_page) == old_scroll);
    assert(tai_page_set_scroll_y(resize_page, 0.0));
    assert(tai_page_scroll_y(resize_page) == 0.0);
    const TaiDisplayList *before_invalid = tai_page_display_list(resize_page);
    double before_width = tai_page_viewport_width(resize_page);
    double before_height = tai_page_viewport_height(resize_page);
    double before_scroll = tai_page_scroll_y(resize_page);
    assert(!tai_page_resize(resize_page, 0.0, 40.0, &error));
    assert(error);
    free(error);
    error = NULL;
    assert(!tai_page_resize(resize_page, NAN, 40.0, &error));
    assert(error);
    free(error);
    error = NULL;
    assert(tai_page_display_list(resize_page) == before_invalid &&
           tai_page_viewport_width(resize_page) == before_width &&
           tai_page_viewport_height(resize_page) == before_height &&
           tai_page_scroll_y(resize_page) == before_scroll);
    tai_page_destroy(resize_page);
    tai_url_destroy(resize_url);

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

    /* RED: viewport activation owns text-to-element normalization, JS event
     * dispatch, and replacement of the immutable frame after JS invalidates
     * the DOM. Presentation can then paint only when changed is true. */
    TaiUrl *activation_url = tai_url_parse(
        "data:text/html,%3Cstyle%3Ep.active%20%7Bcolor%3Ablue%7D%3C%2Fstyle%3E%3Cp%3EActivate%3C%2Fp%3E%3Cscript%3Evar%20p%3Ddocument.querySelectorAll%28%27p%27%29%5B0%5D%3Bp.addEventListener%28%27click%27%2Cfunction%28%29%7Bthis.setAttribute%28%27class%27%2C%27active%27%29%3B%7D%29%3B%3C%2Fscript%3E");
    TaiPage *activation_page = tai_page_load(network, activation_url,
        "html {display:block} body {display:block} p {display:block}",
        300.0, 100.0, false, &error);
    assert(activation_page && !error);
    TaiNode *activation_paragraph = find(tai_page_root(activation_page), "p");
    assert(activation_paragraph);
    const TaiDisplayList *activation_before =
        tai_page_display_list(activation_page);
    bool changed = false;
    assert(tai_page_activate_viewport(activation_page, 14.0, 21.0, &changed,
                                      &error));
    assert(!error && changed);
    assert(!strcmp(tai_map_get(&activation_paragraph->attributes, "class"),
                   "active"));
    assert(!strcmp(tai_map_get(&activation_paragraph->style, "color"),
                   "blue"));
    assert(tai_page_display_list(activation_page) != activation_before);
    tai_page_destroy(activation_page);
    tai_url_destroy(activation_url);

    /* preventDefault without a DOM mutation is a successful no-change click.
     * In particular, it must not replace a presentable immutable frame. */
    TaiUrl *prevented_url = tai_url_parse(
        "data:text/html,%3Cp%3EPrevented%3C%2Fp%3E%3Cscript%3Evar%20p%3Ddocument.querySelectorAll%28%27p%27%29%5B0%5D%3Bp.addEventListener%28%27click%27%2Cfunction%28e%29%7Be.preventDefault%28%29%3B%7D%29%3B%3C%2Fscript%3E");
    TaiPage *prevented_page = tai_page_load(network, prevented_url,
        "html {display:block} body {display:block} p {display:block}",
        300.0, 100.0, false, &error);
    assert(prevented_page && !error);
    const TaiDisplayList *prevented_before =
        tai_page_display_list(prevented_page);
    changed = true;
    assert(tai_page_activate_viewport(prevented_page, 14.0, 21.0, &changed,
                                      &error));
    assert(!error && !changed);
    assert(tai_page_display_list(prevented_page) == prevented_before);
    tai_page_destroy(prevented_page);
    tai_url_destroy(prevented_url);

    /* A finite miss and either non-finite viewport coordinate are handled
     * requests, rather than errors. None may dispatch the listener or replace
     * the display list. */
    TaiUrl *no_change_url = tai_url_parse(
        "data:text/html,%3Cstyle%3Ep.active%20%7Bcolor%3Ablue%7D%3C%2Fstyle%3E%3Cp%3ENo%20change%3C%2Fp%3E%3Cscript%3Evar%20p%3Ddocument.querySelectorAll%28%27p%27%29%5B0%5D%3Bp.addEventListener%28%27click%27%2Cfunction%28%29%7Bthis.setAttribute%28%27class%27%2C%27active%27%29%3B%7D%29%3B%3C%2Fscript%3E");
    TaiPage *no_change_page = tai_page_load(network, no_change_url,
        "html {display:block} body {display:block} p {display:block}",
        300.0, 100.0, false, &error);
    assert(no_change_page && !error);
    TaiNode *no_change_paragraph = find(tai_page_root(no_change_page), "p");
    assert(no_change_paragraph);
    const TaiDisplayList *no_change_before =
        tai_page_display_list(no_change_page);
    changed = true;
    assert(tai_page_activate_viewport(no_change_page, 299.0, 21.0, &changed,
                                      &error));
    assert(!error && !changed);
    assert(tai_page_activate_viewport(no_change_page, NAN, 21.0, &changed,
                                      &error));
    assert(!error && !changed);
    assert(tai_page_activate_viewport(no_change_page, 14.0, INFINITY, &changed,
                                      &error));
    assert(!error && !changed);
    assert(!tai_map_get(&no_change_paragraph->attributes, "class"));
    assert(tai_page_display_list(no_change_page) == no_change_before);
    tai_page_destroy(no_change_page);
    tai_url_destroy(no_change_url);

    /* Activation coordinates are viewport-relative even after page scrolling.
     * The clicked visual point below resolves to section, not the preceding
     * document-space div. */
    TaiUrl *scrolled_activation_url = tai_url_parse(
        "data:text/html,%3Cstyle%3Esection.active%20%7Bcolor%3Ablue%7D%3C%2Fstyle%3E%3Cdiv%20style%3D%22height%3A30px%3Bbackground-color%3Ared%22%3E%3C%2Fdiv%3E%3Csection%20style%3D%22height%3A30px%3Bbackground-color%3Ablue%22%3EScrolled%3C%2Fsection%3E%3Cscript%3Evar%20s%3Ddocument.querySelectorAll%28%27section%27%29%5B0%5D%3Bs.addEventListener%28%27click%27%2Cfunction%28%29%7Bthis.setAttribute%28%27class%27%2C%27active%27%29%3B%7D%29%3B%3C%2Fscript%3E");
    TaiPage *scrolled_activation_page = tai_page_load(network,
        scrolled_activation_url,
        "html {display:block} body {display:block} div {display:block} "
        "section {display:block}", 200.0, 30.0, false, &error);
    assert(scrolled_activation_page && !error);
    TaiNode *scrolled_section =
        find(tai_page_root(scrolled_activation_page), "section");
    assert(scrolled_section);
    assert(tai_page_set_scroll_y(scrolled_activation_page, 30.0));
    assert(tai_page_viewport_hit_test(scrolled_activation_page, 150.0, 20.0,
                                      &hit) == scrolled_section);
    const TaiDisplayList *scrolled_activation_before =
        tai_page_display_list(scrolled_activation_page);
    changed = false;
    assert(tai_page_activate_viewport(scrolled_activation_page, 150.0, 20.0,
                                      &changed, &error));
    assert(!error && changed);
    assert(!strcmp(tai_map_get(&scrolled_section->attributes, "class"),
                   "active"));
    assert(!strcmp(tai_map_get(&scrolled_section->style, "color"), "blue"));
    assert(tai_page_display_list(scrolled_activation_page) !=
           scrolled_activation_before);
    tai_page_destroy(scrolled_activation_page);
    tai_url_destroy(scrolled_activation_url);

    /* RED: controls are laid out as hit-testable native geometry. A checkbox
     * keeps its parsed `checked` attribute as its initial-value record while
     * click default action changes only the live checked state. */
    TaiUrl *checkbox_url = tai_url_parse(
        "data:text/html,%3Cinput%20type%3Dcheckbox%20checked%3E");
    TaiPage *checkbox_page = tai_page_load(network, checkbox_url,
        "html {display:block} body {display:block}",
        300.0, 100.0, false, &error);
    assert(checkbox_page && !error);
    TaiNode *checkbox = find(tai_page_root(checkbox_page), "input");
    assert(checkbox && checkbox->checked);
    const TaiDisplayList *checkbox_before =
        tai_page_display_list(checkbox_page);
    assert(tai_page_viewport_hit_test(checkbox_page, 14.0, 22.0, &hit) ==
           checkbox);
    changed = false;
    assert(tai_page_activate_viewport(checkbox_page, 14.0, 22.0, &changed,
                                      &error));
    assert(!error && changed && !checkbox->checked);
    assert(tai_map_get(&checkbox->attributes, "checked"));
    assert(tai_page_display_list(checkbox_page) != checkbox_before);
    assert(tai_page_viewport_hit_test(checkbox_page, 14.0, 22.0, &hit) ==
           checkbox);
    tai_page_destroy(checkbox_page);
    tai_url_destroy(checkbox_url);

    /* An ordinary text input is also a visible control target. Clicking its
     * left edge focuses it, preserves its value, and places the caret at zero
     * in the refreshed immutable frame. */
    TaiUrl *text_input_url = tai_url_parse(
        "data:text/html,%3Cinput%20type%3Dtext%20value%3Dcat%3E");
    TaiPage *text_input_page = tai_page_load(network, text_input_url,
        "html {display:block} body {display:block}",
        300.0, 100.0, false, &error);
    assert(text_input_page && !error);
    TaiNode *text_input = find(tai_page_root(text_input_page), "input");
    assert(text_input && !text_input->focused);
    const TaiDisplayList *text_input_before =
        tai_page_display_list(text_input_page);
    assert(tai_page_viewport_hit_test(text_input_page, 14.0, 22.0, &hit) ==
           text_input);
    changed = false;
    assert(tai_page_activate_viewport(text_input_page, 14.0, 22.0, &changed,
                                      &error));
    assert(!error && changed && text_input->focused);
    assert(text_input->cursor_index == 0);
    assert(!strcmp(tai_map_get(&text_input->attributes, "value"), "cat"));
    assert(tai_page_display_list(text_input_page) != text_input_before);
    tai_page_destroy(text_input_page);
    tai_url_destroy(text_input_url);

    /* RED: focused controls edit at Unicode code-point cursor positions; the
     * public page seam owns text validation, insertion and special-key redraw. */
    TaiUrl *edit_url = tai_url_parse(
        "data:text/html,%3Cinput%20value%3Dcat%3E");
    TaiPage *edit_page = tai_page_load(network, edit_url,
        "html {display:block} body {display:block}",
        300.0, 100.0, false, &error);
    assert(edit_page && !error);
    TaiNode *edit_input = find(tai_page_root(edit_page), "input");
    assert(edit_input);
    changed = false;
    assert(tai_page_activate_viewport(edit_page, 14.0, 22.0, &changed,
                                      &error));
    assert(changed && edit_input->cursor_index == 0);
    changed = false;
    assert(tai_page_text_input(edit_page, "\303\251", &changed, &error));
    assert(!error && changed && edit_input->cursor_index == 1);
    assert(!strcmp(tai_map_get(&edit_input->attributes, "value"),
                   "\303\251cat"));
    changed = false;
    assert(tai_page_key(edit_page, TAI_PAGE_KEY_RIGHT, &changed, &error));
    assert(changed && edit_input->cursor_index == 2);
    changed = false;
    assert(tai_page_key(edit_page, TAI_PAGE_KEY_BACKSPACE, &changed, &error));
    assert(changed && edit_input->cursor_index == 1);
    assert(!strcmp(tai_map_get(&edit_input->attributes, "value"), "\303\251at"));
    tai_page_destroy(edit_page);
    tai_url_destroy(edit_url);

    /* RED: button activation and focused-input Enter produce owned navigation
     * intents. Form encoding preserves source order and Python quote_plus. */
    TaiUrl *get_form_url = tai_url_parse(
        "data:text/html,%3Cform%20action%3D%22http%3A%2F%2Flocalhost%3A8000%2Fsearch%3Fold%3D1%22%3E%3Cbutton%3ESend%3C%2Fbutton%3E%3Cinput%20name%3D%22a%20b%22%20value%3D%22hello%20world%22%3E%3Cinput%20type%3Dcheckbox%20name%3Dflag%20checked%3E%3Cinput%20type%3Dcheckbox%20name%3Dskip%3E%3Cinput%20name%3Dempty%3E%3C/form%3E");
    TaiPage *get_form_page = tai_page_load(network, get_form_url,
        "html {display:block} body {display:block}", 300.0, 100.0, false,
        &error);
    assert(get_form_url && get_form_page && !error);
    TaiNode *get_button = find(tai_page_root(get_form_page), "button");
    assert(get_button);
    changed = false;
    assert(activate_control(get_form_page, get_button, &changed, &error));
    assert(!error);
    TaiNavigationIntent *intent = NULL;
    assert(tai_page_take_navigation_intent(get_form_page, &intent));
    assert(intent && !tai_navigation_intent_is_post(intent));
    assert(!strcmp(tai_navigation_intent_url(intent),
        "http://localhost:8000/search?old=1&a+b=hello+world&flag=on&empty="));
    assert(tai_navigation_intent_body(intent) == NULL);
    tai_navigation_intent_destroy(intent);
    tai_page_destroy(get_form_page);
    tai_url_destroy(get_form_url);

    /* Preserve the oracle's GET composition quirk: a fragment remains before
     * the appended fields, so the fields become part of the fragment string. */
    TaiUrl *fragment_get_url = tai_url_parse(
        "data:text/html,%3Cform%20action%3D%22http://localhost:8000/search?old=1%23frag%22%3E%3Cbutton%3ESend%3C/button%3E%3Cinput%20name%3D%22x%20y%22%20value%3D1%3E%3C/form%3E");
    TaiPage *fragment_get_page = tai_page_load(network, fragment_get_url,
        "html {display:block} body {display:block}", 300.0, 100.0, false,
        &error);
    assert(fragment_get_url && fragment_get_page && !error);
    TaiNode *fragment_get_button = find(tai_page_root(fragment_get_page),
                                        "button");
    assert(fragment_get_button);
    changed = false;
    assert(activate_control(fragment_get_page, fragment_get_button, &changed,
                             &error));
    assert(!error);
    intent = NULL;
    assert(tai_page_take_navigation_intent(fragment_get_page, &intent));
    assert(intent && !tai_navigation_intent_is_post(intent));
    assert(!strcmp(tai_navigation_intent_url(intent),
        "http://localhost:8000/search?old=1#frag&x+y=1"));
    tai_navigation_intent_destroy(intent);
    tai_page_destroy(fragment_get_page);
    tai_url_destroy(fragment_get_url);

    TaiUrl *post_form_url = tai_url_parse(
        "data:text/html,%3Cform%20action%3D%22http%3A%2F%2Flocalhost%3A8000%2Fpost%22%20method%3DPOST%3E%3Cinput%20name%3Dq%20value%3D%22%C3%A9%20%26%22%3E%3Cbutton%3ESend%3C%2Fbutton%3E%3C/form%3E");
    TaiPage *post_form_page = tai_page_load(network, post_form_url,
        "html {display:block} body {display:block}", 300.0, 100.0, false,
        &error);
    assert(post_form_url && post_form_page && !error);
    TaiNode *post_input = find(tai_page_root(post_form_page), "input");
    assert(post_input);
    changed = false;
    assert(activate_control(post_form_page, post_input, &changed, &error));
    assert(!error && changed && post_input->focused);
    changed = false;
    assert(tai_page_key(post_form_page, TAI_PAGE_KEY_RETURN, &changed, &error));
    assert(!error && !changed);
    intent = NULL;
    assert(tai_page_take_navigation_intent(post_form_page, &intent));
    assert(intent && tai_navigation_intent_is_post(intent));
    assert(!strcmp(tai_navigation_intent_url(intent),
                   "http://localhost:8000/post"));
    assert(!strcmp(tai_navigation_intent_body(intent), "q=%C3%A9+%26"));
    tai_navigation_intent_destroy(intent);
    tai_page_destroy(post_form_page);
    tai_url_destroy(post_form_url);

    TaiUrl *prevent_submit_url = tai_url_parse(
        "data:text/html,%3Cform%20action%3D%22http%3A%2F%2Flocalhost%3A8000%2Fnever%22%3E%3Cscript%3Evar%20f%3Ddocument.querySelectorAll%28%27form%27%29%5B0%5D%3Bf.addEventListener%28%27submit%27%2Cfunction%28e%29%7Be.preventDefault%28%29%3B%7D%29%3B%3C%2Fscript%3E%3Cbutton%3ESend%3C%2Fbutton%3E%3C/form%3E");
    TaiPage *prevent_submit_page = tai_page_load(network, prevent_submit_url,
        "html {display:block} body {display:block}", 300.0, 100.0, false,
        &error);
    assert(prevent_submit_url && prevent_submit_page && !error);
    TaiNode *prevent_button = find(tai_page_root(prevent_submit_page), "button");
    assert(prevent_button);
    changed = false;
    assert(activate_control(prevent_submit_page, prevent_button, &changed,
                            &error));
    assert(!error);
    intent = NULL;
    assert(tai_page_take_navigation_intent(prevent_submit_page, &intent));
    assert(intent == NULL);
    tai_page_destroy(prevent_submit_page);
    tai_url_destroy(prevent_submit_url);

    /* Raw # links stay in-document, update the owned URL, and scroll to the
     * first matching ID. They never return a DOM pointer in the intent. */
    TaiUrl *fragment_url = tai_url_parse(
        "data:text/html,%3Ca%20href%3D%22%23target%22%3Ego%3C/a%3E%3Cdiv%20style%3D%22height%3A200px%22%3Egap%3C/div%3E%3Cp%20id%3Dtarget%3Etarget%3C/p%3E");
    TaiPage *fragment_page = tai_page_load(network, fragment_url,
        "html {display:block} body {display:block} a {display:block} div {display:block} p {display:block}",
        300.0, 50.0, false, &error);
    assert(fragment_url && fragment_page && !error);
    TaiNode *fragment_link = find(tai_page_root(fragment_page), "a");
    assert(fragment_link);
    changed = false;
    assert(tai_page_activate_viewport(fragment_page, 14.0, 21.0,
                                      &changed, &error));
    assert(!error && changed);
    assert(!strcmp(tai_url_fragment(tai_page_url(fragment_page)), "target"));
    assert(tai_page_scroll_y(fragment_page) > 0.0);
    intent = NULL;
    assert(tai_page_take_navigation_intent(fragment_page, &intent));
    assert(intent == NULL);
    tai_page_destroy(fragment_page);
    tai_url_destroy(fragment_url);

    tai_page_json(stdout, page);
    fputc('\n', stdout);
    tai_page_destroy(page);
    tai_url_destroy(url);
    tai_network_destroy(network);
    free(error);
    return 0;
}
