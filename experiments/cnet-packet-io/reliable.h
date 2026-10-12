#ifndef CNET_RESEARCH_RELIABLE_H
#define CNET_RESEARCH_RELIABLE_H

/* Experimental research-only authenticated envelope: host owns key provision. No public ABI. */
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RL_MAX_FRAGMENTS 8u
#define RL_FRAGMENT_BYTES 64u
#define RL_MAX_MESSAGE_BYTES (RL_MAX_FRAGMENTS * RL_FRAGMENT_BYTES)
#define RL_WIRE_HEADER_BYTES 36u
#define RL_MAC_BYTES 32u
#define RL_KEY_BYTES 32u
#define RL_MAX_WIRE_BYTES (RL_WIRE_HEADER_BYTES + RL_FRAGMENT_BYTES + RL_MAC_BYTES)

typedef enum rl_result {
  RL_OK = 0,
  RL_INVALID,
  RL_FULL,
  RL_WRONG_OWNER,
  RL_STALE,
  RL_SEALED,
  RL_BUSY,
  RL_PROTOCOL,
  RL_EMIT_FAILED,
  RL_EXHAUSTED,
  RL_AUTH_FAILED,
  RL_CRYPTO
} rl_result;

typedef enum rl_terminal {
  RL_TERMINAL_ACKED = 1,
  RL_TERMINAL_CANCELLED,
  RL_TERMINAL_TIMEOUT,
  RL_TERMINAL_EMIT_FAILED
} rl_terminal;

/* The wire view is borrowed only during emit(). Return 1 if the entire frame
 * was accepted/copied, 0 on backpressure without ownership transfer, or -1
 * on fatal transport failure. This callback NEVER reports a peer ACK.
 * An underlying Packet I/O provider must copy or independently retain the
 * frame before returning 1. */
typedef int (*rl_emit_fn)(void *user, const void *bytes, size_t size);
typedef void (*rl_deliver_fn)(void *user, uint32_t message_id,
                              const void *bytes, size_t size);
typedef void (*rl_terminal_fn)(void *user, uint64_t tag, rl_terminal result);
/* External, vetted MAC provider; test fixture uses HMAC-SHA256. The provider
 * must compute a full 32-byte cryptographic MAC over exactly the supplied
 * complete header+payload and return false on any error. Verification and
 * constant-time comparison are performed by this protocol engine. */
typedef bool (*rl_mac_fn)(void *user, const uint8_t key[RL_KEY_BYTES],
                          const void *bytes, size_t size,
                          uint8_t out_mac[RL_MAC_BYTES]);

typedef struct rl_callbacks {
  rl_emit_fn emit;
  rl_deliver_fn on_deliver;
  rl_terminal_fn on_terminal;
  void *user;
  rl_mac_fn mac; /* required; there is no unauthenticated fallback */
} rl_callbacks;

typedef struct rl_config {
  uint32_t local_id;      /* unique participant address for this laboratory */
  uint32_t peer_id;
  uint32_t epoch;         /* chosen out of band; nonzero, no automatic rollover */
  uint32_t first_message_id;
  uint32_t rto_ms;
  uint32_t ack_delay_ms;
  uint32_t deadline_ms;
  uint8_t window;         /* 1..RL_MAX_FRAGMENTS, upper bound on cwnd */
  uint8_t max_retries;    /* extra transmissions per fragment, beyond first */
  /* Provision these independent directional keys by an authenticated host
   * authority and rotate for each fresh epoch. rl_start copies keys; caller
   * retains responsibility for erasing its own config. All-zero and identical
   * TX/RX keys are rejected. No key negotiation or derivation is implemented. */
  uint8_t tx_auth_key[RL_KEY_BYTES];
  uint8_t rx_auth_key[RL_KEY_BYTES];
} rl_config;

typedef struct rl_ticket { uint64_t generation; } rl_ticket;

