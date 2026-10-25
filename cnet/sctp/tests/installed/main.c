#include <cnet/sctp_wire.h>
#include <cnet/sctp_session.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct test_context {
  uint32_t tag;
  uint32_t tsn;
  uint32_t key;
  int owner_admitted;
} test_context;

/* The installed consumer rejects non-Owner calls during a pending send. */
static int owned(void *value) {
  const test_context *context = (const test_context *)value;
  return context != NULL && context->owner_admitted;
}

static int entropy(void *value, uint8_t *bytes, size_t size) {
  const test_context *context = (const test_context *)value;
  if (!context || !bytes || size != 8u) return -1;
  bytes[0] = (uint8_t)(context->tag >> 24);
  bytes[1] = (uint8_t)(context->tag >> 16);
  bytes[2] = (uint8_t)(context->tag >> 8);
  bytes[3] = (uint8_t)context->tag;
  bytes[4] = (uint8_t)(context->tsn >> 24);
  bytes[5] = (uint8_t)(context->tsn >> 16);
  bytes[6] = (uint8_t)(context->tsn >> 8);
  bytes[7] = (uint8_t)context->tsn;
  return 0;
}

/* Installed-consumer lifecycle fixture only. Production callers must provide
 * a cryptographic MAC whose key lifetime is bound to the DTLS generation. */
static int test_mac(void *value, const uint8_t *bytes, size_t size,
    uint8_t out[CNET_SCTP_SESSION_MAC_BYTES]) {
  const test_context *context = (const test_context *)value;
  uint32_t state;
  size_t i;
  if (!context || (!bytes && size != 0u) || !out) return -1;
  state = context->key;
  for (i = 0u; i < size; ++i) state = state * UINT32_C(65599) + bytes[i];
  for (i = 0u; i < CNET_SCTP_SESSION_MAC_BYTES; ++i) {
    state = state * UINT32_C(1664525) + UINT32_C(1013904223);
    out[i] = (uint8_t)(state >> 24);
  }
  return 0;
}

static int test_verify(void *value, const uint8_t *bytes, size_t size,
    const uint8_t claimed[CNET_SCTP_SESSION_MAC_BYTES]) {
  uint8_t actual[CNET_SCTP_SESSION_MAC_BYTES];
  uint8_t different = 0u;
  size_t i;
  if (!claimed || test_mac(value, bytes, size, actual) != 0) return -1;
  for (i = 0u; i < sizeof(actual); ++i)
    different |= (uint8_t)(actual[i] ^ claimed[i]);
  return different == 0u ? 0 : -1;
}

static cnet_sctp_session_create_config session_config(uint16_t port,
    int client, test_context *context) {
  cnet_sctp_session_create_config config = {0};
  config.abi_version = CNET_SCTP_SESSION_ABI_VERSION;
  config.struct_size = sizeof(config);
  config.local_port = port;
  config.remote_port = port == 5000u ? 5001u : 5000u;
  /* The installed ABI advertises 24 KiB copied messages. A 32 KiB
   * receiver window permits testing a full-size reassembly without
   * relying on the receiver to publish partial-delivery APIs. */
  config.receive_window = 32768u;
  config.outbound_streams = 4u;
  config.inbound_streams = 4u;
  config.t1_ms = 100u;
  config.t1_max_ms = 400u;
  config.cookie_lifetime_ms = 1000u;
  config.peer_binding = UINT64_C(0x0123456789abcdef);
  config.max_retransmits = 3u;
  config.is_dtls_client = (uint8_t)client;
  config.is_owner = owned;
  config.entropy = entropy;
  config.mac = test_mac;
  config.verify_mac = test_verify;
  config.provider_context = context;
  return config;
}

static int rejected_abi_contract(void) {
  test_context context = {
      UINT32_C(0x11223344), UINT32_C(0x10203040), UINT32_C(0x13579bdf), 1};
  cnet_sctp_session_create_config config = session_config(5000u, 1, &context);
  cnet_sctp_session *session = (cnet_sctp_session *)(uintptr_t)1u;
  if (cnet_sctp_session_create(&config, 0u, NULL) !=
      CNET_SCTP_SESSION_INVALID)
    return 10;
  ++config.abi_version;
  if (cnet_sctp_session_create(&config, 0u, &session) !=
          CNET_SCTP_SESSION_INVALID ||
      session != NULL)
    return 11;
  config.abi_version = CNET_SCTP_SESSION_ABI_VERSION;
  --config.struct_size;
  session = (cnet_sctp_session *)(uintptr_t)1u;
  if (cnet_sctp_session_create(&config, 0u, &session) !=
          CNET_SCTP_SESSION_INVALID ||
      session != NULL)
    return 12;
  return 0;
}

