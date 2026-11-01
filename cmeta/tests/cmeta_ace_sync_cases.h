#ifndef CMETA_ACE_SYNC_CASES_H
#define CMETA_ACE_SYNC_CASES_H

#include <cmeta/ace_synchronization.h>
#include <thread.h>
#include <stdint.h>

#ifdef __cplusplus
#define ACE_SYNC_CAST(type_, ptr_) static_cast<type_>(ptr_)
#else
#define ACE_SYNC_CAST(type_, ptr_) ((type_)(ptr_))
#endif

/* CMeta reflects the actual enum status type rather than pretending the
 * native cmeta_status return carrier is an int. This is a test-local
 * canonical declaration, not a new global metadata registry. */
static const cmeta_type_desc ace_sync_status_type = {
    "cmeta_status", sizeof(cmeta_status), CMETA_ALIGNOF(cmeta_status),
    CMETA_T_INTEGER, NULL, NULL, NULL
};

/* One canonical CMeta Interface with two real native strategies. */
static void ace_sync_mutex_acquire(void *user) {
    cmeta_mutex_lock(ACE_SYNC_CAST(cmeta_mutex_t *, user));
}
static void ace_sync_mutex_release(void *user) {
    cmeta_mutex_unlock(ACE_SYNC_CAST(cmeta_mutex_t *, user));
}
static void ace_sync_rw_acquire(void *user) {
    cmeta_rwlock_wrlock(ACE_SYNC_CAST(cmeta_rwlock_t *, user));
}
static void ace_sync_rw_release(void *user) {
    cmeta_rwlock_wrunlock(ACE_SYNC_CAST(cmeta_rwlock_t *, user));
}
CMETA_IMPLEMENTS(cmeta_ace_lockable, ace_sync_mutex_policy, 0u,
    .acquire = ace_sync_mutex_acquire,
    .release = ace_sync_mutex_release);
CMETA_IMPLEMENTS(cmeta_ace_lockable, ace_sync_rw_policy, 0u,
    .acquire = ace_sync_rw_acquire,
    .release = ace_sync_rw_release);

/* Thread-Safe Interface: public wrapper acquires once; private method is
 * already under the guard and must NEVER enter the public method again. */
typedef struct ace_sync_counter {
    cmeta_ace_lockable policy;
    int value;
    unsigned private_calls;
} ace_sync_counter;

typedef struct ace_sync_operation {
    ace_sync_counter *counter;
    int increment;
    int result;
} ace_sync_operation;

CMETA_ACE_SYNCHRONIZED(ace_sync_counter_gate, ace_sync_operation);

static void ace_sync_add_private(ace_sync_counter *counter, int delta) {
    counter->value += delta;
    ++counter->private_calls;
}
static cmeta_status ace_sync_add_body(ace_sync_operation *operation) {
    ace_sync_add_private(operation->counter, operation->increment);
    ace_sync_add_private(operation->counter, operation->increment);
    operation->result = operation->counter->value;
    return CMETA_OK;
}
static cmeta_status ace_sync_fail_body(ace_sync_operation *operation) {
    (void)operation;
    return CMETA_CALLBACK_ERROR;
}
static cmeta_status ace_sync_read_body(ace_sync_operation *operation) {
    operation->result = operation->counter->value;
    return CMETA_OK;
}

#define ACE_SYNC_COUNTER_METHODS(X,I) \
    X(I,FR1,cmeta_status,add,stateful, \
      &ace_sync_status_type,CMETA_ABI_ENUM,CMETA_RESULT_VALUE, \
      (int,increment,CMETA_PARAM_IN,&cmeta_type_int,CMETA_ABI_SCALAR)) \
    X(I,FR0,int,get,stateful, \
      &cmeta_type_int,CMETA_ABI_SCALAR,CMETA_RESULT_VALUE)
CMETA_INTERFACE(ace_sync_counter_port, ACE_SYNC_COUNTER_METHODS);

static cmeta_status ace_sync_counter_add(void *self, int increment) {
    ace_sync_counter *counter = ACE_SYNC_CAST(ace_sync_counter *, self);
    ace_sync_operation operation = {counter, increment, 0};
    return ace_sync_counter_gate_run(&counter->policy, &operation,
                                     ace_sync_add_body);
}
static int ace_sync_counter_get(void *self) {
    ace_sync_counter *counter = ACE_SYNC_CAST(ace_sync_counter *, self);
    ace_sync_operation operation = {counter, 0, 0};
    if (ace_sync_counter_gate_run(&counter->policy, &operation,
                                  ace_sync_read_body) != CMETA_OK)
        return -1;
    return operation.result;
}
CMETA_IMPLEMENTS(ace_sync_counter_port, ace_sync_counter_impl, 0u,
    .add = ace_sync_counter_add,
    .get = ace_sync_counter_get);

