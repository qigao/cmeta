#include <cflow/cflow.h>
#include <cflow/readiness.h>

#include "../src/readiness_internal.h"
#include <error_codes.h>
#include <thread.h>

#include "readiness_contract_suite.h"
#include "tinytest.h"
#include "../../platform/src/readiness_internal.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#if defined(CFLOW_TEST_EPOLL_READINESS)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

enum {
    CFLOW_READINESS_TEST_RESOURCE = 7001,
    CFLOW_READINESS_TEST_OTHER_RESOURCE = 7002,
    CFLOW_READINESS_TEST_TIMEOUT_NS = 2 * 1000 * 1000 * 1000,
    CFLOW_DIFFERENTIAL_VALUE_COUNT = 3,
    CFLOW_DIFFERENTIAL_DEMAND = 5,
    CFLOW_DIFFERENTIAL_ITERATIONS = 16
};

typedef struct managed_test_value {
    int *owned;
} managed_test_value;

static bool managed_test_copy(void *destination, const void *source) {
    (void)destination;
    (void)source;
    return true;
}

static void managed_test_move(void *destination, void *source) {
    *(managed_test_value *)destination = *(managed_test_value *)source;
    ((managed_test_value *)source)->owned = NULL;
}

static void managed_test_destroy(void *value) { (void)value; }

static const cmeta_type_traits managed_test_traits = {
    .flags = CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
    .copy_construct = managed_test_copy,
    .move_construct = managed_test_move,
    .destroy = managed_test_destroy
};

static const cmeta_type_desc managed_test_type = {
    .name = "managed_test_value",
    .size = sizeof(managed_test_value),
    .align = _Alignof(managed_test_value),
    .kind = CMETA_T_OBJECT,
    .traits = &managed_test_traits
};

typedef struct read_probe {
    cflow_read_status statuses[8];
    int values[8];
    size_t count;
    size_t next;
    size_t reads;
    size_t closes;
    const char *errors[8];
} read_probe;

typedef struct wake_probe {
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    size_t calls;
    bool entered;
    bool released;
} wake_probe;

typedef struct sink_probe {
    int values[4];
    size_t value_count;
    size_t done_count;
    const char *error;
} sink_probe;

typedef struct differential_observation {
    int values[CFLOW_DIFFERENTIAL_VALUE_COUNT];
    size_t value_count;
    size_t done_count;
    size_t outstanding_demand;
    bool done;
    bool errored;
} differential_observation;

typedef struct fake_env {
    const readiness_contract_factory *factory;
    readiness_contract_fixture *fixture;
    cmeta_readiness_reactor reactor;
    cflow_readiness_publisher_owner owner;
} fake_env;

typedef struct emit_thread_args {
    fake_env *env;
    intptr_t resource;
    int status;
} emit_thread_args;

typedef struct destroy_thread_args {
    cflow_publisher *source;
    wake_probe *probe;
    bool returned;
} destroy_thread_args;

typedef struct arm_observing_scheduler_state {
    cflow_scheduler inner;
    cmeta_readiness_registration *registration;
    cmeta_mutex_t lock;
    bool observe_next_post;
    int observation_status;
    uint32_t observation_api_borrows;
} arm_observing_scheduler_state;

static bool arm_observing_take_next(arm_observing_scheduler_state *state) {
    bool observe;
    cmeta_mutex_lock(&state->lock);
    observe = state->observe_next_post;
    state->observe_next_post = false;
    cmeta_mutex_unlock(&state->lock);
    return observe;
}

static void arm_observing_wait_for_rearm(
    arm_observing_scheduler_state *state, bool observe, bool accepted) {
    if (observe && accepted) {
        uint32_t api_borrows = UINT32_MAX;
        int status = cmeta_readiness_backend_wait_arm_waiter_observe(
            state->registration, 1u, CFLOW_READINESS_TEST_TIMEOUT_NS,
            &api_borrows);
        cmeta_mutex_lock(&state->lock);
        state->observation_status = status;
        state->observation_api_borrows = api_borrows;
        cmeta_mutex_unlock(&state->lock);
    }
}

static cflow_schedule_result arm_observing_try_post_after(
    void *self, uint64_t delay_ticks, cflow_task_fn fn, void *user) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    bool observe = arm_observing_take_next(state);
    cflow_schedule_result result = cflow_scheduler_try_post_after(
        &state->inner, delay_ticks, fn, user);
    arm_observing_wait_for_rearm(
        state, observe,
        result.status == CFLOW_ADMISSION_ACCEPTED && result.task_id != 0u);
    return result;
}

static cflow_task_id arm_observing_post_after(
    void *self, uint64_t delay_ticks, cflow_task_fn fn, void *user) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    cflow_task_id task;
    bool observe = arm_observing_take_next(state);

    task = cflow_scheduler_post_after(&state->inner, delay_ticks, fn, user);
    arm_observing_wait_for_rearm(state, observe, task != 0u);
    return task;
}

static bool arm_observing_cancel(void *self, cflow_task_id task) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    return cflow_scheduler_cancel(&state->inner, task);
}

static bool arm_observing_run_one(void *self) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    return cflow_scheduler_run_one(&state->inner);
}

static size_t arm_observing_run_ready(void *self) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    return cflow_scheduler_run_ready(&state->inner);
}

static size_t arm_observing_advance(void *self, uint64_t ticks) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    return cflow_scheduler_advance(&state->inner, ticks);
}

static size_t arm_observing_run_until_idle(void *self, size_t max_steps) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    return cflow_scheduler_run_until_idle(&state->inner, max_steps);
}

static bool arm_observing_wait_idle(void *self) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    return cflow_scheduler_wait_idle(&state->inner);
}

static uint64_t arm_observing_now(void *self) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    return cflow_scheduler_now(&state->inner);
}

static size_t arm_observing_pending(void *self) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    return cflow_scheduler_pending(&state->inner);
}

static bool arm_observing_shutdown(void *self) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    return cflow_scheduler_shutdown(&state->inner);
}

static bool arm_observing_get_stats(void *self, cflow_scheduler_stats *out) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    return cflow_scheduler_get_stats(&state->inner, out);
}

static void arm_observing_destroy(void *self) {
    arm_observing_scheduler_state *state =
        (arm_observing_scheduler_state *)self;
    cflow_scheduler_destroy(&state->inner);
    cmeta_mutex_destroy(&state->lock);
}

