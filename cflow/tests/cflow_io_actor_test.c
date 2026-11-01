#include <cflow/io_actor.h>

#include <error_codes.h>
#include <thread.h>

#include "tinytest.h"

#include <stdatomic.h>
#include <string.h>

enum { IO_TEST_MAX_REQUESTS = 64 };

typedef struct io_backend_probe {
    cflow_io_request_id submitted[IO_TEST_MAX_REQUESTS];
    size_t submitted_count;
    cflow_io_request_id cancelled[IO_TEST_MAX_REQUESTS];
    size_t cancelled_count;
    int submit_status;
    int cancel_status;
    bool complete_during_submit;
    bool run_during_submit;
    cflow_io_run_status nested_run_status;
} io_backend_probe;

typedef struct io_completion_probe {
    cflow_io_request_id request_ids[IO_TEST_MAX_REQUESTS];
    cflow_io_completion completions[IO_TEST_MAX_REQUESTS];
    size_t count;
} io_completion_probe;

typedef struct io_fixture {
    cflow_executor executor;
    cflow_io_actor actor;
    io_backend_probe backend;
    io_completion_probe completions;
} io_fixture;

typedef struct io_wake_driver_probe {
    cflow_io_actor *actor;
    size_t calls;
    size_t busy;
} io_wake_driver_probe;

typedef struct io_blocking_wake_probe {
    atomic_bool block;
    atomic_bool started;
    atomic_bool release;
} io_blocking_wake_probe;

typedef struct io_executor_runner {
    cflow_executor *executor;
    size_t ran;
} io_executor_runner;

static void io_wake_drive(void *user) {
    io_wake_driver_probe *probe = (io_wake_driver_probe *)user;
    cflow_io_run_result result;
    ++probe->calls;
    result = cflow_io_actor_run_ready(probe->actor, 16u);
    if (result.status == CFLOW_IO_RUN_BUSY)
        ++probe->busy;
}

static void io_wake_block(void *user) {
    io_blocking_wake_probe *probe = (io_blocking_wake_probe *)user;
    bool expected = false;
    if (!atomic_load(&probe->block))
        return;
    if (!atomic_compare_exchange_strong(
            &probe->started, &expected, true))
        return;
    while (!atomic_load(&probe->release))
        cmeta_thread_yield();
}

static void io_run_executor(void *user) {
    io_executor_runner *runner = (io_executor_runner *)user;
    runner->ran = cflow_executor_run_ready(runner->executor);
}

static int io_backend_submit(void *user,
                             cflow_io_actor *actor,
                             cflow_io_request_id request_id,
                             cflow_io_lease_id lease_id,
                             void *operation_user) {
    io_backend_probe *probe = (io_backend_probe *)user;
    (void)actor;
    (void)lease_id;
    (void)operation_user;
    if (probe->submitted_count < IO_TEST_MAX_REQUESTS)
        probe->submitted[probe->submitted_count++] = request_id;
    if (probe->complete_during_submit) {
        const cflow_io_completion completion = {
            CFLOW_IO_COMPLETION_OK, 12u, SALTS_OK};
        (void)cflow_io_actor_complete(actor, request_id, &completion);
    }
    if (probe->run_during_submit)
        probe->nested_run_status = cflow_io_actor_run_one(actor).status;
    return probe->submit_status;
}

static int io_backend_cancel(void *user, cflow_io_request_id request_id) {
    io_backend_probe *probe = (io_backend_probe *)user;
    if (probe->cancelled_count < IO_TEST_MAX_REQUESTS)
        probe->cancelled[probe->cancelled_count++] = request_id;
    return probe->cancel_status;
}

static void io_completion_record(void *user,
                                 cflow_io_request_id request_id,
                                 cflow_io_lease_id lease_id,
                                 void *operation_user,
                                 const cflow_io_completion *completion) {
    io_completion_probe *probe = (io_completion_probe *)user;
    (void)lease_id;
    (void)operation_user;
    if (probe == NULL || probe->count >= IO_TEST_MAX_REQUESTS)
        return;
    probe->request_ids[probe->count] = request_id;
    probe->completions[probe->count] = *completion;
    ++probe->count;
}

