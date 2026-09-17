#include "tai/css.h"
#include "tai/layout.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  int status = 1;
  char *html = tai_read_file(argv[1], NULL),
       *css = tai_read_file(argv[2], NULL), *error = NULL;
  TaiDocument *document = NULL;
  TaiStylesheet *sheet = NULL;
  TaiLayout *layout = NULL;
  if (!html || !css)
    goto done;
  document = tai_html_parse(html, &error);
  if (!document)
    goto done;
  sheet = tai_css_parse(css, &error);
  if (!sheet || !tai_css_style(tai_document_root(document), sheet, &error))
    goto done;
  layout = tai_layout_create(tai_document_root(document), strtod(argv[3], NULL),
                             false, &error);
  if (!layout)
    goto done;
  tai_layout_json(stdout, layout);
  status = 0;
done:
  if (status)
    fprintf(stderr, "%s\n", error ? error : "layout input failed");
  tai_layout_destroy(layout);
  tai_css_destroy(sheet);
  tai_document_destroy(document);
  free(error);
  free(html);
  free(css);
  return status;
}