CMETA_IMPLEMENTS(cflow_scheduler, arm_observing_scheduler,
    CMETA_SCHED_CAP_DELAYED | CMETA_SCHED_CAP_CONCURRENT,
    .try_post_after = arm_observing_try_post_after,
    .post_after = arm_observing_post_after,
    .cancel = arm_observing_cancel,
    .run_one = arm_observing_run_one,
    .run_ready = arm_observing_run_ready,
    .advance = arm_observing_advance,
    .run_until_idle = arm_observing_run_until_idle,
    .wait_idle = arm_observing_wait_idle,
    .now = arm_observing_now,
    .pending = arm_observing_pending,
    .shutdown = arm_observing_shutdown,
    .get_stats = arm_observing_get_stats,
    .destroy = arm_observing_destroy
);

static bool arm_observing_scheduler_init(
    arm_observing_scheduler_state *state,
    cmeta_readiness_registration *registration,
    cflow_scheduler *scheduler) {
    memset(state, 0, sizeof(*state));
    state->registration = registration;
    state->observation_status = SALTS_EIO;
    state->observation_api_borrows = UINT32_MAX;
    cmeta_mutex_init(&state->lock);
    if (!state->lock ||
        !cflow_scheduler_worker_init_with_capacity(&state->inner, 1u, 8u, 8u)) {
        cmeta_mutex_destroy(&state->lock);
        return false;
    }
    *scheduler = arm_observing_scheduler_as_cflow_scheduler(state);
    return true;
}

static void arm_observing_scheduler_observe_next(
    arm_observing_scheduler_state *state) {
    cmeta_mutex_lock(&state->lock);
    state->observe_next_post = true;
    cmeta_mutex_unlock(&state->lock);
}

static int arm_observing_scheduler_status(
    arm_observing_scheduler_state *state) {
    int status;
    cmeta_mutex_lock(&state->lock);
    status = state->observation_status;
    cmeta_mutex_unlock(&state->lock);
    return status;
}

static uint32_t arm_observing_scheduler_api_borrows(
    arm_observing_scheduler_state *state) {
    uint32_t api_borrows;
    cmeta_mutex_lock(&state->lock);
    api_borrows = state->observation_api_borrows;
    cmeta_mutex_unlock(&state->lock);
    return api_borrows;
}

static bool fake_env_init(fake_env *env, size_t capacity) {
    int status = SALTS_EINVAL;
    const cmeta_readiness_config config = {capacity, capacity};

    memset(env, 0, sizeof(*env));
    env->factory = readiness_contract_factory_get();
    if (!env->factory)
        return false;
    env->fixture = env->factory->create(config, &env->reactor, &status);
    return env->fixture != NULL && status == SALTS_OK;
}

static void fake_env_destroy(fake_env *env) {
    if (!env || !env->fixture)
        return;
    (void)cmeta_readiness_reactor_shutdown(&env->reactor);
    (void)cmeta_readiness_reactor_destroy(&env->reactor);
    env->factory->destroy(env->fixture);
    memset(env, 0, sizeof(*env));
}

static cflow_read_status probe_read(void *user, void *out_value,
                                    const char **error) {
    read_probe *probe = (read_probe *)user;
    cflow_read_status status;

    if (!probe || probe->next >= probe->count) {
        if (error)
            *error = "read script exhausted";
        return CFLOW_READ_ERROR;
    }
    ++probe->reads;
    status = probe->statuses[probe->next];
    if (status == CFLOW_READ_ERROR && error)
        *error = probe->errors[probe->next];
    if ((status == CFLOW_READ_VALUE ||
         status == CFLOW_READ_VALUE_AND_DONE) && out_value)
        *(int *)out_value = probe->values[probe->next];
    ++probe->next;
    return status;
}

static void probe_close(void *user) {
    read_probe *probe = (read_probe *)user;
    if (probe)
        ++probe->closes;
}

static void blocking_wake(void *user) {
    wake_probe *probe = (wake_probe *)user;
    if (!probe)
        return;
    cmeta_mutex_lock(&probe->lock);
    ++probe->calls;
    probe->entered = true;
    cmeta_cond_broadcast(&probe->changed);
    while (!probe->released)
        cmeta_cond_wait(&probe->changed, &probe->lock);
    cmeta_mutex_unlock(&probe->lock);
}

static bool sink_value(void *user, const cmeta_type_desc *type,
                       const void *value) {
    sink_probe *probe = (sink_probe *)user;
    if (!probe || !cmeta_type_equal(type, &cmeta_type_int) || !value ||
        probe->value_count >= 4u)
        return false;
    probe->values[probe->value_count++] = *(const int *)value;
    return true;
}

static void sink_error(void *user, const char *message) {
    sink_probe *probe = (sink_probe *)user;
    if (probe)
        probe->error = message;
}

static void sink_done(void *user) {
    sink_probe *probe = (sink_probe *)user;
    if (probe)
        ++probe->done_count;
}

static void emit_thread(void *user) {
    emit_thread_args *args = (emit_thread_args *)user;
    args->status = args->env->factory->emit_resource(
        args->env->fixture, args->resource, SALTS_READINESS_EVENT_READ);
}

static void destroy_thread(void *user) {
    destroy_thread_args *args = (destroy_thread_args *)user;
    cflow_publisher_destroy(args->source);
    cmeta_mutex_lock(&args->probe->lock);
    args->returned = true;
    cmeta_cond_broadcast(&args->probe->changed);
    cmeta_mutex_unlock(&args->probe->lock);
}

static void cancel_thread(void *user) {
    destroy_thread_args *args = (destroy_thread_args *)user;
    cflow_publisher_cancel(args->source);
    cmeta_mutex_lock(&args->probe->lock);
    args->returned = true;
    cmeta_cond_broadcast(&args->probe->changed);
    cmeta_mutex_unlock(&args->probe->lock);
}

static int make_source(fake_env *env, intptr_t resource, read_probe *probe,
                       cflow_publisher *source,
                       cmeta_readiness_registration *registration) {
    int status = cmeta_readiness_register(
        &env->reactor, resource, registration);
    if (status != SALTS_OK)
        return status;
    return cflow_publisher_from_readiness_registration(
        source, &env->owner, registration, SALTS_READINESS_EVENT_READ,
        "reactor-test", &cmeta_type_int, probe_read, probe_close, probe);
}

static void capture_observation(differential_observation *out,
                                const sink_probe *sink,
                                const cflow_subscription *run) {
    memset(out, 0, sizeof(*out));
    out->value_count = sink->value_count;
    out->done_count = sink->done_count;
    out->outstanding_demand = cflow_subscription_outstanding_demand(run);
    out->done = cflow_subscription_is_done(run);
    out->errored = sink->error != NULL || cflow_subscription_error(run) != NULL;
    for (size_t index = 0; index < sink->value_count &&
                           index < CFLOW_DIFFERENTIAL_VALUE_COUNT;
         ++index)
        out->values[index] = sink->values[index];
}

