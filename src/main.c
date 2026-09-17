#define _POSIX_C_SOURCE 200809L
#include "tai/browser.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef TAI_DEFAULT_CSS_RELATIVE_PATH
#define TAI_DEFAULT_CSS_RELATIVE_PATH "../share/tai-ci/browser.css"
#endif

enum { TAI_SCREENSHOT_WIDTH = 800 };
static const double TAI_VERTICAL_STEP = 18.0;

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s [--headless] [--rtl] [--screenshot OUTPUT.png] URL\n",
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
    const char *url_text = NULL;
    const char *screenshot_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rtl")) rtl = true;
        else if (!strcmp(argv[i], "--headless")) continue;
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
    size_t css_length = 0;
    char *css = read_default_css(argv[0], &css_length);
    (void)css_length;
    TaiUrl *url = tai_url_parse(url_text);
    TaiNetwork *network = tai_network_create();
    char *error = NULL;
    TaiPage *page = css && url && network
        ? tai_page_load(network, url, css, TAI_SCREENSHOT_WIDTH, rtl, &error)
        : NULL;
    bool success = page != NULL;
    if (!page) fprintf(stderr, "load failed: %s\n", error ? error : "allocation failed");
    else if (screenshot_path) {
        double height = ceil(tai_layout_height(tai_page_layout(page)) +
                             2.0 * TAI_VERTICAL_STEP);
        if (!isfinite(height) || height > INT_MAX) {
            free(error);
            error = tai_strdup("invalid document height");
            success = false;
        } else {
            int pixels = height < 1.0 ? 1 : (int)height;
            success = tai_display_list_write_png(
                tai_page_display_list(page), screenshot_path,
                TAI_SCREENSHOT_WIDTH, pixels, &error);
        }
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