typedef struct ace_sync_worker {
    ace_sync_counter_port counter;
    int failures;
} ace_sync_worker;

static void ace_sync_counter_worker(void *user) {
    ace_sync_worker *worker = ACE_SYNC_CAST(ace_sync_worker *, user);
    for (int i = 0; i < 125; ++i)
        if (ace_sync_counter_port_add(&worker->counter, 1) != CMETA_OK)
            ++worker->failures;
}

/* Owner-affine is NOT synonymous with thread-safe. The canonical Platform
 * thread token gates an exact reflected CMeta Interface on its owner lane.
 * Foreign threads fail before touching unprotected state; synchronous public
 * method serialization is independently proven by ace_sync_counter_port. */
typedef struct ace_sync_owner_resource {
    const void *owner_token;
    int value;
} ace_sync_owner_resource;

#define ACE_SYNC_OWNER_METHODS(X,I) \
    X(I,FR1,cmeta_status,increment,stateful, \
      &ace_sync_status_type,CMETA_ABI_ENUM,CMETA_RESULT_VALUE, \
      (int,delta,CMETA_PARAM_IN,&cmeta_type_int,CMETA_ABI_SCALAR))
CMETA_INTERFACE(ace_sync_owner_port, ACE_SYNC_OWNER_METHODS);

static cmeta_status ace_sync_owner_increment(void *self, int delta) {
    ace_sync_owner_resource *resource = ACE_SYNC_CAST(ace_sync_owner_resource *, self);
    if (resource->owner_token == NULL) return CMETA_INVALID_ARGUMENT;
    if (resource->owner_token != cmeta_thread_current_token()) return CMETA_BUSY;
    resource->value += delta;
    return CMETA_OK;
}
CMETA_IMPLEMENTS(ace_sync_owner_port, ace_sync_owner_impl, 0u,
    .increment = ace_sync_owner_increment);

typedef struct ace_sync_wrong_owner_worker {
    ace_sync_owner_port interface;
    cmeta_status status;
} ace_sync_wrong_owner_worker;
static void ace_sync_wrong_owner_run(void *user) {
    ace_sync_wrong_owner_worker *worker =
        ACE_SYNC_CAST(ace_sync_wrong_owner_worker *, user);
    worker->status = ace_sync_owner_port_increment(&worker->interface, 11);
}

/* ACE Thread-Specific Storage: this is actual Platform TLS, not a
 * thread-affine borrowed Local disguised as per-thread allocation.
 * CMeta's owner token remains a nontransferable lifetime boundary. */
static SALTS_THREAD_LOCAL int ace_sync_tls_counter;

typedef struct ace_sync_tls_worker {
    int seed;
    int initial;
    int nested;
    int final;
    const void *owner;
} ace_sync_tls_worker;

static int ace_sync_tls_nested_add(int increment) {
    ace_sync_tls_counter += increment;
    return ace_sync_tls_counter;
}
static void ace_sync_tls_run(void *user) {
    ace_sync_tls_worker *worker = ACE_SYNC_CAST(ace_sync_tls_worker *, user);
    worker->initial = ace_sync_tls_counter;
    worker->owner = cmeta_thread_current_token();
    ace_sync_tls_counter = worker->seed;
    worker->nested = ace_sync_tls_nested_add(2);
    worker->final = ace_sync_tls_counter;
    /* Explicit cleanup before thread exit; never export an address/borrow. */
    ace_sync_tls_counter = 0;
}

/* A TLS pointer cannot carry an implicit cross-thread borrow. Model its
 * lifetime using the existing Platform thread-affine state, not by comparing
 * cached OS thread tokens after a worker exits. */
typedef struct ace_sync_tls_borrow_probe {
    cmeta_thread_affine_state affinity;
    int value;
    int foreign_status;
} ace_sync_tls_borrow_probe;
static void ace_sync_tls_foreign_borrow(void *user) {
    ace_sync_tls_borrow_probe *probe =
        ACE_SYNC_CAST(ace_sync_tls_borrow_probe *, user);
    int status = cmeta_thread_affine_check(&probe->affinity, probe);
    probe->foreign_status = status;
    if (status == SALTS_OK) ++probe->value;
}

/* POSA2 Wrapper Facade: typed, test-local Platform mutex resource with
 * explicit open/use/close admission and no disguised runtime ownership.
 * Native mutex operations only occur while the facade is live. */
