#ifndef TAI_EXTERNAL_H
#define TAI_EXTERNAL_H

#include <stdbool.h>

/* The default program that receives external URLs (the oracle's
 * webbrowser.open resolves to it on Linux desktops). */
#define TAI_EXTERNAL_OPENER "xdg-open"

/* Starts program, looked up in PATH, with url as its only argument and
 * returns without waiting, like the oracle's background webbrowser launch.
 * No shell is involved. The child runs in a new session with stdin and
 * stdout on /dev/null, inherits stderr and no other descriptors (glibc 2.34
 * closefrom), starts with no blocked signals and default SIGPIPE, and is
 * reaped by a detached thread that lives until it exits. Returns false with an owned diagnostic when
 * the program cannot be started; its exit status is never observed. */
bool tai_external_open(const char *program, const char *url, char **error);

#endif
