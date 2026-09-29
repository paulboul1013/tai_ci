/* Native runner for tests/js_dom_cases.py, driven by tests/js_dom_differential.py.
 *
 *   js_probe HTML_FILE STEPS_FILE
 *
 * STEPS_FILE holds NUL-terminated fields: "js" SOURCE | "dispatch" TYPE
 * SELECTOR | "raf". SOURCE is already wrapped by js_dom_cases.step_source(),
 * so its completion value is the step outcome as JSON. Prints one JSON object
 * with the raw native results; the differential normalizes and compares. */
#define _POSIX_C_SOURCE 200809L
#include "tai/css.h"
#include "tai/js.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t invalidations;
    size_t removed;
    bool first_output;
} Counters;

static void invalidated(void *opaque) { ((Counters *)opaque)->invalidations++; }

static void node_removed(void *opaque, TaiNode *node) {
    (void)node;
    ((Counters *)opaque)->removed++;
}

static void output_separator(Counters *counters) {
    if (!counters->first_output) fputc(',', stdout);
    counters->first_output = false;
}

static void reported(void *opaque, const TaiJsReport *report) {
    Counters *counters = opaque;
    output_separator(counters);
    if (report->kind == TAI_JS_REPORT_LOG) {
        printf("{\"log\":%s}", report->json);
    } else {
        fputs("{\"crash\":\"event\",\"event\":", stdout);
        tai_json_string(stdout, report->event);
        fputs(",\"text\":", stdout);
        tai_json_string(stdout, report->text);
        fputc('}', stdout);
    }
}

static TaiNode *first_match(TaiNode *node, const TaiSelector *selector) {
    if (node->kind == TAI_ELEMENT && tai_selector_matches(selector, node))
        return node;
    for (size_t index = 0; index < node->child_count; index++) {
        TaiNode *found = first_match(node->children[index], selector);
        if (found) return found;
    }
    return NULL;
}

static const char *next_field(const char **cursor, const char *end) {
    if (*cursor >= end) return NULL;
    const char *field = *cursor;
    *cursor += strlen(field) + 1;
    return field;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: js_probe HTML_FILE STEPS_FILE\n", stderr);
        return 2;
    }
    size_t html_length = 0, steps_length = 0;
    char *html = tai_read_file(argv[1], &html_length);
    char *steps = tai_read_file(argv[2], &steps_length);
    char *error = NULL;
    TaiDocument *document = html ? tai_html_parse(html, &error) : NULL;
    if (!document || !steps) {
        fprintf(stderr, "js_probe: %s\n", error ? error : "cannot read input");
        return 1;
    }
    TaiNode *root = tai_document_root(document);
    Counters counters = {.first_output = true};
    TaiJsHost host = {.invalidated = invalidated, .node_removed = node_removed,
                      .report = reported, .userdata = &counters};

    fputs("{\"created\":{\"output\":[", stdout);
    TaiJsContext *js = tai_js_create(root, &host, &error);
    if (!js) {
        fprintf(stderr, "js_probe: %s\n", error ? error : "context failed");
        return 1;
    }
    printf("],\"invalidations\":%zu,\"raf_requests\":0,\"dom\":",
           counters.invalidations);
    tai_dom_json(stdout, root, false);
    fputs("},\"steps\":[", stdout);

    const char *cursor = steps, *end = steps + steps_length;
    bool first_step = true;
    const char *kind;
    while ((kind = next_field(&cursor, end))) {
        fputs(first_step ? "{" : ",{", stdout);
        first_step = false;
        counters.invalidations = 0;
        counters.first_output = true;
        fputs("\"output\":[", stdout);
        if (!strcmp(kind, "js")) {
            const char *source = next_field(&cursor, end);
            char *outcome = NULL;
            bool ok = source && tai_js_eval_value(js, "step.js", source,
                                                  &outcome, &error);
            fputs("],\"outcome\":", stdout);
            if (ok) {
                fputs(outcome, stdout);
            } else {
                fputs("{\"fatal\":", stdout);
                tai_json_string(stdout, error ? error : "missing step");
                fputc('}', stdout);
            }
            free(outcome);
        } else if (!strcmp(kind, "dispatch")) {
            const char *type = next_field(&cursor, end);
            const char *selector_text = next_field(&cursor, end);
            TaiSelector *selector = selector_text
                ? tai_selector_parse(selector_text, &error) : NULL;
            TaiNode *target = selector ? first_match(root, selector) : NULL;
            tai_selector_destroy(selector);
            bool prevented = false;
            if (!type || !target ||
                !tai_js_dispatch_event(js, type, target, &prevented, &error)) {
                fprintf(stderr, "js_probe: dispatch failed: %s\n",
                        error ? error : "no target");
                return 1;
            }
            printf("],\"prevented\":%s", prevented ? "true" : "false");
        } else if (!strcmp(kind, "raf")) {
            bool ok = tai_js_eval(js, "raf.js", "runRAFHandlers()", &error);
            if (!ok) {
                output_separator(&counters);
                fputs("{\"crash\":\"raf\",\"text\":", stdout);
                tai_json_string(stdout, error ? error : "");
                fputc('}', stdout);
            }
            fputc(']', stdout);
        } else {
            fprintf(stderr, "js_probe: unknown step %s\n", kind);
            return 1;
        }
        free(error);
        error = NULL;
        printf(",\"invalidations\":%zu,\"raf_requests\":0,\"dom\":",
               counters.invalidations);
        tai_dom_json(stdout, root, false);
        fputc('}', stdout);
    }
    fputs("]}\n", stdout);
    tai_js_destroy(js);
    tai_document_destroy(document);
    free(html);
    free(steps);
    return 0;
}