static void io_operation_release(void *user) {
    int *released = (int *)user;
    ++*released;
}

typedef struct io_submitter_context {
    cflow_io_actor *actor;
    atomic_bool *go;
    size_t first;
    size_t count;
    cflow_io_submit_result *results;
    int *released;
} io_submitter_context;

static void io_submitter(void *user) {
    io_submitter_context *context = (io_submitter_context *)user;
    size_t offset;
    while (!atomic_load(context->go))
        cmeta_thread_yield();
    for (offset = 0u; offset < context->count; ++offset) {
        const size_t index = context->first + offset;
        cflow_io_operation operation = {
            &context->released[index], io_operation_release};
        context->results[index] = cflow_io_actor_try_submit(
            context->actor, (cflow_io_lease_id)(1000u + index), &operation);
    }
}

static int io_fixture_init(io_fixture *fixture,
                           size_t request_capacity,
                           size_t command_capacity) {
    cflow_io_actor_config config;
    memset(fixture, 0, sizeof(*fixture));
    if (!cflow_executor_manual_init_with_capacity(
            &fixture->executor, request_capacity))
        return 0;
    memset(&config, 0, sizeof(config));
    config.request_capacity = request_capacity;
    config.command_capacity = command_capacity;
    config.executor = &fixture->executor;
    config.backend.submit = io_backend_submit;
    config.backend.cancel = io_backend_cancel;
    config.backend_user = &fixture->backend;
    config.completion = io_completion_record;
    config.completion_user = &fixture->completions;
    if (cflow_io_actor_init(&fixture->actor, &config) != SALTS_OK) {
        cflow_executor_destroy(&fixture->executor);
        return 0;
    }
    return 1;
}

static void io_fixture_settle(io_fixture *fixture,
                              cflow_io_request_id request_id) {
    const cflow_io_completion completion = {
        CFLOW_IO_COMPLETION_OK, 4u, SALTS_OK};
    (void)cflow_io_actor_run_ready(&fixture->actor, 16u);
    check_equal(cflow_io_actor_complete(
                    &fixture->actor, request_id, &completion),
                CFLOW_IO_COMPLETE_ACCEPTED);
    (void)cflow_io_actor_run_ready(&fixture->actor, 16u);
    (void)cflow_executor_run_ready(&fixture->executor);
    check_equal(cflow_io_actor_acknowledge(&fixture->actor, request_id),
                CFLOW_IO_ACK_RELEASED);
}

static void io_fixture_destroy(io_fixture *fixture) {
    const int close_status = cflow_io_actor_close(&fixture->actor);
    check_true(close_status == SALTS_OK || close_status == SALTS_EALREADY);
    (void)cflow_io_actor_run_ready(&fixture->actor, 16u);
    check_true(cflow_io_actor_is_quiescent(&fixture->actor));
    check_equal(cflow_io_actor_destroy(&fixture->actor), SALTS_OK);
    check_true(cflow_executor_shutdown(&fixture->executor));
    cflow_executor_destroy(&fixture->executor);
}

