#define _POSIX_C_SOURCE 200809L
#include <cairo/cairo.h>
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* stdout_path, when non-NULL, receives the child's standard output. */
static int run_to(const char *browser, char *const arguments[],
                  const char *stdout_path) {
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    if (stdout_path) {
      int out = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (out < 0 || dup2(out, STDOUT_FILENO) < 0) _exit(126);
      close(out);
    }
    execv(browser, arguments);
    _exit(127);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status));
  return WEXITSTATUS(status);
}

static int run(const char *browser, char *const arguments[]) {
  return run_to(browser, arguments, NULL);
}

static uint32_t pixel(cairo_surface_t *surface, int x, int y) {
  cairo_surface_flush(surface);
  unsigned char *data = cairo_image_surface_get_data(surface);
  int stride = cairo_image_surface_get_stride(surface);
  unsigned char *value = data + y * stride + x * 4;
  return ((uint32_t)value[2] << 24) | ((uint32_t)value[1] << 16) |
         ((uint32_t)value[0] << 8) | value[3];
}

int main(int argc, char **argv) {
  assert(argc == 3);
  const char *browser = argv[1];
  const char *output = argv[2];
  const char *url =
      "data:text/html,%3Cdiv%20style%3D%22height%3A60px%3Bbackground-color%3A"
      "%23ff0000%22%3Ex%3C%2Fdiv%3E";

  unlink(output);
  char *success[] = {(char *)browser, "--screenshot", (char *)output,
                     (char *)url, NULL};
  assert(run(browser, success) == 0);
  cairo_surface_t *surface = cairo_image_surface_create_from_png(output);
  assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
  assert(cairo_image_surface_get_width(surface) == 800);
  assert(cairo_image_surface_get_height(surface) == 532);
  assert(pixel(surface, 0, 0) == 0xffffffffU);
  assert(pixel(surface, 100, 30) == 0xff0000ffU);
  cairo_surface_destroy(surface);

  char *write_failure[] = {(char *)browser, "--screenshot",
                           "/proc/tai-ci-screenshot.png", (char *)url, NULL};
  assert(run(browser, write_failure) == 1);
  char *missing_value[] = {(char *)browser, "--screenshot", NULL};
  assert(run(browser, missing_value) == 2);
  char *option_as_value[] = {(char *)browser, "--screenshot", "--rtl",
                             "file:///tai-ci-missing-input.html", NULL};
  assert(run(browser, option_as_value) == 2);
  char *unknown_option[] = {(char *)browser, "--unknown-option", NULL};
  assert(run(browser, unknown_option) == 2);
  char *incompatible_options[] = {(char *)browser, "--window",
                                  "--screenshot", (char *)output, NULL};
  assert(run(browser, incompatible_options) == 2);

  /* D11: headless output follows one animation frame. Its throwing callback
   * does not stop the batch (D10); the callback it queues does not run. */
  const char *raf_url =
      "data:text/html,<p id=a>zero</p><script>"
      "requestAnimationFrame(function () { throw Error('boom'); });"
      "requestAnimationFrame(function () { a.innerHTML = 'one';"
      " requestAnimationFrame(function () { a.innerHTML = 'two'; }); });"
      "</script>";
  char *headless[] = {(char *)browser, (char *)raf_url, NULL};
  assert(run_to(browser, headless, output) == 0);
  size_t length = 0;
  FILE *json = fopen(output, "rb");
  assert(json);
  char buffer[65536];
  length = fread(buffer, 1, sizeof(buffer) - 1, json);
  fclose(json);
  buffer[length] = '\0';
  assert(strstr(buffer, "\"text\":\"one\""));
  assert(!strstr(buffer, "\"text\":\"zero\""));
  assert(!strstr(buffer, "\"text\":\"two\""));
  unlink(output);
  return 0;
}
