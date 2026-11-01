#include "tinytest.h"

#include <cflow/cflow.h>
#include <cflow/function_projection.h>
#include <cflow/plan_internal.h>
#include <thread.h>

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic size_t cflow_parallel_prefix_calls;
static _Atomic long cflow_parallel_fail_left;
static _Atomic size_t cflow_parallel_failures;
static _Atomic size_t cflow_parallel_successes;
static _Atomic size_t cflow_parallel_successes_at_failure;
static _Atomic bool cflow_parallel_delay_failure;
static _Atomic bool cflow_parallel_gate_open;
static _Atomic bool cflow_parallel_gate_started;

static _Atomic size_t cflow_parallel_managed_live;
static _Atomic size_t cflow_parallel_managed_destroyed;
static _Atomic int cflow_parallel_managed_fail_right;

typedef struct cflow_parallel_managed_value {
  _Alignas(64) int *resource;
} cflow_parallel_managed_value;

static cflow_parallel_managed_value cflow_parallel_managed_make(int value) {
  cflow_parallel_managed_value result = {0};
  result.resource = (int *)malloc(sizeof(*result.resource));
  if (result.resource) {
    *result.resource = value;
    atomic_fetch_add(&cflow_parallel_managed_live, 1u);
  }
  return result;
}

static bool cflow_parallel_managed_copy(void *destination_,
                                        const void *source_) {
  cflow_parallel_managed_value *destination =
      (cflow_parallel_managed_value *)destination_;
  const cflow_parallel_managed_value *source =
      (const cflow_parallel_managed_value *)source_;
  if (!destination || !source) return false;
  destination->resource = NULL;
  if (!source->resource) return true;
  *destination = cflow_parallel_managed_make(*source->resource);
  return destination->resource != NULL;
}

static void cflow_parallel_managed_move(void *destination_, void *source_) {
  cflow_parallel_managed_value *destination =
      (cflow_parallel_managed_value *)destination_;
  cflow_parallel_managed_value *source =
      (cflow_parallel_managed_value *)source_;
  if (!destination || !source) return;
  destination->resource = source->resource;
  source->resource = NULL;
}

static void cflow_parallel_managed_destroy(void *value_) {
  cflow_parallel_managed_value *value =
      (cflow_parallel_managed_value *)value_;
  if (!value) return;
  if (value->resource) {
    free(value->resource);
    value->resource = NULL;
    atomic_fetch_sub(&cflow_parallel_managed_live, 1u);
  }
  atomic_fetch_add(&cflow_parallel_managed_destroyed, 1u);
}

static const cmeta_type_traits cflow_parallel_managed_traits = {
  .flags = CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
  .copy_construct = cflow_parallel_managed_copy,
  .move_construct = cflow_parallel_managed_move,
  .destroy = cflow_parallel_managed_destroy
};

static const cmeta_type_desc cflow_parallel_managed_type = {
  .name = "cflow_parallel_managed_value",
  .size = sizeof(cflow_parallel_managed_value),
  .align = _Alignof(cflow_parallel_managed_value),
  .kind = CMETA_T_OBJECT,
  .pointee = NULL,
  .traits = &cflow_parallel_managed_traits,
  .identity = NULL
};

static const cmeta_param_desc cflow_parallel_managed_reduce_params[] = {
  {
    .size = sizeof(cmeta_param_desc),
    .name = "left",
    .type = &cflow_parallel_managed_type,
    .flags = CMETA_PARAM_IN
  },
  {
    .size = sizeof(cmeta_param_desc),
    .name = "right",
    .type = &cflow_parallel_managed_type,
    .flags = CMETA_PARAM_IN
  }
};

static const cmeta_function_desc cflow_parallel_managed_reduce_function = {
  .size = sizeof(cmeta_function_desc),
  .name = "cflow.parallel.managed.reduce",
  .return_type = &cflow_parallel_managed_type,
  .params = cflow_parallel_managed_reduce_params,
  .param_count = 2u,
  .effects = CMETA_EFFECT_PURE,
  .properties = CMETA_PROP_DETERMINISTIC | CMETA_PROP_TOTAL |
                CMETA_PROP_ASSOCIATIVE | CMETA_PROP_NO_ALIAS,
  .result_flags = CMETA_RESULT_VALUE
};

static const cmeta_abi_carrier cflow_parallel_managed_reduce_param_abi[] = {
  CMETA_ABI_AGGREGATE,
  CMETA_ABI_AGGREGATE
};

static const cmeta_function_abi_desc cflow_parallel_managed_reduce_abi = {
  .size = sizeof(cmeta_function_abi_desc),
  .function = &cflow_parallel_managed_reduce_function,
  .return_carrier = CMETA_ABI_AGGREGATE,
  .param_carriers = cflow_parallel_managed_reduce_param_abi,
  .param_count = 2u
};

