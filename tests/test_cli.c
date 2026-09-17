#define _POSIX_C_SOURCE 200809L
#include <cairo/cairo.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int run(const char *browser, char *const arguments[]) {
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    execv(browser, arguments);
    _exit(127);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status));
  return WEXITSTATUS(status);
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
  unlink(output);
  return 0;
}
