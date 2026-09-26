#define _POSIX_C_SOURCE 200809L
/* Tabbed chrome with an HTTPS page under SDL's dummy driver. Driven by
 * tests/https_integration.py, which owns the test CA and fixture servers.
 *
 *   test_tabs_secure_window --geometry
 *       prints the native address-field and lock rectangles per breakpoint
 *       for comparison with tests/fixtures/https_oracle.json;
 *   test_tabs_secure_window HTTP_PORT HTTPS_PORT CA_FILE
 *       clicks the lock slot, the shifted address field and the in-field
 *       star in real presentation event handling;
 *   test_tabs_secure_window --window URL
 *       manual/real-window evidence only: an 800x600 tabbed window like
 *       tai-browser --window, trusting test-ca.pem and styled by
 *       assets/browser.css, both read next to this executable. This is how
 *       tests/tools/window_session.sh --bin reaches an HTTPS fixture without
 *       tai-browser ever trusting a test CA.
 *
 * A focused field turns Return into a navigation to its (unchanged) text, so
 * the history count shows whether a click focused the field. */
#include "tai/presentation.h"
#include "tai/tabset.h"
#include "../src/presentation_internal.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,      \
              #condition);                                                     \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

static const char *css =
    "html {display:block} body {display:block} h1 {display:block} "
    "p {display:block}";
static const int breakpoints[] = {800, 232, 231, 120, 70};
/* Every scenario here has one tab, like the oracle probe. */
static const TaiTabSetView one_tab = {.tab_count = 1};

static double single_tab_address_y(int width) {
  return tabs_address_y(width, tai_pres_tab_row_wraps(&one_tab, width));
}

static void print_geometry(void) {
  putchar('{');
  for (size_t i = 0; i < sizeof(breakpoints) / sizeof(*breakpoints); i++) {
    int width = breakpoints[i];
    for (int secure = 1; secure >= 0; secure--) {
      TaiAddressField field = tai_tabs_address_field(width, secure);
      double y = single_tab_address_y(width);
      printf("%s\"%s_%d\":{\"address_rect\":[%.3f,%.3f,%.3f,%.3f],"
             "\"lock_rect\":",
             i || !secure ? "," : "", secure ? "secure" : "insecure", width,
             field.x, y, field.x + field.width, y + TAI_ADDRESS_HEIGHT);
      if (secure) {
        double cx = field.slot_x + TAI_SECURITY_ICON_SLOT / 2.0;
        double cy = y + TAI_ADDRESS_HEIGHT / 2.0;
        double half = TAI_SECURITY_ICON_SIZE / 2.0;
        printf("[%.3f,%.3f,%.3f,%.3f]}", cx - half, cy - half, cx + half,
               cy + half);
      } else {
        fputs("null}", stdout);
      }
    }
  }
  puts("}");
}

typedef enum { CLICK, RETURN_KEY } StepKind;

typedef struct {
  StepKind kind;
  float x;
} Step;

typedef struct {
  atomic_uint window_id;
  const Step *steps;
  size_t count;
  float y;
  bool queued;
} Script;

static bool capture_window_id(void *opaque, SDL_Event *event) {
  Script *script = opaque;
  if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
    atomic_store(&script->window_id, event->window.windowID);
  return true;
}

static void sleep_ms(long ms) {
  struct timespec pause = {.tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L};
  (void)nanosleep(&pause, NULL);
}

static void *post_script(void *opaque) {
  Script *script = opaque;
  SDL_WindowID window_id = 0;
  for (int attempts = 0; attempts < 2000 && !window_id; attempts++) {
    window_id = atomic_load(&script->window_id);
    if (!window_id) sleep_ms(1);
  }
  if (!window_id) return NULL;
  /* The local HTTPS document commits well within this window, including
   * under sanitizers; clicks on a pending page would test nothing. */
  sleep_ms(1000);
  for (size_t index = 0; index < script->count; index++) {
    const Step *step = &script->steps[index];
    SDL_Event event = step->kind == CLICK
        ? (SDL_Event){.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN,
                                 .windowID = window_id,
                                 .button = SDL_BUTTON_LEFT,
                                 .down = true,
                                 .x = step->x,
                                 .y = script->y}}
        : (SDL_Event){.key = {.type = SDL_EVENT_KEY_DOWN,
                              .windowID = window_id,
                              .key = SDLK_RETURN,
                              .down = true}};
    if (!SDL_PushEvent(&event)) return NULL;
  }
  script->queued = true;
  sleep_ms(300);
  SDL_Event quit = {.type = SDL_EVENT_QUIT};
  (void)SDL_PushEvent(&quit);
  return NULL;
}

static bool contains_text(const TaiNode *node, const char *needle) {
  if (!node) return false;
  if (node->kind == TAI_TEXT && node->text && strstr(node->text, needle))
    return true;
  for (size_t index = 0; index < node->child_count; index++)
    if (contains_text(node->children[index], needle)) return true;
  return false;
}

static TaiTabSetView settle(TaiTabSet *tabs) {
  struct timespec deadline;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
  deadline.tv_sec += 8;
  TaiTabSetView view = {0};
  CHECK(tai_tabset_view(tabs, &view));
  for (;;) {
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    if ((!view.loading && view.page) || now.tv_sec > deadline.tv_sec) break;
    bool changed = false;
    char *error = NULL;
    CHECK(tai_tabset_pump(tabs, &changed, &error));
    CHECK(error == NULL);
    sleep_ms(1);
    CHECK(tai_tabset_view(tabs, &view));
  }
  CHECK(!view.loading && view.page);
  return view;
}