static int deliver(cnet_sctp_session *receiver, cnet_sctp_session *sender,
    uint64_t now, const uint8_t *packet, size_t packet_size) {
  uint8_t reply[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t final[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  size_t reply_size = 0u, final_size = 0u;
  if (cnet_sctp_session_input(receiver, now, packet, packet_size,
          reply, sizeof(reply), &reply_size) != CNET_SCTP_SESSION_OK)
    return -1;
  if (reply_size != 0u &&
      cnet_sctp_session_input(sender, now + 1u, reply, reply_size,
          final, sizeof(final), &final_size) != CNET_SCTP_SESSION_OK)
    return -1;
  return final_size == 0u ? 0 : -1;
}

static int session_round_trip(void) {
  test_context client_context = {
      UINT32_C(0x11223344), UINT32_C(0x10203040), UINT32_C(0x13579bdf), 1};
  test_context server_context = {
      UINT32_C(0x55667788), UINT32_C(0x50607080), UINT32_C(0x13579bdf), 1};
  cnet_sctp_session_create_config client_config =
      session_config(5000u, 1, &client_context);
  cnet_sctp_session_create_config server_config =
      session_config(5001u, 0, &server_context);
  cnet_sctp_session *client = NULL, *server = NULL;
  cnet_sctp_session_event event = {0};
  uint8_t first[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t second[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t third[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t fourth[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t message[CNET_SCTP_SESSION_MAX_MESSAGE_BYTES];
  uint8_t fragmented[2048];
  uint8_t maximum[CNET_SCTP_SESSION_MAX_MESSAGE_BYTES + 1u];
  uint8_t stale_data[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  size_t stale_size = 0u;
  size_t first_size = 0u, second_size = 0u, third_size = 0u, fourth_size = 0u;
  size_t i, fragments_sent = 0u;
  uint64_t now_ms = 29u, close_generation = 0u;
  uint64_t client_old_generation = 0u, server_old_generation = 0u;
  uint64_t client_new_generation = 0u, server_new_generation = 0u;
  uint32_t gap_cum_ack = 0u;
  cnet_sctp_wire_header sack_header = {0};
  cnet_sctp_wire_decoder sack_decoder = {0};
  cnet_sctp_wire_chunk sack_chunk = {0};
  cnet_sctp_session_status pump_status;
  int result = 20;

  if (cnet_sctp_session_create(&client_config, 0u, &client) !=
          CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_create(&server_config, 0u, &server) !=
          CNET_SCTP_SESSION_OK)
    goto cleanup;
  if (cnet_sctp_session_start(client, 1u, first, sizeof(first), &first_size) !=
          CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(server, 2u, first, first_size,
          second, sizeof(second), &second_size) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(client, 3u, second, second_size,
          third, sizeof(third), &third_size) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(server, 4u, third, third_size,
          fourth, sizeof(fourth), &fourth_size) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(client, 5u, fourth, fourth_size,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      first_size != 0u)
    goto cleanup;

  if (cnet_sctp_session_open_channel(client, 6u, 0u,
          CNET_SCTP_SESSION_CHANNEL_RELIABLE_UNORDERED, 0u, 0u,
          "installed", 9u, NULL, 0u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      deliver(server, client, 7u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, 9u, &event, message, sizeof(message),
          second, sizeof(second), &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN || second_size == 0u ||
      event.stream_id != 0u || event.generation == 0u ||
      (server_old_generation = event.generation) == 0u ||
      deliver(client, server, 10u, second, second_size) != 0 ||
      cnet_sctp_session_poll(client, 12u, &event, message, sizeof(message),
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.stream_id != 0u || event.generation == 0u)
    goto cleanup;
  client_old_generation = event.generation;

  if (cnet_sctp_session_send(client, 13u, 0u, 1, "sdk", 3u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      deliver(server, client, 14u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, 16u, &event, message, sizeof(message),
          second, sizeof(second), &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE || !event.binary ||
      event.message_size != 3u || memcmp(message, "sdk", 3u) != 0)
    goto cleanup;

  /* Installed, opaque-only consumer: the single-packet API refuses an
   * oversized message atomically. The bounded queue copies it and the
   * explicit Owner pump emits two independently acknowledged fragments. */
  for (i = 0u; i < sizeof(fragmented); ++i)
    fragmented[i] = (uint8_t)((i * 37u + (i >> 3u)) & 0xffu);
  first_size = sizeof(first);
  if (cnet_sctp_session_send(client, 17u, 0u, 1,
          fragmented, sizeof(fragmented),
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_CAPACITY ||
      first_size != 0u ||
      cnet_sctp_session_queue_message(client, 18u, 0u, 1,
          fragmented, sizeof(fragmented)) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_queue_message(client, 18u, 0u, 1,
          fragmented, sizeof(fragmented)) != CNET_SCTP_SESSION_CAPACITY)
    goto cleanup;
  first_size = sizeof(first);
  if (cnet_sctp_session_pump_message(client, 19u, first, 8u,
          &first_size) != CNET_SCTP_SESSION_CAPACITY || first_size != 0u ||
      cnet_sctp_session_pump_message(client, 19u, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      deliver(server, client, 20u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, 22u, &event, message, sizeof(message),
          second, sizeof(second), &second_size) != CNET_SCTP_SESSION_EMPTY)
    goto cleanup;
  if (cnet_sctp_session_pump_message(client, 23u, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      deliver(server, client, 24u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, 26u, &event, message, sizeof(message),
          second, sizeof(second), &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE || !event.binary ||
      !event.unordered || event.message_size != sizeof(fragmented) ||
      memcmp(message, fragmented, sizeof(fragmented)) != 0 ||
      cnet_sctp_session_pump_message(client, 27u, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_EMPTY || first_size != 0u)
    goto cleanup;

  /* Full advertised 24 KiB boundary: reject 24 KiB+1 without admission,
   * then copy and deliver exactly 24 KiB through the installed Session
   * target, pumping only as SACK/window credit permits. No internal
   * Association/DATA header or private fragment size may be referenced. */
  for (i = 0u; i < sizeof(maximum); ++i)
    maximum[i] = (uint8_t)((i * 73u + (i >> 5u)) & 0xffu);
  /* The copied DATA queue uses CAPACITY for a validly shaped message
   * that exceeds its fixed 24 KiB slot, before committing any state. */
  if (cnet_sctp_session_queue_message(client, 28u, 0u, 1,
          maximum, sizeof(maximum)) != CNET_SCTP_SESSION_CAPACITY ||
      cnet_sctp_session_queue_message(client, 28u, 0u, 1,
          maximum, CNET_SCTP_SESSION_MAX_MESSAGE_BYTES) !=
              CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_queue_message(client, 28u, 0u, 1,
          "overlap", 7u) != CNET_SCTP_SESSION_CAPACITY)
    goto cleanup;
  /* Do not let an immediate send or a foreign Owner corrupt the copied
   * pending 24 KiB record. Both rejection paths must be state-atomic.
   * Once Owner access returns, the original queue must remain occupied. */
  first_size = sizeof(first);
  if (cnet_sctp_session_send(client, 28u, 0u, 1,
          "intrude", 7u, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_CAPACITY ||
      first_size != 0u)
    goto cleanup;
  client_context.owner_admitted = 0;
  first_size = sizeof(first);
  if (cnet_sctp_session_pump_message(client, 29u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OWNER ||
      first_size != 0u ||
      cnet_sctp_session_queue_message(client, 29u, 0u, 1,
          "intrude", 7u) != CNET_SCTP_SESSION_OWNER ||
      /* Neither an unadmitted stop nor destroy may free/revoke the
       * queued copy, provider callbacks or the opaque Session handle. */
      cnet_sctp_session_stop(client) != CNET_SCTP_SESSION_OWNER ||
      cnet_sctp_session_destroy(client) != CNET_SCTP_SESSION_OWNER)
    goto cleanup;
  client_context.owner_admitted = 1;
  if (cnet_sctp_session_queue_message(client, 29u, 0u, 1,
          "intrude", 7u) != CNET_SCTP_SESSION_CAPACITY)
    goto cleanup;
  /* Closing the queue's active DCEP SID must wait for its outstanding
   * copied message and TX tickets. An early RFC6525 close cannot consume
   * an SSN/MID, allocate a closing generation, or discard queue bytes. */
  first_size = sizeof(first);
  if (cnet_sctp_session_close_channel(client, 29u, 0u, first,
          sizeof(first), &first_size, &close_generation) !=
              CNET_SCTP_SESSION_BUSY ||
      first_size != 0u || close_generation != 0u)
    goto cleanup;
  /* The full message must have been copied before admission returns.
   * Poison the entire caller-owned input; reassembly must still return
   * the ORIGINAL deterministic bytes, not these changed bytes. */
  for (i = 0u; i < CNET_SCTP_SESSION_MAX_MESSAGE_BYTES; ++i)
    maximum[i] = (uint8_t)(maximum[i] ^ 0xffu);
  for (i = 0u; i < 32u; ++i) {
    first_size = 0u;
    pump_status = cnet_sctp_session_pump_message(client, now_ms,
        first, sizeof(first), &first_size);
    if (pump_status == CNET_SCTP_SESSION_EMPTY) break;
    if (pump_status != CNET_SCTP_SESSION_OK || first_size == 0u ||
        deliver(server, client, now_ms + 1u, first, first_size) != 0)
      goto cleanup;
    ++fragments_sent;
    now_ms += 3u;
  }
  if (fragments_sent < 2u || i == 32u ||
      cnet_sctp_session_poll(server, now_ms + 2u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
      !event.binary || !event.unordered ||
      event.message_size != CNET_SCTP_SESSION_MAX_MESSAGE_BYTES)
    goto cleanup;
  for (i = 0u; i < CNET_SCTP_SESSION_MAX_MESSAGE_BYTES; ++i)
    if (message[i] != (uint8_t)(maximum[i] ^ 0xffu))
      goto cleanup;

  /* A TIMED message may expire while it is still wholly in the
   * Owner-owned pending queue, BEFORE the first DATA TSN is allocated.
   * The installed Session must report an abandonment without inventing a
   * first TSN, clear the slot, and allow a new same-SID message afterward. */
  now_ms += 8u;
  first_size = 0u;
  if (cnet_sctp_session_open_channel(client, now_ms, 2u,
          CNET_SCTP_SESSION_CHANNEL_TIMED_ORDERED, 0u, 500u,
          "expiry", 6u, NULL, 0u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      deliver(server, client, now_ms + 1u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, now_ms + 3u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.stream_id != 2u || second_size == 0u ||
      deliver(client, server, now_ms + 4u, second, second_size) != 0 ||
      cnet_sctp_session_poll(client, now_ms + 6u, &event,
          message, sizeof(message), first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.stream_id != 2u)
    goto cleanup;
  now_ms += 9u;
  if (cnet_sctp_session_queue_message(client, now_ms, 2u, 1,
          "expire", 6u) != CNET_SCTP_SESSION_OK)
    goto cleanup;
  first_size = sizeof(first);
  if (cnet_sctp_session_pump_message(client, now_ms + 500u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_BUSY ||
      first_size != 0u)
    goto cleanup;
  first_size = sizeof(first);
  if (cnet_sctp_session_advance(client, now_ms + 500u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      first_size != 0u ||
      cnet_sctp_session_poll(client, now_ms + 501u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED ||
      event.stream_id != 2u || event.has_tsn ||
      event.message_size != 6u || !event.binary ||
      event.reliability.mode != CNET_SCTP_SESSION_TIMED ||
      event.reliability.value != 500u ||
      event.abandon_reason != CNET_SCTP_SESSION_ABANDON_TIMED)
    goto cleanup;
  first_size = sizeof(first);
  if (cnet_sctp_session_pump_message(client, now_ms + 502u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_EMPTY ||
      first_size != 0u ||
      cnet_sctp_session_send(client, now_ms + 503u, 2u, 1,
          "next", 4u, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      deliver(server, client, now_ms + 504u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, now_ms + 506u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
      event.stream_id != 2u || event.message_size != 4u ||
      memcmp(message, "next", 4u) != 0)
    goto cleanup;

  /* Installed PPID adapter: logical zero-length text and binary records
   * occupy an on-wire one-byte DATA payload with the respective empty PPID,
   * yet must surface as exactly zero-length application messages. Their
   * input pointer is permitted to be NULL only when the length is zero. */
  first_size = sizeof(first);
  if (cnet_sctp_session_send(client, now_ms + 507u, 2u, 0,
          NULL, 0u, first, sizeof(first), &first_size) !=
              CNET_SCTP_SESSION_OK ||
      deliver(server, client, now_ms + 508u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, now_ms + 510u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
      event.stream_id != 2u || event.binary || event.message_size != 0u ||
      event.ppid != 56u)
    goto cleanup;
  first_size = sizeof(first);
  if (cnet_sctp_session_send(client, now_ms + 511u, 2u, 1,
          NULL, 0u, first, sizeof(first), &first_size) !=
              CNET_SCTP_SESSION_OK ||
      deliver(server, client, now_ms + 512u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, now_ms + 514u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
      event.stream_id != 2u || !event.binary || event.message_size != 0u ||
      event.ppid != 57u)
    goto cleanup;

  /* ACK-gated reset admission: receiving a DATA message is NOT proof
   * that its send ticket retired. Hold the real SACK back, attempt an
   * RFC6525 close while one TSN remains live, then deliver the SACK and
   * require successful close admission with a real generation. */
  first_size = sizeof(first);
  if (cnet_sctp_session_send(client, now_ms + 516u, 0u, 1,
          "ack-gate", 8u, first, sizeof(first), &first_size) !=
              CNET_SCTP_SESSION_OK)
    goto cleanup;
  /* Keep the exact old-generation SCTP DATA bytes before they can be
   * overwritten by the two-direction RFC6525 reset exchange. */
  stale_size = first_size;
  if (stale_size == 0u || stale_size > sizeof(stale_data))
    goto cleanup;
  memcpy(stale_data, first, stale_size);
  second_size = 0u;
  if (cnet_sctp_session_input(server, now_ms + 517u, first, first_size,
          second, sizeof(second), &second_size) != CNET_SCTP_SESSION_OK ||
      second_size == 0u ||
      cnet_sctp_session_poll(server, now_ms + 518u, &event,
          message, sizeof(message), third, sizeof(third),
          &third_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
      event.stream_id != 0u || event.message_size != 8u ||
      memcmp(message, "ack-gate", 8u) != 0)
    goto cleanup;
  close_generation = 0u;
  fourth_size = 0u;
  if (cnet_sctp_session_close_channel(client, now_ms + 519u, 0u,
          fourth, sizeof(fourth), &fourth_size, &close_generation) !=
              CNET_SCTP_SESSION_BUSY ||
      fourth_size != 0u || close_generation != 0u)
    goto cleanup;
  first_size = 0u;
  if (cnet_sctp_session_input(client, now_ms + 520u,
          second, second_size, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      first_size != 0u)
    goto cleanup;
  fourth_size = 0u;
  if (cnet_sctp_session_close_channel(client, now_ms + 521u, 0u,
          fourth, sizeof(fourth), &fourth_size,
          &close_generation) != CNET_SCTP_SESSION_OK ||
      fourth_size == 0u || close_generation != client_old_generation)
    goto cleanup;

  /* Complete both actual RFC6525 reset directions, not just emit a close
   * request. The event generation must still reference the OLD channel. */
  second_size = 0u;
  if (cnet_sctp_session_input(server, now_ms + 522u, fourth, fourth_size,
          second, sizeof(second), &second_size) != CNET_SCTP_SESSION_OK ||
      second_size == 0u ||
      cnet_sctp_session_poll(server, now_ms + 523u, &event,
          message, sizeof(message), third, sizeof(third),
          &third_size) != CNET_SCTP_SESSION_EMPTY ||
      cnet_sctp_session_input(client, now_ms + 524u,
          second, second_size, third, sizeof(third),
          &third_size) != CNET_SCTP_SESSION_OK ||
      third_size == 0u ||
      cnet_sctp_session_poll(client, now_ms + 525u, &event,
          message, sizeof(message), first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSED ||
      event.stream_id != 0u ||
      event.generation != client_old_generation ||
      cnet_sctp_session_input(server, now_ms + 526u,
          third, third_size, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_poll(server, now_ms + 527u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSED ||
      event.stream_id != 0u ||
      event.generation != server_old_generation)
    goto cleanup;

  /* A new DCEP OPEN on that same SID must own a NEW generation. */
  first_size = 0u;
  if (cnet_sctp_session_open_channel(client, now_ms + 530u, 0u,
          CNET_SCTP_SESSION_CHANNEL_RELIABLE_UNORDERED, 0u, 0u,
          "reopened", 8u, NULL, 0u, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      deliver(server, client, now_ms + 531u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, now_ms + 533u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.stream_id != 0u ||
      event.generation == 0u ||
      event.generation == server_old_generation ||
      second_size == 0u)
    goto cleanup;
  server_new_generation = event.generation;
  if (deliver(client, server, now_ms + 534u,
          second, second_size) != 0 ||
      cnet_sctp_session_poll(client, now_ms + 536u, &event,
          message, sizeof(message), first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.stream_id != 0u ||
      event.generation == 0u ||
      event.generation == client_old_generation)
    goto cleanup;
  client_new_generation = event.generation;

  /* Replay a *real* old wire packet after the same SID is reopened.
   * A duplicate ACK is allowed, but old payload must NOT be surfaced as
   * new-generation application data. */
  second_size = 0u;
  if (cnet_sctp_session_input(server, now_ms + 538u,
          stale_data, stale_size, second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK)
    goto cleanup;
  if (second_size != 0u) {
    first_size = 0u;
    if (cnet_sctp_session_input(client, now_ms + 539u,
            second, second_size, first, sizeof(first),
            &first_size) != CNET_SCTP_SESSION_OK ||
        first_size != 0u)
      goto cleanup;
  }
  if (cnet_sctp_session_poll(server, now_ms + 540u, &event,
          message, sizeof(message), third, sizeof(third),
          &third_size) != CNET_SCTP_SESSION_EMPTY)
    goto cleanup;

  /* New application DATA must still arrive under the new SID generation
   * with its original bytes and the negotiated unordered PPID. */
  first_size = 0u;
  if (cnet_sctp_session_send(client, now_ms + 542u, 0u, 1,
          "new-generation", 14u, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      deliver(server, client, now_ms + 543u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, now_ms + 545u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
      event.generation != server_new_generation ||
      event.stream_id != 0u || !event.binary || !event.unordered ||
      event.message_size != 14u ||
      memcmp(message, "new-generation", 14u) != 0 ||
      client_new_generation == client_old_generation)
    goto cleanup;

  /* Gap ACK is revocable and cannot retire a DATA ticket. Hold TSN N,
   * deliver only N+1 and verify its real SACK contains a gap block while
   * cumulative ACK stays behind N. RFC6525 close must remain BUSY even
   * though that later fragment was acknowledged out of order. */
  first_size = 0u;
  if (cnet_sctp_session_send(client, now_ms + 547u, 0u, 1,
          "first-gap", 9u, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      first_size == 0u || first_size > sizeof(stale_data))
    goto cleanup;
  stale_size = first_size;
  memcpy(stale_data, first, stale_size);
  third_size = 0u;
  if (cnet_sctp_session_send(client, now_ms + 548u, 0u, 1,
          "later-gap", 9u, third, sizeof(third),
          &third_size) != CNET_SCTP_SESSION_OK ||
      third_size == 0u)
    goto cleanup;
  second_size = 0u;
  if (cnet_sctp_session_input(server, now_ms + 549u,
          third, third_size, second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      second_size == 0u ||
      cnet_sctp_wire_decode(second, second_size,
          &sack_header, &sack_decoder) != CNET_SCTP_WIRE_OK ||
      cnet_sctp_wire_next_chunk(&sack_decoder, &sack_chunk) !=
          CNET_SCTP_WIRE_OK ||
      sack_chunk.type != 3u || sack_chunk.value_size < 12u ||
      (sack_chunk.value[8u] == 0u && sack_chunk.value[9u] == 0u))
    goto cleanup;
  gap_cum_ack = ((uint32_t)sack_chunk.value[0u] << 24u) |
      ((uint32_t)sack_chunk.value[1u] << 16u) |
      ((uint32_t)sack_chunk.value[2u] << 8u) |
      (uint32_t)sack_chunk.value[3u];
  first_size = 0u;
  if (cnet_sctp_session_input(client, now_ms + 550u,
          second, second_size, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      first_size != 0u)
    goto cleanup;
  close_generation = 0u;
  fourth_size = 0u;
  if (cnet_sctp_session_close_channel(client, now_ms + 551u, 0u,
          fourth, sizeof(fourth), &fourth_size,
          &close_generation) != CNET_SCTP_SESSION_BUSY ||
      fourth_size != 0u || close_generation != 0u)
    goto cleanup;

  /* Deliver the originally delayed TSN. Only the subsequent cumulative
   * SACK may release the earlier TX ticket and admit the real reset.
   * The gap-ACKed later DATA must not be counted twice or falsely
   * settle its unacknowledged predecessor. */
  second_size = 0u;
  if (cnet_sctp_session_input(server, now_ms + 552u,
          stale_data, stale_size, second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      second_size == 0u ||
      cnet_sctp_wire_decode(second, second_size,
          &sack_header, &sack_decoder) != CNET_SCTP_WIRE_OK ||
      cnet_sctp_wire_next_chunk(&sack_decoder, &sack_chunk) !=
          CNET_SCTP_WIRE_OK ||
      sack_chunk.type != 3u || sack_chunk.value_size < 12u)
    goto cleanup;
  {
    const uint32_t final_cum_ack =
        ((uint32_t)sack_chunk.value[0u] << 24u) |
        ((uint32_t)sack_chunk.value[1u] << 16u) |
        ((uint32_t)sack_chunk.value[2u] << 8u) |
        (uint32_t)sack_chunk.value[3u];
    if (final_cum_ack != gap_cum_ack + 2u)
      goto cleanup;
  }
  first_size = 0u;
  if (cnet_sctp_session_input(client, now_ms + 553u,
          second, second_size, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      first_size != 0u)
    goto cleanup;
  fourth_size = 0u;
  if (cnet_sctp_session_close_channel(client, now_ms + 554u, 0u,
          fourth, sizeof(fourth), &fourth_size,
          &close_generation) != CNET_SCTP_SESSION_OK ||
      fourth_size == 0u || close_generation != client_new_generation)
    goto cleanup;

  result = 0;

cleanup:
  if (client) {
    if (cnet_sctp_session_stop(client) != CNET_SCTP_SESSION_OK ||
        !cnet_sctp_session_drained(client) ||
        cnet_sctp_session_destroy(client) != CNET_SCTP_SESSION_OK)
      result = 21;
  }
  if (server) {
    if (cnet_sctp_session_stop(server) != CNET_SCTP_SESSION_OK ||
        !cnet_sctp_session_drained(server) ||
        cnet_sctp_session_destroy(server) != CNET_SCTP_SESSION_OK)
      result = 22;
  }
  return result;
}

int main(void) {
  const uint8_t payload[] = {1u, 2u, 3u};
  const cnet_sctp_wire_chunk chunk = {0u, 0u, payload, sizeof(payload)};
  cnet_sctp_wire_chunk received = {0};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  uint8_t packet[64] = {0};
  size_t packet_size = 0u;
  int abi_result;
  if (CNET_SCTP_WIRE_ABI_VERSION != 1u) return 1;
  abi_result = rejected_abi_contract();
  if (abi_result != 0) return abi_result;
  if (cnet_sctp_wire_encode(5000u, 5001u, 0x10203040u, &chunk, 1u,
                            packet, sizeof(packet), &packet_size) != CNET_SCTP_WIRE_OK)
    return 2;
  if (cnet_sctp_wire_decode(packet, packet_size, &header, &decoder) != CNET_SCTP_WIRE_OK)
    return 3;
  if (header.chunk_count != 1u || header.verification_tag != 0x10203040u)
    return 4;
  if (cnet_sctp_wire_next_chunk(&decoder, &received) != CNET_SCTP_WIRE_OK)
    return 5;
  if (received.value_size != sizeof(payload) || received.value[2] != 3u)
    return 6;
  if (cnet_sctp_wire_next_chunk(&decoder, &received) != CNET_SCTP_WIRE_END)
    return 7;
  return session_round_trip();
}