typedef enum rl_state {
  RL_EMPTY = 0,
  RL_OPEN,
  RL_DRAINING,
  RL_STOPPED
} rl_state;

typedef struct rl_stats {
  uint64_t tx_data;
  uint64_t tx_retransmitted;
  uint64_t tx_acks;
  uint64_t rx_duplicate;
  uint64_t rx_invalid;
  uint64_t rx_auth_failed;
  uint64_t tx_crypto_failed;
  uint64_t congestion_reduced;
  uint64_t terminal_acked;
  uint64_t terminal_failed;
} rl_stats;

typedef struct rl_tx_state {
  uint8_t bytes[RL_MAX_MESSAGE_BYTES];
  uint16_t size;
  uint32_t message_id;
  uint64_t tag;
  uint64_t admitted_at;
  uint64_t last_sent[RL_MAX_FRAGMENTS];
  uint8_t tries[RL_MAX_FRAGMENTS];
  uint8_t count;
  uint8_t sent_mask;
  uint8_t acked_mask;
  bool active;
} rl_tx_state;

typedef struct rl_rx_state {
  uint8_t bytes[RL_MAX_MESSAGE_BYTES];
  uint16_t size;
  uint8_t count;
  uint8_t mask;
} rl_rx_state;

typedef struct rl_ack_state {
  uint32_t message_id;
  uint16_t size;
  uint8_t count;
  uint8_t mask;
  uint64_t due_at;
  bool pending;
} rl_ack_state;

typedef struct rl_session {
  rl_config config;
  rl_callbacks callbacks;
  pthread_t owner;
  uint64_t generation;
  uint64_t now_ms;
  uint32_t tx_next_id;
  uint32_t rx_next_id;
  uint8_t cwnd;
  rl_tx_state tx;
  rl_rx_state rx;
  rl_ack_state ack;
  rl_ack_state completed_ack;
  rl_stats stats;
  rl_state state;
  bool in_call;
} rl_session;

/* Caller owns the stable session memory; no dynamic allocations. Exactly one
 * logical TX may be active, and exactly one RX message can be reassembled.
 * A generation may only be restarted after a successful stop. */
rl_result rl_start(rl_session *s, const rl_config *config, rl_callbacks callbacks,
                   uint64_t generation, rl_ticket *out_ticket);

/* Successfully admitted send COPY is immutable and independent of caller
 * storage. Exactly one terminal callback after a real ACK or final failure.
 * Rejected admission creates no terminal. Same-owner, non-reentrant calls. */
rl_result rl_send(rl_session *s, rl_ticket t, const void *data, size_t size,
                  uint64_t tag, uint64_t now_ms);

/* Authenticated receive. CRC32 only detects random corruption; the mandatory
 * full-length keyed MAC is the actual authenticity gate. Never claim a peer
 * handshake, confidentiality or authenticated key provisioning here. */
rl_result rl_input(rl_session *s, rl_ticket t, const void *wire, size_t size,
                   uint64_t now_ms);

/* Explicit timer-based progress, bounded by max emitted frames. Never sleeps,
 * spins, allocates, or automatically retries an application transaction. */
rl_result rl_step(rl_session *s, rl_ticket t, uint64_t now_ms,
                  size_t budget, size_t *out_emitted);
rl_result rl_cancel(rl_session *s, rl_ticket t);
rl_result rl_seal(rl_session *s, rl_ticket t);
/* Explicit fail-closed shutdown escape: after sealing, discard locally pending
 * ACKs and terminally cancel active logical TX. Physical Packet I/O leases are
 * NOT reclaimed here; the host must separately drain their real DMA owner. */
rl_result rl_abort(rl_session *s, rl_ticket t);
rl_result rl_drain(rl_session *s, rl_ticket t);
/* On successful stop the owned TX/RX MAC keys are explicitly wiped. This
 * does not release independent Packet I/O driver frame leases. */
rl_result rl_stop(rl_session *s, rl_ticket t);

#endif