static void run_array_differential(const int *values,
                                   differential_observation *out) {
    cflow_publisher source = {0};
    cflow_graph graph = {0};
    cflow_scheduler scheduler = {0};
    cflow_subscription run = {0};
    sink_probe observed = {0};
    cflow_subscriber_callbacks callbacks = {
        sink_value, sink_error, sink_done, &observed
    };
    cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

    cflow_graph_init(&graph, &cmeta_type_int);
    check_true(cflow_scheduler_test_init(&scheduler));
    check_true(cflow_publisher_from_array(&source, &cmeta_type_int, values,
                                       CFLOW_DIFFERENTIAL_VALUE_COUNT));
    check_true(cflow_subscribe(&run, &graph, &source, &scheduler, &sink));
    check_true(cflow_subscription_request(&run, CFLOW_DIFFERENTIAL_DEMAND));
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    capture_observation(out, &observed, &run);

    cflow_subscription_close(&run);
    cflow_scheduler_destroy(&scheduler);
    cflow_graph_destroy(&graph);
}

static void run_fake_readiness_differential(const int *values,
                                            differential_observation *out) {
    fake_env env;
    cmeta_readiness_registration registration = {0};
    cmeta_readiness_stats stats = {0};
    cflow_publisher source = {0};
    cflow_graph graph = {0};
    cflow_scheduler scheduler = {0};
    cflow_subscription run = {0};
    read_probe read = {
        {CFLOW_READ_WOULD_BLOCK, CFLOW_READ_VALUE,
         CFLOW_READ_WOULD_BLOCK, CFLOW_READ_VALUE,
         CFLOW_READ_WOULD_BLOCK, CFLOW_READ_VALUE_AND_DONE},
        {0}, 6u, 0u, 0u, 0u, {NULL}
    };
    sink_probe observed = {0};
    cflow_subscriber_callbacks callbacks = {
        sink_value, sink_error, sink_done, &observed
    };
    cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

    read.values[1] = values[0];
    read.values[3] = values[1];
    read.values[5] = values[2];
    check_true(fake_env_init(&env, 1u));
    check_equal(make_source(&env, CFLOW_READINESS_TEST_RESOURCE, &read,
                            &source, &registration), SALTS_OK);
    check_null(registration.impl);
    check_not_null(env.owner.impl);
    cflow_graph_init(&graph, &cmeta_type_int);
    check_true(cflow_scheduler_test_init(&scheduler));
    check_true(cflow_subscribe(&run, &graph, &source, &scheduler, &sink));
    check_true(cflow_subscription_request(&run, CFLOW_DIFFERENTIAL_DEMAND));
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

    for (size_t index = 0; index < CFLOW_DIFFERENTIAL_VALUE_COUNT; ++index) {
        check_equal(env.factory->emit_resource(
                        env.fixture, CFLOW_READINESS_TEST_RESOURCE,
                        SALTS_READINESS_EVENT_READ), SALTS_OK);
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
        check_equal(observed.value_count, index + 1u);
    }
    capture_observation(out, &observed, &run);

    cflow_subscription_close(&run);
    check_equal(read.closes, (size_t)1u);
    check_equal(cmeta_readiness_reactor_stats(&env.reactor, &stats), SALTS_OK);
    check_equal(stats.registered_count, (size_t)0u);
    check_equal(stats.armed_count, (size_t)0u);
    check_equal(stats.callbacks_inflight, (size_t)0u);
    check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
    check_null(env.owner.impl);
    cflow_scheduler_destroy(&scheduler);
    cflow_graph_destroy(&graph);
    fake_env_destroy(&env);
}

static void check_differential_observation(
    const differential_observation *array,
    const differential_observation *readiness,
    const int *expected_values) {
    check_equal(array->value_count, (size_t)CFLOW_DIFFERENTIAL_VALUE_COUNT);
    check_equal(readiness->value_count, array->value_count);
    check_equal(array->values, expected_values,
                sizeof(int) * CFLOW_DIFFERENTIAL_VALUE_COUNT);
    check_equal(readiness->values, array->values,
                sizeof(int) * CFLOW_DIFFERENTIAL_VALUE_COUNT);
    check_equal(readiness->done_count, array->done_count);
    check_equal(readiness->errored, array->errored);
    check_equal(readiness->outstanding_demand, array->outstanding_demand);
    check_equal(readiness->done, array->done);
    check_equal(array->done_count, (size_t)1u);
    check_false(array->errored);
    check_true(array->done);
    check_equal(array->outstanding_demand,
                (size_t)(CFLOW_DIFFERENTIAL_DEMAND -
                         CFLOW_DIFFERENTIAL_VALUE_COUNT));
}

#if defined(CFLOW_TEST_EPOLL_READINESS)
typedef struct native_pipe_read_probe {
    int read_fd;
    size_t closes;
} native_pipe_read_probe;

typedef struct concurrent_sink_probe {
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    int values[CFLOW_DIFFERENTIAL_VALUE_COUNT];
    size_t value_count;
    size_t done_count;
    const char *error;
} concurrent_sink_probe;

static bool concurrent_sink_value(void *user,
                                  const cmeta_type_desc *type,
                                  const void *value) {
    concurrent_sink_probe *probe = (concurrent_sink_probe *)user;
    bool accepted = false;
    if (!probe || !cmeta_type_equal(type, &cmeta_type_int) || !value)
        return false;
    cmeta_mutex_lock(&probe->lock);
    if (probe->value_count < CFLOW_DIFFERENTIAL_VALUE_COUNT) {
        probe->values[probe->value_count++] = *(const int *)value;
        accepted = true;
        cmeta_cond_broadcast(&probe->changed);
    }
    cmeta_mutex_unlock(&probe->lock);
    return accepted;
}

static void concurrent_sink_error(void *user, const char *message) {
    concurrent_sink_probe *probe = (concurrent_sink_probe *)user;
    cmeta_mutex_lock(&probe->lock);
    probe->error = message;
    cmeta_cond_broadcast(&probe->changed);
    cmeta_mutex_unlock(&probe->lock);
}

static void concurrent_sink_done(void *user) {
    concurrent_sink_probe *probe = (concurrent_sink_probe *)user;
    cmeta_mutex_lock(&probe->lock);
    ++probe->done_count;
    cmeta_cond_broadcast(&probe->changed);
    cmeta_mutex_unlock(&probe->lock);
}