spec("CFlow IO Actor protocol") {
    it("rejects invalid zero-capacity configuration") {
        cflow_executor executor = {0};
        cflow_io_actor actor = {0};
        cflow_io_actor_config config = {0};

        check_true(cflow_executor_manual_init(&executor));
        config.command_capacity = 1u;
        config.executor = &executor;
        config.backend.submit = io_backend_submit;
        config.completion = io_completion_record;
        check_equal(cflow_io_actor_init(&actor, &config), SALTS_EINVAL);
        check_null(actor.impl);
        cflow_executor_destroy(&executor);
    }

    it("moves an operation only after transactional submit") {
        io_fixture fixture;
        int released = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;

        check_true(io_fixture_init(&fixture, 1u, 1u));
        submitted = cflow_io_actor_try_submit(&fixture.actor, 41u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_not_equal(submitted.request_id, (cflow_io_request_id)0u);
        check_null(operation.user);
        check_null(operation.release);

        io_fixture_settle(&fixture, submitted.request_id);
        check_equal(released, 1);
        io_fixture_destroy(&fixture);
    }

    it("preserves caller ownership when request capacity is full") {
        io_fixture fixture;
        int first_released = 0;
        int rejected_released = 0;
        cflow_io_operation first = {&first_released, io_operation_release};
        cflow_io_operation rejected = {
            &rejected_released, io_operation_release};
        cflow_io_submit_result accepted;
        cflow_io_submit_result full;

        check_true(io_fixture_init(&fixture, 1u, 2u));
        accepted = cflow_io_actor_try_submit(&fixture.actor, 1u, &first);
        full = cflow_io_actor_try_submit(&fixture.actor, 2u, &rejected);
        check_equal(accepted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(full.status, CFLOW_IO_SUBMIT_FULL);
        check_equal(full.request_id, (cflow_io_request_id)0u);
        check_true(rejected.user == &rejected_released);
        check_true(rejected.release == io_operation_release);

        rejected.release(rejected.user);
        io_fixture_settle(&fixture, accepted.request_id);
        check_equal(first_released, 1);
        check_equal(rejected_released, 1);
        io_fixture_destroy(&fixture);
    }

    it("preserves caller ownership when command capacity is full") {
        io_fixture fixture;
        int first_released = 0;
        int rejected_released = 0;
        cflow_io_operation first = {&first_released, io_operation_release};
        cflow_io_operation rejected = {
            &rejected_released, io_operation_release};
        cflow_io_submit_result accepted;
        cflow_io_submit_result full;

        check_true(io_fixture_init(&fixture, 2u, 1u));
        accepted = cflow_io_actor_try_submit(&fixture.actor, 1u, &first);
        full = cflow_io_actor_try_submit(&fixture.actor, 2u, &rejected);
        check_equal(accepted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(full.status, CFLOW_IO_SUBMIT_FULL);
        check_true(rejected.user == &rejected_released);
        check_true(rejected.release == io_operation_release);

        rejected.release(rejected.user);
        io_fixture_settle(&fixture, accepted.request_id);
        io_fixture_destroy(&fixture);
    }

    it("rejects a duplicate live lease without consuming ownership") {
        io_fixture fixture;
        int accepted_released = 0;
        int rejected_released = 0;
        cflow_io_operation accepted = {
            &accepted_released, io_operation_release};
        cflow_io_operation rejected = {
            &rejected_released, io_operation_release};
        cflow_io_submit_result first;
        cflow_io_submit_result duplicate;

        check_true(io_fixture_init(&fixture, 2u, 2u));
        first = cflow_io_actor_try_submit(&fixture.actor, 11u, &accepted);
        duplicate = cflow_io_actor_try_submit(
            &fixture.actor, 11u, &rejected);
        check_equal(first.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(duplicate.status, CFLOW_IO_SUBMIT_LEASE_IN_USE);
        check_true(rejected.user == &rejected_released);
        rejected.release(rejected.user);
        io_fixture_settle(&fixture, first.request_id);
        check_equal(accepted_released, 1);
        check_equal(rejected_released, 1);
        io_fixture_destroy(&fixture);
    }

    it("enforces logical command capacity below the rounded ring size") {
        io_fixture fixture;
        int released[4] = {0};
        cflow_io_operation operations[4];
        cflow_io_submit_result results[4];
        size_t index;

        check_true(io_fixture_init(&fixture, 4u, 3u));
        for (index = 0u; index < 4u; ++index) {
            operations[index] = (cflow_io_operation){
                &released[index], io_operation_release};
            results[index] = cflow_io_actor_try_submit(
                &fixture.actor, (cflow_io_lease_id)(21u + index),
                &operations[index]);
        }
        for (index = 0u; index < 3u; ++index)
            check_equal(results[index].status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(results[3].status, CFLOW_IO_SUBMIT_FULL);
        check_true(operations[3].user == &released[3]);
        operations[3].release(operations[3].user);
        for (index = 0u; index < 3u; ++index)
            io_fixture_settle(&fixture, results[index].request_id);
        for (index = 0u; index < 4u; ++index)
            check_equal(released[index], 1);
        io_fixture_destroy(&fixture);
    }

    it("cancels a queued submit before invoking the native backend") {
        io_fixture fixture;
        int released = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;

        check_true(io_fixture_init(&fixture, 1u, 2u));
        submitted = cflow_io_actor_try_submit(&fixture.actor, 71u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(cflow_io_actor_try_cancel(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_CANCEL_ACCEPTED);

        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(fixture.backend.submitted_count, (size_t)0u);
        (void)cflow_executor_run_ready(&fixture.executor);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        check_equal(released, 1);
        io_fixture_destroy(&fixture);
    }

    it("observes backend submission in mailbox FIFO order") {
        io_fixture fixture;
        int first_released = 0;
        int second_released = 0;
        cflow_io_operation first = {&first_released, io_operation_release};
        cflow_io_operation second = {&second_released, io_operation_release};
        cflow_io_submit_result first_result;
        cflow_io_submit_result second_result;

        check_true(io_fixture_init(&fixture, 2u, 2u));
        first_result = cflow_io_actor_try_submit(&fixture.actor, 1u, &first);
        second_result = cflow_io_actor_try_submit(&fixture.actor, 2u, &second);
        check_equal(first_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(second_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(fixture.backend.submitted_count, (size_t)2u);
        check_equal(fixture.backend.submitted[0], first_result.request_id);
        check_equal(fixture.backend.submitted[1], second_result.request_id);

        io_fixture_settle(&fixture, first_result.request_id);
        io_fixture_settle(&fixture, second_result.request_id);
        io_fixture_destroy(&fixture);
    }

    it("preserves FIFO after request slots are released out of order") {
        io_fixture fixture;
        int released[4] = {0};
        cflow_io_operation operations[4];
        cflow_io_submit_result results[4];
        const cflow_io_completion completion = {
            CFLOW_IO_COMPLETION_OK, 1u, SALTS_OK};
        size_t index;

        check_true(io_fixture_init(&fixture, 2u, 2u));
        for (index = 0u; index < 2u; ++index) {
            operations[index] = (cflow_io_operation){
                &released[index], io_operation_release};
            results[index] = cflow_io_actor_try_submit(
                &fixture.actor, (cflow_io_lease_id)(51u + index),
                &operations[index]);
            check_equal(results[index].status, CFLOW_IO_SUBMIT_ACCEPTED);
        }
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        for (index = 0u; index < 2u; ++index)
            check_equal(cflow_io_actor_complete(
                            &fixture.actor, results[index].request_id,
                            &completion),
                        CFLOW_IO_COMPLETE_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        (void)cflow_executor_run_ready(&fixture.executor);

        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, results[1].request_id),
                    CFLOW_IO_ACK_RELEASED);
        operations[2] = (cflow_io_operation){
            &released[2], io_operation_release};
        results[2] = cflow_io_actor_try_submit(
            &fixture.actor, 53u, &operations[2]);
        check_equal(results[2].status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, results[0].request_id),
                    CFLOW_IO_ACK_RELEASED);
        operations[3] = (cflow_io_operation){
            &released[3], io_operation_release};
        results[3] = cflow_io_actor_try_submit(
            &fixture.actor, 54u, &operations[3]);
        check_equal(results[3].status, CFLOW_IO_SUBMIT_ACCEPTED);

        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(fixture.backend.submitted_count, (size_t)4u);
        check_equal(fixture.backend.submitted[2], results[2].request_id);
        check_equal(fixture.backend.submitted[3], results[3].request_id);

        for (index = 2u; index < 4u; ++index)
            io_fixture_settle(&fixture, results[index].request_id);
        for (index = 0u; index < 4u; ++index)
            check_equal(released[index], 1);
        io_fixture_destroy(&fixture);
    }

    it("reports admitted and ready credits without losing conservation") {
        io_fixture fixture;
        int released = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;
        cflow_io_actor_stats stats = {0};

        check_true(io_fixture_init(&fixture, 1u, 1u));
        submitted = cflow_io_actor_try_submit(&fixture.actor, 81u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_true(cflow_io_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.admitted, (size_t)1u);
        check_equal(stats.ready, (size_t)0u);
        check_equal(stats.accepted,
                    stats.acknowledged + (uint64_t)stats.active_requests);

        check_equal(cflow_io_actor_run_one(&fixture.actor).status,
                    CFLOW_IO_RUN_PROGRESSED);
        check_true(cflow_io_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.admitted, (size_t)0u);
        check_equal(stats.ready, (size_t)1u);
        check_equal(stats.accepted,
                    stats.acknowledged + (uint64_t)stats.active_requests);

        io_fixture_settle(&fixture, submitted.request_id);
        io_fixture_destroy(&fixture);
    }

    it("issues an in-flight cancellation before starting newly ready IO") {
        io_fixture fixture;
        int released[2] = {0};
        cflow_io_operation first = {&released[0], io_operation_release};
        cflow_io_operation second = {&released[1], io_operation_release};
        cflow_io_submit_result first_result;
        cflow_io_submit_result second_result;

        check_true(io_fixture_init(&fixture, 2u, 2u));
        first_result = cflow_io_actor_try_submit(&fixture.actor, 82u, &first);
        check_equal(first_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(fixture.backend.submitted_count, (size_t)1u);

        check_equal(cflow_io_actor_try_cancel(
                        &fixture.actor, first_result.request_id),
                    CFLOW_IO_CANCEL_ACCEPTED);
        second_result = cflow_io_actor_try_submit(&fixture.actor, 83u, &second);
        check_equal(second_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(cflow_io_actor_run_one(&fixture.actor).status,
                    CFLOW_IO_RUN_PROGRESSED);
        check_equal(cflow_io_actor_run_one(&fixture.actor).status,
                    CFLOW_IO_RUN_PROGRESSED);
        check_equal(cflow_io_actor_run_one(&fixture.actor).status,
                    CFLOW_IO_RUN_PROGRESSED);
        check_equal(fixture.backend.cancelled_count, (size_t)1u);
        check_equal(fixture.backend.cancelled[0], first_result.request_id);
        check_equal(fixture.backend.submitted_count, (size_t)1u);

        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(fixture.backend.submitted_count, (size_t)2u);
        io_fixture_settle(&fixture, first_result.request_id);
        io_fixture_settle(&fixture, second_result.request_id);
        io_fixture_destroy(&fixture);
    }

    it("treats pending cancellation as a request until native completion") {
        io_fixture fixture;
        int released = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;
        cflow_io_actor_stats stats = {0};
        const cflow_io_completion cancelled = {
            CFLOW_IO_COMPLETION_CANCELLED, 0u, SALTS_OK};

        check_true(io_fixture_init(&fixture, 1u, 2u));
        submitted = cflow_io_actor_try_submit(&fixture.actor, 91u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(fixture.backend.submitted_count, (size_t)1u);
        check_equal(cflow_io_actor_try_cancel(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_CANCEL_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(fixture.backend.cancelled_count, (size_t)1u);
        check_equal(fixture.backend.cancelled[0], submitted.request_id);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_BUSY);

        check_equal(cflow_io_actor_complete(
                        &fixture.actor, submitted.request_id, &cancelled),
                    CFLOW_IO_COMPLETE_ACCEPTED);
        check_equal(cflow_io_actor_complete(
                        &fixture.actor, submitted.request_id, &cancelled),
                    CFLOW_IO_COMPLETE_NOT_PENDING);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_BUSY);
        (void)cflow_executor_run_ready(&fixture.executor);
        check_equal(fixture.completions.count, (size_t)1u);
        check_equal(fixture.completions.completions[0].kind,
                    CFLOW_IO_COMPLETION_CANCELLED);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        check_true(cflow_io_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.stale_completions, (uint64_t)1u);
        check_equal(released, 1);
        io_fixture_destroy(&fixture);
    }

    it("retains completion credit while the Executor is full") {
        io_fixture fixture;
        int released = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;
        cflow_io_actor_stats stats = {0};
        const cflow_io_completion completed = {
            CFLOW_IO_COMPLETION_OK, 8u, SALTS_OK};

        check_true(io_fixture_init(&fixture, 1u, 2u));
        check_equal(cflow_executor_try_post(
                        &fixture.executor, io_operation_release, &released),
                    CFLOW_ADMISSION_ACCEPTED);
        submitted = cflow_io_actor_try_submit(&fixture.actor, 101u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(cflow_io_actor_complete(
                        &fixture.actor, submitted.request_id, &completed),
                    CFLOW_IO_COMPLETE_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_true(cflow_io_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.completions_ready, (size_t)1u);
        check_equal(stats.executor_rejected_full, (uint64_t)1u);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_BUSY);

        (void)cflow_executor_run_ready(&fixture.executor);
        check_equal(released, 1);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        (void)cflow_executor_run_ready(&fixture.executor);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        check_equal(released, 2);
        io_fixture_destroy(&fixture);
    }

    it("delivers through the concurrent serial Executor boundary") {
        io_fixture fixture;
        cflow_io_actor_config config = {0};
        int released = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;
        const cflow_io_completion completed = {
            CFLOW_IO_COMPLETION_OK, 9u, SALTS_OK};

        memset(&fixture, 0, sizeof(fixture));
        check_true(cflow_executor_serial_init_with_capacity(
            &fixture.executor, 1u));
        config.request_capacity = 1u;
        config.command_capacity = 1u;
        config.executor = &fixture.executor;
        config.backend.submit = io_backend_submit;
        config.backend.cancel = io_backend_cancel;
        config.backend_user = &fixture.backend;
        config.completion = io_completion_record;
        config.completion_user = &fixture.completions;
        check_equal(cflow_io_actor_init(&fixture.actor, &config), SALTS_OK);

        submitted = cflow_io_actor_try_submit(&fixture.actor, 102u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(cflow_io_actor_complete(
                        &fixture.actor, submitted.request_id, &completed),
                    CFLOW_IO_COMPLETE_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(fixture.completions.count, (size_t)1u);
        check_equal(fixture.completions.completions[0].bytes, (size_t)9u);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        check_equal(released, 1);
        io_fixture_destroy(&fixture);
    }

    it("closes admission and delivers cancellation for unsubmitted work") {
        io_fixture fixture;
        int accepted_released = 0;
        int rejected_released = 0;
        cflow_io_operation accepted = {
            &accepted_released, io_operation_release};
        cflow_io_operation rejected = {
            &rejected_released, io_operation_release};
        cflow_io_submit_result submitted;
        cflow_io_submit_result after_close;

        check_true(io_fixture_init(&fixture, 2u, 2u));
        submitted = cflow_io_actor_try_submit(&fixture.actor, 201u, &accepted);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(cflow_io_actor_close(&fixture.actor), SALTS_OK);
        after_close = cflow_io_actor_try_submit(
            &fixture.actor, 202u, &rejected);
        check_equal(after_close.status, CFLOW_IO_SUBMIT_CLOSED);
        check_true(rejected.user == &rejected_released);
        check_equal(cflow_io_actor_destroy(&fixture.actor), SALTS_EBUSY);

        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(fixture.backend.submitted_count, (size_t)0u);
        (void)cflow_executor_run_ready(&fixture.executor);
        check_equal(fixture.completions.count, (size_t)1u);
        check_equal(fixture.completions.completions[0].kind,
                    CFLOW_IO_COMPLETION_CANCELLED);
        check_equal(cflow_io_actor_destroy(&fixture.actor), SALTS_EBUSY);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        rejected.release(rejected.user);
        check_equal(cflow_io_actor_destroy(&fixture.actor), SALTS_OK);
        check_true(cflow_executor_shutdown(&fixture.executor));
        cflow_executor_destroy(&fixture.executor);
        check_equal(accepted_released, 1);
        check_equal(rejected_released, 1);
    }

    it("converts synchronous backend submission failure into one completion") {
        io_fixture fixture;
        int released = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;

        check_true(io_fixture_init(&fixture, 1u, 1u));
        fixture.backend.submit_status = SALTS_EBUSY;
        submitted = cflow_io_actor_try_submit(&fixture.actor, 301u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        (void)cflow_executor_run_ready(&fixture.executor);
        check_equal(fixture.completions.count, (size_t)1u);
        check_equal(fixture.completions.completions[0].kind,
                    CFLOW_IO_COMPLETION_FAILED);
        check_equal(fixture.completions.completions[0].error, SALTS_EBUSY);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        check_equal(released, 1);
        io_fixture_destroy(&fixture);
    }

    it("does not overwrite synchronous completion with submit return failure") {
        io_fixture fixture;
        int released = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;
        cflow_io_actor_stats stats = {0};

        check_true(io_fixture_init(&fixture, 1u, 1u));
        fixture.backend.complete_during_submit = true;
        fixture.backend.submit_status = SALTS_EBUSY;
        submitted = cflow_io_actor_try_submit(&fixture.actor, 401u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        (void)cflow_executor_run_ready(&fixture.executor);
        check_equal(fixture.completions.count, (size_t)1u);
        check_equal(fixture.completions.completions[0].kind,
                    CFLOW_IO_COMPLETION_OK);
        check_equal(fixture.completions.completions[0].bytes, (size_t)12u);
        check_true(cflow_io_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.backend_submit_errors, (uint64_t)1u);
        check_equal(stats.stale_completions, (uint64_t)0u);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        check_equal(released, 1);
        io_fixture_destroy(&fixture);
    }

    it("rejects recursive driver entry from a backend callback") {
        io_fixture fixture;
        int released = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;

        check_true(io_fixture_init(&fixture, 1u, 1u));
        fixture.backend.run_during_submit = true;
        submitted = cflow_io_actor_try_submit(&fixture.actor, 402u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(fixture.backend.nested_run_status, CFLOW_IO_RUN_BUSY);
        io_fixture_settle(&fixture, submitted.request_id);
        io_fixture_destroy(&fixture);
    }

    it("coalesces wake requests raised while the driver is active") {
        io_fixture fixture;
        cflow_io_actor_config config = {0};
        io_wake_driver_probe wake = {0};
        int released = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;

        memset(&fixture, 0, sizeof(fixture));
        check_true(cflow_executor_manual_init_with_capacity(
            &fixture.executor, 1u));
        config.request_capacity = 1u;
        config.command_capacity = 1u;
        config.executor = &fixture.executor;
        config.backend.submit = io_backend_submit;
        config.backend.cancel = io_backend_cancel;
        config.backend_user = &fixture.backend;
        config.completion = io_completion_record;
        config.completion_user = &fixture.completions;
        config.wake = io_wake_drive;
        config.wake_user = &wake;
        wake.actor = &fixture.actor;
        fixture.backend.complete_during_submit = true;
        check_equal(cflow_io_actor_init(&fixture.actor, &config), SALTS_OK);

        submitted = cflow_io_actor_try_submit(&fixture.actor, 403u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(wake.busy, (size_t)0u);
        check_true(wake.calls >= (size_t)2u);
        (void)cflow_executor_run_ready(&fixture.executor);
        check_equal(fixture.completions.count, (size_t)1u);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        check_equal(released, 1);
        io_fixture_destroy(&fixture);
    }

    it("keeps destroy busy until a delivery wake callback returns") {
        io_fixture fixture;
        cflow_io_actor_config config = {0};
        io_blocking_wake_probe wake;
        io_executor_runner runner;
        cmeta_thread_t thread = {0};
        int released = 0;
        int attempts = 0;
        cflow_io_operation operation = {&released, io_operation_release};
        cflow_io_submit_result submitted;
        const cflow_io_completion completion = {
            CFLOW_IO_COMPLETION_OK, 10u, SALTS_OK};

        memset(&fixture, 0, sizeof(fixture));
        atomic_init(&wake.block, false);
        atomic_init(&wake.started, false);
        atomic_init(&wake.release, false);
        runner = (io_executor_runner){&fixture.executor, 0u};
        check_true(cflow_executor_manual_init_with_capacity(
            &fixture.executor, 1u));
        config.request_capacity = 1u;
        config.command_capacity = 1u;
        config.executor = &fixture.executor;
        config.backend.submit = io_backend_submit;
        config.backend.cancel = io_backend_cancel;
        config.backend_user = &fixture.backend;
        config.completion = io_completion_record;
        config.completion_user = &fixture.completions;
        config.wake = io_wake_block;
        config.wake_user = &wake;
        check_equal(cflow_io_actor_init(&fixture.actor, &config), SALTS_OK);

        submitted = cflow_io_actor_try_submit(&fixture.actor, 404u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        check_equal(cflow_io_actor_complete(
                        &fixture.actor, submitted.request_id, &completion),
                    CFLOW_IO_COMPLETE_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 16u);
        atomic_store(&wake.block, true);
        check_equal(cmeta_thread_create(&thread, io_run_executor, &runner), 0);
        while (!atomic_load(&wake.started) && attempts++ < 1000)
            cmeta_sleep_ms(1);
        check_true(atomic_load(&wake.started));

        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        check_equal(cflow_io_actor_close(&fixture.actor), SALTS_OK);
        check_false(cflow_io_actor_is_quiescent(&fixture.actor));
        check_equal(cflow_io_actor_destroy(&fixture.actor), SALTS_EBUSY);

        atomic_store(&wake.release, true);
        check_equal(cmeta_thread_join(&thread), 0);
        check_equal(runner.ran, (size_t)1u);
        check_true(cflow_io_actor_is_quiescent(&fixture.actor));
        check_equal(cflow_io_actor_destroy(&fixture.actor), SALTS_OK);
        check_true(cflow_executor_shutdown(&fixture.executor));
        cflow_executor_destroy(&fixture.executor);
        check_equal(released, 1);
    }

    it("admits concurrent publishers with unique request ownership") {
        enum { PRODUCERS = 4, PER_PRODUCER = 8, TOTAL = 32 };
        io_fixture fixture;
        cmeta_thread_t threads[PRODUCERS] = {0};
        io_submitter_context contexts[PRODUCERS] = {0};
        cflow_io_submit_result results[TOTAL] = {0};
        int released[TOTAL] = {0};
        atomic_bool go = false;
        const cflow_io_completion completion = {
            CFLOW_IO_COMPLETION_OK, 1u, SALTS_OK};
        size_t producer;
        size_t index;
        size_t other;

        check_true(io_fixture_init(&fixture, TOTAL, TOTAL));
        for (producer = 0u; producer < PRODUCERS; ++producer) {
            contexts[producer] = (io_submitter_context){
                &fixture.actor, &go, producer * PER_PRODUCER,
                PER_PRODUCER, results, released};
            check_equal(cmeta_thread_create(
                            &threads[producer], io_submitter,
                            &contexts[producer]),
                        0);
        }
        atomic_store(&go, true);
        for (producer = 0u; producer < PRODUCERS; ++producer)
            check_equal(cmeta_thread_join(&threads[producer]), 0);

        for (index = 0u; index < TOTAL; ++index) {
            check_equal(results[index].status, CFLOW_IO_SUBMIT_ACCEPTED);
            check_not_equal(results[index].request_id,
                            (cflow_io_request_id)0u);
            for (other = index + 1u; other < TOTAL; ++other)
                check_not_equal(results[index].request_id,
                                results[other].request_id);
        }
        (void)cflow_io_actor_run_ready(&fixture.actor, 256u);
        check_equal(fixture.backend.submitted_count, (size_t)TOTAL);
        for (index = 0u; index < TOTAL; ++index)
            check_equal(cflow_io_actor_complete(
                            &fixture.actor, results[index].request_id,
                            &completion),
                        CFLOW_IO_COMPLETE_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 256u);
        (void)cflow_executor_run_ready(&fixture.executor);
        for (index = 0u; index < TOTAL; ++index) {
            check_equal(cflow_io_actor_acknowledge(
                            &fixture.actor, results[index].request_id),
                        CFLOW_IO_ACK_RELEASED);
            check_equal(released[index], 1);
        }
        io_fixture_destroy(&fixture);
    }
}
