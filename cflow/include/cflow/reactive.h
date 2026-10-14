#ifndef CFLOW_REACTIVE_H
#define CFLOW_REACTIVE_H

#include <cflow/graph.h>
#include <cflow/backend.h>
#include <cflow/scheduler.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

Enum(cflow_step_kind,
    (CFLOW_STEP_VALUE,          "value"),
    (CFLOW_STEP_VALUE_AND_DONE, "value_and_done"),
    (CFLOW_STEP_WAIT,           "wait"),
    (CFLOW_STEP_DONE,           "done"),
    (CFLOW_STEP_ERROR,          "error")
);

typedef struct cflow_waker {
    void (*wake)(void *user);
    void *user;
} cflow_waker;

extern const cmeta_type_desc cflow_type_waker;

#define CMETA_WAITABLE_METHODS(X,I) \
    X(I,FR1,bool,arm,stateful, \
      &cmeta_type_bool,CMETA_ABI_SCALAR,CMETA_RESULT_VALUE, \
      (cflow_waker,waker,CMETA_PARAM_IN, \
       &cflow_type_waker,CMETA_ABI_AGGREGATE)) \
    X(I,FV0,void,cancel,stateful, \
      &cmeta_type_void,CMETA_ABI_VOID)
CMETA_INTERFACE(cflow_waitable, CMETA_WAITABLE_METHODS);

typedef struct cflow_step {
    cflow_step_kind kind;
    cflow_waitable waitable;
    const char *error;
} cflow_step;

extern const cmeta_type_desc cflow_type_step;

typedef struct cflow_publish_context {
    cflow_scheduler *scheduler;
    /**
     * Exact outstanding downstream-value demand immediately before the
     * Subscription invokes resume. Zero means no Subscription snapshot was
     * supplied by a direct caller.
     */
    size_t downstream_demand;
} cflow_publish_context;

extern const cmeta_type_desc cflow_type_publish_context;
extern const cmeta_type_desc cflow_type_publish_context_ptr;

typedef struct cflow_resumable_ops {
    /* resume receives empty output storage and constructs a live value only
     * for VALUE or VALUE_AND_DONE. All other steps leave it empty. */
    cflow_step (*resume)(void *state, cflow_publish_context *ctx, void *out_value);
    void (*cancel)(void *state);
    void (*destroy)(void *state);
} cflow_resumable_ops;

typedef struct cflow_resumable {
    const char *name;
    const cmeta_type_desc *output_type;
    const cflow_resumable_ops *ops;
    void *state;
} cflow_resumable;

Enum(cflow_publisher_terminal,
    (CFLOW_PUBLISHER_OPEN,  "open"),
    (CFLOW_PUBLISHER_DONE,  "done"),
    (CFLOW_PUBLISHER_ERROR, "error")
);

extern const cmeta_type_desc cflow_type_publisher_terminal;

#define CFLOW_PUBLISHER_METHODS(X,I) \
    X(I,FR0,const char *,name,value, \
      &cmeta_type_char_ptr,CMETA_ABI_OBJECT_POINTER, \
      CMETA_RESULT_BORROWED) \
    X(I,FR0,const cmeta_type_desc *,output_type,value, \
      &cmeta_type_descriptor_ptr,CMETA_ABI_OBJECT_POINTER, \
      CMETA_RESULT_BORROWED) \
    X(I,FR2,cflow_step,resume,stateful, \
      &cflow_type_step,CMETA_ABI_AGGREGATE,CMETA_RESULT_VALUE, \
      (cflow_publish_context *,ctx, \
       CMETA_PARAM_IN | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE, \
       &cflow_type_publish_context_ptr,CMETA_ABI_OBJECT_POINTER), \
      (void *,out_value,CMETA_PARAM_OUT | CMETA_PARAM_BORROWED, \
       &cmeta_type_void_ptr,CMETA_ABI_OBJECT_POINTER)) \
    X(I,FV0,void,cancel,stateful, \
      &cmeta_type_void,CMETA_ABI_VOID) \
    X(I,FD0,void,destroy,stateful, \
      &cmeta_type_void,CMETA_ABI_VOID) \
    X(I,FV1,void,bind_terminal_waker,stateful, \
      &cmeta_type_void,CMETA_ABI_VOID, \
      (cflow_waker,waker,CMETA_PARAM_IN, \
       &cflow_type_waker,CMETA_ABI_AGGREGATE)) \
    X(I,FR1,cflow_publisher_terminal,poll_terminal,stateful, \
      &cflow_type_publisher_terminal,CMETA_ABI_ENUM,CMETA_RESULT_VALUE, \
      (const char **,error, \
       CMETA_PARAM_OUT | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE, \
       &cmeta_type_char_ptr_ptr,CMETA_ABI_OBJECT_POINTER))
CMETA_INTERFACE(cflow_publisher, CFLOW_PUBLISHER_METHODS);

enum {
    /* resume() constructs a live value in empty out_value storage only when it
     * returns VALUE or VALUE_AND_DONE. Other results leave storage empty. */
    CFLOW_PUBLISHER_CAP_CONSTRUCTS_VALUES = UINT64_C(1) << 0
};

/* Move a Publisher interface into a generic resumable state machine. */
bool cflow_resumable_from_publisher(cflow_resumable *out,
                                    cflow_publisher *publisher);

/* value is borrowed and remains live only until the callback returns. */
typedef bool (*cflow_value_fn)(void *user,
                               const cmeta_type_desc *type,
                               const void *value);
