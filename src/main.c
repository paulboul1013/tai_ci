#define _POSIX_C_SOURCE 200809L
#include "tai/browser.h"
#include "tai/presentation.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef TAI_DEFAULT_CSS_RELATIVE_PATH
#define TAI_DEFAULT_CSS_RELATIVE_PATH "../share/tai-ci/browser.css"
#endif

enum { TAI_SCREENSHOT_WIDTH = 800 };
/* Frozen Python rounds 600 - its rendered Chrome.bottom to 532 pixels. */
enum { TAI_SCREENSHOT_HEIGHT = 532 };

typedef struct {
    TaiNetwork *network;
    const char *default_css;
    bool rtl;
} NavigationContext;

static bool navigate_page(void *opaque, TaiPage **current_page,
                          const TaiNavigationIntent *intent, char **error) {
    NavigationContext *context = opaque;
    if (!tai_page_replace_from_intent(context->network, current_page, intent,
                                      context->default_css, context->rtl,
                                      error)) {
        fprintf(stderr, "navigation failed: %s\n",
                error && *error ? *error : "page load failed");
        if (error) { free(*error); *error = NULL; }
    }
    return true;
}

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s [--headless|--window] [--rtl] [--screenshot OUTPUT.png] URL\n",
            program);
}

static char *relative_to_executable(const char *argv0, const char *relative) {
    char executable[4096];
    const char *source = argv0;
    ssize_t length = readlink("/proc/self/exe", executable,
                              sizeof(executable) - 1);
    if (length > 0) {
        executable[length] = '\0';
        source = executable;
    }
    const char *slash = strrchr(source, '/');
    if (!slash) return NULL;
    size_t directory_length = (size_t)(slash - source);
    size_t relative_length = strlen(relative);
    if (directory_length > SIZE_MAX - relative_length - 2) return NULL;
    char *path = malloc(directory_length + relative_length + 2);
    if (!path) return NULL;
    memcpy(path, source, directory_length);
    path[directory_length] = '/';
    memcpy(path + directory_length + 1, relative, relative_length + 1);
    return path;
}

static char *read_default_css(const char *argv0, size_t *length) {
    char *css;
    const char *candidates[] = {
        TAI_DEFAULT_CSS_RELATIVE_PATH,
        "assets/browser.css",
        "../assets/browser.css",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *path = relative_to_executable(argv0, candidates[i]);
        if (!path) continue;
        css = tai_read_file(path, length);
        free(path);
        if (css) return css;
    }
    /* Keep source-tree invocation convenient when /proc is unavailable. */
    return tai_read_file("assets/browser.css", length);
}

int main(int argc, char **argv) {
    bool rtl = false;
    bool window = false;
    const char *url_text = NULL;
    const char *screenshot_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rtl")) rtl = true;
        else if (!strcmp(argv[i], "--headless")) continue;
        else if (!strcmp(argv[i], "--window")) window = true;
        else if (!strcmp(argv[i], "--screenshot")) {
            if (++i == argc || !strncmp(argv[i], "--", 2)) {
                usage(argv[0]);
                return 2;
            }
            screenshot_path = argv[i];
        }
        else if (!strncmp(argv[i], "--", 2)) {
            usage(argv[0]);
            return 2;
        }
        else if (!url_text) url_text = argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!url_text) url_text = "about:blank";
    if (window && screenshot_path) { usage(argv[0]); return 2; }
    size_t css_length = 0;
    char *css = read_default_css(argv[0], &css_length);
    (void)css_length;
    TaiUrl *url = tai_url_parse(url_text);
    TaiNetwork *network = tai_network_create();
    char *error = NULL;
    TaiPage *page = css && url && network
        ? tai_page_load(network, url, css, TAI_SCREENSHOT_WIDTH,
                        TAI_SCREENSHOT_HEIGHT, rtl, &error)
        : NULL;
    bool success = page != NULL;
    if (!page) fprintf(stderr, "load failed: %s\n", error ? error : "allocation failed");
    else if (window) {
        NavigationContext navigation = {
            .network = network,
            .default_css = css,
            .rtl = rtl,
        };
        success = tai_present_window_with_navigation(
            &page, TAI_SCREENSHOT_WIDTH, TAI_SCREENSHOT_HEIGHT,
            navigate_page, &navigation, &error);
        if (!success) fprintf(stderr, "window failed: %s\n",
                              error ? error : "presentation failed");
    } else if (screenshot_path) {
        success = tai_page_write_viewport_png(page, screenshot_path, &error);
        if (!success)
            fprintf(stderr, "screenshot failed: %s\n",
                    error ? error : "PNG output failed");
    } else {
        tai_page_json(stdout, page);
        fputc('\n', stdout);
    }
    tai_page_destroy(page);
    tai_network_destroy(network);
    tai_url_destroy(url);
    free(css);
    free(error);
    return success ? 0 : 1;
}
