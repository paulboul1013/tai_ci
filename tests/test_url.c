#include "tai/url.h"
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
static void parsing(void **state) {
    (void)state;
    TaiUrl *u = tai_url_parse("http://Example.test:8080/a/b#part");
    assert_non_null(u);
    assert_string_equal(tai_url_key(u), "http://Example.test:8080/a/b");
    assert_string_equal(tai_url_origin(u), "http://Example.test:8080");
    assert_string_equal(tai_url_fragment(u), "part");
    TaiUrl *v = tai_url_resolve(u, "../c");
    assert_non_null(v);
    assert_string_equal(tai_url_string(v), "http://Example.test:8080/c");
    tai_url_destroy(v);
    assert_null(tai_url_resolve(u, "javascript:alert(1)"));
    tai_url_destroy(u);
    u = tai_url_parse("view-source:https://example.test");
    assert_string_equal(tai_url_string(u), "view-source:https://example.test:443/");
    tai_url_destroy(u);
    u = tai_url_parse("broken");
    assert_string_equal(tai_url_string(u), "about:blank");
    tai_url_destroy(u);
}
int main(void) {
    const struct CMUnitTest tests[] = {cmocka_unit_test(parsing)};
    return cmocka_run_group_tests(tests, NULL, NULL);
}