static bool cflow_parallel_managed_reduce_invoke(
    const cmeta_callable *self, void *out, const void *const *args) {
  const cflow_parallel_managed_value *left;
  const cflow_parallel_managed_value *right;
  cflow_parallel_managed_value sum;
  int fail_right;

  (void)self;
  if (!out || !args || !args[0] || !args[1]) return false;
  left = (const cflow_parallel_managed_value *)args[0];
  right = (const cflow_parallel_managed_value *)args[1];
  if (!left->resource || !right->resource) return false;
  fail_right = atomic_load(&cflow_parallel_managed_fail_right);
  if (fail_right != INT32_MIN && *right->resource == fail_right)
    return false;
  sum = cflow_parallel_managed_make(*left->resource + *right->resource);
  if (!sum.resource) return false;
  *(cflow_parallel_managed_value *)out = sum;
  return true;
}

static cmeta_callable cflow_parallel_managed_reduce_adapter(void) {
  cmeta_callable adapter = {0};
  adapter.meta.effects = cflow_parallel_managed_reduce_function.effects;
  adapter.meta.properties = cflow_parallel_managed_reduce_function.properties;
  adapter.invoke = cflow_parallel_managed_reduce_invoke;
  adapter.dispatch = CMETA_CALLABLE_DISPATCH_ADAPTER;
  return adapter;
}

static bool cflow_parallel_managed_projected_plan(
    cflow_graph *graph,
    cflow_plan *plan) {
  cflow_function_typed_adapter_projection projection = {0};
  const char *validation = NULL;

  if (!graph || !plan ||
      cflow_function_typed_reduce_projection_admit(
          &cflow_parallel_managed_reduce_function,
          &cflow_parallel_managed_reduce_abi,
          cflow_parallel_managed_reduce_adapter(),
          &cflow_parallel_managed_type,
          &projection) != CFLOW_FUNCTION_PROJECTION_OK)
    return false;

  cflow_graph_init(graph, &cflow_parallel_managed_type);
  if (!cflow_graph_add_function_typed_reduce_projection(
          graph, &projection) ||
      !cflow_graph_validate(graph, &validation) ||
      validation != NULL ||
      !cflow_plan_compile_surface(plan, graph, NULL))
    return false;

  return cflow_plan_parallel_reduce_supported(plan);
}

static bool cflow_parallel_managed_plan(cflow_plan *plan,
                                        cflow_plan_impl *impl,
                                        cflow_plan_inst *instruction) {
  if (!plan || !impl || !instruction) return false;
  memset(plan, 0, sizeof(*plan));
  memset(impl, 0, sizeof(*impl));
  memset(instruction, 0, sizeof(*instruction));
  instruction->opcode = CMETA_PLAN_REDUCE;
  instruction->step = cflow_plan_step_for_opcode(CMETA_PLAN_REDUCE);
  instruction->input_type = &cflow_parallel_managed_type;
  instruction->output_type = &cflow_parallel_managed_type;
  instruction->call.invoke = cflow_parallel_managed_reduce_invoke;
  instruction->call.input_type = &cflow_parallel_managed_type;
  instruction->call.output_type = &cflow_parallel_managed_type;
  impl->code = instruction;
  impl->count = 1u;
  impl->terminal_reduce_index = 0u;
  impl->parallel_reduce_supported = true;
  impl->managed_values = true;
  plan->impl = impl;
  plan->input_type = &cflow_parallel_managed_type;
  plan->output_type = &cflow_parallel_managed_type;
  return true;
}

static void cflow_parallel_managed_destroy_inputs(
    cflow_parallel_managed_value *values, size_t count) {
  for (size_t index = 0u; index < count; ++index)
    cflow_parallel_managed_destroy(&values[index]);
}

cmeta_function(reduce, associative, long, cflow_parallel_left,
      (long left, long right)) {
  (void)right;
  return left;
}

cmeta_function(map, value, long, cflow_parallel_widen, (int value)) {
  atomic_fetch_add(&cflow_parallel_prefix_calls, 1u);
  return (long)value * 2L;
}

static bool cflow_parallel_injected_invoke(
    const cmeta_callable *self, void *out, const void *const *args) {
  long left;
  long right;
  long fail_left;

  if (!self || !out || !args || !args[0] || !args[1]) return false;
  memcpy(&left, args[0], sizeof(left));
  memcpy(&right, args[1], sizeof(right));
  (void)right;
  fail_left = atomic_load(&cflow_parallel_fail_left);
  if (left == fail_left) {
    if (atomic_load(&cflow_parallel_delay_failure)) {
      size_t attempts = 0u;
      while (atomic_load(&cflow_parallel_successes) < 2u && attempts++ < 500u)
        cmeta_sleep_ms(1u);
    }
    atomic_store(&cflow_parallel_successes_at_failure,
                 atomic_load(&cflow_parallel_successes));
    atomic_fetch_add(&cflow_parallel_failures, 1u);
    return false;
  }
  if (!atomic_load(&cflow_parallel_delay_failure)) {
    size_t attempts = 0u;
    while (atomic_load(&cflow_parallel_failures) == 0u && attempts++ < 500u)
      cmeta_sleep_ms(1u);
  }
  memcpy(out, &left, sizeof(left));
  atomic_fetch_add(&cflow_parallel_successes, 1u);
  return true;
}

static cflow_reduce_callable cflow_parallel_injected_reducer(void) {
  cflow_reduce_callable reducer = cflow_parallel_left;
  reducer.fn.invoke = cflow_parallel_injected_invoke;
  return reducer;
}

