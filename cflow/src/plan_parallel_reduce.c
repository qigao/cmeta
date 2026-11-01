#include <cflow/plan_internal.h>
#include "executor_internal.h"
#include "result_storage.h"
#include "value_storage.h"
#include <thread.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct cflow_parallel_reduce_frame cflow_parallel_reduce_frame;

enum { CFLOW_PARALLEL_REDUCE_WRITE_ISOLATION = 128u };

typedef struct cflow_parallel_reduce_task {
    cflow_parallel_reduce_frame *frame;
    size_t index;
    size_t begin;
    size_t count;
} cflow_parallel_reduce_task;

struct cflow_parallel_reduce_frame {
    cmeta_mutex_t mutex;
    cmeta_cond_t condition;
    const cflow_plan_inst *reducer;
    cflow_plan_value_vec prefix;
    cflow_parallel_reduce_task *tasks;
    unsigned char *partials;
    unsigned char *scratch;
    cflow_value_slot *partial_slots;
    cflow_value_slot *scratch_slots;
    size_t slot_stride;
    size_t slot_count;
    size_t completed;
    bool managed_values;
    bool failed;
};

static bool checked_bytes(size_t count, size_t size, size_t *bytes) {
    if (!bytes || (size && count > SIZE_MAX / size)) return false;
    *bytes = count * size;
    return true;
}

static cflow_value_slot *managed_slot_at(
    cflow_value_slot *slots, size_t stride, size_t index) {
    if (!slots || stride < sizeof(cflow_value_slot)) return NULL;
    return (cflow_value_slot *)((unsigned char *)slots + index * stride);
}

static void parallel_reduce_task_settle(cflow_parallel_reduce_task *task,
                                        bool succeeded) {
    cflow_parallel_reduce_frame *frame = task ? task->frame : NULL;
    if (!frame) return;
    cmeta_mutex_lock(&frame->mutex);
    if (!succeeded) frame->failed = true;
    ++frame->completed;
    cmeta_cond_broadcast(&frame->condition);
    cmeta_mutex_unlock(&frame->mutex);
}

static void parallel_reduce_task_run(void *user) {
    cflow_parallel_reduce_task *task = (cflow_parallel_reduce_task *)user;
    cflow_parallel_reduce_frame *frame = task ? task->frame : NULL;
    bool ok = frame && frame->reducer && task->count;

    if (ok && frame->managed_values) {
        const size_t value_size = frame->prefix.type->size;
        cflow_value_slot *acc = managed_slot_at(
            frame->partial_slots, frame->slot_stride, task->index);
        cflow_value_slot *tmp = managed_slot_at(
            frame->scratch_slots, frame->slot_stride, task->index);

        ok = cflow_value_slot_copy(
            acc, frame->prefix.data + task->begin * value_size);
        for (size_t offset = 1u; ok && offset < task->count; ++offset) {
            const void *args[2] = {
                acc->storage,
                frame->prefix.data + (task->begin + offset) * value_size
            };
            if (!frame->reducer->call.invoke ||
                !frame->reducer->call.invoke(
                    &frame->reducer->call.fn, tmp->storage, args)) {
                ok = false;
                break;
            }
            tmp->live = true;
            cflow_value_slot_reset(acc);
            if (!cflow_value_slot_move(acc, tmp)) {
                ok = false;
                break;
            }
        }
    } else if (ok) {
        const size_t value_size = frame->prefix.type->size;
        unsigned char *acc = frame->partials + task->index * frame->slot_stride;
        unsigned char *tmp = frame->scratch + task->index * frame->slot_stride;
        memcpy(acc, frame->prefix.data + task->begin * value_size, value_size);
        for (size_t offset = 1u; offset < task->count; ++offset) {
            const void *args[2] = {
                acc,
                frame->prefix.data + (task->begin + offset) * value_size
            };
            if (!frame->reducer->call.invoke ||
                !frame->reducer->call.invoke(&frame->reducer->call.fn, tmp, args)) {
                ok = false;
                break;
            }
            memcpy(acc, tmp, value_size);
        }
    }

    parallel_reduce_task_settle(task, ok);
}