/* Presents url at width x 300 with the scripted input, then returns the
 * settled view. The caller destroys *out_tabs. */
static TaiTabSetView run_window(const char *url, const char *ca_file,
                                int width, const Step *steps, size_t count,
                                TaiTabSet **out_tabs) {
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_for_test(css, false, url, ca_file,
                                               &error);
  CHECK(tabs && !error);
  Script script = {.steps = steps, .count = count,
                   .y = (float)(single_tab_address_y(width) +
                                TAI_ADDRESS_HEIGHT / 2.0)};
  atomic_init(&script.window_id, 0);
  CHECK(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
  SDL_SetEventFilter(capture_window_id, &script);
  pthread_t poster;
  CHECK(pthread_create(&poster, NULL, post_script, &script) == 0);
  bool presented = tai_present_window_with_tabs(tabs, url, width, 300,
                                                &error);
  CHECK(pthread_join(poster, NULL) == 0);
  SDL_SetEventFilter(NULL, NULL);
  if (!presented)
    fprintf(stderr, "presentation failed: %s\n", error ? error : "unknown");
  CHECK(presented && !error && script.queued);
  *out_tabs = tabs;
  return settle(tabs);
}

/* Returns an owned path to name in this executable's directory. */
static char *beside_executable(const char *name) {
  char executable[PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", executable,
                            sizeof(executable) - 1);
  if (length <= 0) return NULL;
  executable[length] = '\0';
  char *slash = strrchr(executable, '/');
  if (!slash) return NULL;
  slash[1] = '\0';
  size_t size = strlen(executable) + strlen(name) + 1;
  char *path = malloc(size);
  if (path) (void)snprintf(path, size, "%s%s", executable, name);
  return path;
}

static int run_real_window(const char *url) {
  char *ca_file = beside_executable("test-ca.pem");
  char *css_path = beside_executable("assets/browser.css");
  size_t css_length = 0;
  char *window_css = css_path ? tai_read_file(css_path, &css_length) : NULL;
  CHECK(ca_file && window_css);
  char *error = NULL;
  TaiTabSet *tabs = tai_tabset_create_for_test(window_css, false, url,
                                               ca_file, &error);
  bool ok = tabs && tai_present_window_with_tabs(tabs, url, 800, 600,
                                                 &error);
  if (!ok) fprintf(stderr, "window failed: %s\n", error ? error : "unknown");
  free(error);
  tai_tabset_destroy(tabs);
  free(window_css);
  free(css_path);
  free(ca_file);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

typedef struct {
  int width;
  float slot, field, star; /* x inside the lock slot, field and star */
} SecureCase;

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--geometry")) {
    print_geometry();
    return EXIT_SUCCESS;
  }
  if (argc == 3 && !strcmp(argv[1], "--window")) return run_real_window(argv[2]);
  CHECK(argc == 4);
  char secure_url[256], plain_url[256];
  CHECK(snprintf(secure_url, sizeof(secure_url),
                 "https://127.0.0.1:%s/secure-home", argv[2]) > 0);
  CHECK(snprintf(plain_url, sizeof(plain_url),
                 "http://127.0.0.1:%s/plain-home", argv[1]) > 0);
  const char *ca_file = argv[3];

  /* x positions from tests/fixtures/https_oracle.json: the lock slot is
   * [address_x, address_x + 30), the field starts after it, and the star
   * sits in the (clamped) field's rightmost 23px. */
  static const SecureCase cases[] = {
      {800, 147.0f, 163.0f, 770.0f},
      {232, 147.0f, 165.0f, 220.0f},
      {120, 15.0f, 35.0f, 110.0f},
      {70, 15.0f, 35.0f, 60.0f},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    const SecureCase *c = &cases[i];
    const Step steps[] = {
        {CLICK, c->slot}, {RETURN_KEY, 0.0f}, /* slot: no focus */
        {CLICK, c->star},                     /* bookmark the page */
        {CLICK, c->field}, {RETURN_KEY, 0.0f}, /* field: focus, reload */
    };
    TaiTabSet *tabs = NULL;
    TaiTabSetView view = run_window(secure_url, ca_file, c->width, steps,
                                    sizeof(steps) / sizeof(*steps), &tabs);
    if (!(view.secure && view.bookmarked && view.history_count == 2 &&
          !strcmp(view.url, secure_url) &&
          contains_text(tai_page_root(view.page), "secure-home"))) {
      fprintf(stderr, "width %d: secure=%d bookmarked=%d history=%zu url=%s\n",
              c->width, view.secure, view.bookmarked, view.history_count,
              view.url);
      CHECK(false);
    }
    tai_tabset_destroy(tabs);
  }

  /* Without a lock the same x is inside the field and focuses it. */
  const Step insecure_steps[] = {{CLICK, 147.0f}, {RETURN_KEY, 0.0f}};
  TaiTabSet *tabs = NULL;
  TaiTabSetView view = run_window(plain_url, ca_file, 800, insecure_steps, 2,
                                  &tabs);
  CHECK(!view.secure && view.history_count == 2 &&
        !strcmp(view.url, plain_url));
  tai_tabset_destroy(tabs);

  puts("tabbed SDL HTTPS chrome passed: lock slot, shifted field, and star "
       "at 800/232/120/70px");
  return EXIT_SUCCESS;
}