static void cflow_parallel_gate_task(void *user) {
  (void)user;
  atomic_store(&cflow_parallel_gate_started, true);
  while (!atomic_load(&cflow_parallel_gate_open)) cmeta_sleep_ms(1u);
}

typedef struct cflow_parallel_eval_thread {
  const cflow_plan *plan;
  const void *inputs;
  size_t input_count;
  cflow_plan_eval_options options;
  cflow_result result;
  bool ok;
} cflow_parallel_eval_thread;

static void cflow_parallel_eval_thread_run(void *user) {
  cflow_parallel_eval_thread *ctx = (cflow_parallel_eval_thread *)user;
  ctx->ok = cflow_plan_eval_array_with_options(
      ctx->plan, ctx->inputs, ctx->input_count, &ctx->options, &ctx->result);
}

typedef struct cflow_parallel_callback_eval {
  const cflow_plan *plan;
  const void *inputs;
  size_t input_count;
  cflow_plan_eval_options options;
  cflow_result result;
  _Atomic bool returned;
  bool ok;
} cflow_parallel_callback_eval;

static void cflow_parallel_callback_eval_run(void *user) {
  cflow_parallel_callback_eval *ctx = (cflow_parallel_callback_eval *)user;
  ctx->ok = cflow_plan_eval_array_with_options(
      ctx->plan, ctx->inputs, ctx->input_count, &ctx->options, &ctx->result);
  atomic_store(&ctx->returned, true);
}

typedef struct cflow_closing_executor_state {
  cmeta_mutex_t mutex;
  cmeta_cond_t condition;
  cflow_task_fn accepted_fn;
  void *accepted_user;
  bool second_waiting;
  bool closed;
  size_t rejected_closed;
} cflow_closing_executor_state;

static cflow_admission_status cflow_closing_try_post(
    void *self, cflow_task_fn fn, void *user) {
  cflow_closing_executor_state *state = (cflow_closing_executor_state *)self;
  if (!state || !fn) return CFLOW_ADMISSION_INVALID_ARGUMENT;
  cmeta_mutex_lock(&state->mutex);
  if (state->closed) {
    ++state->rejected_closed;
    cmeta_mutex_unlock(&state->mutex);
    return CFLOW_ADMISSION_CLOSED;
  }
  if (!state->accepted_fn) {
    state->accepted_fn = fn;
    state->accepted_user = user;
    cmeta_cond_broadcast(&state->condition);
    cmeta_mutex_unlock(&state->mutex);
    return CFLOW_ADMISSION_ACCEPTED;
  }
  state->second_waiting = true;
  cmeta_cond_broadcast(&state->condition);
  while (!state->closed)
    cmeta_cond_wait(&state->condition, &state->mutex);
  ++state->rejected_closed;
  cmeta_mutex_unlock(&state->mutex);
  return CFLOW_ADMISSION_CLOSED;
}

static bool cflow_closing_post(void *self, cflow_task_fn fn, void *user) {
  return cflow_closing_try_post(self, fn, user) == CFLOW_ADMISSION_ACCEPTED;
}

static bool cflow_closing_run_one(void *self) {
  (void)self;
  return false;
}

static size_t cflow_closing_run_ready(void *self) {
  (void)self;
  return 0u;
}

static bool cflow_closing_wait_idle(void *self) {
  cflow_closing_executor_state *state = (cflow_closing_executor_state *)self;
  bool idle;
  if (!state) return false;
  cmeta_mutex_lock(&state->mutex);
  idle = state->accepted_fn == NULL;
  cmeta_mutex_unlock(&state->mutex);
  return idle;
}

static size_t cflow_closing_pending(void *self) {
  cflow_closing_executor_state *state = (cflow_closing_executor_state *)self;
  size_t pending;
  if (!state) return 0u;
  cmeta_mutex_lock(&state->mutex);
  pending = state->accepted_fn ? 1u : 0u;
  cmeta_mutex_unlock(&state->mutex);
  return pending;
}

static bool cflow_closing_shutdown(void *self) {
  cflow_closing_executor_state *state = (cflow_closing_executor_state *)self;
  cflow_task_fn fn;
  void *user;
  if (!state) return false;
  cmeta_mutex_lock(&state->mutex);
  state->closed = true;
  fn = state->accepted_fn;
  user = state->accepted_user;
  state->accepted_fn = NULL;
  state->accepted_user = NULL;
  cmeta_cond_broadcast(&state->condition);
  cmeta_mutex_unlock(&state->mutex);
  if (fn) fn(user);
  return true;
}

static bool cflow_closing_get_stats(void *self, cflow_executor_stats *out) {
  cflow_closing_executor_state *state = (cflow_closing_executor_state *)self;
  if (!state || !out) return false;
  cmeta_mutex_lock(&state->mutex);
  *out = (cflow_executor_stats){
      .capacity = 1u,
      .pending = state->accepted_fn ? 1u : 0u,
      .peak_pending = 1u,
      .rejected_closed = state->rejected_closed};
  cmeta_mutex_unlock(&state->mutex);
  return true;
}

static void cflow_closing_destroy(void *self) {
  cflow_closing_executor_state *state = (cflow_closing_executor_state *)self;
  if (!state) return;
  cmeta_cond_destroy(&state->condition);
  cmeta_mutex_destroy(&state->mutex);
}

