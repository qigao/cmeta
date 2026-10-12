#ifndef CNET_RESEARCH_R2_MUX_H
#define CNET_RESEARCH_R2_MUX_H

/* Research-only shared Owner for at most eight existing authenticated R2
 * sessions. The mux does NOT own the physical PMD or the MAC provider. */
#include "reliable.h"

#define R2_MUX_MAX_SESSIONS 8u
#define R2_MUX_MAX_BUDGET 64u
#define R2_MUX_MAX_PHYSICAL_REFS 16u

typedef enum r2_mux_status {
  R2_MUX_OK = 0,
  R2_MUX_INVALID,
  R2_MUX_WRONG_OWNER,
  R2_MUX_STALE,
  R2_MUX_BUSY,
  R2_MUX_SEALED,
  R2_MUX_FULL,
  R2_MUX_UNROUTABLE,
  R2_MUX_CHILD_ERROR
} r2_mux_status;

typedef struct r2_mux_handle {
  uint8_t slot;
  uint64_t generation; /* slot incarnation; independent of R2 session ticket */
} r2_mux_handle;

typedef struct r2_mux_slot {
  rl_session *session; /* external address-stable Owner-local session, borrowed */
  rl_ticket ticket;
  uint64_t generation; /* monotonic, never reset on reuse */
  uint32_t local_id, peer_id, epoch;
  size_t physical_refs; /* credits held until actual physical frame release */
  rl_result last_error;
  /* Saturating diagnostic counters; all are Owner-local and bounded. A grant
   * means an R2 packet was actually accepted, NOT that a peer ACK arrived. */
  uint64_t visits, grants, idle_visits, errors, denied_pins;
  size_t peak_physical_refs;
  bool active;
} r2_mux_slot;

typedef struct r2_mux {
  r2_mux_slot slots[R2_MUX_MAX_SESSIONS];
  pthread_t owner;
  uint64_t generation;
  uint64_t now_ms;
  uint64_t child_errors, unroutable;
  uint8_t capacity, frame_limit, cursor;
  size_t active_count;
  bool running, sealed, in_call;
} r2_mux;

/* Requires initially zeroed storage. Restart only after all slots drained
 * and stopped, with an increasing owner generation. No implicit fallback. */
r2_mux_status r2_mux_start(r2_mux *m, uint64_t generation,
                           uint8_t capacity, uint8_t per_session_frame_limit);
/* Attach a *live* R2 session whose MAC callback/provider was already bound.
 * Duplicate session pointers and (local,peer,epoch) identity are rejected.
 * Caller's session and credentials remain owned by their existing Owner. */
r2_mux_status r2_mux_attach(r2_mux *m, rl_session *session, rl_ticket ticket,
                            r2_mux_handle *out);
/* A successful detach demands protocol STOPPED and no physical frame refs.
 * The caller must independently release the MAC lease after protocol stop. */
r2_mux_status r2_mux_detach(r2_mux *m, r2_mux_handle handle);
/* Count an accepted physical frame exactly once, then decrement only from its
 * authoritative frame finalizer. Rejected admission must unpin immediately.
 * Each session has a fixed hard bound, even with a permissive PMD. */
r2_mux_status r2_mux_frame_pin(r2_mux *m, r2_mux_handle handle);
r2_mux_status r2_mux_frame_unpin(r2_mux *m, r2_mux_handle handle);
/* One R2 packet maximum per session per scheduling visit, strict round-robin.
 * A blocked neighbor never prevents a healthy neighbor's scheduling attempt.
 * A child error is reported but other peers are still advanced in this call.
 * All work stays on the calling Owner and uses caller-provided time/budget. */
r2_mux_status r2_mux_step(r2_mux *m, uint64_t now_ms,
                          size_t budget, size_t *out_emitted);
/* Demux by source, destination and epoch, then R2 performs real HMAC auth.
 * Identity header alone never confers authority or permits peer ACK terminal.
 * out_child receives the R2 auth/protocol result only for a matching slot. */
r2_mux_status r2_mux_input(r2_mux *m, const void *wire, size_t size,
                           uint64_t now_ms, rl_result *out_child);
r2_mux_status r2_mux_seal(r2_mux *m);
r2_mux_status r2_mux_drain(r2_mux *m);
r2_mux_status r2_mux_stop(r2_mux *m);

#endif
