#include "cmeta_trace_fixture.h"
#include <cmeta/manifest.h>
#include "tinytest.h"
#include <thread.h>

enum { TRACE_WORKERS = 4, TRACE_ROUNDS = 10000, TRACE_REQUEST = 42, TRACE_STATUS = 200 };
cmeta_tracepoint(http_request, cmeta_field(uint64_t, request_id) cmeta_field(int, status));
cmeta_tracepoint(unbound_request, cmeta_field(int, status));
cmeta_tracepoint(default_request, cmeta_field(unsigned, count));
cmeta_tracepoint(effect_request, cmeta_field(unsigned, count));
SALTS_FAST_KEY(alloc_fail, false);
SALTS_FAST_KEY(default_fault, false);
cmeta_registry(trace_manifest,
    cmeta_manifest_entry("http_request", CMETA_MANIFEST_TRACEPOINT,
        StructMeta(http_request_payload), UINT64_C(0), UINT32_C(0))
);
static atomic_uint backend_calls;
static atomic_uint alternate_calls;
static atomic_uint payload_errors;
static unsigned observed_effect;
static cmeta_status callback_disable_status;
static void capture_effect(const effect_request_payload *event) {
    observed_effect = event->count;
}
static void capture_request(const http_request_payload *event) {
    if (event->request_id != TRACE_REQUEST || event->status != TRACE_STATUS)
        atomic_fetch_add(&payload_errors, 1u);
    atomic_fetch_add(&backend_calls, 1u);
}
static void alternate_request(const http_request_payload *event) {
    if (event->request_id != TRACE_REQUEST || event->status != TRACE_STATUS)
        atomic_fetch_add(&payload_errors, 1u);
    atomic_fetch_add(&alternate_calls, 1u);
}
static void capture_and_disable(const http_request_payload *event) {
    capture_request(event);
    callback_disable_status = cmeta_trace_disable(http_request);
}
typedef struct trace_worker_state { atomic_uint errors; atomic_uint fault_hits; } trace_worker_state;
static void trace_writer(void *arg) {
    trace_worker_state *state = (trace_worker_state *)arg;
    for (int i = 0; i < TRACE_ROUNDS; ++i) {
        cmeta_status status = i % 2 == 0 ? cmeta_trace_bind(http_request, capture_request)
                                        : cmeta_trace_bind(http_request, alternate_request);
        if (status != CMETA_OK) atomic_fetch_add(&state->errors, 1u);
    }
}
static void trace_reader(void *arg) {
    trace_worker_state *state = (trace_worker_state *)arg;
    for (int i = 0; i < TRACE_ROUNDS; ++i) {
        cmeta_trace_emit(http_request, (uint64_t)TRACE_REQUEST, TRACE_STATUS);
        if (cmeta_fast_key_consume(&alloc_fail)) atomic_fetch_add(&state->fault_hits, 1u);
    }
}

