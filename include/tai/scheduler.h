#ifndef TAI_SCHEDULER_H
#define TAI_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TAI_TASK_RENDER = 0,
    TAI_TASK_INPUT = 1,
    TAI_TASK_NORMAL = 2,
    TAI_TASK_JS_TIMER = 3
} TaiTaskPriority;

typedef enum {
    TAI_SCHEDULER_EMPTY,
    TAI_SCHEDULER_HELD_FOR_FRAME,
    TAI_SCHEDULER_RAN
} TaiSchedulerResult;

typedef void (*TaiTaskFunction)(void *userdata);
typedef void (*TaiTaskDestroy)(void *userdata);
typedef struct TaiScheduler TaiScheduler;

typedef struct {
    bool fifo;
    double starvation_seconds;
    size_t max_foreground_burst;
    double frame_guard_seconds;
} TaiSchedulerConfig;

/* A scheduler owns accepted task userdata until execution or cancellation.
 * All calls are serialized by the implementation. Callbacks execute without
 * holding the queue mutex and may enqueue more tasks. */
TaiScheduler *tai_scheduler_create(TaiSchedulerConfig config);
void tai_scheduler_destroy(TaiScheduler *scheduler);
bool tai_scheduler_schedule(TaiScheduler *scheduler, TaiTaskPriority priority,
    uint64_t generation, double enqueued_at, TaiTaskFunction function,
    TaiTaskDestroy destroy, void *userdata);
TaiSchedulerResult tai_scheduler_run_one(TaiScheduler *scheduler, double now,
    const char **reason);
void tai_scheduler_set_frame_deadline(TaiScheduler *scheduler,
    bool armed, double deadline);
void tai_scheduler_set_generation(TaiScheduler *scheduler, uint64_t generation);
uint64_t tai_scheduler_generation(const TaiScheduler *scheduler);
size_t tai_scheduler_pending(const TaiScheduler *scheduler);
void tai_scheduler_clear(TaiScheduler *scheduler);

#endif