static cflow_admission_status cflow_closing_task_admit(
    void *self, const cflow_executor_task *task) {
  if (!task || !task->run || task->cancel || task->finalize)
    return CFLOW_ADMISSION_INVALID_ARGUMENT;
  return cflow_closing_try_post(self, task->run, task->user);
}

static bool cflow_closing_project_control(
    void *self, struct cflow_executor_control *out) {
  (void)self;
  (void)out;
  return false;
}

static bool cflow_closing_is_current(void *self) {
  (void)self;
  return false;
}

CMETA_IMPLEMENTS(cflow_executor, cflow_closing_executor,
    CMETA_EXEC_CAP_CONCURRENT,
    .try_post = cflow_closing_try_post,
    .post = cflow_closing_post,
    .run_one = cflow_closing_run_one,
    .run_ready = cflow_closing_run_ready,
    .wait_idle = cflow_closing_wait_idle,
    .pending = cflow_closing_pending,
    .shutdown = cflow_closing_shutdown,
    .get_stats = cflow_closing_get_stats,
    .destroy = cflow_closing_destroy,
    .task_admit = cflow_closing_task_admit,
    .project_control = cflow_closing_project_control,
    .is_current = cflow_closing_is_current
);

static bool cflow_closing_executor_init(cflow_executor *executor,
                                        cflow_closing_executor_state *state) {
  if (!executor || !state) return false;
  memset(state, 0, sizeof(*state));
  cmeta_mutex_init(&state->mutex);
  cmeta_cond_init(&state->condition);
  if (!state->mutex || !state->condition) {
    cflow_closing_destroy(state);
    return false;
  }
  *executor = cflow_closing_executor_as_cflow_executor(state);
  return true;
}

typedef struct cflow_parallel_reduce_fixture {
  cflow_stream stream;
  cflow_plan plan;
  cflow_executor executor;
  cflow_result result;
  cflow_plan_eval_options options;
} cflow_parallel_reduce_fixture;

static cflow_parallel_reduce_fixture cflow_parallel_state;

static bool cflow_parallel_fixture_init(cflow_parallel_reduce_fixture *state) {
  if (!state || !cflow_stream_init(&state->stream, &cmeta_type_long) ||
      !state->stream.reduce(&state->stream, cflow_parallel_left) ||
      !cflow_plan_compile_surface(&state->plan, &state->stream.graph, NULL) ||
      !cflow_executor_worker_init_with_capacity(&state->executor, 4u, 16u))
    return false;
  state->options = (cflow_plan_eval_options){
      .mode = CFLOW_PLAN_EXECUTION_PARALLEL_REDUCE,
      .executor = &state->executor,
      .max_tasks = 4u,
      .min_items_per_task = 2u};
  return true;
}

