#ifndef CNET_SCTP_ASSOC_INTERNAL_H
#define CNET_SCTP_ASSOC_INTERNAL_H

#include <cnet/sctp_wire.h>
#include <stddef.h>
#include <stdint.h>

/* Research-only handshake engine. Intentionally NOT installed or public SDK.
 * Caller MUST zero-initialize cnet_sctp_assoc before first init and must not
 * invoke init concurrently; an initialized association rejects repeated init.
 */
#define CNET_SCTP_ASSOC_MAX_PACKET 1200u
#define CNET_SCTP_ASSOC_COOKIE_BYTES 88u
#define CNET_SCTP_ASSOC_MAC_BYTES 32u
#define CNET_SCTP_ASSOC_ENTROPY_BYTES 8u

typedef enum cnet_sctp_assoc_state {
  CNET_SCTP_ASSOC_CLOSED = 0,
  CNET_SCTP_ASSOC_COOKIE_WAIT = 1,
  CNET_SCTP_ASSOC_COOKIE_ECHOED = 2,
  CNET_SCTP_ASSOC_ESTABLISHED = 3,
  CNET_SCTP_ASSOC_FAILED = 4,
  CNET_SCTP_ASSOC_STOPPED = 5
} cnet_sctp_assoc_state;

typedef enum cnet_sctp_assoc_status {
  CNET_SCTP_ASSOC_OK = 0,
  CNET_SCTP_ASSOC_INVALID = -1,
  CNET_SCTP_ASSOC_OWNER = -2,
  CNET_SCTP_ASSOC_CAPACITY = -3,
  CNET_SCTP_ASSOC_PROTOCOL = -4,
  CNET_SCTP_ASSOC_AUTH = -5,
  CNET_SCTP_ASSOC_CLOCK = -6,
  CNET_SCTP_ASSOC_CLOSED_ERROR = -7,
  CNET_SCTP_ASSOC_BUSY = -8,
  CNET_SCTP_ASSOC_EXPIRED = -9,
  CNET_SCTP_ASSOC_ENTROPY = -10
} cnet_sctp_assoc_status;

typedef int (*cnet_sctp_assoc_owner_fn)(void *context);
/* Required CSPRNG provider (0=success, negative=failure). It MUST fill every
 * requested byte from an unpredictable OS/cryptographic source and MUST NOT
 * recurse into this Association. Deterministic callbacks are TEST-ONLY.
 * The engine does not save or control the provider's internal entropy state. */
typedef int (*cnet_sctp_assoc_entropy_fn)(void *context,
    uint8_t *destination, size_t destination_size);
/* MUST be a cryptographic, keyed MAC in any real integration. The key remains
 * exclusively with the provider. The engine never persists a key or pointer in
 * a state cookie. Return 0 only if the output was fully initialized. */
typedef int (*cnet_sctp_assoc_mac_fn)(void *context,
    const uint8_t *message, size_t message_size,
    uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]);

typedef struct cnet_sctp_assoc_config {
  uint16_t local_port;
  uint16_t remote_port;
  uint32_t receive_window; /* at least 1500 bytes */
  uint16_t outbound_streams;
  uint16_t inbound_streams;
  uint64_t t1_ms;
  uint64_t t1_max_ms;
  uint64_t cookie_lifetime_ms;
  uint64_t peer_binding; /* Opaque, non-pointer DTLS peer generation identity. */
  uint8_t max_retransmits;
  cnet_sctp_assoc_owner_fn is_owner;
  cnet_sctp_assoc_entropy_fn entropy;
  cnet_sctp_assoc_mac_fn mac;
  void *provider_context;
} cnet_sctp_assoc_config;

typedef struct cnet_sctp_assoc {
  cnet_sctp_assoc_config config;
  cnet_sctp_assoc_state state;
  uint32_t local_tag; /* Generated once from required CSPRNG in init. */
  uint32_t initial_tsn; /* Generated once from required CSPRNG in init. */
  uint64_t last_time_ms;
  uint64_t deadline_ms;
  uint64_t current_rto_ms;
  uint32_t peer_tag;
  uint32_t peer_initial_tsn;
  uint16_t peer_outbound_streams;
  uint16_t peer_inbound_streams;
  uint8_t saved_packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t saved_size;
  uint8_t retransmits;
  uint8_t in_progress;
  uint32_t magic;
} cnet_sctp_assoc;

cnet_sctp_assoc_status cnet_sctp_assoc_init(
    cnet_sctp_assoc *association, const cnet_sctp_assoc_config *config,
    uint64_t initial_time_ms);
cnet_sctp_assoc_status cnet_sctp_assoc_start(
    cnet_sctp_assoc *association, uint64_t now_ms,
    uint8_t *output, size_t output_capacity, size_t *output_size);
cnet_sctp_assoc_status cnet_sctp_assoc_input(
    cnet_sctp_assoc *association, uint64_t now_ms,
    const uint8_t *packet, size_t packet_size,
    uint8_t *output, size_t output_capacity, size_t *output_size);
cnet_sctp_assoc_status cnet_sctp_assoc_advance(
    cnet_sctp_assoc *association, uint64_t now_ms,
    uint8_t *output, size_t output_capacity, size_t *output_size);
cnet_sctp_assoc_status cnet_sctp_assoc_next_deadline(
    cnet_sctp_assoc *association, uint64_t *deadline_ms, int *has_deadline);
cnet_sctp_assoc_status cnet_sctp_assoc_stop(cnet_sctp_assoc *association);
cnet_sctp_assoc_status cnet_sctp_assoc_destroy(cnet_sctp_assoc *association);

/* Owner-only query. The prototype never lends packet views beyond calls. */
cnet_sctp_assoc_state cnet_sctp_assoc_state_get(const cnet_sctp_assoc *association);
int cnet_sctp_assoc_drained(const cnet_sctp_assoc *association);

#endif