static int concurrent_sink_wait_values(concurrent_sink_probe *probe,
                                       size_t expected) {
    int status = SALTS_OK;
    cmeta_mutex_lock(&probe->lock);
    while (probe->value_count < expected && probe->error == NULL &&
           probe->done_count == 0u && status == SALTS_OK)
        status = cmeta_cond_timedwait(
            &probe->changed, &probe->lock, CFLOW_READINESS_TEST_TIMEOUT_NS);
    if (status == SALTS_OK && probe->value_count < expected)
        status = SALTS_EIO;
    cmeta_mutex_unlock(&probe->lock);
    return status;
}

static int concurrent_sink_wait_done(concurrent_sink_probe *probe) {
    int status = SALTS_OK;
    cmeta_mutex_lock(&probe->lock);
    while (probe->done_count == 0u && probe->error == NULL &&
           status == SALTS_OK)
        status = cmeta_cond_timedwait(
            &probe->changed, &probe->lock, CFLOW_READINESS_TEST_TIMEOUT_NS);
    if (status == SALTS_OK && probe->done_count == 0u)
        status = SALTS_EIO;
    cmeta_mutex_unlock(&probe->lock);
    return status;
}

static void capture_concurrent_observation(
    differential_observation *out, concurrent_sink_probe *sink,
    const cflow_subscription *run) {
    memset(out, 0, sizeof(*out));
    cmeta_mutex_lock(&sink->lock);
    out->value_count = sink->value_count;
    out->done_count = sink->done_count;
    out->errored = sink->error != NULL;
    for (size_t index = 0; index < sink->value_count; ++index)
        out->values[index] = sink->values[index];
    cmeta_mutex_unlock(&sink->lock);
    out->outstanding_demand = cflow_subscription_outstanding_demand(run);
    out->done = cflow_subscription_is_done(run);
    out->errored = out->errored || cflow_subscription_error(run) != NULL;
}