suite("CFlow ordered parallel reduce") {
  before_each() {
    memset(&cflow_parallel_state, 0, sizeof(cflow_parallel_state));
    check_true(cflow_parallel_fixture_init(&cflow_parallel_state));
  }

  after_each() {
    cflow_result_destroy(&cflow_parallel_state.result);
    cflow_executor_destroy(&cflow_parallel_state.executor);
    cflow_plan_destroy(&cflow_parallel_state.plan);
    cflow_stream_destroy(&cflow_parallel_state.stream);
  }

  it("admits a custom managed FunctionDesc reducer without widening cmeta_sig") {
    cflow_parallel_managed_value input[8] = {0};
    cflow_graph graph = {0};
    cflow_plan plan = {0};
    cflow_result sequential = {0};
    cflow_result parallel = {0};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;
    const cflow_parallel_managed_value *output;
    size_t index;

    atomic_store(&cflow_parallel_managed_live, 0u);
    atomic_store(&cflow_parallel_managed_destroyed, 0u);
    atomic_store(&cflow_parallel_managed_fail_right, INT32_MIN);
    for (index = 0u; index < 8u; ++index)
      input[index] = cflow_parallel_managed_make((int)index + 1);

    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)8u);
    check_true(cmeta_function_desc_valid(
        &cflow_parallel_managed_reduce_function));
    check_true(cmeta_function_abi_desc_valid(
        &cflow_parallel_managed_reduce_abi));
    check_true(cflow_parallel_managed_projected_plan(&graph, &plan));
    check_true(cflow_plan_parallel_reduce_supported(&plan));

    check_true(cflow_plan_eval_array(
        &plan, input, 8u, &sequential));
    check_equal(sequential.count, (size_t)1u);
    check_true(cmeta_type_equal(
        sequential.type, &cflow_parallel_managed_type));
    output = (const cflow_parallel_managed_value *)sequential.data;
    check_not_null(output);
    check_not_null(output->resource);
    check_equal(*output->resource, 36);
    cflow_result_destroy(&sequential);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)8u);

    check_true(cflow_plan_eval_array_with_options(
        &plan, input, 8u, &state->options, &parallel));
    check_equal(parallel.count, (size_t)1u);
    check_true(cmeta_type_equal(
        parallel.type, &cflow_parallel_managed_type));
    output = (const cflow_parallel_managed_value *)parallel.data;
    check_not_null(output);
    check_not_null(output->resource);
    check_equal(*output->resource, 36);
    cflow_result_destroy(&parallel);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)8u);

    cflow_plan_destroy(&plan);
    cflow_graph_destroy(&graph);
    cflow_parallel_managed_destroy_inputs(input, 8u);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)0u);
  }

  it("reduces managed values in parallel without leaking internal ownership") {
    cflow_parallel_managed_value input[] = {
        cflow_parallel_managed_make(1), cflow_parallel_managed_make(2),
        cflow_parallel_managed_make(3), cflow_parallel_managed_make(4),
        cflow_parallel_managed_make(5), cflow_parallel_managed_make(6),
        cflow_parallel_managed_make(7), cflow_parallel_managed_make(8)};
    cflow_plan plan = {0};
    cflow_plan_impl impl = {0};
    cflow_plan_inst instruction = {0};
    cflow_result result = {0};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;
    const cflow_parallel_managed_value *output;

    atomic_store(&cflow_parallel_managed_live, 8u);
    atomic_store(&cflow_parallel_managed_destroyed, 0u);
    atomic_store(&cflow_parallel_managed_fail_right, INT32_MIN);
    check_true(cflow_parallel_managed_plan(&plan, &impl, &instruction));
    check_true(cflow_plan_parallel_reduce_supported(&plan));
    check_true(cflow_plan_eval_array_with_options(
        &plan, input, 8u, &state->options, &result));
    check_equal(result.count, (size_t)1u);
    check_true(cmeta_type_equal(result.type, &cflow_parallel_managed_type));
    output = (const cflow_parallel_managed_value *)result.data;
    check_not_null(output);
    check_not_null(output->resource);
    check_equal(*output->resource, 36);
    check_equal((uintptr_t)result.data % _Alignof(cflow_parallel_managed_value),
                (uintptr_t)0u);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)9u);

    cflow_result_destroy(&result);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)8u);
    cflow_parallel_managed_destroy_inputs(input, 8u);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)0u);
  }

  it("joins managed tasks and destroys every internal value on reducer failure") {
    cflow_parallel_managed_value input[] = {
        cflow_parallel_managed_make(1), cflow_parallel_managed_make(2),
        cflow_parallel_managed_make(3), cflow_parallel_managed_make(4),
        cflow_parallel_managed_make(5), cflow_parallel_managed_make(6),
        cflow_parallel_managed_make(7), cflow_parallel_managed_make(8)};
    cflow_plan plan = {0};
    cflow_plan_impl impl = {0};
    cflow_plan_inst instruction = {0};
    cflow_result result = {0};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;

    atomic_store(&cflow_parallel_managed_live, 8u);
    atomic_store(&cflow_parallel_managed_destroyed, 0u);
    atomic_store(&cflow_parallel_managed_fail_right, 4);
    check_true(cflow_parallel_managed_plan(&plan, &impl, &instruction));
    check_false(cflow_plan_eval_array_with_options(
        &plan, input, 8u, &state->options, &result));
    check_null(result.data);
    check_equal(result.count, (size_t)0u);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)8u);
    check_true(cflow_executor_wait_idle(&state->executor));
    check_equal(cflow_executor_pending(&state->executor), (size_t)0u);

    cflow_result_destroy(&result);
    cflow_parallel_managed_destroy_inputs(input, 8u);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)0u);
  }

  it("cleans managed state after bounded executor admission failure") {
    cflow_parallel_managed_value input[] = {
        cflow_parallel_managed_make(11), cflow_parallel_managed_make(12),
        cflow_parallel_managed_make(13), cflow_parallel_managed_make(14),
        cflow_parallel_managed_make(15), cflow_parallel_managed_make(16),
        cflow_parallel_managed_make(17), cflow_parallel_managed_make(18)};
    cflow_plan plan = {0};
    cflow_plan_impl impl = {0};
    cflow_plan_inst instruction = {0};
    cflow_executor executor = {0};
    cflow_executor_stats stats = {0};
    cflow_parallel_eval_thread eval = {0};
    cmeta_thread_t thread = NULL;
    size_t attempts = 0u;

    atomic_store(&cflow_parallel_managed_live, 8u);
    atomic_store(&cflow_parallel_managed_destroyed, 0u);
    atomic_store(&cflow_parallel_managed_fail_right, INT32_MIN);
    atomic_store(&cflow_parallel_gate_open, false);
    atomic_store(&cflow_parallel_gate_started, false);
    check_true(cflow_parallel_managed_plan(&plan, &impl, &instruction));
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 1u));
    check_true(cflow_executor_post(&executor, cflow_parallel_gate_task, NULL));
    while (!atomic_load(&cflow_parallel_gate_started) && attempts++ < 500u)
      cmeta_sleep_ms(1u);
    check_true(atomic_load(&cflow_parallel_gate_started));

    eval.plan = &plan;
    eval.inputs = input;
    eval.input_count = 8u;
    eval.options = (cflow_plan_eval_options){
        .mode = CFLOW_PLAN_EXECUTION_PARALLEL_REDUCE,
        .executor = &executor,
        .max_tasks = 4u,
        .min_items_per_task = 2u};
    check_equal(cmeta_thread_create(
        &thread, cflow_parallel_eval_thread_run, &eval), 0);

    attempts = 0u;
    do {
      check_true(cflow_executor_get_stats(&executor, &stats));
      if (stats.rejected_full) break;
      cmeta_sleep_ms(1u);
    } while (attempts++ < 500u);
    check_equal(stats.rejected_full, (size_t)1u);

    atomic_store(&cflow_parallel_gate_open, true);
    check_equal(cmeta_thread_join(&thread), 0);
    check_false(eval.ok);
    check_null(eval.result.data);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)8u);
    check_true(cflow_executor_wait_idle(&executor));

    cflow_result_destroy(&eval.result);
    cflow_executor_destroy(&executor);
    cflow_parallel_managed_destroy_inputs(input, 8u);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)0u);
  }

  it("cleans managed state when WorkerExecutor cancels pending shards") {
    cflow_parallel_managed_value input[] = {
        cflow_parallel_managed_make(21), cflow_parallel_managed_make(22),
        cflow_parallel_managed_make(23), cflow_parallel_managed_make(24),
        cflow_parallel_managed_make(25), cflow_parallel_managed_make(26),
        cflow_parallel_managed_make(27), cflow_parallel_managed_make(28)};
    cflow_plan plan = {0};
    cflow_plan_impl impl = {0};
    cflow_plan_inst instruction = {0};
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_executor_protocol_stats stats = {0};
    cflow_parallel_eval_thread eval = {0};
    cmeta_thread_t thread = NULL;
    size_t attempts = 0u;

    atomic_store(&cflow_parallel_managed_live, 8u);
    atomic_store(&cflow_parallel_managed_destroyed, 0u);
    atomic_store(&cflow_parallel_managed_fail_right, INT32_MIN);
    atomic_store(&cflow_parallel_gate_open, false);
    atomic_store(&cflow_parallel_gate_started, false);
    check_true(cflow_parallel_managed_plan(&plan, &impl, &instruction));
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 4u));
    check_true(cflow_executor_as_control(&executor, &control));
    check_true(cflow_executor_post(&executor, cflow_parallel_gate_task, NULL));
    while (!atomic_load(&cflow_parallel_gate_started) && attempts++ < 500u)
      cmeta_sleep_ms(1u);
    check_true(atomic_load(&cflow_parallel_gate_started));

    eval.plan = &plan;
    eval.inputs = input;
    eval.input_count = 8u;
    eval.options = (cflow_plan_eval_options){
        .mode = CFLOW_PLAN_EXECUTION_PARALLEL_REDUCE,
        .executor = &executor,
        .max_tasks = 4u,
        .min_items_per_task = 2u};
    check_equal(cmeta_thread_create(
        &thread, cflow_parallel_eval_thread_run, &eval), 0);

    attempts = 0u;
    do {
      check_true(cflow_executor_control_get_stats(&control, &stats));
      if (stats.accepted == 5u) break;
      cmeta_sleep_ms(1u);
    } while (attempts++ < 500u);
    check_equal(stats.accepted, (size_t)5u);

    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    atomic_store(&cflow_parallel_gate_open, true);
    check_equal(cmeta_thread_join(&thread), 0);
    check_false(eval.ok);
    check_null(eval.result.data);
    check_equal(cflow_executor_control_wait_idle(&control),
                CFLOW_EXECUTOR_WAIT_IDLE);
    check_true(cflow_executor_control_get_stats(&control, &stats));
    check_equal(stats.accepted, stats.completed + stats.cancelled);
    check_equal(stats.cancelled, (size_t)4u);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)8u);

    cflow_result_destroy(&eval.result);
    cflow_executor_destroy(&executor);
    cflow_parallel_managed_destroy_inputs(input, 8u);
    check_equal(atomic_load(&cflow_parallel_managed_live), (size_t)0u);
  }

  it("rejects empty and one-item inputs without sequential fallback") {
    const long one[] = {41L};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;

    check_false(cflow_plan_eval_array_with_options(
        &state->plan, NULL, 0u, &state->options, &state->result));
    check_null(state->result.data);
    check_equal(state->result.count, (size_t)0u);

    check_false(cflow_plan_eval_array_with_options(
        &state->plan, one, 1u, &state->options, &state->result));
    check_null(state->result.data);
    check_equal(state->result.count, (size_t)0u);
  }

  it("rejects invalid options and size overflow before committing output") {
    const long input[] = {61L, 62L, 63L, 64L};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;
    cflow_result untouched = {
        .data = (void *)input,
        .count = 99u,
        .type = &cmeta_type_long
    };

    state->options.max_tasks = 0u;
    check_false(cflow_plan_eval_array_with_options(
        &state->plan, input, 4u, &state->options, &untouched));
    check_true(untouched.data == input);
    check_equal(untouched.count, (size_t)99u);

    state->options.max_tasks = 4u;
    state->options.min_items_per_task = 0u;
    check_false(cflow_plan_eval_array_with_options(
        &state->plan, input, 4u, &state->options, &untouched));
    check_true(untouched.data == input);

    state->options.min_items_per_task = 1u;
    check_false(cflow_plan_eval_array_with_options(
        &state->plan, input, SIZE_MAX, &state->options, &untouched));
    check_true(untouched.data == input);
  }

  it("preserves encounter order for exact and remainder chunks") {
    const long exact[] = {11L, 12L, 13L, 14L, 15L, 16L, 17L, 18L};
    const long remainder[] = {21L, 22L, 23L, 24L, 25L, 26L, 27L};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;

    check_true(cflow_plan_eval_array_with_options(
        &state->plan, exact, 8u, &state->options, &state->result));
    check_equal(state->result.count, (size_t)1u);
    check_true(cmeta_type_equal(state->result.type, &cmeta_type_long));
    check_equal(*(const long *)state->result.data, 11L);
    cflow_result_destroy(&state->result);

    state->options.max_tasks = 3u;
    check_true(cflow_plan_eval_array_with_options(
        &state->plan, remainder, 7u, &state->options, &state->result));
    check_equal(state->result.count, (size_t)1u);
    check_equal(*(const long *)state->result.data, 21L);
  }

  it("supports repeated evaluation without retaining task state") {
    const long first[] = {31L, 32L, 33L, 34L};
    const long second[] = {51L, 52L, 53L, 54L, 55L};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;

    check_true(cflow_plan_eval_array_with_options(
        &state->plan, first, 4u, &state->options, &state->result));
    check_equal(*(const long *)state->result.data, 31L);
    cflow_result_destroy(&state->result);

    check_true(cflow_plan_eval_array_with_options(
        &state->plan, second, 5u, &state->options, &state->result));
    check_equal(*(const long *)state->result.data, 51L);
  }

  it("materializes the sequential prefix exactly once before splitting") {
    const int input[] = {7, 8, 9, 10, 11, 12};
    cflow_stream stream = {0};
    cflow_plan plan = {0};
    cflow_result result = {0};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;

    atomic_store(&cflow_parallel_prefix_calls, 0u);
    check_not_null(cflow_stream_init(&stream, &cmeta_type_int));
    check_not_null(stream.map(&stream, cflow_parallel_widen));
    check_not_null(stream.reduce(&stream, cflow_parallel_left));
    check_true(cflow_plan_compile_surface(&plan, &stream.graph, NULL));
    check_true(cflow_plan_parallel_reduce_supported(&plan));
    check_true(cflow_plan_eval_array_with_options(
        &plan, input, 6u, &state->options, &result));
    check_equal(result.count, (size_t)1u);
    check_equal(*(const long *)result.data, 14L);
    check_equal(atomic_load(&cflow_parallel_prefix_calls), (size_t)6u);

    cflow_result_destroy(&result);
    cflow_plan_destroy(&plan);
    cflow_stream_destroy(&stream);
  }

  it("joins every accepted task when a reducer callback fails") {
    const long input[] = {1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;

    for (size_t delayed = 0u; delayed < 2u; ++delayed) {
      cflow_stream stream = {0};
      cflow_plan plan = {0};
      cflow_result result = {0};
      cflow_reduce_callable reducer = cflow_parallel_injected_reducer();

      atomic_store(&cflow_parallel_fail_left, delayed ? 7L : 1L);
      atomic_store(&cflow_parallel_failures, 0u);
      atomic_store(&cflow_parallel_successes, 0u);
      atomic_store(&cflow_parallel_successes_at_failure, SIZE_MAX);
      atomic_store(&cflow_parallel_delay_failure, delayed != 0u);
      check_not_null(cflow_stream_init(&stream, &cmeta_type_long));
      check_not_null(stream.reduce(&stream, reducer));
      check_true(cflow_plan_compile_surface(&plan, &stream.graph, NULL));
      check_true(cflow_plan_eval_array_with_options(
          &plan, input, 8u, &state->options, &result) == false);
      check_null(result.data);
      check_equal(result.count, (size_t)0u);
      check_equal(atomic_load(&cflow_parallel_failures), (size_t)1u);
      if (delayed)
        check(atomic_load(&cflow_parallel_successes_at_failure) >= 2u);
      else
        check_equal(atomic_load(&cflow_parallel_successes_at_failure),
                    (size_t)0u);
      check_true(cflow_executor_wait_idle(&state->executor));
      check_equal(cflow_executor_pending(&state->executor), (size_t)0u);

      cflow_result_destroy(&result);
      cflow_plan_destroy(&plan);
      cflow_stream_destroy(&stream);
    }
  }

  it("closes accepted work after bounded executor saturation") {
    const long input[] = {11L, 12L, 13L, 14L, 15L, 16L, 17L, 18L};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;
    cflow_executor executor = {0};
    cflow_executor_stats stats = {0};
    cflow_parallel_eval_thread eval = {
        .plan = &state->plan,
        .inputs = input,
        .input_count = 8u,
        .options = state->options
    };
    cmeta_thread_t thread = NULL;
    size_t attempts = 0u;

    atomic_store(&cflow_parallel_gate_open, false);
    atomic_store(&cflow_parallel_gate_started, false);
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 1u));
    check_true(cflow_executor_post(&executor, cflow_parallel_gate_task, NULL));
    while (!atomic_load(&cflow_parallel_gate_started) && attempts++ < 500u)
      cmeta_sleep_ms(1u);
    check_true(atomic_load(&cflow_parallel_gate_started));

    eval.options.executor = &executor;
    check_equal(cmeta_thread_create(
        &thread, cflow_parallel_eval_thread_run, &eval), 0);
    attempts = 0u;
    do {
      check_true(cflow_executor_get_stats(&executor, &stats));
      if (stats.rejected_full) break;
      cmeta_sleep_ms(1u);
    } while (attempts++ < 500u);
    check_equal(stats.rejected_full, (size_t)1u);

    atomic_store(&cflow_parallel_gate_open, true);
    check_equal(cmeta_thread_join(&thread), 0);
    check_false(eval.ok);
    check_null(eval.result.data);
    check_true(cflow_executor_wait_idle(&executor));
    check_true(cflow_executor_get_stats(&executor, &stats));
    check_equal(stats.pending, (size_t)0u);

    cflow_result_destroy(&eval.result);
    cflow_executor_destroy(&executor);
  }

  it("joins accepted work when shutdown closes a later submission") {
    const long input[] = {21L, 22L, 23L, 24L, 25L, 26L, 27L, 28L};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;
    cflow_closing_executor_state closing = {0};
    cflow_executor executor = {0};
    cflow_executor_stats stats = {0};
    cflow_parallel_eval_thread eval = {
        .plan = &state->plan,
        .inputs = input,
        .input_count = 8u,
        .options = state->options
    };
    cmeta_thread_t thread = NULL;
    size_t attempts = 0u;
    bool second_waiting = false;

    check_true(cflow_closing_executor_init(&executor, &closing));
    eval.options.executor = &executor;
    check_equal(cmeta_thread_create(
        &thread, cflow_parallel_eval_thread_run, &eval), 0);
    do {
      cmeta_mutex_lock(&closing.mutex);
      second_waiting = closing.second_waiting;
      cmeta_mutex_unlock(&closing.mutex);
      if (second_waiting) break;
      cmeta_sleep_ms(1u);
    } while (attempts++ < 500u);

    check_true(cflow_executor_shutdown(&executor));
    check_equal(cmeta_thread_join(&thread), 0);
    check_true(second_waiting);
    check_false(eval.ok);
    check_null(eval.result.data);
    check_true(cflow_executor_wait_idle(&executor));
    check_true(cflow_executor_get_stats(&executor, &stats));
    check_equal(stats.pending, (size_t)0u);
    check_equal(stats.rejected_closed, (size_t)1u);

    cflow_result_destroy(&eval.result);
    cflow_executor_destroy(&executor);
  }

  it("settles accepted shards cancelled by WorkerExecutor shutdown") {
    const long input[] = {31L, 32L, 33L, 34L, 35L, 36L, 37L, 38L};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_executor_protocol_stats stats = {0};
    cflow_parallel_eval_thread eval = {
        .plan = &state->plan,
        .inputs = input,
        .input_count = 8u,
        .options = state->options
    };
    cmeta_thread_t thread = NULL;
    size_t attempts = 0u;

    atomic_store(&cflow_parallel_gate_open, false);
    atomic_store(&cflow_parallel_gate_started, false);
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 4u));
    check_true(cflow_executor_as_control(&executor, &control));
    check_true(cflow_executor_post(&executor, cflow_parallel_gate_task, NULL));
    while (!atomic_load(&cflow_parallel_gate_started) && attempts++ < 500u)
      cmeta_sleep_ms(1u);
    check_true(atomic_load(&cflow_parallel_gate_started));

    eval.options.executor = &executor;
    check_equal(cmeta_thread_create(
        &thread, cflow_parallel_eval_thread_run, &eval), 0);
    attempts = 0u;
    do {
      check_true(cflow_executor_control_get_stats(&control, &stats));
      if (stats.accepted == 5u) break;
      cmeta_sleep_ms(1u);
    } while (attempts++ < 500u);
    check_equal(stats.accepted, (size_t)5u);

    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    atomic_store(&cflow_parallel_gate_open, true);
    check_equal(cmeta_thread_join(&thread), 0);

    check_false(eval.ok);
    check_null(eval.result.data);
    check_equal(cflow_executor_control_wait_idle(&control),
                CFLOW_EXECUTOR_WAIT_IDLE);
    check_true(cflow_executor_control_get_stats(&control, &stats));
    check_equal(stats.accepted, stats.completed + stats.cancelled);
    check_equal(stats.cancelled, (size_t)4u);

    cflow_result_destroy(&eval.result);
    cflow_executor_destroy(&executor);
  }

  it("rejects a synchronous join from the same single WorkerExecutor") {
    const long input[] = {41L, 42L, 43L, 44L};
    cflow_parallel_reduce_fixture *state = &cflow_parallel_state;
    cflow_executor executor = {0};
    cflow_parallel_callback_eval eval = {
        .plan = &state->plan,
        .inputs = input,
        .input_count = 4u,
        .options = state->options
    };

    atomic_init(&eval.returned, false);
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 4u));
    eval.options.executor = &executor;
    check_true(cflow_executor_post(
        &executor, cflow_parallel_callback_eval_run, &eval));

    check_true(cflow_executor_wait_idle(&executor));
    check_true(atomic_load(&eval.returned));
    check_false(eval.ok);
    check_null(eval.result.data);

    cflow_result_destroy(&eval.result);
    cflow_executor_destroy(&executor);
  }
}