typedef struct ace_sync_mutex_facade {
    cmeta_mutex_t native;
    bool live;
    int value;
} ace_sync_mutex_facade;

static cmeta_status ace_sync_facade_open(ace_sync_mutex_facade *facade) {
    if (facade == NULL) return CMETA_INVALID_ARGUMENT;
    if (facade->live) return CMETA_BUSY;
    cmeta_mutex_init(&facade->native);
    facade->value = 0;
    facade->live = true;
    return CMETA_OK;
}
static cmeta_status ace_sync_facade_add(ace_sync_mutex_facade *facade,
                                         int delta, int *out) {
    if (facade == NULL || out == NULL) return CMETA_INVALID_ARGUMENT;
    if (!facade->live) return CMETA_BUSY;
    cmeta_mutex_lock(&facade->native);
    facade->value += delta;
    *out = facade->value;
    cmeta_mutex_unlock(&facade->native);
    return CMETA_OK;
}
static cmeta_status ace_sync_facade_close(ace_sync_mutex_facade *facade) {
    if (facade == NULL) return CMETA_INVALID_ARGUMENT;
    if (!facade->live) return CMETA_BUSY;
    /* Caller must have joined all borrowers before closing. */
    facade->live = false;
    cmeta_mutex_destroy(&facade->native);
    facade->native = NULL;
    return CMETA_OK;
}

/* POSA2 safe Once/DCL intent: use the canonical Platform once primitive,
 * never the historical unchecked read/unsynchronized double-check idiom.
 * The published value is immutable after the one-time callback returns. */
static cmeta_once_t ace_sync_once_guard = SALTS_ONCE_INIT;
static int ace_sync_once_value;
static int ace_sync_once_calls;
static void ace_sync_once_initialize(void) {
    ace_sync_once_value = 0x5a17;
    ++ace_sync_once_calls;
}
typedef struct ace_sync_once_worker {
    int observed;
} ace_sync_once_worker;
static void ace_sync_once_run(void *user) {
    ace_sync_once_worker *worker = ACE_SYNC_CAST(ace_sync_once_worker *, user);
    cmeta_once(&ace_sync_once_guard, ace_sync_once_initialize);
    worker->observed = ace_sync_once_value;
}

/* Monitor Object: predicate, condition and close belong to the same guarded
 * native object. The public CMeta interface supplies exact synchronous ABI. */
typedef struct ace_sync_monitor {
    cmeta_mutex_t mutex;
    cmeta_cond_t changed;
    bool full;
    bool closed;
    unsigned waiters;
    int value;
} ace_sync_monitor;

static void ace_sync_monitor_init(ace_sync_monitor *monitor) {
    ace_sync_monitor empty = {0};
    *monitor = empty;
    cmeta_mutex_init(&monitor->mutex);
    cmeta_cond_init(&monitor->changed);
}
static void ace_sync_monitor_destroy(ace_sync_monitor *monitor) {
    /* Only after close plus joining all waiters: no hidden lease or worker. */
    cmeta_cond_destroy(&monitor->changed);
    cmeta_mutex_destroy(&monitor->mutex);
}
static int ace_sync_monitor_wait(ace_sync_monitor *monitor) {
    int rc;
    ++monitor->waiters;
    rc = cmeta_cond_timedwait(&monitor->changed, &monitor->mutex,
                              UINT64_C(2000000000));
    --monitor->waiters;
    return rc;
}

#define ACE_SYNC_MONITOR_METHODS(X,I) \
    X(I,FR1,cmeta_status,put,stateful, \
      &ace_sync_status_type,CMETA_ABI_ENUM,CMETA_RESULT_VALUE, \
      (int,value,CMETA_PARAM_IN,&cmeta_type_int,CMETA_ABI_SCALAR)) \
    X(I,FR1,cmeta_status,take,stateful, \
      &ace_sync_status_type,CMETA_ABI_ENUM,CMETA_RESULT_VALUE, \
      (int *,out,CMETA_PARAM_OUT | CMETA_PARAM_BORROWED, \
       &cmeta_type_int_ptr,CMETA_ABI_OBJECT_POINTER)) \
    X(I,FV0,void,close,stateful, \
      &cmeta_type_void,CMETA_ABI_VOID)
CMETA_INTERFACE(ace_sync_monitor_port, ACE_SYNC_MONITOR_METHODS);

