#include "tai/scheduler.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct TaiScheduledTask TaiScheduledTask;
struct TaiScheduledTask {
    TaiScheduledTask *next;
    TaiTaskFunction function;
    TaiTaskDestroy destroy;
    void *userdata;
    TaiTaskPriority priority;
    uint64_t generation;
    uint64_t sequence;
    double enqueued_at;
};

typedef struct {
    TaiScheduledTask *head;
    TaiScheduledTask *tail;
} TaiTaskQueue;

struct TaiScheduler {
    pthread_mutex_t mutex;
    TaiTaskQueue queues[4];
    TaiSchedulerConfig config;
    uint64_t sequence;
    uint64_t generation;
    size_t pending;
    size_t foreground_burst;
    bool frame_armed;
    double frame_deadline;
};

static void destroy_tasks(TaiScheduledTask *task) {
    while (task) {
        TaiScheduledTask *next = task->next;
        if (task->destroy) task->destroy(task->userdata);
        free(task);
        task = next;
    }
}

TaiScheduler *tai_scheduler_create(TaiSchedulerConfig config) {
    if (config.starvation_seconds < 0.0 || config.frame_guard_seconds < 0.0 ||
        config.max_foreground_burst == 0) return NULL;
    TaiScheduler *scheduler = calloc(1, sizeof(*scheduler));
    if (!scheduler) return NULL;
    scheduler->config = config;
    if (pthread_mutex_init(&scheduler->mutex, NULL) != 0) {
        free(scheduler);
        return NULL;
    }
    return scheduler;
}

void tai_scheduler_clear(TaiScheduler *scheduler) {
    if (!scheduler) return;
    TaiScheduledTask *discarded = NULL;
    pthread_mutex_lock(&scheduler->mutex);
    for (size_t i = 0; i < 4; i++) {
        if (scheduler->queues[i].tail) {
            scheduler->queues[i].tail->next = discarded;
            discarded = scheduler->queues[i].head;
        }
        scheduler->queues[i] = (TaiTaskQueue){0};
    }
    scheduler->pending = 0;
    scheduler->foreground_burst = 0;
    pthread_mutex_unlock(&scheduler->mutex);
    destroy_tasks(discarded);
}

void tai_scheduler_destroy(TaiScheduler *scheduler) {
    if (!scheduler) return;
    tai_scheduler_clear(scheduler);
    pthread_mutex_destroy(&scheduler->mutex);
    free(scheduler);
}

bool tai_scheduler_schedule(TaiScheduler *scheduler, TaiTaskPriority priority,
                            uint64_t generation, double enqueued_at,
                            TaiTaskFunction function, TaiTaskDestroy destroy,
                            void *userdata) {
    if (!scheduler || !function || priority < TAI_TASK_RENDER ||
        priority > TAI_TASK_JS_TIMER) return false;
    TaiScheduledTask *task = calloc(1, sizeof(*task));
    if (!task) return false;
    task->function = function;
    task->destroy = destroy;
    task->userdata = userdata;
    task->priority = priority;
    task->generation = generation;
    task->enqueued_at = enqueued_at;

    pthread_mutex_lock(&scheduler->mutex);
    if (generation != 0 && generation != scheduler->generation) {
        pthread_mutex_unlock(&scheduler->mutex);
        free(task);
        return false;
    }
    task->sequence = scheduler->sequence++;
    TaiTaskQueue *queue = &scheduler->queues[priority];
    if (queue->tail) queue->tail->next = task;
    else queue->head = task;
    queue->tail = task;
    scheduler->pending++;
    pthread_mutex_unlock(&scheduler->mutex);
    return true;
}

static TaiScheduledTask *pop(TaiScheduler *scheduler, TaiTaskPriority priority,
                             const char **reason, const char *why) {
    TaiTaskQueue *queue = &scheduler->queues[priority];
    TaiScheduledTask *task = queue->head;
    if (!task) return NULL;
    queue->head = task->next;
    if (!queue->head) queue->tail = NULL;
    task->next = NULL;
    scheduler->pending--;
    if (priority == TAI_TASK_JS_TIMER) scheduler->foreground_burst = 0;
    else scheduler->foreground_burst++;
    if (reason) *reason = why;
    return task;
}

static TaiScheduledTask *pick_fifo(TaiScheduler *scheduler,
                                   const char **reason) {
    bool found = false;
    uint64_t sequence = 0;
    TaiTaskPriority selected = TAI_TASK_RENDER;
    for (int i = TAI_TASK_RENDER; i <= TAI_TASK_JS_TIMER; i++) {
        TaiScheduledTask *head = scheduler->queues[i].head;
        if (head && (!found || head->sequence < sequence)) {
            found = true;
            sequence = head->sequence;
            selected = (TaiTaskPriority)i;
        }
    }
    return found ? pop(scheduler, selected, reason, "fifo") : NULL;
}

