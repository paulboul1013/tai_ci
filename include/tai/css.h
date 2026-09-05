#ifndef TAI_CSS_H
#define TAI_CSS_H
#include "tai/dom.h"
typedef struct TaiStylesheet TaiStylesheet;
typedef struct TaiSelector TaiSelector;
/* Stylesheets/selectors own all parsed data. No DOM references retained. */
TaiStylesheet *tai_css_parse(const char *css, char **error);
bool tai_css_extend(TaiStylesheet *sheet, const char *css, char **error);
void tai_css_destroy(TaiStylesheet *sheet);
bool tai_css_style(TaiNode *root, const TaiStylesheet *sheet, char **error);
TaiSelector *tai_selector_parse(const char *text, char **error);
bool tai_selector_matches(const TaiSelector *selector, const TaiNode *node);
void tai_selector_destroy(TaiSelector *selector);
void tai_css_json(FILE *out, const TaiStylesheet *sheet);
#endif
