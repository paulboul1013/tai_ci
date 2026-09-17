#include "tai/scheduler.h"
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>

typedef struct { int *log; int value; int *destroyed; } Entry;
static void run_entry(void *opaque) {
    Entry *entry = opaque;
    *entry->log = *entry->log * 10 + entry->value;
}
static void destroy_entry(void *opaque) {
    Entry *entry = opaque;
    if (entry->destroyed) (*entry->destroyed)++;
    free(entry);
}
static bool enqueue(TaiScheduler *scheduler, TaiTaskPriority priority,
                    uint64_t generation, double now, int value,
                    int *log, int *destroyed) {
    Entry *entry = malloc(sizeof(*entry));
    assert_non_null(entry);
    *entry = (Entry){log, value, destroyed};
    if (!tai_scheduler_schedule(scheduler, priority, generation, now,
                                run_entry, destroy_entry, entry)) {
        free(entry);
        return false;
    }
    return true;
}
static TaiScheduler *make_scheduler(bool fifo) {
    return tai_scheduler_create((TaiSchedulerConfig){
        .fifo = fifo, .starvation_seconds = .120,
        .max_foreground_burst = 2, .frame_guard_seconds = .003});
}
static void priority_and_fairness(void **state) {
    (void)state;
    int log = 0, destroyed = 0;
    TaiScheduler *s = make_scheduler(false);
    assert_true(enqueue(s, TAI_TASK_JS_TIMER, 0, 0.0, 4, &log, &destroyed));
    assert_true(enqueue(s, TAI_TASK_NORMAL, 0, 0.0, 3, &log, &destroyed));
    assert_true(enqueue(s, TAI_TASK_INPUT, 0, 0.0, 2, &log, &destroyed));
    assert_true(enqueue(s, TAI_TASK_RENDER, 0, 0.0, 1, &log, &destroyed));
    const char *reason = NULL;
    assert_int_equal(tai_scheduler_run_one(s, .010, &reason), TAI_SCHEDULER_RAN);
    assert_string_equal(reason, "render_ready");
    assert_int_equal(tai_scheduler_run_one(s, .011, &reason), TAI_SCHEDULER_RAN);
    assert_string_equal(reason, "input_ready");
    /* Two foreground tasks grant the waiting timer a fairness slot. */
    assert_int_equal(tai_scheduler_run_one(s, .012, &reason), TAI_SCHEDULER_RAN);
    assert_string_equal(reason, "timer_fairness");
    assert_int_equal(tai_scheduler_run_one(s, .013, &reason), TAI_SCHEDULER_RAN);
    assert_string_equal(reason, "normal_ready");
    assert_int_equal(log, 1243);
    assert_int_equal(destroyed, 4);
    tai_scheduler_destroy(s);
}
static void frame_guard_and_aging(void **state) {
    (void)state;
    int log = 0;
    TaiScheduler *s = make_scheduler(false);
    assert_true(enqueue(s, TAI_TASK_JS_TIMER, 0, 1.0, 1, &log, NULL));
    tai_scheduler_set_frame_deadline(s, true, 1.002);
    assert_int_equal(tai_scheduler_run_one(s, 1.0, NULL), TAI_SCHEDULER_HELD_FOR_FRAME);
    tai_scheduler_set_frame_deadline(s, false, 0.0);
    assert_true(enqueue(s, TAI_TASK_NORMAL, 0, 1.0, 2, &log, NULL));
    const char *reason = NULL;
    assert_int_equal(tai_scheduler_run_one(s, 1.2, &reason), TAI_SCHEDULER_RAN);
    assert_string_equal(reason, "timer_aging");
    tai_scheduler_destroy(s);
}
static void generation_cancels_stale_work(void **state) {
    (void)state;
    int log = 0, destroyed = 0;
    TaiScheduler *s = make_scheduler(false);
    tai_scheduler_set_generation(s, 7);
    assert_true(enqueue(s, TAI_TASK_NORMAL, 7, 0.0, 1, &log, &destroyed));
    assert_true(enqueue(s, TAI_TASK_NORMAL, 0, 0.0, 2, &log, &destroyed));
    tai_scheduler_set_generation(s, 8);
    assert_int_equal(destroyed, 1);
    assert_int_equal(tai_scheduler_pending(s), 1);
    assert_int_equal(tai_scheduler_run_one(s, 0.1, NULL), TAI_SCHEDULER_RAN);
    assert_int_equal(log, 2);
    assert_int_equal(destroyed, 2);
    tai_scheduler_destroy(s);
}
static void fifo_preserves_global_sequence(void **state) {
    (void)state;
    int log = 0;
    TaiScheduler *s = make_scheduler(true);
    assert_true(enqueue(s, TAI_TASK_JS_TIMER, 0, 0.0, 1, &log, NULL));
    assert_true(enqueue(s, TAI_TASK_RENDER, 0, 0.0, 2, &log, NULL));
    assert_int_equal(tai_scheduler_run_one(s, 0.1, NULL), TAI_SCHEDULER_RAN);
    assert_int_equal(tai_scheduler_run_one(s, 0.1, NULL), TAI_SCHEDULER_RAN);
    assert_int_equal(log, 12);
    tai_scheduler_destroy(s);
}
int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(priority_and_fairness),
        cmocka_unit_test(frame_guard_and_aging),
        cmocka_unit_test(generation_cancels_stale_work),
        cmocka_unit_test(fifo_preserves_global_sequence),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