static bool frame_guard(const TaiScheduler *scheduler, double now) {
    return scheduler->frame_armed &&
        scheduler->frame_deadline - now <= scheduler->config.frame_guard_seconds;
}

static TaiScheduledTask *pick_priority(TaiScheduler *scheduler, double now,
                                       const char **reason) {
    if (scheduler->queues[TAI_TASK_RENDER].head)
        return pop(scheduler, TAI_TASK_RENDER, reason, "render_ready");
    bool guarded = frame_guard(scheduler, now);
    if (scheduler->queues[TAI_TASK_INPUT].head)
        return pop(scheduler, TAI_TASK_INPUT, reason, "input_ready");
    TaiScheduledTask *timer = scheduler->queues[TAI_TASK_JS_TIMER].head;
    if (timer && !guarded &&
        now - timer->enqueued_at >= scheduler->config.starvation_seconds)
        return pop(scheduler, TAI_TASK_JS_TIMER, reason, "timer_aging");
    if (timer && !guarded && scheduler->foreground_burst >=
        scheduler->config.max_foreground_burst)
        return pop(scheduler, TAI_TASK_JS_TIMER, reason, "timer_fairness");
    if (scheduler->queues[TAI_TASK_NORMAL].head)
        return pop(scheduler, TAI_TASK_NORMAL, reason, "normal_ready");
    if (timer && !guarded)
        return pop(scheduler, TAI_TASK_JS_TIMER, reason, "timer_background");
    return NULL;
}

TaiSchedulerResult tai_scheduler_run_one(TaiScheduler *scheduler, double now,
                                         const char **reason) {
    if (!scheduler) return TAI_SCHEDULER_EMPTY;
    if (reason) *reason = NULL;
    pthread_mutex_lock(&scheduler->mutex);
    size_t pending = scheduler->pending;
    TaiScheduledTask *task = scheduler->config.fifo
        ? pick_fifo(scheduler, reason)
        : pick_priority(scheduler, now, reason);
    pthread_mutex_unlock(&scheduler->mutex);
    if (!task) return pending ? TAI_SCHEDULER_HELD_FOR_FRAME : TAI_SCHEDULER_EMPTY;
    task->function(task->userdata);
    if (task->destroy) task->destroy(task->userdata);
    free(task);
    return TAI_SCHEDULER_RAN;
}

void tai_scheduler_set_frame_deadline(TaiScheduler *scheduler, bool armed,
                                      double deadline) {
    if (!scheduler) return;
    pthread_mutex_lock(&scheduler->mutex);
    scheduler->frame_armed = armed;
    scheduler->frame_deadline = deadline;
    pthread_mutex_unlock(&scheduler->mutex);
}

void tai_scheduler_set_generation(TaiScheduler *scheduler, uint64_t generation) {
    if (!scheduler) return;
    TaiScheduledTask *discarded = NULL;
    pthread_mutex_lock(&scheduler->mutex);
    scheduler->generation = generation;
    for (size_t i = 0; i < 4; i++) {
        TaiTaskQueue kept = {0};
        TaiScheduledTask *task = scheduler->queues[i].head;
        while (task) {
            TaiScheduledTask *next = task->next;
            task->next = NULL;
            if (task->generation == 0 || task->generation == generation) {
                if (kept.tail) kept.tail->next = task;
                else kept.head = task;
                kept.tail = task;
            } else {
                task->next = discarded;
                discarded = task;
                scheduler->pending--;
            }
            task = next;
        }
        scheduler->queues[i] = kept;
    }
    pthread_mutex_unlock(&scheduler->mutex);
    destroy_tasks(discarded);
}

uint64_t tai_scheduler_generation(const TaiScheduler *scheduler) {
    if (!scheduler) return 0;
    TaiScheduler *mutable = (TaiScheduler *)scheduler;
    pthread_mutex_lock(&mutable->mutex);
    uint64_t generation = mutable->generation;
    pthread_mutex_unlock(&mutable->mutex);
    return generation;
}

size_t tai_scheduler_pending(const TaiScheduler *scheduler) {
    if (!scheduler) return 0;
    TaiScheduler *mutable = (TaiScheduler *)scheduler;
    pthread_mutex_lock(&mutable->mutex);
    size_t pending = mutable->pending;
    pthread_mutex_unlock(&mutable->mutex);
    return pending;
}