static void parallel_reduce_task_cancel(void *user) {
    parallel_reduce_task_settle(
        (cflow_parallel_reduce_task *)user, false);
}

static void parallel_reduce_frame_destroy(cflow_parallel_reduce_frame *frame) {
    if (!frame) return;
    cmeta_cond_destroy(&frame->condition);
    cmeta_mutex_destroy(&frame->mutex);
    if (frame->partial_slots) {
        for (size_t index = 0u; index < frame->slot_count; ++index)
            cflow_value_slot_destroy(managed_slot_at(
                frame->partial_slots, frame->slot_stride, index));
    }
    if (frame->scratch_slots) {
        for (size_t index = 0u; index < frame->slot_count; ++index)
            cflow_value_slot_destroy(managed_slot_at(
                frame->scratch_slots, frame->slot_stride, index));
    }
    free(frame->scratch_slots);
    free(frame->partial_slots);
    free(frame->scratch);
    free(frame->partials);
    free(frame->tasks);
    cflow_plan_value_vec_destroy(&frame->prefix);
    free(frame);
}

static bool parallel_reduce_frame_init(cflow_parallel_reduce_frame **out,
                                       const cflow_plan *plan,
                                       const void *inputs,
                                       size_t input_count) {
    const cflow_plan_impl *impl = (const cflow_plan_impl *)plan->impl;
    cflow_parallel_reduce_frame *frame;

    if (!out || !impl || impl->terminal_reduce_index >= impl->count)
        return false;
    *out = NULL;
    frame = (cflow_parallel_reduce_frame *)calloc(1, sizeof(*frame));
    if (!frame) return false;
    frame->reducer = &impl->code[impl->terminal_reduce_index];
    if (!cflow_plan_eval_prefix_materialized(
            plan, inputs, input_count, impl->terminal_reduce_index,
            &frame->prefix) ||
        !frame->prefix.type || !frame->prefix.type->size) {
        parallel_reduce_frame_destroy(frame);
        return false;
    }
    frame->managed_values =
        !cflow_value_storage_type_supported(frame->prefix.type);
    cmeta_mutex_init(&frame->mutex);
    cmeta_cond_init(&frame->condition);
    if (!frame->mutex || !frame->condition) {
        parallel_reduce_frame_destroy(frame);
        return false;
    }
    *out = frame;
    return true;
}

static bool parallel_reduce_frame_allocate_tasks(
    cflow_parallel_reduce_frame *frame, size_t task_count) {
    size_t slot_bytes;
    size_t stride;
    if (!frame || !task_count ||
        task_count > SIZE_MAX / sizeof(cflow_parallel_reduce_task))
        return false;

    frame->tasks = (cflow_parallel_reduce_task *)calloc(
        task_count, sizeof(*frame->tasks));
    if (!frame->tasks) return false;

    if (frame->managed_values) {
        /*
         * Each worker toggles its partial/scratch slot live state on every
         * reduction step. Keep write-hot slot metadata on the same isolation
         * stride used by trivial partial storage to avoid cross-task cache-line
         * sharing without changing value lifecycle semantics.
         */
        if (sizeof(cflow_value_slot) >
            SIZE_MAX - (CFLOW_PARALLEL_REDUCE_WRITE_ISOLATION - 1u))
            return false;
        stride = sizeof(cflow_value_slot) +
            (CFLOW_PARALLEL_REDUCE_WRITE_ISOLATION - 1u);
        stride -= stride % CFLOW_PARALLEL_REDUCE_WRITE_ISOLATION;
        if (!checked_bytes(task_count, stride, &slot_bytes))
            return false;
        frame->partial_slots = (cflow_value_slot *)calloc(1u, slot_bytes);
        frame->scratch_slots = (cflow_value_slot *)calloc(1u, slot_bytes);
        frame->slot_stride = stride;
        frame->slot_count = task_count;
        if (!frame->partial_slots || !frame->scratch_slots)
            return false;
        for (size_t index = 0u; index < task_count; ++index) {
            if (!cflow_value_slot_init(
                    managed_slot_at(
                        frame->partial_slots, frame->slot_stride, index),
                    frame->prefix.type) ||
                !cflow_value_slot_init(
                    managed_slot_at(
                        frame->scratch_slots, frame->slot_stride, index),
                    frame->prefix.type))
                return false;
        }
        return true;
    }

    if (frame->prefix.type->size >
        SIZE_MAX - (CFLOW_PARALLEL_REDUCE_WRITE_ISOLATION - 1u))
        return false;
    stride = frame->prefix.type->size +
        (CFLOW_PARALLEL_REDUCE_WRITE_ISOLATION - 1u);
    stride -= stride % CFLOW_PARALLEL_REDUCE_WRITE_ISOLATION;
    if (!checked_bytes(task_count, stride, &slot_bytes)) return false;
    frame->partials = (unsigned char *)malloc(slot_bytes);
    frame->scratch = (unsigned char *)malloc(slot_bytes);
    frame->slot_stride = stride;
    return frame->partials && frame->scratch;
}