static cmeta_status ace_sync_monitor_put(void *self, int value) {
    ace_sync_monitor *monitor = ACE_SYNC_CAST(ace_sync_monitor *, self);
    cmeta_mutex_lock(&monitor->mutex);
    while (monitor->full && !monitor->closed) {
        if (ace_sync_monitor_wait(monitor) != 0) {
            cmeta_mutex_unlock(&monitor->mutex);
            return CMETA_BUSY;
        }
    }
    if (monitor->closed) {
        cmeta_mutex_unlock(&monitor->mutex);
        return CMETA_BUSY;
    }
    monitor->value = value;
    monitor->full = true;
    cmeta_cond_broadcast(&monitor->changed);
    cmeta_mutex_unlock(&monitor->mutex);
    return CMETA_OK;
}
static cmeta_status ace_sync_monitor_take(void *self, int *out) {
    ace_sync_monitor *monitor = ACE_SYNC_CAST(ace_sync_monitor *, self);
    if (out == NULL) return CMETA_INVALID_ARGUMENT;
    cmeta_mutex_lock(&monitor->mutex);
    while (!monitor->full && !monitor->closed) {
        if (ace_sync_monitor_wait(monitor) != 0) {
            cmeta_mutex_unlock(&monitor->mutex);
            return CMETA_BUSY;
        }
    }
    if (!monitor->full) {
        cmeta_mutex_unlock(&monitor->mutex);
        return CMETA_BUSY;
    }
    *out = monitor->value;
    monitor->full = false;
    cmeta_cond_broadcast(&monitor->changed);
    cmeta_mutex_unlock(&monitor->mutex);
    return CMETA_OK;
}
static void ace_sync_monitor_close(void *self) {
    ace_sync_monitor *monitor = ACE_SYNC_CAST(ace_sync_monitor *, self);
    cmeta_mutex_lock(&monitor->mutex);
    monitor->closed = true;
    cmeta_cond_broadcast(&monitor->changed);
    cmeta_mutex_unlock(&monitor->mutex);
}
CMETA_IMPLEMENTS(ace_sync_monitor_port, ace_sync_monitor_impl, 0u,
    .put = ace_sync_monitor_put,
    .take = ace_sync_monitor_take,
    .close = ace_sync_monitor_close);

typedef struct ace_sync_consumer {
    ace_sync_monitor_port monitor;
    cmeta_status status;
    int value;
} ace_sync_consumer;
static void ace_sync_monitor_worker(void *user) {
    ace_sync_consumer *consumer = ACE_SYNC_CAST(ace_sync_consumer *, user);
    consumer->status = ace_sync_monitor_port_take(&consumer->monitor,
                                                  &consumer->value);
}

typedef struct ace_sync_producer {
    ace_sync_monitor_port monitor;
    cmeta_status status;
    int value;
} ace_sync_producer;
static void ace_sync_producer_worker(void *user) {
    ace_sync_producer *producer = ACE_SYNC_CAST(ace_sync_producer *, user);
    producer->status = ace_sync_monitor_port_put(&producer->monitor,
                                                 producer->value);
}

#ifdef __cplusplus
static cmeta_status ace_sync_throws(ace_sync_operation *operation) {
    (void)operation;
    throw 7;
}
#endif

