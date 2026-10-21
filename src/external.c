#define _GNU_SOURCE
#include "tai/external.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static bool failure(char **error, const char *program, int code) {
    if (error && !*error) {
        const char *reason = strerror(code);
        size_t size = strlen(program) + strlen(reason) + 32;
        *error = malloc(size);
        if (*error)
            snprintf(*error, size, "could not start %s: %s", program, reason);
    }
    return false;
}

static void *reap(void *opaque) {
    pid_t pid = (pid_t)(intptr_t)opaque;
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    return NULL;
}

bool tai_external_open(const char *program, const char *url, char **error) {
    if (!program || !url) return failure(error, "external opener", EINVAL);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int code = posix_spawn_file_actions_init(&actions);
    if (code) return failure(error, program, code);
    code = posix_spawnattr_init(&attributes);
    if (code) {
        posix_spawn_file_actions_destroy(&actions);
        return failure(error, program, code);
    }
    if (!code) code = posix_spawn_file_actions_addopen(
        &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!code) code = posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    /* Sockets, pipes and the loader's descriptors stay in this process. */
    if (!code) code = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    /* A terminal's Ctrl+C must not reach the mail application, and it starts
     * with no blocked signals and SIGPIPE at its default action. */
    sigset_t signals;
    sigemptyset(&signals);
    if (!code) code = posix_spawnattr_setsigmask(&attributes, &signals);
    sigaddset(&signals, SIGPIPE);
    if (!code) code = posix_spawnattr_setsigdefault(&attributes, &signals);
    if (!code) code = posix_spawnattr_setflags(
        &attributes,
        POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid = 0;
    char *argv[] = {(char *)program, (char *)url, NULL};
    if (!code)
        code = posix_spawnp(&pid, program, &actions, &attributes, argv,
                            environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (code) return failure(error, program, code);

    pthread_t reaper;
    pthread_attr_t detached;
    bool started = false;
    if (!pthread_attr_init(&detached)) {
        started = !pthread_attr_setdetachstate(&detached,
                                               PTHREAD_CREATE_DETACHED) &&
            !pthread_create(&reaper, &detached, reap, (void *)(intptr_t)pid);
        pthread_attr_destroy(&detached);
    }
    /* The program has started, so the open succeeded; without a reaper it
     * stays a zombie until this process exits. */
    if (!started)
        fprintf(stderr, "external opener %s will not be reaped\n", program);
    return true;
}