static size_t parallel_task_count(size_t item_count,
                                  const cflow_plan_eval_options *options) {
    size_t desired;
    if (!options || !options->max_tasks || !options->min_items_per_task)
        return 0u;
    desired = item_count / options->min_items_per_task;
    if (item_count % options->min_items_per_task) ++desired;
    return desired < options->max_tasks ? desired : options->max_tasks;
}

static bool wait_for_accepted(cflow_parallel_reduce_frame *frame,
                              size_t accepted) {
    bool succeeded;
    cmeta_mutex_lock(&frame->mutex);
    while (frame->completed < accepted)
        cmeta_cond_wait(&frame->condition, &frame->mutex);
    succeeded = !frame->failed;
    cmeta_mutex_unlock(&frame->mutex);
    return succeeded;
}

static bool merge_managed_partials(cflow_parallel_reduce_frame *frame,
                                   size_t task_count,
                                   cflow_result *out) {
    cflow_value_slot acc = {0};
    cflow_value_slot tmp = {0};
    void *result_allocation = NULL;
    unsigned char *result_data = NULL;
    cflow_value_slot *first =
        frame && frame->managed_values
            ? managed_slot_at(frame->partial_slots, frame->slot_stride, 0u)
            : NULL;
    bool ok = frame && task_count && out && frame->managed_values && first &&
        cflow_value_slot_init(&acc, frame->prefix.type) &&
        cflow_value_slot_init(&tmp, frame->prefix.type) &&
        cflow_value_slot_move(&acc, first);

    for (size_t index = 1u; ok && index < task_count; ++index) {
        cflow_value_slot *partial = managed_slot_at(
            frame->partial_slots, frame->slot_stride, index);
        const void *args[2] = {
            acc.storage,
            partial ? partial->storage : NULL
        };
        if (!partial || !partial->live ||
            !frame->reducer->call.invoke ||
            !frame->reducer->call.invoke(
                &frame->reducer->call.fn, tmp.storage, args)) {
            ok = false;
            break;
        }
        tmp.live = true;
        cflow_value_slot_reset(&acc);
        if (!cflow_value_slot_move(&acc, &tmp)) {
            ok = false;
            break;
        }
    }

    if (ok) {
        ok = cflow_result_storage_allocate(
            frame->reducer->output_type, 1u,
            &result_allocation, &result_data);
    }
    if (ok) {
        ok = cflow_value_move_construct(
            acc.type, result_data, acc.storage);
        if (ok) acc.live = false;
    }
    if (ok) {
        out->data = result_data;
        out->count = 1u;
        out->type = frame->reducer->output_type;
        result_allocation = NULL;
        result_data = NULL;
    }

    free(result_allocation);
    cflow_value_slot_destroy(&tmp);
    cflow_value_slot_destroy(&acc);
    return ok;
}