suite("CMeta ACE concurrent pattern composition") {
    it("wraps a native mutex resource with explicit open use and close admission") {
        ace_sync_mutex_facade facade = {0};
        int result = 91;
        check_equal(ace_sync_facade_add(&facade, 1, &result), CMETA_BUSY);
        check_equal(result, 91);
        check_equal(ace_sync_facade_open(NULL), CMETA_INVALID_ARGUMENT);
        check_equal(ace_sync_facade_open(&facade), CMETA_OK);
        check_equal(ace_sync_facade_open(&facade), CMETA_BUSY);
        check_equal(ace_sync_facade_add(&facade, 5, NULL),
                    CMETA_INVALID_ARGUMENT);
        check_equal(ace_sync_facade_add(&facade, 5, &result), CMETA_OK);
        check_equal(result, 5);
        check_equal(ace_sync_facade_close(&facade), CMETA_OK);
        check_equal(ace_sync_facade_close(&facade), CMETA_BUSY);
        check_equal(ace_sync_facade_add(&facade, 1, &result), CMETA_BUSY);
        check_equal(result, 5);
        check_equal(ace_sync_facade_open(&facade), CMETA_OK);
        check_equal(ace_sync_facade_add(&facade, 2, &result), CMETA_OK);
        check_equal(result, 2);
        check_equal(ace_sync_facade_close(&facade), CMETA_OK);
    }

    it("publishes initialized state exactly once across native concurrent callers") {
        cmeta_thread_t threads[4] = {0};
        ace_sync_once_worker workers[4] = {{0}, {0}, {0}, {0}};
        for (size_t i = 0; i < 4u; ++i)
            check_equal(cmeta_thread_create(&threads[i], ace_sync_once_run,
                                             &workers[i]), 0);
        for (size_t i = 0; i < 4u; ++i) {
            check_equal(cmeta_thread_join(&threads[i]), 0);
            check_equal(workers[i].observed, 0x5a17);
        }
        cmeta_once(&ace_sync_once_guard, ace_sync_once_initialize);
        check_equal(ace_sync_once_value, 0x5a17);
        check_equal(ace_sync_once_calls, 1);
    }

    it("starts a fresh TLS instance after each completed worker lifetime") {
        /* Workers are joined before another starts; never compare tokens
         * across terminated OS threads, since identities may be recycled. */
        ace_sync_tls_worker first = {17, -1, 0, 0, NULL};
        ace_sync_tls_worker second = {31, -1, 0, 0, NULL};
        cmeta_thread_t thread = NULL;
        ace_sync_tls_counter = 77;
        check_equal(cmeta_thread_create(&thread, ace_sync_tls_run, &first), 0);
        check_equal(cmeta_thread_join(&thread), 0);
        check_equal(first.initial, 0);
        check_equal(first.final, 19);
        thread = NULL;
        check_equal(cmeta_thread_create(&thread, ace_sync_tls_run, &second), 0);
        check_equal(cmeta_thread_join(&thread), 0);
        check_equal(second.initial, 0);
        check_equal(second.final, 33);
        check_equal(ace_sync_tls_counter, 77);
        ace_sync_tls_counter = 0;
    }

    it("rejects a cross-thread borrowed TLS owner before touching state") {
        ace_sync_tls_borrow_probe probe = {0};
        cmeta_thread_t thread = NULL;
        probe.value = 23;
        probe.foreign_status = SALTS_OK;
        check_equal(cmeta_thread_affine_init(&probe.affinity, &probe), SALTS_OK);
        check_equal(cmeta_thread_create(&thread, ace_sync_tls_foreign_borrow,
                                        &probe), 0);
        check_equal(cmeta_thread_join(&thread), 0);
        check_equal(probe.foreign_status, SALTS_EINVAL);
        check_equal(probe.value, 23);
        check_equal(cmeta_thread_affine_check(&probe.affinity, &probe), SALTS_OK);
        check_equal(cmeta_thread_affine_reset(&probe.affinity, &probe), SALTS_OK);
    }

    it("isolates real Thread-Specific Storage across native workers and nested calls") {
        cmeta_thread_t threads[2] = {0};
        ace_sync_tls_worker workers[2] = {{11, 0, 0, 0, NULL},
                                          {29, 0, 0, 0, NULL}};
        const void *main_owner = cmeta_thread_current_token();
        ace_sync_tls_counter = 101;
        check_equal(cmeta_thread_create(&threads[0], ace_sync_tls_run,
                                        &workers[0]), 0);
        check_equal(cmeta_thread_create(&threads[1], ace_sync_tls_run,
                                        &workers[1]), 0);
        for (size_t i = 0; i < 2u; ++i) {
            check_equal(cmeta_thread_join(&threads[i]), 0);
            check_equal(workers[i].initial, 0);
            check_equal(workers[i].nested, workers[i].seed + 2);
            check_equal(workers[i].final, workers[i].seed + 2);
            check_true(workers[i].owner != NULL);
            check_true(workers[i].owner != main_owner);
        }
        check_equal(ace_sync_tls_counter, 101);
        check_equal(ace_sync_tls_nested_add(3), 104);
        ace_sync_tls_counter = 0;
    }

    it("selects real typed locking strategies and rejects double release") {
        cmeta_mutex_t mutex = NULL;
        cmeta_rwlock_t rw = NULL;
        cmeta_ace_guard guard = {0};
        cmeta_ace_lockable missing = cmeta_ace_lockable_bind(NULL, NULL);
        cmeta_mutex_init(&mutex);
        check_equal(cmeta_rwlock_init(&rw), 0);
        cmeta_ace_lockable lock = ace_sync_mutex_policy_as_cmeta_ace_lockable(&mutex);
        const cmeta_interface_desc *lock_desc = cmeta_ace_lockable_interface();
        check_true(cmeta_interface_desc_valid(lock_desc));
        check_equal(lock_desc->method_count, 2u);
        check_true(cmeta_interface_method_reflection_valid(&lock_desc->methods[0]));
        check_true(cmeta_interface_method_reflection_valid(&lock_desc->methods[1]));
        check_equal(cmeta_ace_guard_enter(&guard, &missing), CMETA_INVALID_ARGUMENT);
        check_equal(cmeta_ace_guard_enter(&guard, &lock), CMETA_OK);
        check_equal(cmeta_ace_guard_enter(&guard, &lock), CMETA_BUSY);
        cmeta_ace_guard copied = guard;
        check_equal(cmeta_ace_guard_leave(&copied), CMETA_INVALID_ARGUMENT);
        check_equal(cmeta_ace_guard_leave(&guard), CMETA_OK);
        check_equal(cmeta_ace_guard_leave(&guard), CMETA_INVALID_ARGUMENT);
        lock = ace_sync_rw_policy_as_cmeta_ace_lockable(&rw);
        check_equal(cmeta_ace_guard_enter(&guard, &lock), CMETA_OK);
        check_equal(cmeta_ace_guard_leave(&guard), CMETA_OK);
        cmeta_rwlock_destroy(&rw);
        cmeta_mutex_destroy(&mutex);
    }

    it("releases nested Scoped Locking guards in reverse order on failure") {
        cmeta_mutex_t outer_mutex = NULL, inner_mutex = NULL;
        cmeta_mutex_init(&outer_mutex);
        cmeta_mutex_init(&inner_mutex);
        cmeta_ace_lockable outer =
            ace_sync_mutex_policy_as_cmeta_ace_lockable(&outer_mutex);
        cmeta_ace_lockable inner =
            ace_sync_mutex_policy_as_cmeta_ace_lockable(&inner_mutex);
        cmeta_ace_guard outer_guard = {0}, inner_guard = {0};
        check_equal(cmeta_ace_guard_enter(&outer_guard, &outer), CMETA_OK);
        /* Inner typed public gate must release on failed body while outer
         * remains held; this is deliberately not a recursive acquisition. */
        ace_sync_counter counter = {inner, 0, 0u};
        ace_sync_operation operation = {&counter, 7, 55};
        check_equal(ace_sync_counter_gate_run(&inner, &operation,
                                              ace_sync_fail_body),
                    CMETA_CALLBACK_ERROR);
        check_equal(operation.result, 55);
        check_equal(cmeta_ace_guard_leave(&outer_guard), CMETA_OK);
        check_equal(cmeta_ace_guard_leave(&inner_guard), CMETA_INVALID_ARGUMENT);
        /* A later operation must be able to reacquire both native locks. */
        check_equal(cmeta_ace_guard_enter(&outer_guard, &outer), CMETA_OK);
        check_equal(cmeta_ace_guard_enter(&inner_guard, &inner), CMETA_OK);
        check_equal(cmeta_ace_guard_leave(&inner_guard), CMETA_OK);
        check_equal(cmeta_ace_guard_leave(&outer_guard), CMETA_OK);
        cmeta_mutex_destroy(&inner_mutex);
        cmeta_mutex_destroy(&outer_mutex);
    }

    it("serializes concurrent public Interface calls with a single private gate") {
        cmeta_mutex_t mutex = NULL;
        cmeta_rwlock_t rw = NULL;
        cmeta_thread_t threads[4] = {0};
        ace_sync_worker workers[4] = {0};
        ace_sync_counter counter = {0};
        cmeta_mutex_init(&mutex);
        counter.policy = ace_sync_mutex_policy_as_cmeta_ace_lockable(&mutex);
        ace_sync_counter_port port = ace_sync_counter_impl_as_ace_sync_counter_port(&counter);
        for (size_t i = 0; i < 4u; ++i) {
            workers[i].counter = port;
            check_equal(cmeta_thread_create(&threads[i], ace_sync_counter_worker,
                                             &workers[i]), 0);
        }
        for (size_t i = 0; i < 4u; ++i) {
            check_equal(cmeta_thread_join(&threads[i]), 0);
            check_equal(workers[i].failures, 0);
        }
        const cmeta_interface_desc *counter_contract =
            ace_sync_counter_port_interface();
        check_true(cmeta_interface_desc_valid(counter_contract));
        for (size_t method = 0u; method < counter_contract->method_count; ++method)
            check_true(cmeta_interface_method_reflection_valid(
                &counter_contract->methods[method]));
        check_equal(ace_sync_counter_port_get(&port), 1000);
        check_equal(counter.private_calls, 1000u);
        /* Policy switch is legal only when all concurrent callers are joined. */
        check_equal(cmeta_rwlock_init(&rw), 0);
        counter.policy = ace_sync_rw_policy_as_cmeta_ace_lockable(&rw);
        check_equal(ace_sync_counter_port_add(&port, 2), CMETA_OK);
        check_equal(ace_sync_counter_port_get(&port), 1004);
        cmeta_rwlock_destroy(&rw);
        cmeta_mutex_destroy(&mutex);
    }

    it("distinguishes owner-affine Interface from synchronized Thread-Safe Interface") {
        ace_sync_owner_resource resource = {cmeta_thread_current_token(), 0};
        ace_sync_owner_port port =
            ace_sync_owner_impl_as_ace_sync_owner_port(&resource);
        ace_sync_wrong_owner_worker foreign = {port, CMETA_OK};
        cmeta_thread_t thread = NULL;
        const cmeta_interface_desc *contract = ace_sync_owner_port_interface();
        check_true(cmeta_interface_desc_valid(contract));
        check_equal(contract->method_count, 1u);
        check_true(cmeta_interface_method_reflection_valid(
            &contract->methods[0]));
        check_true(resource.owner_token != NULL);
        check_equal(ace_sync_owner_port_increment(&port, 3), CMETA_OK);
        check_equal(resource.value, 3);

        /* Non-owner admission fails without acquiring a lock, changing
         * the value, or silently redirecting the owner-affine operation. */
        check_equal(cmeta_thread_create(&thread, ace_sync_wrong_owner_run, &foreign), 0);
        check_equal(cmeta_thread_join(&thread), 0);
        check_equal(foreign.status, CMETA_BUSY);
        check_equal(resource.value, 3);
        check_equal(ace_sync_owner_port_increment(&port, 2), CMETA_OK);
        check_equal(resource.value, 5);

        ace_sync_owner_resource unowned = {0};
        ace_sync_owner_port invalid =
            ace_sync_owner_impl_as_ace_sync_owner_port(&unowned);
        check_equal(ace_sync_owner_port_increment(&invalid, 1),
                    CMETA_INVALID_ARGUMENT);
        check_equal(unowned.value, 0);
    }

    it("runs a real Monitor Object with bounded predicate and terminal close") {
        ace_sync_monitor monitor;
        ace_sync_monitor_init(&monitor);
        ace_sync_monitor_port port =
            ace_sync_monitor_impl_as_ace_sync_monitor_port(&monitor);
        const cmeta_interface_desc *monitor_contract =
            ace_sync_monitor_port_interface();
        check_true(cmeta_interface_desc_valid(monitor_contract));
        for (size_t method = 0u; method < monitor_contract->method_count; ++method)
            check_true(cmeta_interface_method_reflection_valid(
                &monitor_contract->methods[method]));
        int value = -1;
        check_equal(ace_sync_monitor_port_take(&port, NULL), CMETA_INVALID_ARGUMENT);
        check_equal(ace_sync_monitor_port_put(&port, 42), CMETA_OK);
        check_equal(ace_sync_monitor_port_take(&port, &value), CMETA_OK);
        check_equal(value, 42);
        ace_sync_monitor_port_close(&port);
        check_equal(ace_sync_monitor_port_put(&port, 43), CMETA_BUSY);
        check_equal(ace_sync_monitor_port_take(&port, &value), CMETA_BUSY);
        ace_sync_monitor_destroy(&monitor);
    }

    it("wakes an actual blocked Monitor waiter before resource destruction") {
        ace_sync_monitor monitor;
        ace_sync_monitor_init(&monitor);
        ace_sync_monitor_port port =
            ace_sync_monitor_impl_as_ace_sync_monitor_port(&monitor);
        ace_sync_consumer consumer = {port, CMETA_OK, -1};
        cmeta_thread_t thread = NULL;
        bool observed_waiter = false;
        check_equal(cmeta_thread_create(&thread, ace_sync_monitor_worker,
                                        &consumer), 0);
        for (unsigned attempt = 0u; attempt < 500u; ++attempt) {
            cmeta_mutex_lock(&monitor.mutex);
            observed_waiter = monitor.waiters != 0u;
            cmeta_mutex_unlock(&monitor.mutex);
            if (observed_waiter) break;
            cmeta_sleep_ms(1u);
        }
        /* Even if the worker has not reached wait, close must reject it. */
        ace_sync_monitor_port_close(&port);
        check_equal(cmeta_thread_join(&thread), 0);
        check_true(observed_waiter);
        check_equal(consumer.status, CMETA_BUSY);
        check_equal(consumer.value, -1);
        check_equal(monitor.waiters, 0u);
        ace_sync_monitor_destroy(&monitor);
    }


    it("rechecks full predicate after Monitor broadcast and drains blocked producer") {
        ace_sync_monitor monitor;
        ace_sync_monitor_init(&monitor);
        ace_sync_monitor_port port =
            ace_sync_monitor_impl_as_ace_sync_monitor_port(&monitor);
        check_equal(ace_sync_monitor_port_put(&port, 11), CMETA_OK);
        ace_sync_producer producer = {port, CMETA_BUSY, 22};
        cmeta_thread_t thread = NULL;
        bool waiting = false;
        check_equal(cmeta_thread_create(&thread, ace_sync_producer_worker,
                                        &producer), 0);
        for (unsigned i = 0u; i < 800u; ++i) {
            cmeta_mutex_lock(&monitor.mutex);
            waiting = monitor.waiters != 0u;
            cmeta_mutex_unlock(&monitor.mutex);
            if (waiting) break;
            cmeta_sleep_ms(1u);
        }
        cmeta_mutex_lock(&monitor.mutex);
        cmeta_cond_broadcast(&monitor.changed); /* Predicate remains false. */
        bool still_full = monitor.full;
        int initial = monitor.value;
        cmeta_mutex_unlock(&monitor.mutex);
        int first = 0, second = 0;
        check_equal(ace_sync_monitor_port_take(&port, &first), CMETA_OK);
        check_equal(cmeta_thread_join(&thread), 0);
        check_true(waiting);
        check_true(still_full);
        check_equal(initial, 11);
        check_equal(producer.status, CMETA_OK);
        check_equal(first, 11);
        check_equal(ace_sync_monitor_port_take(&port, &second), CMETA_OK);
        check_equal(second, 22);
        ace_sync_monitor_port_close(&port);
        ace_sync_monitor_destroy(&monitor);
    }

    it("rejects a blocked Monitor producer on close without losing prior value") {
        ace_sync_monitor monitor;
        ace_sync_monitor_init(&monitor);
        ace_sync_monitor_port port =
            ace_sync_monitor_impl_as_ace_sync_monitor_port(&monitor);
        check_equal(ace_sync_monitor_port_put(&port, 30), CMETA_OK);
        ace_sync_producer producer = {port, CMETA_OK, 40};
        cmeta_thread_t thread = NULL;
        bool waiting = false;
        check_equal(cmeta_thread_create(&thread, ace_sync_producer_worker,
                                        &producer), 0);
        for (unsigned i = 0u; i < 800u; ++i) {
            cmeta_mutex_lock(&monitor.mutex);
            waiting = monitor.waiters != 0u;
            cmeta_mutex_unlock(&monitor.mutex);
            if (waiting) break;
            cmeta_sleep_ms(1u);
        }
        ace_sync_monitor_port_close(&port);
        check_equal(cmeta_thread_join(&thread), 0);
        check_true(waiting);
        check_equal(producer.status, CMETA_BUSY);
        int value = 0;
        check_equal(ace_sync_monitor_port_take(&port, &value), CMETA_OK);
        check_equal(value, 30);
        check_equal(ace_sync_monitor_port_take(&port, &value), CMETA_BUSY);
        check_equal(monitor.waiters, 0u);
        ace_sync_monitor_destroy(&monitor);
    }

    it("releases Thread-Safe Interface guard after a failed private body") {
        cmeta_mutex_t mutex = NULL;
        cmeta_mutex_init(&mutex);
        cmeta_ace_lockable lock = ace_sync_mutex_policy_as_cmeta_ace_lockable(&mutex);
        ace_sync_counter counter = {lock, 0, 0u};
        ace_sync_operation operation = {&counter, 3, 0};
        check_equal(ace_sync_counter_gate_run(&lock, &operation,
            ace_sync_fail_body), CMETA_CALLBACK_ERROR);
        check_equal(ace_sync_counter_gate_run(&lock, &operation,
            ace_sync_add_body), CMETA_OK);
        check_equal(counter.value, 6);
        cmeta_mutex_destroy(&mutex);
    }

#ifdef __cplusplus
    it("releases the typed public guard when a C++ body throws") {
        cmeta_mutex_t mutex = NULL;
        cmeta_mutex_init(&mutex);
        cmeta_ace_lockable lock = ace_sync_mutex_policy_as_cmeta_ace_lockable(&mutex);
        ace_sync_counter counter = {lock, 0, 0u};
        ace_sync_operation operation = {&counter, 1, 0};
        bool caught = false;
        try { (void)ace_sync_counter_gate_run(&lock, &operation, ace_sync_throws); }
        catch (int value) { caught = value == 7; }
        check_true(caught);
        check_equal(ace_sync_counter_gate_run(&lock, &operation,
                                               ace_sync_add_body), CMETA_OK);
        check_equal(counter.value, 2);
        cmeta_mutex_destroy(&mutex);
    }
#endif
}

#undef ACE_SYNC_CAST
#endif /* CMETA_ACE_SYNC_CASES_H */
