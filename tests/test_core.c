#include "tai/core.h"
#include <stdarg.h>
#include <setjmp.h>
#include <stdlib.h>
#include <cmocka.h>
static void map_ownership(void **state) {
    (void)state;
    TaiMap map = {0}, copy = {0};
    char key[] = "color", value[] = "red";
    assert_true(tai_map_set(&map, key, value, 10));
    value[0] = 'b';
    assert_string_equal(tai_map_get(&map, key), "red");
    assert_true(tai_map_copy(&copy, &map));
    assert_true(tai_map_set(&map, key, tai_map_get(&map, key), 100));
    assert_int_equal(tai_map_priority(&map, key), 100);
    tai_map_clear(&map);
    assert_string_equal(tai_map_get(&copy, key), "red");
    assert_null(tai_map_get(&copy, "absent"));
    tai_map_clear(&copy);
    tai_map_clear(&copy);
}
int main(void) {
    const struct CMUnitTest tests[] = { cmocka_unit_test(map_ownership) };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
