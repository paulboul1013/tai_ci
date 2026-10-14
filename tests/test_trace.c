#define _POSIX_C_SOURCE 200809L
#include "tai/core.h"
#include "tai/trace.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char directory[] = "/tmp/tai-trace-XXXXXX";

static char *trace_path(const char *name) {
    size_t length = strlen(directory) + strlen(name) + 2;
    char *path = malloc(length);
    assert(path);
    snprintf(path, length, "%s/%s", directory, name);
    return path;
}

static size_t count(const char *text, const char *needle) {
    size_t n = 0;
    for (const char *p = strstr(text, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

static char *read_trace(const char *path) {
    char *text = tai_read_file(path, NULL);
    assert(text);
    return text;
}

static void test_disabled(void) {
    char *path = trace_path("disabled.json");
    tai_trace_thread_name("nobody");
    tai_trace_begin(TAI_SPAN_RENDER);
    tai_trace_end(TAI_SPAN_RENDER);
    TaiTraceFetch fetch = tai_trace_fetch_begin(TAI_FETCH_DOCUMENT, 0);
    assert(fetch.id == 0);
    tai_trace_fetch_end(&fetch);
    tai_trace_close();
    assert(access(path, F_OK) != 0);
    free(path);
}

static void test_shape_and_close(void) {
    char *path = trace_path("shape.json");
    char *error = NULL;
    assert(tai_trace_open(path, &error) && !error);
    assert(!tai_trace_open(path, &error));
    assert(error && strstr(error, "already open"));
    free(error);
    tai_trace_close();
    tai_trace_close();
    tai_trace_begin(TAI_SPAN_RENDER);
    TaiTraceFetch late = tai_trace_fetch_begin(TAI_FETCH_DOCUMENT, 0);
    assert(late.id == 0);
    char *text = read_trace(path);
    const char *head = "{\"traceEvents\":[{\"name\":\"process_name\",\"ph\":\"M\",\"ts\":";
    assert(!strncmp(text, head, strlen(head)));
    const char *tail = ",\"pid\":1,\"cat\":\"__metadata\",\"args\":{\"name\":\"Browser\"}}]}";
    assert(strlen(text) > strlen(tail));
    assert(!strcmp(text + strlen(text) - strlen(tail), tail));
    free(text);
    free(path);
}

static void test_open_failure(void) {
    char *error = NULL;
    assert(!tai_trace_open("/nonexistent-tai-dir/t.json", &error));
    assert(error && strstr(error, "/nonexistent-tai-dir/t.json"));
    free(error);
    tai_trace_begin(TAI_SPAN_RENDER);
}

static void *unnamed_worker(void *unused) {
    (void)unused;
    tai_trace_begin(TAI_SPAN_JAVASCRIPT);
    tai_trace_end(TAI_SPAN_JAVASCRIPT);
    return NULL;
}

static void test_threads_and_generations(void) {
    char *path = trace_path("threads.json");
    assert(tai_trace_open(path, NULL));
    tai_trace_thread_name("Browser thread");
    tai_trace_thread_name("Browser thread");
    tai_trace_begin(TAI_SPAN_RENDER);
    pthread_t worker;
    int failed = pthread_create(&worker, NULL, unnamed_worker, NULL);
    assert(!failed);
    pthread_join(worker, NULL);
    tai_trace_end(TAI_SPAN_RENDER);
    tai_trace_close();
    char *text = read_trace(path);
    assert(count(text, "\"thread_name\"") == 2);
    assert(count(text, "{\"name\":\"Browser thread\"}") == 1);
    assert(count(text, "{\"name\":\"Thread ") == 1);
    assert(count(text, "\"name\":\"render\"") == 2);
    assert(count(text, "\"name\":\"javascript\"") == 2);
    free(text);

    /* A new trace names the thread again, here automatically. */
    assert(tai_trace_open(path, NULL));
    tai_trace_begin(TAI_SPAN_RENDER);
    tai_trace_end(TAI_SPAN_RENDER);
    tai_trace_close();
    text = read_trace(path);
    assert(count(text, "\"thread_name\"") == 1);
    assert(count(text, "{\"name\":\"Thread ") == 1);
    free(text);
    free(path);
}

static void test_fetches_and_escaping(void) {
    char *path = trace_path("fetch.json");
    assert(tai_trace_open(path, NULL));
    tai_trace_thread_name("Loader \"one\"\n");
    TaiTraceFetch document = tai_trace_fetch_begin(TAI_FETCH_DOCUMENT, 7);
    TaiTraceFetch script = tai_trace_fetch_begin(TAI_FETCH_SCRIPT, 3);
    assert(document.id && script.id && document.id != script.id);
    tai_trace_fetch_end(&document);
    assert(document.id == 0);
    tai_trace_fetch_end(&document);
    tai_trace_fetch_end(&script);
    tai_trace_close();
    char *text = read_trace(path);
    assert(strstr(text, "{\"name\":\"Loader \\\"one\\\"\\u000a\"}"));
    assert(count(text, "\"name\":\"Network I/O: document\"") == 2);
    assert(count(text, "\"name\":\"resource_fetch[3]: script\"") == 2);
    assert(count(text, "\"ph\":\"e\"") == 2);
    free(text);
    free(path);
}

enum { STRESS_THREADS = 4, STRESS_PAIRS = 10000 };

static void *stress_worker(void *unused) {
    (void)unused;
    for (int i = 0; i < STRESS_PAIRS; i++) {
        tai_trace_begin(TAI_SPAN_TASK_SET_TIMEOUT);
        tai_trace_begin(TAI_SPAN_JAVASCRIPT);
        TaiTraceFetch fetch = tai_trace_fetch_begin(TAI_FETCH_STYLE, (size_t)i % 5);
        tai_trace_end(TAI_SPAN_JAVASCRIPT);
        tai_trace_end(TAI_SPAN_TASK_SET_TIMEOUT);
        tai_trace_fetch_end(&fetch);
    }
    return NULL;
}

/* Writes a multi-threaded trace for tests/trace_check.py to validate. */
static int write_stress(const char *path) {
    char *error = NULL;
    if (!tai_trace_open(path, &error)) {
        fprintf(stderr, "trace failed: %s\n", error);
        free(error);
        return 1;
    }
    tai_trace_thread_name("Browser thread");
    pthread_t workers[STRESS_THREADS];
    for (int i = 0; i < STRESS_THREADS; i++)
        if (pthread_create(&workers[i], NULL, stress_worker, NULL)) return 1;
    stress_worker(NULL);
    for (int i = 0; i < STRESS_THREADS; i++) pthread_join(workers[i], NULL);
    tai_trace_close();
    return 0;
}

static void print_vocabulary(void) {
    for (int span = 0; span < TAI_SPAN_COUNT; span++)
        printf("B\t%s\n", tai_trace_span_name((TaiTraceSpan)span));
    for (int kind = 0; kind < TAI_FETCH_COUNT; kind++)
        printf("b\t%s\n", tai_trace_fetch_format((TaiTraceFetchKind)kind));
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--vocabulary")) {
        print_vocabulary();
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--stress")) return write_stress(argv[2]);
    for (int span = 0; span < TAI_SPAN_COUNT; span++)
        assert(tai_trace_span_name((TaiTraceSpan)span));
    for (int kind = 0; kind < TAI_FETCH_COUNT; kind++)
        assert(tai_trace_fetch_format((TaiTraceFetchKind)kind));
    if (!mkdtemp(directory)) return 1;
    test_disabled();
    test_shape_and_close();
    test_open_failure();
    test_threads_and_generations();
    test_fetches_and_escaping();
    const char *names[] = {"shape.json", "threads.json", "fetch.json"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char *path = trace_path(names[i]);
        unlink(path);
        free(path);
    }
    rmdir(directory);
    puts("trace tests passed");
    return 0;
}