suite("CMeta typed trace and deterministic fault points") {
    before_each() {
        check_equal(cmeta_trace_disable(http_request), CMETA_OK);
        check_equal(cmeta_trace_disable(unbound_request), CMETA_OK);
        check_equal(cmeta_trace_disable(effect_request), CMETA_OK);
        check_equal(cmeta_fast_disable(&alloc_fail), SALTS_OK);
        atomic_store(&backend_calls, 0u);
        atomic_store(&alternate_calls, 0u);
        atomic_store(&payload_errors, 0u);
    }
    it("starts new trace and fault points disabled") {
        unsigned effects = 0u;
        check_false(cmeta_fast_branch(&default_request_key));
        cmeta_trace_emit(default_request, ++effects);
        check_equal(effects, 0u);
        check_false(cmeta_fast_key_consume(&default_fault));
    }
    it("does not evaluate arguments while disabled and exposes canonical payload layout") {
        unsigned effects = 0u;
        cmeta_trace_emit(http_request, ++effects, 0);
        check_equal(effects, 0u);
        check_equal(cmeta_trace_bind(http_request, capture_request), CMETA_OK);
        cmeta_trace_emit(http_request, 0u, ++effects);
        check_equal(effects, 0u);
        const cmeta_struct_desc *meta = StructMeta(http_request_payload);
        check_equal(meta->field_count, (size_t)2u);
        check_equal(meta->fields[0].name, "request_id");
        check_equal(meta->fields[0].offset, offsetof(http_request_payload, request_id));
        check_equal(meta->fields[0].size, sizeof(uint64_t));
        check_equal(meta->fields[1].name, "status");
        check_true(cmeta_type_equal(meta->fields[1].type, &cmeta_type_int));
        check_equal(trace_manifest.count, (size_t)1u);
        check_equal(trace_manifest.entries[0].kind, CMETA_MANIFEST_TRACEPOINT);
        check_equal(trace_manifest.entries[0].descriptor, (const void *)meta);
    }
    it("evaluates each enabled payload value exactly once") {
        unsigned effects = 0u;
        observed_effect = 0u;
        check_equal(cmeta_trace_bind(effect_request, capture_effect), CMETA_OK);
        check_equal(cmeta_trace_enable(effect_request), CMETA_OK);
        cmeta_trace_emit(effect_request, ++effects);
        check_equal(effects, 1u);
        check_equal(observed_effect, 1u);
        check_equal(cmeta_trace_disable(effect_request), CMETA_OK);
    }
    it("fails fast when enabling an unbound point and skips unbound payload arguments") {
        unsigned effects = 0u;
        check_equal(cmeta_trace_enable(unbound_request), CMETA_INVALID_ARGUMENT);
        check_equal(cmeta_fast_enable(&unbound_request_key), SALTS_OK);
        cmeta_trace_emit(unbound_request, ++effects);
        check_equal(effects, 0u);
        check_equal(cmeta_trace_disable(unbound_request), CMETA_OK);
    }
    it("dispatches exact typed payloads and replaces backends without reflection") {
        check_equal(cmeta_trace_bind(http_request, capture_request), CMETA_OK);
        check_equal(http_request_bind(NULL), CMETA_INVALID_ARGUMENT);
        check_equal(cmeta_trace_enable(http_request), CMETA_OK);
        cmeta_trace_emit(http_request, (uint64_t)TRACE_REQUEST, TRACE_STATUS);
        check_equal(atomic_load(&backend_calls), 1u);
        check_equal(cmeta_trace_bind(http_request, alternate_request), CMETA_OK);
        cmeta_trace_emit(http_request, (uint64_t)TRACE_REQUEST, TRACE_STATUS);
        check_equal(atomic_load(&backend_calls), 1u);
        check_equal(atomic_load(&alternate_calls), 1u);
        check_equal(atomic_load(&payload_errors), 0u);
        check_equal(cmeta_trace_disable(http_request), CMETA_OK);
        cmeta_trace_emit(http_request, (uint64_t)TRACE_REQUEST, TRACE_STATUS);
        check_equal(atomic_load(&alternate_calls), 1u);
    }
    it("allows a synchronous backend to close its own point") {
        unsigned effects = 0u;
        check_equal(cmeta_trace_bind(http_request, capture_and_disable), CMETA_OK);
        check_equal(cmeta_trace_enable(http_request), CMETA_OK);
        cmeta_trace_emit(http_request, (uint64_t)TRACE_REQUEST, TRACE_STATUS);
        check_equal(callback_disable_status, CMETA_OK);
        check_false(cmeta_fast_branch(&http_request_key));
        cmeta_trace_emit(http_request, ++effects, 0);
        check_equal(effects, 0u);
        check_equal(atomic_load(&backend_calls), 1u);
        check_equal(atomic_load(&payload_errors), 0u);
    }
    it("preserves normal behavior and consumes each armed fault once") {
        check_false(cmeta_fast_key_consume(&alloc_fail));
        check_equal(cmeta_fast_enable(&alloc_fail), SALTS_OK);
        check_equal(cmeta_fast_enable(&alloc_fail), SALTS_OK);
        check_true(cmeta_fast_key_consume(&alloc_fail));
        check_false(cmeta_fast_key_consume(&alloc_fail));
        check_equal(cmeta_fast_enable(&alloc_fail), SALTS_OK);
        check_true(cmeta_fast_key_consume(&alloc_fail));
        check_false(cmeta_fast_key_consume(&alloc_fail));
        check_equal(cmeta_fast_enable(&alloc_fail), SALTS_OK);
        check_equal(cmeta_fast_disable(&alloc_fail), SALTS_OK);
        check_false(cmeta_fast_key_consume(&alloc_fail));
    }
    it("publishes backends to concurrent readers and consumes one fault across readers") {
        trace_worker_state state = {0};
        cmeta_thread_t threads[TRACE_WORKERS] = {NULL};
        int created = 0, joined = 0;
        check_equal(cmeta_trace_bind(http_request, capture_request), CMETA_OK);
        check_equal(cmeta_trace_enable(http_request), CMETA_OK);
        check_equal(cmeta_fast_enable(&alloc_fail), SALTS_OK);
        for (int i = 0; i < TRACE_WORKERS; ++i) {
            if (cmeta_thread_create(&threads[i], i % 2 == 0 ? trace_writer : trace_reader, &state) != 0) break;
            ++created;
        }
        for (int i = 0; i < created; ++i) {
            if (cmeta_thread_join(&threads[i]) == 0) ++joined;
            cmeta_thread_destroy(&threads[i]);
        }
        check_equal(created, TRACE_WORKERS);
        check_equal(joined, TRACE_WORKERS);
        check_equal(atomic_load(&state.errors), 0u);
        check_equal(atomic_load(&payload_errors), 0u);
        check_equal(atomic_load(&state.fault_hits), 1u);
        check_equal(atomic_load(&backend_calls) + atomic_load(&alternate_calls),
                    (unsigned)(TRACE_WORKERS / 2) * TRACE_ROUNDS);
    }
}