static int set_nonblocking_fd(int fd) {
    int flags;
    do {
        flags = fcntl(fd, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0)
        return -errno;
    while (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        if (errno != EINTR)
            return -errno;
    }
    return SALTS_OK;
}

static int make_native_pipe(int fds[2]) {
    int status;
    if (pipe(fds) != 0)
        return -errno;
    status = set_nonblocking_fd(fds[0]);
    if (status == SALTS_OK)
        status = set_nonblocking_fd(fds[1]);
    if (status != SALTS_OK) {
        (void)close(fds[0]);
        (void)close(fds[1]);
    }
    return status;
}

static int write_native_int(int fd, int value) {
    ssize_t written;
    do {
        written = write(fd, &value, sizeof(value));
    } while (written < 0 && errno == EINTR);
    return written == (ssize_t)sizeof(value) ? SALTS_OK : -errno;
}

static cflow_read_status native_pipe_read(void *user, void *out_value,
                                          const char **error) {
    native_pipe_read_probe *probe = (native_pipe_read_probe *)user;
    ssize_t count;
    do {
        count = read(probe->read_fd, out_value, sizeof(int));
    } while (count < 0 && errno == EINTR);
    if (count == (ssize_t)sizeof(int))
        return CFLOW_READ_VALUE;
    if (count == 0)
        return CFLOW_READ_DONE;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return CFLOW_READ_WOULD_BLOCK;
    if (error)
        *error = "native pipe read failed";
    return CFLOW_READ_ERROR;
}

static void native_pipe_close(void *user) {
    native_pipe_read_probe *probe = (native_pipe_read_probe *)user;
    if (probe->read_fd >= 0) {
        (void)close(probe->read_fd);
        probe->read_fd = -1;
    }
    probe->closes += 1u;
}

static void run_native_readiness_differential(
    const int *values, differential_observation *out) {
    const cmeta_readiness_config config = {1u, 1u};
    cmeta_readiness_reactor reactor = {0};
    cmeta_readiness_registration registration = {0};
    cmeta_readiness_stats stats = {0};
    cflow_readiness_publisher_owner owner = {0};
    cflow_publisher source = {0};
    cflow_graph graph = {0};
    cflow_scheduler scheduler = {0};
    cflow_subscription run = {0};
    native_pipe_read_probe read = {-1, 0u};
    concurrent_sink_probe observed = {0};
    cflow_subscriber_callbacks callbacks = {
        concurrent_sink_value, concurrent_sink_error,
        concurrent_sink_done, &observed
    };
    cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
    int fds[2] = {-1, -1};

    check_equal(make_native_pipe(fds), SALTS_OK);
    cmeta_mutex_init(&observed.lock);
    cmeta_cond_init(&observed.changed);
    check_not_null(observed.lock);
    check_not_null(observed.changed);
    read.read_fd = fds[0];
    check_equal(cmeta_readiness_reactor_init(&reactor, &config), SALTS_OK);
    check_equal(cmeta_readiness_register(&reactor, read.read_fd, &registration),
                SALTS_OK);
    check_equal(cflow_publisher_from_readiness_registration(
                    &source, &owner, &registration,
                    SALTS_READINESS_EVENT_READ | SALTS_READINESS_EVENT_HANGUP,
                    "native-pipe", &cmeta_type_int, native_pipe_read,
                    native_pipe_close, &read), SALTS_OK);
    check_null(registration.impl);
    check_not_null(owner.impl);
    cflow_graph_init(&graph, &cmeta_type_int);
    check_true(cflow_scheduler_worker_init_with_capacity(
        &scheduler, 1u, 8u, 8u));
    check_true(cflow_subscribe(&run, &graph, &source, &scheduler, &sink));
    check_true(cflow_subscription_request(&run, CFLOW_DIFFERENTIAL_DEMAND));
    check_true(cflow_scheduler_wait_idle(&scheduler));

    for (size_t index = 0; index < CFLOW_DIFFERENTIAL_VALUE_COUNT; ++index) {
        check_equal(cmeta_readiness_reactor_stats(&reactor, &stats), SALTS_OK);
        check_equal(stats.registered_count, (size_t)1u);
        check_equal(stats.armed_count, (size_t)1u);
        check_equal(stats.callbacks_inflight, (size_t)0u);
        check_equal(write_native_int(fds[1], values[index]), SALTS_OK);
        check_equal(concurrent_sink_wait_values(&observed, index + 1u),
                    SALTS_OK);
        check_true(cflow_scheduler_wait_idle(&scheduler));
        cmeta_mutex_lock(&observed.lock);
        check_equal(observed.value_count, index + 1u);
        check_null(observed.error);
        cmeta_mutex_unlock(&observed.lock);
    }
    check_equal(close(fds[1]), 0);
    fds[1] = -1;
    check_equal(concurrent_sink_wait_done(&observed), SALTS_OK);
    check_true(cflow_scheduler_wait_idle(&scheduler));
    cmeta_mutex_lock(&observed.lock);
    check_equal(observed.done_count, (size_t)1u);
    check_null(observed.error);
    cmeta_mutex_unlock(&observed.lock);
    capture_concurrent_observation(out, &observed, &run);

    check_not_equal(read.read_fd, -1);
    cflow_subscription_close(&run);
    check_equal(read.closes, (size_t)1u);
    check_equal(read.read_fd, -1);
    check_equal(cflow_readiness_publisher_owner_close(&owner), SALTS_OK);
    check_null(owner.impl);
    check_equal(cmeta_readiness_reactor_stats(&reactor, &stats), SALTS_OK);
    check_equal(stats.registered_count, (size_t)0u);
    check_equal(stats.armed_count, (size_t)0u);
    check_equal(stats.callbacks_inflight, (size_t)0u);
    check_equal(cmeta_readiness_reactor_shutdown(&reactor), SALTS_OK);
    check_equal(cmeta_readiness_reactor_destroy(&reactor), SALTS_OK);
    cflow_scheduler_destroy(&scheduler);
    cflow_graph_destroy(&graph);
    cmeta_cond_destroy(&observed.changed);
    cmeta_mutex_destroy(&observed.lock);
    if (fds[1] >= 0)
        (void)close(fds[1]);
}
#endif

suite("CFlow reactor registration Source") {
    it("keeps caller registration ownership on precise admission failure") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        read_probe probe = {0};

        check_true(fake_env_init(&env, 2u));
        check_equal(cmeta_readiness_register(
                        &env.reactor, CFLOW_READINESS_TEST_RESOURCE,
                        &registration), SALTS_OK);
        check_not_null(registration.impl);
        memset(&source, 0xa5, sizeof(source));
        env.owner.impl = (void *)(uintptr_t)1u;

        check_equal(cflow_publisher_from_readiness_registration(
                        &source, &env.owner, &registration,
                        SALTS_READINESS_EVENT_READ,
                        "managed", &managed_test_type, probe_read,
                        probe_close, &probe), SALTS_ENOTSUP);
        check_false(cflow_publisher_valid(&source));
        check_null(env.owner.impl);
        check_not_null(registration.impl);
        check_equal(cflow_publisher_from_readiness_registration(
                        &source, &env.owner, &registration,
                        SALTS_READINESS_EVENT_READ,
                        "missing-read", &cmeta_type_int, NULL,
                        probe_close, &probe), SALTS_EINVAL);
        check_false(cflow_publisher_valid(&source));
        check_null(env.owner.impl);
        check_not_null(registration.impl);
        check_equal(cmeta_readiness_close(&registration), SALTS_OK);
        check_null(registration.impl);
        fake_env_destroy(&env);
    }

    it("keeps external owner live and side-effect free while Source exists") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        read_probe probe = {0};
        void *owner_impl;

        check_true(fake_env_init(&env, 2u));
        check_equal(make_source(
                        &env, CFLOW_READINESS_TEST_RESOURCE, &probe,
                        &source, &registration), SALTS_OK);
        owner_impl = env.owner.impl;
        check_not_null(owner_impl);

        check_equal(cflow_readiness_publisher_owner_close(&env.owner),
                    SALTS_EBUSY);
        check_equal(env.owner.impl, owner_impl);
        check_equal(env.factory->backend_close_calls(env.fixture),
                    (size_t)0u);
        check_equal(probe.closes, (size_t)0u);

        cflow_publisher_destroy(&source);
        check_equal(probe.closes, (size_t)1u);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        check_null(env.owner.impl);
        fake_env_destroy(&env);
    }

    it("moves registration only after complete construction") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        read_probe probe = {0};

        check_true(fake_env_init(&env, 2u));
        check_equal(make_source(
                        &env, CFLOW_READINESS_TEST_RESOURCE, &probe,
                        &source, &registration), SALTS_OK);
        check_true(cflow_publisher_valid(&source));
        check_not_null(env.owner.impl);
        check_null(registration.impl);

        cflow_publisher_destroy(&source);
        check_equal(probe.closes, (size_t)1u);
        check_equal(env.factory->backend_close_calls(env.fixture),
                    (size_t)1u);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        check_null(env.owner.impl);
        fake_env_destroy(&env);
    }

    it("makes cancel terminal and keeps destroy cleanup exactly once") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        read_probe probe = {0};

        check_true(fake_env_init(&env, 2u));
        check_equal(make_source(
                        &env, CFLOW_READINESS_TEST_RESOURCE, &probe,
                        &source, &registration), SALTS_OK);

        cflow_publisher_cancel(&source);
        check_equal(probe.closes, (size_t)1u);
        check_equal(env.factory->backend_close_calls(env.fixture),
                    (size_t)1u);
        cflow_publisher_destroy(&source);
        check_equal(probe.closes, (size_t)1u);
        check_equal(env.factory->backend_close_calls(env.fixture),
                    (size_t)1u);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        fake_env_destroy(&env);
    }

    it("retains borrowed user after close error and retries on destroy") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        cflow_publish_context resume = {0};
        read_probe probe = {0};
        cflow_step step;
        int output = 0;

        check_true(fake_env_init(&env, 2u));
        check_equal(make_source(
                        &env, CFLOW_READINESS_TEST_RESOURCE, &probe,
                        &source, &registration), SALTS_OK);
        env.factory->fail_hook(env.fixture, READINESS_CONTRACT_HOOK_CLOSE,
                               SALTS_EIO, 1u);

        cflow_publisher_cancel(&source);
        check_equal(probe.closes, (size_t)0u);
        check_equal(env.factory->backend_close_calls(env.fixture),
                    (size_t)1u);
        step = cflow_publisher_resume(&source, &resume, &output);
        check_equal(step.kind, CFLOW_STEP_ERROR);
        check_equal(step.error, "reactor readiness close failed: -4017");
        cflow_publisher_destroy(&source);
        check_equal(probe.closes, (size_t)1u);
        check_equal(env.factory->backend_close_calls(env.fixture),
                    (size_t)2u);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        fake_env_destroy(&env);
    }

    it("keeps persistent close ownership reachable after Run release") {
        enum { PERSISTENT_CLOSE_FAILURES = 16 };
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        cflow_graph graph = {0};
        cflow_scheduler scheduler = {0};
        cflow_subscription run = {0};
        read_probe read = {
            {CFLOW_READ_WOULD_BLOCK}, {0}, 1u, 0u, 0u, 0u, {NULL}
        };
        sink_probe observed = {0};
        cflow_subscriber_callbacks callbacks = {
            sink_value, sink_error, sink_done, &observed
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
        void *owner_impl;

        check_true(fake_env_init(&env, 2u));
        check_equal(make_source(
                        &env, CFLOW_READINESS_TEST_RESOURCE, &read,
                        &source, &registration), SALTS_OK);
        owner_impl = env.owner.impl;
        env.factory->fail_hook(env.fixture, READINESS_CONTRACT_HOOK_CLOSE,
                               SALTS_EIO, PERSISTENT_CLOSE_FAILURES);
        cflow_graph_init(&graph, &cmeta_type_int);
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_subscribe(&run, &graph, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, 1u));
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

        cflow_subscription_close(&run);
        check_equal(env.owner.impl, owner_impl);
        check_equal(read.closes, (size_t)0u);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_EIO);
        check_equal(env.owner.impl, owner_impl);
        check_equal(read.closes, (size_t)0u);

        env.factory->fail_hook(env.fixture, READINESS_CONTRACT_HOOK_CLOSE,
                               SALTS_EIO, 0u);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        check_null(env.owner.impl);
        check_equal(read.closes, (size_t)1u);
        check_equal(cmeta_readiness_reactor_shutdown(&env.reactor), SALTS_OK);
        check_equal(cmeta_readiness_reactor_destroy(&env.reactor), SALTS_OK);
        env.factory->destroy(env.fixture);
        env.fixture = NULL;
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&graph);
    }

    it("maps terminal and invalid read statuses through Run") {
        typedef struct read_case {
            cflow_read_status status;
            const char *read_error;
            const char *expected_error;
            size_t expected_done;
        } read_case;
        static const read_case cases[] = {
            {CFLOW_READ_DONE, NULL, NULL, 1u},
            {CFLOW_READ_ERROR, "scripted read error", "scripted read error", 0u},
            {CFLOW_READ_ERROR, NULL, "reactor readiness source error", 0u},
            {CFLOW_READ_ERROR, "", "reactor readiness source error", 0u},
            {(cflow_read_status)99, NULL,
             "invalid reactor readiness read status", 0u}
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            fake_env env;
            cmeta_readiness_registration registration = {0};
            cflow_publisher source = {0};
            cflow_graph graph = {0};
            cflow_scheduler scheduler = {0};
            cflow_subscription run = {0};
            read_probe read = {0};
            sink_probe observed = {0};
            cflow_subscriber_callbacks callbacks = {
                sink_value, sink_error, sink_done, &observed
            };
            cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

            read.statuses[0] = cases[i].status;
            read.errors[0] = cases[i].read_error;
            read.count = 1u;
            check_true(fake_env_init(&env, 2u));
            check_equal(make_source(
                            &env, CFLOW_READINESS_TEST_RESOURCE, &read,
                            &source, &registration), SALTS_OK);
            cflow_graph_init(&graph, &cmeta_type_int);
            check_true(cflow_scheduler_test_init(&scheduler));
            check_true(cflow_subscribe(
                &run, &graph, &source, &scheduler, &sink));
            check_true(cflow_subscription_request(&run, 1u));
            (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

            check_equal(observed.value_count, (size_t)0u);
            check_equal(observed.done_count, cases[i].expected_done);
            check_equal(read.reads, (size_t)1u);
            if (cases[i].expected_error) {
                check_equal(observed.error, cases[i].expected_error);
                check_equal(cflow_subscription_error(&run), cases[i].expected_error);
            } else {
                check_null(observed.error);
                check_null(cflow_subscription_error(&run));
            }
            check_equal(cflow_subscription_is_done(&run),
                        cases[i].expected_error == NULL);
            cflow_subscription_close(&run);
            check_not_null(env.owner.impl);
            check_equal(read.closes, (size_t)1u);
            check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
            check_null(env.owner.impl);
            check_equal(read.closes, (size_t)1u);
            cflow_scheduler_destroy(&scheduler);
            cflow_graph_destroy(&graph);
            fake_env_destroy(&env);
        }
    }

    it("drives Run through WOULD_BLOCK WAIT wake rearm and final value") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        cflow_graph graph = {0};
        cflow_scheduler scheduler = {0};
        cflow_subscription run = {0};
        read_probe read = {
            {CFLOW_READ_WOULD_BLOCK, CFLOW_READ_VALUE,
             CFLOW_READ_WOULD_BLOCK, CFLOW_READ_VALUE_AND_DONE},
            {0, 17, 0, 29}, 4u, 0u, 0u, 0u
        };
        sink_probe observed = {0};
        cflow_subscriber_callbacks callbacks = {
            sink_value, sink_error, sink_done, &observed
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
        cmeta_readiness_stats stats = {0};
        uint64_t registration_token;
        uint64_t first_arm_token;

        check_true(fake_env_init(&env, 2u));
        check_equal(make_source(
                        &env, CFLOW_READINESS_TEST_RESOURCE, &read,
                        &source, &registration), SALTS_OK);
        registration_token = env.factory->token_for_resource(
            env.fixture, CFLOW_READINESS_TEST_RESOURCE);
        cflow_graph_init(&graph, &cmeta_type_int);
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_subscribe(&run, &graph, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, 2u));
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
        first_arm_token = env.factory->arm_token_for_resource(
            env.fixture, CFLOW_READINESS_TEST_RESOURCE);
        check_not_equal(first_arm_token, (uint64_t)0u);
        check_equal(env.factory->emit_resource(
                        env.fixture, CFLOW_READINESS_TEST_RESOURCE,
                        SALTS_READINESS_EVENT_READ), SALTS_OK);
        check_equal(env.factory->emit_arm_token(
                        env.fixture, registration_token, first_arm_token,
                        SALTS_READINESS_EVENT_READ), SALTS_OK);
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
        check_equal(observed.value_count, (size_t)1u);
        check_equal(observed.values[0], 17);
        check_equal(read.reads, (size_t)3u);
        check_equal(env.factory->emit_arm_token(
                        env.fixture, registration_token, first_arm_token,
                        SALTS_READINESS_EVENT_READ), SALTS_OK);
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
        check_equal(observed.value_count, (size_t)1u);
        check_equal(env.factory->emit_resource(
                        env.fixture, CFLOW_READINESS_TEST_RESOURCE,
                        SALTS_READINESS_EVENT_READ), SALTS_OK);
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
        check_equal(observed.value_count, (size_t)2u);
        check_equal(observed.values[1], 29);
        check_equal(observed.done_count, (size_t)1u);
        check_null(observed.error);
        check_true(cflow_subscription_is_done(&run));
        check_equal(cmeta_readiness_reactor_stats(&env.reactor, &stats),
                    SALTS_OK);
        check_equal(stats.duplicate_events, (uint64_t)1u);
        check_equal(stats.stale_events, (uint64_t)1u);

        cflow_subscription_close(&run);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&graph);
        fake_env_destroy(&env);
    }

    it("turns synchronous arm status into the exact Run error") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        cflow_graph graph = {0};
        cflow_scheduler scheduler = {0};
        cflow_subscription run = {0};
        read_probe read = {{CFLOW_READ_WOULD_BLOCK}, {0}, 1u, 0u, 0u, 0u};
        sink_probe observed = {0};
        cflow_subscriber_callbacks callbacks = {
            sink_value, sink_error, sink_done, &observed
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

        check_true(fake_env_init(&env, 2u));
        check_equal(make_source(
                        &env, CFLOW_READINESS_TEST_RESOURCE, &read,
                        &source, &registration), SALTS_OK);
        env.factory->fail_next_arm(env.fixture, SALTS_EIO);
        cflow_graph_init(&graph, &cmeta_type_int);
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_subscribe(&run, &graph, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, 1u));
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

        check_equal(cflow_subscription_error(&run),
                    "reactor readiness arm failed: -4017");
        check_equal(observed.error,
                    "reactor readiness arm failed: -4017");
        check_equal(observed.value_count, (size_t)0u);
        cflow_subscription_close(&run);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&graph);
        fake_env_destroy(&env);
    }

    it("turns terminal backend status into the exact Run error") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        cflow_graph graph = {0};
        cflow_scheduler scheduler = {0};
        cflow_subscription run = {0};
        read_probe read = {{CFLOW_READ_WOULD_BLOCK}, {0}, 1u, 0u, 0u, 0u};
        sink_probe observed = {0};
        cflow_subscriber_callbacks callbacks = {
            sink_value, sink_error, sink_done, &observed
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

        check_true(fake_env_init(&env, 2u));
        check_equal(make_source(
                        &env, CFLOW_READINESS_TEST_RESOURCE, &read,
                        &source, &registration), SALTS_OK);
        cflow_graph_init(&graph, &cmeta_type_int);
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_subscribe(&run, &graph, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, 1u));
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
        check_equal(env.factory->fail_backend(env.fixture, SALTS_EIO),
                    SALTS_OK);
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

        check_equal(cflow_subscription_error(&run),
                    "reactor readiness backend failed: -4017");
        check_equal(observed.error,
                    "reactor readiness backend failed: -4017");
        cflow_subscription_close(&run);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&graph);
        fake_env_destroy(&env);
    }

    it("makes cancel wait until the old callback waker is quiescent") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        cflow_publish_context resume = {0};
        cflow_step step;
        read_probe read = {{CFLOW_READ_WOULD_BLOCK}, {0}, 1u, 0u, 0u, 0u};
        wake_probe wake = {0};
        emit_thread_args emit_args = {
            &env, CFLOW_READINESS_TEST_RESOURCE, SALTS_EINVAL
        };
        destroy_thread_args cancel_args = {&source, &wake, false};
        cmeta_thread_t emitter = NULL;
        cmeta_thread_t canceller = NULL;
        cmeta_readiness_stats stats = {0};
        int output = 0;

        check_true(fake_env_init(&env, 2u));
        cmeta_mutex_init(&wake.lock);
        cmeta_cond_init(&wake.changed);
        check_not_null(wake.lock);
        check_not_null(wake.changed);
        check_equal(make_source(
                        &env, CFLOW_READINESS_TEST_RESOURCE, &read,
                        &source, &registration), SALTS_OK);
        step = cflow_publisher_resume(&source, &resume, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable, (cflow_waker){blocking_wake, &wake}));

        check_equal(cmeta_thread_create(&emitter, emit_thread, &emit_args),
                    SALTS_OK);
        cmeta_mutex_lock(&wake.lock);
        while (!wake.entered)
            cmeta_cond_wait(&wake.changed, &wake.lock);
        cmeta_mutex_unlock(&wake.lock);

        env.factory->block_hook(env.fixture, READINESS_CONTRACT_HOOK_CLOSE);
        check_equal(cmeta_thread_create(
                        &canceller, cancel_thread, &cancel_args), SALTS_OK);
        check_equal(env.factory->wait_hook_calls(
                        env.fixture, READINESS_CONTRACT_HOOK_CLOSE, 1u,
                        CFLOW_READINESS_TEST_TIMEOUT_NS), SALTS_OK);
        env.factory->release_hook(env.fixture, READINESS_CONTRACT_HOOK_CLOSE);
        check_equal(cmeta_readiness_reactor_stats(&env.reactor, &stats),
                    SALTS_OK);
        check_equal(stats.callbacks_inflight, (size_t)1u);
        cmeta_mutex_lock(&wake.lock);
        check_false(cancel_args.returned);
        check_equal(read.closes, (size_t)0u);
        wake.released = true;
        cmeta_cond_broadcast(&wake.changed);
        cmeta_mutex_unlock(&wake.lock);

        check_equal(cmeta_thread_join(&emitter), SALTS_OK);
        check_equal(cmeta_thread_join(&canceller), SALTS_OK);
        check_equal(emit_args.status, SALTS_OK);
        check_true(cancel_args.returned);
        check_equal(read.closes, (size_t)1u);
        cflow_publisher_destroy(&source);
        check_equal(read.closes, (size_t)1u);
        check_equal(env.factory->backend_close_calls(env.fixture),
                    (size_t)1u);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        cmeta_cond_destroy(&wake.changed);
        cmeta_mutex_destroy(&wake.lock);
        fake_env_destroy(&env);
    }

    it("waits for an inflight callback before user close and free") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cflow_publisher source = {0};
        cflow_publish_context resume = {0};
        cflow_step step;
        read_probe read = {{CFLOW_READ_WOULD_BLOCK}, {0}, 1u, 0u, 0u, 0u};
        wake_probe wake = {0};
        emit_thread_args emit_args = {
            &env, CFLOW_READINESS_TEST_RESOURCE, SALTS_EINVAL
        };
        destroy_thread_args destroy_args = {&source, &wake, false};
        cmeta_thread_t emitter = NULL;
        cmeta_thread_t destroyer = NULL;
        cmeta_readiness_stats stats = {0};
        int output = 0;

        check_true(fake_env_init(&env, 2u));
        cmeta_mutex_init(&wake.lock);
        cmeta_cond_init(&wake.changed);
        check_not_null(wake.lock);
        check_not_null(wake.changed);
        check_equal(make_source(
                        &env, CFLOW_READINESS_TEST_RESOURCE, &read,
                        &source, &registration), SALTS_OK);
        step = cflow_publisher_resume(&source, &resume, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable, (cflow_waker){blocking_wake, &wake}));

        check_equal(cmeta_thread_create(&emitter, emit_thread, &emit_args),
                    SALTS_OK);
        cmeta_mutex_lock(&wake.lock);
        while (!wake.entered)
            cmeta_cond_wait(&wake.changed, &wake.lock);
        cmeta_mutex_unlock(&wake.lock);

        env.factory->block_hook(env.fixture, READINESS_CONTRACT_HOOK_CLOSE);
        check_equal(cmeta_thread_create(
                        &destroyer, destroy_thread, &destroy_args), SALTS_OK);
        check_equal(env.factory->wait_hook_calls(
                        env.fixture, READINESS_CONTRACT_HOOK_CLOSE, 1u,
                        CFLOW_READINESS_TEST_TIMEOUT_NS), SALTS_OK);
        env.factory->release_hook(env.fixture, READINESS_CONTRACT_HOOK_CLOSE);
        check_equal(cmeta_readiness_reactor_stats(&env.reactor, &stats),
                    SALTS_OK);
        check_equal(stats.callbacks_inflight, (size_t)1u);
        cmeta_mutex_lock(&wake.lock);
        check_false(destroy_args.returned);
        check_equal(read.closes, (size_t)0u);
        wake.released = true;
        cmeta_cond_broadcast(&wake.changed);
        cmeta_mutex_unlock(&wake.lock);

        check_equal(cmeta_thread_join(&emitter), SALTS_OK);
        check_equal(cmeta_thread_join(&destroyer), SALTS_OK);
        check_equal(emit_args.status, SALTS_OK);
        check_true(destroy_args.returned);
        check_equal(read.closes, (size_t)1u);
        check_equal(env.factory->backend_close_calls(env.fixture),
                    (size_t)1u);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        cmeta_cond_destroy(&wake.changed);
        cmeta_mutex_destroy(&wake.lock);
        fake_env_destroy(&env);
    }

    it("matches synchronous array observations under bounded readiness reuse") {
        for (size_t iteration = 0;
             iteration < CFLOW_DIFFERENTIAL_ITERATIONS; ++iteration) {
            int values[CFLOW_DIFFERENTIAL_VALUE_COUNT] = {
                (int)(iteration * 10u + 1u),
                (int)(iteration * 10u + 4u),
                (int)(iteration * 10u + 9u)
            };
            differential_observation array = {0};
            differential_observation readiness = {0};

            run_array_differential(values, &array);
            run_fake_readiness_differential(values, &readiness);
            check_differential_observation(&array, &readiness, values);
        }
    }

    it("rearms on a worker before the readiness callback returns") {
        fake_env env;
        cmeta_readiness_registration registration = {0};
        cmeta_readiness_stats stats = {0};
        cflow_publisher source = {0};
        cflow_graph graph = {0};
        cflow_scheduler scheduler = {0};
        cflow_subscription run = {0};
        arm_observing_scheduler_state scheduler_state;
        cmeta_readiness_registration *owner_registration;
        read_probe read = {
            {CFLOW_READ_WOULD_BLOCK, CFLOW_READ_VALUE,
             CFLOW_READ_WOULD_BLOCK, CFLOW_READ_VALUE_AND_DONE},
            {0, 41, 0, 73}, 4u, 0u, 0u, 0u, {NULL}
        };
        sink_probe observed = {0};
        cflow_subscriber_callbacks callbacks = {
            sink_value, sink_error, sink_done, &observed
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

        check_true(fake_env_init(&env, 1u));
        check_equal(cmeta_readiness_register(
                        &env.reactor, CFLOW_READINESS_TEST_RESOURCE,
                        &registration), SALTS_OK);
        check_equal(cflow_publisher_from_readiness_registration(
                        &source, &env.owner, &registration,
                        SALTS_READINESS_EVENT_READ, "worker-rearm",
                        &cmeta_type_int, probe_read, probe_close, &read),
                    SALTS_OK);
        check_null(registration.impl);
        owner_registration =
            cflow_readiness_publisher_owner_observe_registration(&env.owner);
        check_not_null(owner_registration);
        check_not_null(owner_registration->impl);
        check_true(arm_observing_scheduler_init(
            &scheduler_state, owner_registration, &scheduler));
        cflow_graph_init(&graph, &cmeta_type_int);
        check_true(cflow_subscribe(&run, &graph, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, 2u));
        check_true(cflow_scheduler_wait_idle(&scheduler));

        arm_observing_scheduler_observe_next(&scheduler_state);
        check_equal(env.factory->emit_resource(
                        env.fixture, CFLOW_READINESS_TEST_RESOURCE,
                        SALTS_READINESS_EVENT_READ), SALTS_OK);
        check_true(cflow_scheduler_wait_idle(&scheduler));
        check_equal(observed.error ? observed.error : "no error", "no error");
        check_equal(arm_observing_scheduler_status(&scheduler_state), SALTS_OK);
        check_equal(arm_observing_scheduler_api_borrows(&scheduler_state),
                    (uint32_t)1u);
        check_equal(observed.value_count, (size_t)1u);
        check_equal(observed.values[0], 41);

        check_equal(env.factory->emit_resource(
                        env.fixture, CFLOW_READINESS_TEST_RESOURCE,
                        SALTS_READINESS_EVENT_READ), SALTS_OK);
        check_true(cflow_scheduler_wait_idle(&scheduler));
        check_equal(observed.value_count, (size_t)2u);
        check_equal(observed.values[1], 73);
        check_equal(observed.done_count, (size_t)1u);
        check_null(observed.error);
        check_true(cflow_subscription_is_done(&run));

        cflow_subscription_close(&run);
        check_equal(read.closes, (size_t)1u);
        check_equal(cflow_readiness_publisher_owner_close(&env.owner), SALTS_OK);
        check_equal(cmeta_readiness_reactor_stats(&env.reactor, &stats), SALTS_OK);
        check_equal(stats.registered_count, (size_t)0u);
        check_equal(stats.armed_count, (size_t)0u);
        check_equal(stats.callbacks_inflight, (size_t)0u);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&graph);
        fake_env_destroy(&env);
    }

#if defined(CFLOW_TEST_EPOLL_READINESS)
    it("matches synchronous array observations through a native pipe") {
        const int values[CFLOW_DIFFERENTIAL_VALUE_COUNT] = {31, 47, 59};
        differential_observation array = {0};
        differential_observation readiness = {0};

        run_array_differential(values, &array);
        run_native_readiness_differential(values, &readiness);
        check_differential_observation(&array, &readiness, values);
    }
#endif
}
