#define _POSIX_C_SOURCE 200809L
/* tai_external_open: the URL reaches the program verbatim as its only
 * argument, the child gets its own session, /dev/null for stdin and no
 * descriptors beyond stderr, and a missing program fails with a diagnostic. */
#include "tai/external.h"
#include "tai/core.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char *wait_for_file(const char *path) {
  for (int attempt = 0; attempt < 500; attempt++) {
    size_t length = 0;
    char *text = tai_read_file(path, &length);
    if (text && strstr(text, "end\n")) return text;
    free(text);
    struct timespec pause = {0, 10 * 1000 * 1000};
    nanosleep(&pause, NULL);
  }
  return NULL;
}

int main(void) {
  char directory[] = "/tmp/tai-external-XXXXXX";
  assert(mkdtemp(directory));
  char script[256], report[256];
  snprintf(script, sizeof(script), "%s/opener", directory);
  snprintf(report, sizeof(report), "%s/report", directory);

  /* An open descriptor without FD_CLOEXEC must not reach the child. */
  int inherited = open("/dev/null", O_RDONLY);
  assert(inherited > 2);

  FILE *file = fopen(script, "w");
  assert(file);
  fprintf(file,
          "#!/bin/sh\n"
          "{\n"
          "  echo \"count=$#\"\n"
          "  printf 'arg=%%s\\n' \"$1\"\n"
          "  echo \"session=$(cut -d' ' -f6 /proc/$$/stat) pid=$$\"\n"
          "  echo \"stdin=$(cat)\"\n"
          "  [ -e /proc/$$/fd/%d ] && echo leaked || echo closed\n"
          "  echo end\n"
          "} > %s\n",
          inherited, report);
  assert(fclose(file) == 0);
  assert(chmod(script, 0700) == 0);

  const char *url = "mailto:a b;$(touch x)'\"@example.com?s=1&t=2";
  char *error = NULL;
  assert(tai_external_open(script, url, &error));
  assert(error == NULL);
  char *text = wait_for_file(report);
  assert(text);
  assert(strstr(text, "count=1\n"));
  char expected[256];
  snprintf(expected, sizeof(expected), "arg=%s\n", url);
  assert(strstr(text, expected));
  int session = -1, pid = -2;
  const char *line = strstr(text, "session=");
  assert(line && sscanf(line, "session=%d pid=%d", &session, &pid) == 2);
  assert(session == pid);
  assert(strstr(text, "stdin=\n"));
  assert(strstr(text, "closed\n"));
  free(text);
  close(inherited);

  assert(!tai_external_open("/nonexistent/tai-opener", url, &error));
  assert(error && strstr(error, "could not start /nonexistent/tai-opener"));
  free(error);

  unlink(report);
  unlink(script);
  rmdir(directory);
  puts("external ok");
  return 0;
}
