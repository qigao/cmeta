#ifndef CNET_RESEARCH_DMA_COMPLETION_GATE_H
#define CNET_RESEARCH_DMA_COMPLETION_GATE_H

/* Experimental, non-installed DMA-like completion handoff. It is NOT a
 * hardware NIC completion API; the host still owns PMD and IOMMU teardown. */
#include "packet_io.h"
#include "r2_mux.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define R2_DMA_GATE_MAX_TICKETS 32u

typedef enum r2_dma_status {
  R2_DMA_OK = 0,
  R2_DMA_INVALID,
  R2_DMA_WRONG_OWNER,
  R2_DMA_STALE,
  R2_DMA_FULL,
  R2_DMA_DUPLICATE,
  R2_DMA_BUSY,
  R2_DMA_SEALED,
  R2_DMA_OWNER_ERROR
} r2_dma_status;

typedef struct r2_dma_ticket {
  uint64_t generation; /* completion gate generation */
  uint64_t serial;     /* never reused during gate lifetime */
  uint8_t slot;
} r2_dma_ticket;

typedef enum r2_dma_slot_state {
  R2_DMA_FREE = 0,
  R2_DMA_ARMED,
  R2_DMA_POSTED,
  R2_DMA_RECLAIMING
} r2_dma_slot_state;

typedef struct r2_dma_slot {
  r2_mux_handle handle; /* copied generation-safe mux handle, never raw state */
  packet_io_release_fn finalizer;
  void *finalizer_context;
  uint64_t finalizer_tag;
  uint64_t serial;
  r2_dma_slot_state state;
} r2_dma_slot;

typedef struct r2_dma_gate {
  pthread_mutex_t mutex;
  pthread_t owner;
  r2_mux *mux; /* borrowed: must outlive the gate and final frame drain */
  r2_dma_slot slots[R2_DMA_GATE_MAX_TICKETS];
  uint64_t generation, next_serial;
  size_t capacity, active, posted;
  bool initialized, running, sealed, producers_fenced, polling;
} r2_dma_gate;

/* Caller supplies zero-initialized stable gate storage. The mutex is
 * initialized once, retained across generations, destroyed only AFTER all
 * external completion producers have been joined. No hidden worker. */
r2_dma_status r2_dma_gate_start(r2_dma_gate *gate, r2_mux *mux,
                                uint64_t generation, size_t capacity);

/* Owner-only two-stage admission. Reserve completion record AND pin the
 * mux's physical frame reference before allowing a PMD to use the frame.
 * If Packet I/O admission fails, rollback the still-ARMED ticket explicitly;
 * the original frame still belongs to the caller. */
r2_dma_status r2_dma_gate_arm(r2_dma_gate *gate, r2_mux_handle handle,
                             packet_io_release_fn finalizer, void *context,
                             uint64_t finalizer_tag, r2_dma_ticket *ticket);
r2_dma_status r2_dma_gate_rollback(r2_dma_gate *gate, r2_dma_ticket ticket);

/* Only cross-thread entrypoint. A producer has already established that the
 * backend will no longer use the retained frame. Posting NEVER invokes the
 * finalizer, accesses the R2 mux, frees the frame or settles a protocol ACK.
 * A reserved slot guarantees posting capacity even after the gate is sealed.
 * The caller must keep gate storage alive until all producer threads join. */
r2_dma_status r2_dma_gate_post(r2_dma_gate *gate, r2_dma_ticket ticket);

/* Owner-only: consume up to 'budget' posted tickets; call mux unpin and then
 * exactly one finalizer on this Owner. Reentrant owner API use is rejected.
 * A post is one-shot and cannot be replayed after slot/generation reuse. */
r2_dma_status r2_dma_gate_poll(r2_dma_gate *gate, size_t budget,
                               size_t *out_finalized);
r2_dma_status r2_dma_gate_seal(r2_dma_gate *gate);

/* The host MUST stop/join all off-Owner producers before asserting this
 * barrier; the gate cannot infer a vendor driver's DMA quiescence. */
r2_dma_status r2_dma_gate_fence_joined_producers(r2_dma_gate *gate);
r2_dma_status r2_dma_gate_drain(r2_dma_gate *gate);
r2_dma_status r2_dma_gate_stop(r2_dma_gate *gate);
r2_dma_status r2_dma_gate_destroy(r2_dma_gate *gate);

#endif