static bool merge_partials(cflow_parallel_reduce_frame *frame,
                           size_t task_count,
                           cflow_result *out) {
    const size_t value_size = frame->prefix.type->size;
    unsigned char *result;
    unsigned char *tmp;
    bool ok;

    if (frame->managed_values)
        return merge_managed_partials(frame, task_count, out);

    result = (unsigned char *)malloc(value_size);
    tmp = (unsigned char *)malloc(value_size);
    ok = result && tmp;

    if (ok) memcpy(result, frame->partials, value_size);
    for (size_t index = 1u; ok && index < task_count; ++index) {
        const void *args[2] = {
            result,
            frame->partials + index * frame->slot_stride
        };
        ok = frame->reducer->call.invoke &&
            frame->reducer->call.invoke(&frame->reducer->call.fn, tmp, args);
        if (ok) memcpy(result, tmp, value_size);
    }
    free(tmp);
    if (!ok) {
        free(result);
        return false;
    }
    out->data = result;
    out->count = 1u;
    out->type = frame->reducer->output_type;
    return true;
}

static bool eval_parallel_reduce(const cflow_plan *plan,
                                 const void *inputs,
                                 size_t input_count,
                                 const cflow_plan_eval_options *options,
                                 cflow_result *out) {
    cflow_parallel_reduce_frame *frame = NULL;
    size_t task_count;
    size_t accepted = 0u;
    size_t begin = 0u;
    bool admission_failed = false;
    bool ok = false;

    if (!plan || !plan->impl || !out || !options ||
        !options->max_tasks || !options->min_items_per_task ||
        !cflow_plan_parallel_reduce_supported(plan) ||
        !cflow_executor_has(options->executor, CMETA_EXEC_CAP_CONCURRENT) ||
        cflow_executor_is_current_internal(options->executor))
        return false;

    if (!parallel_reduce_frame_init(&frame, plan, inputs, input_count))
        goto done;
    task_count = parallel_task_count(frame->prefix.count, options);
    if (task_count < 2u || task_count > frame->prefix.count ||
        !parallel_reduce_frame_allocate_tasks(frame, task_count))
        goto done;

    {
        const size_t base = frame->prefix.count / task_count;
        const size_t remainder = frame->prefix.count % task_count;
        for (size_t index = 0u; index < task_count; ++index) {
            cflow_parallel_reduce_task *task = &frame->tasks[index];
            const size_t count = base + (index < remainder ? 1u : 0u);
            cflow_admission_status status;
            cflow_executor_task descriptor;
            task->frame = frame;
            task->index = index;
            task->begin = begin;
            task->count = count;
            begin += count;
            descriptor = (cflow_executor_task){
                .run = parallel_reduce_task_run,
                .cancel = parallel_reduce_task_cancel,
                .finalize = NULL,
                .user = task
            };
            status = cflow_executor_try_post_task(
                options->executor, &descriptor);
            if (status == CFLOW_ADMISSION_INVALID_ARGUMENT) {
                status = cflow_executor_try_post(
                    options->executor, parallel_reduce_task_run, task);
            }
            if (status != CFLOW_ADMISSION_ACCEPTED) {
                admission_failed = true;
                break;
            }
            ++accepted;
        }
    }

    if (!wait_for_accepted(frame, accepted) || admission_failed ||
        accepted != task_count)
        goto done;
    ok = merge_partials(frame, task_count, out);

done:
    parallel_reduce_frame_destroy(frame);
    return ok;
}

bool cflow_plan_eval_array_with_options(
    const cflow_plan *plan,
    const void *inputs,
    size_t input_count,
    const cflow_plan_eval_options *options,
    cflow_result *out) {
    if (!options || !out) return false;
    if (options->mode == CFLOW_PLAN_EXECUTION_SEQUENTIAL)
        return cflow_plan_eval_array_profile(plan, inputs, input_count, out, NULL);
    if (options->mode != CFLOW_PLAN_EXECUTION_PARALLEL_REDUCE)
        return false;
    return eval_parallel_reduce(plan, inputs, input_count, options, out);
}
