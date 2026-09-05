#include "tai/css.h"
#include <assert.h>
#include <string.h>
int main(void) {
    char *error = NULL;
    TaiStylesheet *sheet = tai_css_parse("/* swallowed */ p {color:red} p {color:blue!important;color:green} .card:has(span) {font:italic bold 150% Times New Roman}", &error);
    assert(sheet && !error);
    TaiNode root = {.kind=TAI_ELEMENT,.tag="p"};
    assert(tai_css_style(&root,sheet,&error));
    assert(strcmp(tai_map_get(&root.style,"color"),"green")==0);
    assert(tai_map_set(&root.attributes,"style","color:red!important; color:blue; font-size:150%",0));
    assert(tai_css_style(&root,sheet,&error));
    assert(strcmp(tai_map_get(&root.style,"font-size"),"24.0px")==0);
    assert(strcmp(tai_map_get(&root.style,"color"),"blue")==0);
    TaiSelector *sel=tai_selector_parse("p:has(span)",&error);
    TaiNode child={.kind=TAI_ELEMENT,.tag="span",.parent=&root};
    TaiNode *children[]={&child}; root.children=children; root.child_count=1;
    assert(sel && tai_selector_matches(sel,&root));
    tai_selector_destroy(sel); tai_css_destroy(sheet);
    tai_map_clear(&root.style); tai_map_clear(&root.attributes);
    return 0;
}