typedef void (*cflow_error_fn)(void *user, const char *message);
typedef void (*cflow_done_fn)(void *user);

#define CMETA_SUBSCRIBER_METHODS(X,I) \
    X(I,FR2,bool,value,stateful, \
      &cmeta_type_bool,CMETA_ABI_SCALAR,CMETA_RESULT_VALUE, \
      (const cmeta_type_desc *,type, \
       CMETA_PARAM_IN | CMETA_PARAM_BORROWED, \
       &cmeta_type_descriptor_ptr,CMETA_ABI_OBJECT_POINTER), \
      (const void *,value,CMETA_PARAM_IN | CMETA_PARAM_BORROWED, \
       &cmeta_type_void_ptr,CMETA_ABI_OBJECT_POINTER)) \
    X(I,FV1,void,error,stateful, \
      &cmeta_type_void,CMETA_ABI_VOID, \
      (const char *,message,CMETA_PARAM_IN | CMETA_PARAM_BORROWED, \
       &cmeta_type_char_ptr,CMETA_ABI_OBJECT_POINTER)) \
    X(I,FV0,void,done,stateful, \
      &cmeta_type_void,CMETA_ABI_VOID)
CMETA_INTERFACE(cflow_subscriber, CMETA_SUBSCRIBER_METHODS);

typedef struct cflow_subscriber_callbacks {
    cflow_value_fn on_value;
    cflow_error_fn on_error;
    cflow_done_fn on_done;
    void *user;
} cflow_subscriber_callbacks;

cflow_subscriber cflow_subscriber_from_callbacks(cflow_subscriber_callbacks *callbacks);

typedef struct cflow_subscription {
    void *impl;
} cflow_subscription;

/* Move-style ownership: subscription must be zero-initialized. Reopening a
 * live Subscription fails without changing either owner. On success the
 * Subscription takes publisher and clears *publisher.
 * Graph, Scheduler, and Subscriber are borrowed. Admission failure leaves
 * Publisher ownership
 * with the caller. Interpreted graphs accept managed COPY/MOVE/DESTROY values
 * from a CONSTRUCTS_VALUES Publisher. Sequential materialized Plans use the
 * same lifecycle admission; fused and parallel Plan paths remain trivial-only. */
bool cflow_subscribe(cflow_subscription *subscription,
                    const cflow_graph *graph,
                    cflow_publisher *publisher,
                    cflow_scheduler *scheduler,
                    const cflow_subscriber *subscriber);
/* Structured admission variant. The options value and interface tables are
 * borrowed; their function pointers and contexts reachable through them must
 * remain valid until cflow_subscription_close(). Missing state capability is reported
 * as UNSUPPORTED before Publisher ownership moves or any value is pulled. */
cflow_status_result cflow_subscribe_with_options(
    cflow_subscription *subscription,
    const cflow_graph *graph,
    cflow_publisher *publisher,
    cflow_scheduler *scheduler,
    const cflow_subscriber *subscriber,
    const cflow_eval_options *options);

/* Execute a specific immutable subgraph from the same Graph-wide IR table.
 * Primarily used by Subflow composition; public to keep Subscription/Subflow
 * ownership symmetric. */
bool cflow_subscribe_subgraph(cflow_subscription *subscription,
                             const cflow_graph *graph,
                             cflow_subgraph_id subgraph,
                             cflow_publisher *publisher,
                             cflow_scheduler *scheduler,
                             const cflow_subscriber *subscriber);
/* Subgraph form of cflow_subscribe_with_options(). */
cflow_status_result cflow_subscribe_subgraph_with_options(
    cflow_subscription *subscription,
    const cflow_graph *graph,
    cflow_subgraph_id subgraph,
    cflow_publisher *publisher,
    cflow_scheduler *scheduler,
    const cflow_subscriber *subscriber,
    const cflow_eval_options *options);

/* Demand is always downstream-value demand, never Publisher-item demand.
 * On a live Subscription, publishing demand under the Run lock accepts this
 * request. If another Worker finishes it before the caller's pump scheduling
 * check, this request still returns OK; a NEW request after terminal returns
 * CLOSED. Immediate Scheduler admission failures retain saturated demand
 * and keep their exact error status for a later explicit wake/request.
 * Cancellation, explicit close and Run error are never normalized to OK. */
cflow_status_result cflow_subscription_request_result(
    cflow_subscription *subscription, size_t n);
/* Compatibility facade returning true only when demand was accepted without
 * an immediate admission or terminal error. */
bool cflow_subscription_request(cflow_subscription *subscription, size_t n);
void cflow_subscription_cancel(cflow_subscription *subscription);
/* External calls close synchronously. A call from this Subscription's
 * Subscriber callback
 * requests close and defers destruction until the active pump returns. */
void cflow_subscription_close(cflow_subscription *subscription);

bool cflow_subscription_is_done(const cflow_subscription *subscription);
bool cflow_subscription_is_cancelled(const cflow_subscription *subscription);
const char *cflow_subscription_error(const cflow_subscription *subscription);
/* Generic cross-layer category. Domain-specific diagnostic text remains
 * available through cflow_subscription_error(); a cancelled live Subscription
 * reports CANCELLED. */
cflow_status cflow_subscription_status(const cflow_subscription *subscription);
size_t cflow_subscription_outstanding_demand(
    const cflow_subscription *subscription);

/* Advanced integration hook: safe to call from arbitrary event-loop/driver callbacks. */
void cflow_subscription_wake(cflow_subscription *subscription);

#ifdef __cplusplus
}
#endif
#endif
