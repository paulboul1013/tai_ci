/* Layout and display list under allocation failure.
 *
 * Linked with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc. The document
 * and style are built unarmed; then allocation N fails (once, or from N on)
 * while a page that reaches every oracle layout rule is laid out and
 * painted: abbr pieces, nested button contents, h6+p groups, nav#toc
 * caption, sup, li and controls. N counts up until a run injects nothing.
 * The process must not crash, a failed layout must report an error, and
 * under ASan/LSan nothing may leak. */
#include "tai/core.h"
#include "tai/css.h"
#include "tai/dom.h"
#include "tai/layout.h"
#include "tai/render.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);

static long budget = -1; /* <0 disarmed; allocations left before failing */
static long counted;
static bool one_shot, injected;

static bool refuse(void) {
  counted++;
  if (budget < 0) return false;
  if (budget == 0) {
    injected = true;
    if (one_shot) budget = -1;
    return true;
  }
  budget--;
  return false;
}
void *__wrap_malloc(size_t size) {
  return refuse() ? NULL : __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
  return refuse() ? NULL : __real_calloc(count, size);
}
void *__wrap_realloc(void *ptr, size_t size) {
  return refuse() ? NULL : __real_realloc(ptr, size);
}

static const char PAGE[] =
    "<nav id='toc'><p>Intro</p><p>Second</p></nav><nav id='toc'>inline</nav>"
    "<nav class='links'><p>home</p></nav>"
    "<p>E = mc<sup>2 <abbr>Html \xc3\x9f</abbr></sup> <abbr>css ok</abbr></p>"
    "<ul><li>one</li><li>two</li></ul>"
    "<form><input value='hi'> <input type='password' value='pw'> "
    "<button><b>Send</b> it <button>in<div>block</div></button></button>"
    "<button></button><button style='display:block'>blk</button></form>"
    "<div>x</div><h6>Head</h6> <p>merged <i>p</i></p><h6>Alone</h6>"
    "<pre>pre <sup>s</sup></pre>";

static TaiNode *root;

/* Returns whether the run injected a failure. */
static bool run(long n, bool once) {
  char *error = NULL;
  budget = n;
  one_shot = once;
  injected = false;
  TaiLayout *layout = tai_layout_create(root, 800, false, &error);
  TaiDisplayList *list = layout ? tai_display_list_create(layout, &error) : NULL;
  budget = -1;
  /* Only an injected failure may stop the layout or display list. */
  if (!layout || !list) assert(injected);
  tai_display_list_destroy(list);
  tai_layout_destroy(layout);
  free(error);
  return injected;
}

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) return 2;
  char *css = tai_read_file(argv[1], NULL), *error = NULL;
  /* Only allocations from the statically linked project code are wrapped;
   * fontconfig, FreeType and Cairo keep their own allocator. */
  TaiDocument *document = tai_html_parse(PAGE, &error);
  TaiStylesheet *sheet = css ? tai_css_parse(css, &error) : NULL;
  assert(document && sheet);
  root = tai_document_root(document);
  bool styled = tai_css_style(root, sheet, &error);
  assert(styled);
  counted = 0;
  bool injected_unarmed = run(-1, false);
  assert(!injected_unarmed);
  long total = counted;
  if (argc == 3 && !strcmp(argv[2], "--count")) {
    printf("%ld\n", total);
    return 0;
  }
  long runs = 0;
  for (int mode = 0; mode < 2; mode++)
    for (long n = 0; run(n, mode == 0); n++) runs++;
  printf("layout_oom: %ld allocations per run, %ld injected runs passed\n",
         total, runs);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(css);
  free(error);
  return 0;
}
