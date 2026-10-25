#include <cnet/sctp_wire.h>
#include <cnet/sctp_session.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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
  cnet_sctp_session_tx_usage tx_usage = {0};
  cnet_sctp_session_reliable_ack acked = {0};
  cnet_sctp_session_unsent_cancelled cancelled = {0};
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
  uint64_t old_ticket = 0u, gap_ticket_a = 0u, gap_ticket_b = 0u;
  uint64_t first_batch_ticket = 0u, next_batch_ticket = 0u;
  uint64_t fragment_ticket = 0u, reordered_ticket = 0u;
  uint64_t timed_generation = 0u, cancelled_ticket = 0u;
  uint64_t post_cancel_ticket = 0u;
  uint32_t last_fragment_tsn = 0u, withheld_tsn = 0u;
  uint32_t reordered_last_tsn = 0u;
  uint32_t gap_cum_ack = 0u;
  cnet_sctp_wire_header sack_header = {0};
  cnet_sctp_wire_decoder sack_decoder = {0};
  cnet_sctp_wire_chunk sack_chunk = {0};
  cnet_sctp_session_status pump_status;
  int result = 20;
  int cancel_stage = 0;

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
          "overlap", 7u) != CNET_SCTP_SESSION_CAPACITY ||
      cnet_sctp_session_get_tx_usage(client, 0u,
          client_old_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.generation != client_old_generation ||
      tx_usage.queued_bytes != CNET_SCTP_SESSION_MAX_MESSAGE_BYTES ||
      !tx_usage.has_pending_message || tx_usage.retained_bytes != 0u ||
      tx_usage.retained_fragments != 0u)
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
      cnet_sctp_session_destroy(client) != CNET_SCTP_SESSION_OWNER ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_OWNER ||
      acked.ticket != 0u ||
      cnet_sctp_session_get_tx_usage(client, 0u,
          client_old_generation, &tx_usage) != CNET_SCTP_SESSION_OWNER ||
      tx_usage.generation != 0u || tx_usage.queued_bytes != 0u ||
      tx_usage.retained_bytes != 0u)
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
  if (cnet_sctp_session_get_tx_usage(client, 0u,
          client_old_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.has_pending_message || tx_usage.queued_bytes != 0u ||
      tx_usage.retained_bytes != 0u || tx_usage.retained_fragments != 0u)
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
      event.stream_id != 2u || event.generation == 0u)
    goto cleanup;
  timed_generation = event.generation;
  now_ms += 9u;
  if (cnet_sctp_session_queue_message(client, now_ms, 2u, 1,
          "expire", 6u) != CNET_SCTP_SESSION_OK)
    goto cleanup;
  first_size = sizeof(first);
  if (cnet_sctp_session_pump_message(client, now_ms + 500u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_BUSY ||
      first_size != 0u ||
      /* Exactly at the expiry deadline, cancellation must refuse and
       * preserve the original TIMED message for the Owner advance() path.
       * It is not a safe local pre-TSN CANCELLED result any more. */
      cnet_sctp_session_cancel_unsent_message(client, now_ms + 500u,
          2u, timed_generation, &cancelled) != CNET_SCTP_SESSION_BUSY ||
      cancelled.ticket != 0u || cancelled.generation != 0u)
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

  /* Untracked, pre-TSN TIMED cancellation is a synchronous outcome,
   * NOT a DATA abandonment or fake FORWARD-TSN. A logical empty text
   * record must report size=0 (though PPID56 wire storage is one byte).
   * Repeated cancel is EMPTY; no SSN/TSN, packet or ULP event is created. */
  cancel_stage = 110;
  if (cnet_sctp_session_queue_message(client, now_ms + 515u,
          2u, 0, NULL, 0u) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_cancel_unsent_message(client, now_ms + 515u,
          2u, timed_generation, &cancelled) != CNET_SCTP_SESSION_OK ||
      cancelled.generation != timed_generation ||
      cancelled.ticket != 0u || cancelled.stream_id != 2u ||
      cancelled.ppid != 56u || cancelled.message_size != 0u ||
      cancelled.binary || cancelled.unordered ||
      cancelled.reliability.mode != CNET_SCTP_SESSION_TIMED ||
      cancelled.reliability.value != 500u ||
      cnet_sctp_session_cancel_unsent_message(client, now_ms + 515u,
          2u, timed_generation, &cancelled) != CNET_SCTP_SESSION_EMPTY ||
      cancelled.ticket != 0u || cancelled.generation != 0u)
    goto cleanup;
  cancel_stage = 111;
  first_size = 0u;
  if (cnet_sctp_session_pump_message(client, now_ms + 515u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_EMPTY ||
      first_size != 0u ||
      cnet_sctp_session_poll(client, now_ms + 515u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_EMPTY)
    goto cleanup;

  /* ACK-gated reset admission: receiving a DATA message is NOT proof
   * that its send ticket retired. Hold the real SACK back, attempt an
   * RFC6525 close while one TSN remains live, then deliver the SACK and
   * require successful close admission with a real generation. */
  first_size = sizeof(first);
  if (cnet_sctp_session_send_reliable_tracked(client, now_ms + 516u,
          0u, client_old_generation, 1, "ack-gate", 8u, first,
          sizeof(first), &first_size, &old_ticket) !=
              CNET_SCTP_SESSION_OK ||
      old_ticket == 0u ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_EMPTY || acked.ticket != 0u)
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
  /* Take a REAL cumulative ACK receipt only after the original SID has
   * been retired and reopened, preserving its old generation forever. */
  if (cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_OK ||
      acked.ticket != old_ticket ||
      acked.generation != client_old_generation ||
      acked.stream_id != 0u || acked.message_size != 8u ||
      acked.ppid != 53u || !acked.binary ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_EMPTY || acked.ticket != 0u)
    goto cleanup;
  first_size = sizeof(first);
  old_ticket = UINT64_C(999);
  if (cnet_sctp_session_send_reliable_tracked(client, now_ms + 537u,
          0u, client_old_generation, 1, "stale", 5u,
          first, sizeof(first), &first_size, &old_ticket) !=
              CNET_SCTP_SESSION_STALE ||
      old_ticket != 0u || first_size != 0u)
    goto cleanup;
  if (cnet_sctp_session_get_tx_usage(client, 0u,
          client_old_generation, &tx_usage) != CNET_SCTP_SESSION_STALE ||
      tx_usage.generation != 0u || tx_usage.retained_bytes != 0u ||
      cnet_sctp_session_get_tx_usage(client, 0u,
          client_new_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.generation != client_new_generation ||
      tx_usage.queued_bytes != 0u || tx_usage.retained_bytes != 0u)
    goto cleanup;

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
  if (cnet_sctp_session_send_reliable_tracked(client, now_ms + 547u,
          0u, client_new_generation, 1, "first-gap", 9u,
          first, sizeof(first), &first_size, &gap_ticket_a) !=
              CNET_SCTP_SESSION_OK ||
      first_size == 0u || first_size > sizeof(stale_data) ||
      gap_ticket_a == 0u)
    goto cleanup;
  stale_size = first_size;
  memcpy(stale_data, first, stale_size);
  third_size = 0u;
  if (cnet_sctp_session_send_reliable_tracked(client, now_ms + 548u,
          0u, client_new_generation, 1, "later-gap", 9u,
          third, sizeof(third), &third_size, &gap_ticket_b) !=
              CNET_SCTP_SESSION_OK ||
      third_size == 0u || gap_ticket_b <= gap_ticket_a)
    goto cleanup;
  if (cnet_sctp_session_get_tx_usage(client, 0u,
          client_new_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.retained_bytes != 18u || tx_usage.retained_fragments != 2u ||
      tx_usage.gap_acked_bytes != 0u || tx_usage.abandoned_bytes != 0u)
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
  if (cnet_sctp_session_get_tx_usage(client, 0u,
          client_new_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.retained_bytes != 18u || tx_usage.gap_acked_bytes != 9u ||
      tx_usage.retained_fragments != 2u ||
      /* Revocable gap ACK must NEVER create a terminal ACK receipt. */
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_EMPTY || acked.ticket != 0u ||
      /* The Gap SACK is revocable. A fully emitted SID message has no
       * pending copied tail, but its two retained DATA tickets still make
       * pre-TSN cancellation conservatively BUSY. */
      cnet_sctp_session_cancel_unsent_message(client, now_ms + 550u,
          0u, client_new_generation, &cancelled) !=
              CNET_SCTP_SESSION_BUSY ||
      cancelled.ticket != 0u || cancelled.generation != 0u)
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
  if (cnet_sctp_session_get_tx_usage(client, 0u,
          client_new_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.retained_bytes != 0u || tx_usage.gap_acked_bytes != 0u ||
      tx_usage.retained_fragments != 0u ||
      /* Only after the real cumulative SACK clears both tickets can the
       * pre-TSN cancellation API truthfully report no relevant work. The
       * independent, already-latched ACK receipts are still available. */
      cnet_sctp_session_cancel_unsent_message(client, now_ms + 553u,
          0u, client_new_generation, &cancelled) !=
              CNET_SCTP_SESSION_EMPTY ||
      cancelled.ticket != 0u || cancelled.generation != 0u ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_OK ||
      acked.ticket != gap_ticket_a ||
      acked.generation != client_new_generation ||
      acked.stream_id != 0u || acked.message_size != 9u ||
      acked.ppid != 53u || !acked.binary)
    goto cleanup;
  {
    const uint32_t previous_tsn = acked.tsn;
    if (cnet_sctp_session_take_reliable_ack(client, &acked) !=
            CNET_SCTP_SESSION_OK ||
        acked.ticket != gap_ticket_b ||
        acked.generation != client_new_generation ||
        acked.tsn != previous_tsn + 1u ||
        acked.stream_id != 0u || acked.message_size != 9u ||
        cnet_sctp_session_take_reliable_ack(client, &acked) !=
            CNET_SCTP_SESSION_EMPTY || acked.ticket != 0u)
      goto cleanup;
  }
  /* The preceding revocable-gap fixture deliberately did not poll the
   * receiver's user messages while asserting raw SACK behavior. Drain and
   * check BOTH real ordered-independent DATA messages before the separate
   * ACK-receipt capacity fixture, rather than attributing an old message to
   * the first new generation-bound ticket. */
  {
    int saw_first = 0, saw_later = 0;
    for (i = 0u; i < 2u; ++i) {
      second_size = 0u;
      if (cnet_sctp_session_poll(server, now_ms + 555u + i, &event,
              message, sizeof(message), second, sizeof(second),
              &second_size) != CNET_SCTP_SESSION_OK ||
          event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
          event.stream_id != 0u || !event.binary || !event.unordered ||
          event.message_size != 9u)
        goto cleanup;
      if (memcmp(message, "first-gap", 9u) == 0) saw_first = 1;
      else if (memcmp(message, "later-gap", 9u) == 0) saw_later = 1;
      else goto cleanup;
    }
    if (!saw_first || !saw_later ||
        cnet_sctp_session_poll(server, now_ms + 557u, &event,
            message, sizeof(message), second, sizeof(second),
            &second_size) != CNET_SCTP_SESSION_EMPTY)
      goto cleanup;
  }

  /* Exactly 16 unconsumed reliable ACK receipts fit without additional
   * dynamic allocation. All DATA must already be cumulatively ACKed; one
   * more send MUST reject atomically until a receipt is consumed. */
  for (i = 0u; i < CNET_SCTP_SESSION_MAX_TRACKED_RELIABLE_RECEIPTS; ++i) {
    const uint64_t tick = now_ms + 560u + 4u * i;
    uint64_t admitted = 0u;
    first_size = 0u;
    if (cnet_sctp_session_send_reliable_tracked(client, tick,
            0u, client_new_generation, 1, "z", 1u,
            first, sizeof(first), &first_size, &admitted) !=
                CNET_SCTP_SESSION_OK ||
        first_size == 0u || admitted == 0u)
      goto cleanup;
    if (i == 0u) first_batch_ticket = admitted;
    if (admitted != first_batch_ticket + i ||
        deliver(server, client, tick + 1u, first, first_size) != 0 ||
        cnet_sctp_session_poll(server, tick + 3u, &event,
            message, sizeof(message), second, sizeof(second),
            &second_size) != CNET_SCTP_SESSION_OK ||
        event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
        event.stream_id != 0u || event.message_size != 1u ||
        message[0] != (uint8_t)'z')
      goto cleanup;
  }
  first_size = sizeof(first);
  next_batch_ticket = UINT64_C(1234);
  fragment_ticket = UINT64_C(1234);
  if (cnet_sctp_session_send_reliable_tracked(client, now_ms + 625u,
          0u, client_new_generation, 1, "capacity", 8u,
          first, sizeof(first), &first_size, &next_batch_ticket) !=
              CNET_SCTP_SESSION_CAPACITY ||
      first_size != 0u || next_batch_ticket != 0u ||
      /* An equally valid multi-fragment message must NOT reserve a
       * copied queue or TSN when all ACK receipt slots remain occupied. */
      cnet_sctp_session_queue_reliable_tracked(client, now_ms + 625u,
          0u, client_new_generation, 1,
          fragmented, sizeof(fragmented), &fragment_ticket) !=
              CNET_SCTP_SESSION_CAPACITY ||
      fragment_ticket != 0u ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_OK ||
      acked.ticket != first_batch_ticket ||
      acked.generation != client_new_generation ||
      acked.message_size != 1u)
    goto cleanup;
  first_size = 0u;
  if (cnet_sctp_session_send_reliable_tracked(client, now_ms + 626u,
          0u, client_new_generation, 1, "again", 5u,
          first, sizeof(first), &first_size, &next_batch_ticket) !=
              CNET_SCTP_SESSION_OK ||
      next_batch_ticket != first_batch_ticket +
          CNET_SCTP_SESSION_MAX_TRACKED_RELIABLE_RECEIPTS ||
      deliver(server, client, now_ms + 627u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, now_ms + 629u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
      event.message_size != 5u ||
      memcmp(message, "again", 5u) != 0)
    goto cleanup;
  for (i = 1u; i <= CNET_SCTP_SESSION_MAX_TRACKED_RELIABLE_RECEIPTS; ++i)
    if (cnet_sctp_session_take_reliable_ack(client, &acked) !=
            CNET_SCTP_SESSION_OK ||
        acked.ticket != first_batch_ticket + i ||
        acked.generation != client_new_generation ||
        acked.stream_id != 0u || acked.ppid != 53u ||
        !acked.binary ||
        acked.message_size !=
            (i == CNET_SCTP_SESSION_MAX_TRACKED_RELIABLE_RECEIPTS ?
             5u : 1u))
      goto cleanup;
  if (cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_EMPTY || acked.ticket != 0u)
    goto cleanup;

  /* A reliable, copied 2048-byte message produces TWO real DATA fragments
   * and a SINGLE original-generation ACK receipt only when the final
   * fragment has been cumulatively acknowledged. No first-fragment ACK
   * (nor receipt reservation) is full-message delivery. */
  if (cnet_sctp_session_queue_reliable_tracked(client, now_ms + 635u,
          0u, client_old_generation, 1,
          fragmented, sizeof(fragmented), &fragment_ticket) !=
              CNET_SCTP_SESSION_STALE || fragment_ticket != 0u ||
      cnet_sctp_session_queue_reliable_tracked(client, now_ms + 635u,
          0u, client_new_generation, 1,
          fragmented, sizeof(fragmented), &fragment_ticket) !=
              CNET_SCTP_SESSION_OK || fragment_ticket == 0u ||
      cnet_sctp_session_get_tx_usage(client, 0u,
          client_new_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.queued_bytes != sizeof(fragmented) ||
      !tx_usage.has_pending_message ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_EMPTY)
    goto cleanup;
  /* A copied pending message also prevents concurrent tracked admission
   * and direct packet sends. Rejections leave the original ticket, queue
   * payload, stream sequence, and all SCTP output unchanged. */
  reordered_ticket = UINT64_C(9876);
  first_size = sizeof(first);
  if (cnet_sctp_session_queue_reliable_tracked(client, now_ms + 635u,
          0u, client_new_generation, 1, "intruder", 8u,
          &reordered_ticket) != CNET_SCTP_SESSION_CAPACITY ||
      reordered_ticket != 0u ||
      cnet_sctp_session_send_reliable_tracked(client, now_ms + 635u,
          0u, client_new_generation, 1, "intruder", 8u,
          first, sizeof(first), &first_size,
          &reordered_ticket) != CNET_SCTP_SESSION_CAPACITY ||
      reordered_ticket != 0u || first_size != 0u ||
      cnet_sctp_session_get_tx_usage(client, 0u,
          client_new_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.queued_bytes != sizeof(fragmented) ||
      !tx_usage.has_pending_message)
    goto cleanup;

  /* The successful queue call COPIED the input: mutate every source byte
   * before any packet is pumped, so a borrowed buffer cannot pass. */
  for (i = 0u; i < sizeof(fragmented); ++i)
    fragmented[i] ^= 0xffu;
  first_size = 0u;
  if (cnet_sctp_session_pump_message(client, now_ms + 636u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      first_size == 0u ||
      deliver(server, client, now_ms + 637u, first, first_size) != 0 ||
      cnet_sctp_session_get_tx_usage(client, 0u,
          client_new_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.queued_bytes == 0u ||
      tx_usage.queued_bytes >= sizeof(fragmented) ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_EMPTY || acked.ticket != 0u ||
      cnet_sctp_session_poll(server, now_ms + 639u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_EMPTY)
    goto cleanup;
  /* Once any DATA fragment has a TSN, cancellation becomes BUSY and
   * must not discard the pending suffix or already retained TX ticket. */
  cancel_stage = 120;
  if (cnet_sctp_session_cancel_unsent_message(client, now_ms + 638u,
          0u, client_new_generation, &cancelled) !=
              CNET_SCTP_SESSION_BUSY ||
      cancelled.ticket != 0u || cancelled.generation != 0u ||
      cnet_sctp_session_get_tx_usage(client, 0u,
          client_new_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      !tx_usage.has_pending_message ||
      tx_usage.queued_bytes == 0u ||
      tx_usage.queued_bytes >= sizeof(fragmented) ||
      /* Delivering B already produced a real cumulative SACK, so its
       * retained DATA ticket is correctly retired. The copied E suffix
       * still blocks cancellation even without retained first-fragment
       * bytes. BUSY must not rely on retained_bytes being nonzero. */
      tx_usage.retained_bytes != 0u)
    goto cleanup;
  first_size = 0u;
  if (cnet_sctp_session_pump_message(client, now_ms + 640u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      first_size == 0u ||
      cnet_sctp_wire_decode(first, first_size,
          &sack_header, &sack_decoder) != CNET_SCTP_WIRE_OK ||
      cnet_sctp_wire_next_chunk(&sack_decoder, &sack_chunk) !=
          CNET_SCTP_WIRE_OK ||
      (sack_chunk.type != 0u && sack_chunk.type != 64u) ||
      sack_chunk.value_size < 12u)
    goto cleanup;
  last_fragment_tsn = ((uint32_t)sack_chunk.value[0u] << 24u) |
      ((uint32_t)sack_chunk.value[1u] << 16u) |
      ((uint32_t)sack_chunk.value[2u] << 8u) |
      (uint32_t)sack_chunk.value[3u];
  if (deliver(server, client, now_ms + 641u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, now_ms + 643u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
      event.stream_id != 0u || event.generation != server_new_generation ||
      !event.binary || !event.unordered ||
      event.message_size != sizeof(fragmented))
    goto cleanup;
  for (i = 0u; i < sizeof(fragmented); ++i)
    if (message[i] != (uint8_t)((i * 37u + (i >> 3u)) & 0xffu))
      goto cleanup;
  if (cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_OK ||
      acked.ticket != fragment_ticket ||
      acked.generation != client_new_generation ||
      acked.stream_id != 0u || acked.tsn != last_fragment_tsn ||
      acked.ppid != 53u || !acked.binary ||
      acked.message_size != sizeof(fragmented) ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_EMPTY || acked.ticket != 0u)
    goto cleanup;

  /* Second independently copied reliable message: retain B fragment,
   * deliver the LAST E fragment first, and verify its revocable Gap SACK
   * does NOT create a whole-message ACK receipt. After delayed B arrives,
   * the peer's cumulative SACK must release the one matching ticket.
   * The original input remains caller-owned and must not be borrowed. */
  if (cnet_sctp_session_queue_reliable_tracked(client, now_ms + 648u,
          0u, client_new_generation, 1,
          fragmented, sizeof(fragmented), &reordered_ticket) !=
              CNET_SCTP_SESSION_OK ||
      reordered_ticket <= fragment_ticket)
    goto cleanup;
  first_size = 0u;
  if (cnet_sctp_session_pump_message(client, now_ms + 649u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      first_size == 0u || first_size > sizeof(stale_data) ||
      cnet_sctp_wire_decode(first, first_size, &sack_header,
          &sack_decoder) != CNET_SCTP_WIRE_OK ||
      cnet_sctp_wire_next_chunk(&sack_decoder, &sack_chunk) !=
          CNET_SCTP_WIRE_OK ||
      (sack_chunk.type != 0u && sack_chunk.type != 64u) ||
      sack_chunk.value_size < 12u)
    goto cleanup;
  withheld_tsn = ((uint32_t)sack_chunk.value[0u] << 24u) |
      ((uint32_t)sack_chunk.value[1u] << 16u) |
      ((uint32_t)sack_chunk.value[2u] << 8u) |
      (uint32_t)sack_chunk.value[3u];
  stale_size = first_size;
  memcpy(stale_data, first, stale_size);
  third_size = 0u;
  if (cnet_sctp_session_pump_message(client, now_ms + 650u,
          third, sizeof(third), &third_size) != CNET_SCTP_SESSION_OK ||
      third_size == 0u ||
      cnet_sctp_wire_decode(third, third_size,
          &sack_header, &sack_decoder) != CNET_SCTP_WIRE_OK ||
      cnet_sctp_wire_next_chunk(&sack_decoder, &sack_chunk) !=
          CNET_SCTP_WIRE_OK ||
      (sack_chunk.type != 0u && sack_chunk.type != 64u) ||
      sack_chunk.value_size < 12u)
    goto cleanup;
  reordered_last_tsn = ((uint32_t)sack_chunk.value[0u] << 24u) |
      ((uint32_t)sack_chunk.value[1u] << 16u) |
      ((uint32_t)sack_chunk.value[2u] << 8u) |
      (uint32_t)sack_chunk.value[3u];
  if (reordered_last_tsn != withheld_tsn + 1u)
    goto cleanup;
  /* The last fragment is only Gap-ACKed: the cumulative ACK remains
   * before B. No full-message receipt is eligible, even though every
   * message fragment now has a TSN and E was physically received. */
  second_size = 0u;
  if (cnet_sctp_session_input(server, now_ms + 651u,
          third, third_size, second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      second_size == 0u ||
      cnet_sctp_wire_decode(second, second_size,
          &sack_header, &sack_decoder) != CNET_SCTP_WIRE_OK ||
      cnet_sctp_wire_next_chunk(&sack_decoder, &sack_chunk) !=
          CNET_SCTP_WIRE_OK ||
      sack_chunk.type != 3u || sack_chunk.value_size < 16u ||
      (sack_chunk.value[8u] == 0u && sack_chunk.value[9u] == 0u))
    goto cleanup;
  first_size = 0u;
  if (cnet_sctp_session_input(client, now_ms + 652u,
          second, second_size, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      first_size != 0u ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_EMPTY || acked.ticket != 0u ||
      cnet_sctp_session_get_tx_usage(client, 0u,
          client_new_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.gap_acked_bytes == 0u ||
      tx_usage.retained_bytes != sizeof(fragmented) ||
      cnet_sctp_session_poll(server, now_ms + 653u, &event,
          message, sizeof(message), fourth, sizeof(fourth),
          &fourth_size) != CNET_SCTP_SESSION_EMPTY)
    goto cleanup;
  second_size = 0u;
  if (cnet_sctp_session_input(server, now_ms + 654u,
          stale_data, stale_size, second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      second_size == 0u)
    goto cleanup;
  first_size = 0u;
  if (cnet_sctp_session_input(client, now_ms + 655u,
          second, second_size, first, sizeof(first),
          &first_size) != CNET_SCTP_SESSION_OK ||
      first_size != 0u ||
      cnet_sctp_session_poll(server, now_ms + 656u, &event,
          message, sizeof(message), fourth, sizeof(fourth),
          &fourth_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
      event.stream_id != 0u || event.generation != server_new_generation ||
      !event.binary || !event.unordered ||
      event.message_size != sizeof(fragmented) ||
      memcmp(message, fragmented, sizeof(fragmented)) != 0 ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_OK ||
      acked.ticket != reordered_ticket ||
      acked.tsn != reordered_last_tsn ||
      acked.generation != client_new_generation ||
      acked.stream_id != 0u || acked.message_size != sizeof(fragmented) ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_EMPTY || acked.ticket != 0u)
    goto cleanup;

  /* Tracked reliable queued message: cancel while NO DATA TSN exists.
   * Wrong generation and foreign Owner are failure-atomic. Success returns
   * the original nonzero receipt token and generation exactly once, and
   * must never later appear as a cumulative ACK. */
  cancel_stage = 130;
  if (cnet_sctp_session_queue_reliable_tracked(client, now_ms + 657u,
          0u, client_new_generation, 1,
          "cancel-me", 9u, &cancelled_ticket) != CNET_SCTP_SESSION_OK ||
      cancelled_ticket == 0u ||
      cnet_sctp_session_cancel_unsent_message(client, now_ms + 657u,
          0u, client_old_generation, &cancelled) !=
              CNET_SCTP_SESSION_STALE ||
      cancelled.generation != 0u || cancelled.ticket != 0u)
    goto cleanup;
  cancel_stage = 131;
  client_context.owner_admitted = 0;
  if (cnet_sctp_session_cancel_unsent_message(client, now_ms + 657u,
          0u, client_new_generation, &cancelled) !=
              CNET_SCTP_SESSION_OWNER ||
      cancelled.ticket != 0u || cancelled.generation != 0u)
    goto cleanup;
  cancel_stage = 132;
  client_context.owner_admitted = 1;
  if (cnet_sctp_session_cancel_unsent_message(client, now_ms + 658u,
          0u, client_new_generation, &cancelled) !=
              CNET_SCTP_SESSION_OK ||
      cancelled.generation != client_new_generation ||
      cancelled.ticket != cancelled_ticket ||
      cancelled.stream_id != 0u || cancelled.ppid != 53u ||
      cancelled.message_size != 9u || !cancelled.binary ||
      !cancelled.unordered ||
      cancelled.reliability.mode != CNET_SCTP_SESSION_RELIABLE ||
      cancelled.reliability.value != 0u ||
      cnet_sctp_session_cancel_unsent_message(client, now_ms + 658u,
          0u, client_new_generation, &cancelled) !=
              CNET_SCTP_SESSION_EMPTY ||
      cancelled.ticket != 0u || cancelled.generation != 0u ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
              CNET_SCTP_SESSION_EMPTY || acked.ticket != 0u ||
      cnet_sctp_session_get_tx_usage(client, 0u,
          client_new_generation, &tx_usage) != CNET_SCTP_SESSION_OK ||
      tx_usage.queued_bytes != 0u ||
      tx_usage.has_pending_message || tx_usage.retained_bytes != 0u)
    goto cleanup;

  /* The cancelled token can never be reused, but its freed slot can
   * immediately admit the next legitimate reliable send on the same SID. */
  cancel_stage = 133;
  first_size = 0u;
  if (cnet_sctp_session_send_reliable_tracked(client, now_ms + 660u,
          0u, client_new_generation, 1, "after", 5u,
          first, sizeof(first), &first_size, &post_cancel_ticket) !=
              CNET_SCTP_SESSION_OK ||
      post_cancel_ticket <= cancelled_ticket ||
      deliver(server, client, now_ms + 661u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, now_ms + 663u, &event,
          message, sizeof(message), second, sizeof(second),
          &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
      event.message_size != 5u || memcmp(message, "after", 5u) != 0 ||
      cnet_sctp_session_take_reliable_ack(client, &acked) !=
          CNET_SCTP_SESSION_OK ||
      acked.ticket != post_cancel_ticket ||
      acked.generation != client_new_generation ||
      acked.message_size != 5u)
    goto cleanup;

  cancel_stage = 134;
  fourth_size = 0u;
  if (cnet_sctp_session_close_channel(client, now_ms + 665u, 0u,
          fourth, sizeof(fourth), &fourth_size,
          &close_generation) != CNET_SCTP_SESSION_OK ||
      fourth_size == 0u || close_generation != client_new_generation)
    goto cleanup;

  result = 0;

cleanup:
  if (result != 0)
    fprintf(stderr, "SCTP installed cancel fixture failed at stage %d\n",
            cancel_stage);
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


/* Installed-headers-only end-to-end proof that a LOCAL PR-SCTP
 * MESSAGE_ABANDONED is not a PEER cumulative-progress receipt. Retain the
 * second result across a real bilateral RFC6525 close/reopen on the SAME SID.
 * Never include or introspect a private Association/DATA/DCEP structure. */
static int session_forward_progress(void) {
  test_context cc = {UINT32_C(0x11223344), UINT32_C(0x10203040),
      UINT32_C(0x13579bdf), 1};
  test_context sc = {UINT32_C(0x55667788), UINT32_C(0x50607080),
      UINT32_C(0x13579bdf), 1};
  cnet_sctp_session_create_config cconf = session_config(5000u, 1, &cc);
  cnet_sctp_session_create_config sconf = session_config(5001u, 0, &sc);
  cnet_sctp_session *client = NULL, *server = NULL;
  cnet_sctp_session_event event = {0};
  cnet_sctp_session_forward_progress progress = {0};
  uint8_t first[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t second[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t third[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t fourth[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t message[128];
  size_t nf = 0u, ns = 0u, nt = 0u, nq = 0u;
  uint64_t old_generation = 0u, new_generation = 0u;
  uint64_t peer_generation = 0u, close_generation = 0u, deadline = 0u;
  uint32_t original_tsn = 0u;
  int has_deadline = 0, stage = 0, result = 40;

  if (cnet_sctp_session_create(&cconf, 0u, &client) !=
          CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_create(&sconf, 0u, &server) !=
          CNET_SCTP_SESSION_OK)
    goto cleanup;
  stage = 1;
  if (cnet_sctp_session_start(client, 1u, first, sizeof(first),
          &nf) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(server, 2u, first, nf, second,
          sizeof(second), &ns) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(client, 3u, second, ns, third,
          sizeof(third), &nt) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(server, 4u, third, nt, fourth,
          sizeof(fourth), &nq) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(client, 5u, fourth, nq, first,
          sizeof(first), &nf) != CNET_SCTP_SESSION_OK || nf != 0u)
    goto cleanup;
  stage = 2;
  nf = 0u;
  if (cnet_sctp_session_open_channel(client, 6u, 2u,
          CNET_SCTP_SESSION_CHANNEL_RETRANSMIT_ORDERED, 0u, 0u,
          "pr", 2u, NULL, 0u, first, sizeof(first), &nf) !=
          CNET_SCTP_SESSION_OK || nf == 0u ||
      deliver(server, client, 7u, first, nf) != 0 ||
      cnet_sctp_session_poll(server, 9u, &event,
          message, sizeof(message), second, sizeof(second),
          &ns) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.stream_id != 2u || event.generation == 0u || ns == 0u)
    goto cleanup;
  peer_generation = event.generation;
  if (deliver(client, server, 10u, second, ns) != 0 ||
      cnet_sctp_session_poll(client, 12u, &event,
          message, sizeof(message), first, sizeof(first),
          &nf) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.stream_id != 2u || event.generation == 0u)
    goto cleanup;
  old_generation = event.generation;

  stage = 3;
  nf = 0u;
  if (cnet_sctp_session_send(client, 13u, 2u, 1,
          "lost", 4u, first, sizeof(first), &nf) !=
          CNET_SCTP_SESSION_OK || nf == 0u ||
      cnet_sctp_session_next_deadline(client, &deadline,
          &has_deadline) != CNET_SCTP_SESSION_OK ||
      !has_deadline || deadline <= 13u ||
      cnet_sctp_session_take_forward_progress(client, &progress) !=
          CNET_SCTP_SESSION_EMPTY || progress.generation != 0u)
    goto cleanup;
  /* Withhold the actual admitted DATA; the real Owner must expire RTX0
   * and send negotiated RFC3758/RFC8260 forward control, not retry DATA. */
  stage = 4;
  nf = 0u;
  if (cnet_sctp_session_advance(client, deadline, first,
          sizeof(first), &nf) != CNET_SCTP_SESSION_OK ||
      nf == 0u || (first[12u] != 192u && first[12u] != 194u) ||
      cnet_sctp_session_poll(client, deadline, &event,
          message, sizeof(message), second, sizeof(second),
          &ns) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED ||
      event.stream_id != 2u || event.generation != old_generation ||
      !event.has_tsn || event.message_size != 4u ||
      event.first_tsn != event.last_tsn)
    goto cleanup;
  original_tsn = event.last_tsn;
  if (cnet_sctp_session_take_forward_progress(client, &progress) !=
          CNET_SCTP_SESSION_EMPTY || progress.last_tsn != 0u ||
      cnet_sctp_session_poll(client, deadline, &event,
          message, sizeof(message), second, sizeof(second),
          &ns) != CNET_SCTP_SESSION_EMPTY)
    goto cleanup;

  stage = 5;
  ns = 0u;
  if (cnet_sctp_session_input(server, deadline + 1u, first, nf, second,
          sizeof(second), &ns) != CNET_SCTP_SESSION_OK || ns == 0u ||
      cnet_sctp_session_input(client, deadline + 2u, second, ns, third,
          sizeof(third), &nt) != CNET_SCTP_SESSION_OK || nt != 0u)
    goto cleanup;

  /* Deliberately leave the already cumulative-ACK-qualified progress
   * result unread, then retire and reopen its DCEP SID generation. */
  stage = 6;
  nf = 0u;
  if (cnet_sctp_session_close_channel(client, deadline + 3u, 2u,
          first, sizeof(first), &nf,
          &close_generation) != CNET_SCTP_SESSION_OK ||
      close_generation != old_generation || nf == 0u ||
      cnet_sctp_session_input(server, deadline + 4u, first, nf,
          second, sizeof(second), &ns) != CNET_SCTP_SESSION_OK ||
      ns == 0u ||
      cnet_sctp_session_poll(server, deadline + 5u, &event,
          message, sizeof(message), fourth, sizeof(fourth),
          &nq) != CNET_SCTP_SESSION_EMPTY ||
      cnet_sctp_session_input(client, deadline + 6u, second, ns,
          third, sizeof(third), &nt) != CNET_SCTP_SESSION_OK ||
      nt == 0u ||
      cnet_sctp_session_poll(client, deadline + 7u, &event,
          message, sizeof(message), fourth, sizeof(fourth),
          &nq) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSED ||
      event.generation != old_generation ||
      cnet_sctp_session_input(server, deadline + 8u, third, nt,
          fourth, sizeof(fourth), &nq) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_poll(server, deadline + 9u, &event,
          message, sizeof(message), second, sizeof(second),
          &ns) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSED ||
      event.generation != peer_generation)
    goto cleanup;

  stage = 7;
  nf = 0u;
  if (cnet_sctp_session_open_channel(client, deadline + 10u, 2u,
          CNET_SCTP_SESSION_CHANNEL_RELIABLE_ORDERED, 0u, 0u,
          "fresh", 5u, NULL, 0u, first, sizeof(first), &nf) !=
          CNET_SCTP_SESSION_OK || nf == 0u ||
      deliver(server, client, deadline + 11u, first, nf) != 0 ||
      cnet_sctp_session_poll(server, deadline + 13u, &event,
          message, sizeof(message), second, sizeof(second),
          &ns) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.generation == peer_generation || event.stream_id != 2u ||
      ns == 0u ||
      deliver(client, server, deadline + 14u, second, ns) != 0 ||
      cnet_sctp_session_poll(client, deadline + 16u, &event,
          message, sizeof(message), fourth, sizeof(fourth),
          &nq) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.generation == old_generation || event.stream_id != 2u)
    goto cleanup;
  new_generation = event.generation;

  stage = 8;
  if (cnet_sctp_session_take_forward_progress(client, &progress) !=
          CNET_SCTP_SESSION_OK ||
      progress.generation != old_generation ||
      progress.generation == new_generation ||
      progress.stream_id != 2u || progress.message_size != 4u ||
      progress.first_tsn != original_tsn ||
      progress.last_tsn != original_tsn || progress.ppid != 53u ||
      progress.reliability.mode != CNET_SCTP_SESSION_RETRANSMITS ||
      progress.reliability.value != 0u ||
      progress.abandon_reason != CNET_SCTP_SESSION_ABANDON_RETRANSMITS ||
      !progress.binary || progress.unordered ||
      cnet_sctp_session_take_forward_progress(client, &progress) !=
          CNET_SCTP_SESSION_EMPTY ||
      progress.generation != 0u)
    goto cleanup;
  result = 0;

cleanup:
  if (result != 0)
    fprintf(stderr, "installed forward progress fixture failed at stage %d\n",
            stage);
  if (client) {
    if (cnet_sctp_session_stop(client) != CNET_SCTP_SESSION_OK ||
        !cnet_sctp_session_drained(client) ||
        cnet_sctp_session_destroy(client) != CNET_SCTP_SESSION_OK)
      result = 41;
  }
  if (server) {
    if (cnet_sctp_session_stop(server) != CNET_SCTP_SESSION_OK ||
        !cnet_sctp_session_drained(server) ||
        cnet_sctp_session_destroy(server) != CNET_SCTP_SESSION_OK)
      result = 42;
  }
  return result;
}


/* Installed public-ABI-only terminal failure qualification. A peer never
 * receives the original DATA (nor, for PR, either retained FORWARD packet).
 * Failure is driven ONLY by monotonic Owner deadlines, not synthetic ACKs.
 * A genuine retry-exhaustion result must retain the original SID/generation
 * and must never reclassify a successful ACK/peer-progress receipt. */
static int session_terminal_retry_failure(int forward_case) {
  test_context cc = {UINT32_C(0x11223344), UINT32_C(0x10203040),
      UINT32_C(0x13579bdf), 1};
  test_context sc = {UINT32_C(0x55667788), UINT32_C(0x50607080),
      UINT32_C(0x13579bdf), 1};
  cnet_sctp_session_create_config cfg = session_config(5000u, 1, &cc);
  cnet_sctp_session_create_config peer = session_config(5001u, 0, &sc);
  cnet_sctp_session *client = NULL, *server = NULL;
  cnet_sctp_session_event event = {0};
  cnet_sctp_session_reliable_ack ack = {0};
  cnet_sctp_session_forward_progress progress = {0};
  cnet_sctp_session_terminal_failure failed = {0};
  uint8_t first[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t second[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t third[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t fourth[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t payload[128];
  size_t nf = 0u, ns = 0u, nt = 0u, nq = 0u;
  uint16_t sid = forward_case ? 2u : 0u;
  uint64_t generation = 0u, ticket = 0u, due = 0u, confirmed_ticket = 0u;
  uint32_t abandoned_tsn = 0u;
  int has_due = 0, stage = 0, result = forward_case ? 43 : 44;
  cfg.max_retransmits = 1u;
  peer.max_retransmits = 1u;
  if (cnet_sctp_session_create(&cfg, 0u, &client) !=
          CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_create(&peer, 0u, &server) !=
          CNET_SCTP_SESSION_OK)
    goto cleanup;
  stage = 1;
  if (cnet_sctp_session_start(client, 1u, first, sizeof(first), &nf) !=
          CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(server, 2u, first, nf,
          second, sizeof(second), &ns) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(client, 3u, second, ns,
          third, sizeof(third), &nt) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(server, 4u, third, nt,
          fourth, sizeof(fourth), &nq) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(client, 5u, fourth, nq,
          first, sizeof(first), &nf) != CNET_SCTP_SESSION_OK || nf != 0u)
    goto cleanup;
  stage = 2;
  nf = 0u;
  if (cnet_sctp_session_open_channel(client, 6u, sid,
          forward_case ? CNET_SCTP_SESSION_CHANNEL_RETRANSMIT_ORDERED :
          CNET_SCTP_SESSION_CHANNEL_RELIABLE_ORDERED, 0u, 0u,
          "failure", 7u, NULL, 0u, first, sizeof(first),
          &nf) != CNET_SCTP_SESSION_OK || nf == 0u ||
      deliver(server, client, 7u, first, nf) != 0 ||
      cnet_sctp_session_poll(server, 9u, &event, payload,
          sizeof(payload), second, sizeof(second), &ns) !=
          CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.stream_id != sid || ns == 0u ||
      deliver(client, server, 10u, second, ns) != 0 ||
      cnet_sctp_session_poll(client, 12u, &event, payload,
          sizeof(payload), first, sizeof(first), &nf) !=
          CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN ||
      event.stream_id != sid || event.generation == 0u)
    goto cleanup;
  generation = event.generation;
  stage = 3;
  /* An earlier genuinely cumulatively ACKed reliable ticket must NEVER be
   * reclassified as terminal failure when a later DATA attempt exhausts its
   * retry budget. Keep this success receipt unread until AFTER failure. */
  if (!forward_case) {
    nf = 0u;
    if (cnet_sctp_session_send_reliable_tracked(client, 15u, sid,
            generation, 1, "good", 4u, first, sizeof(first),
            &nf, &confirmed_ticket) != CNET_SCTP_SESSION_OK ||
        confirmed_ticket == 0u || nf == 0u ||
        deliver(server, client, 16u, first, nf) != 0 ||
        cnet_sctp_session_poll(server, 18u, &event,
            payload, sizeof(payload), second, sizeof(second),
            &ns) != CNET_SCTP_SESSION_OK ||
        event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE ||
        event.stream_id != sid || event.message_size != 4u ||
        memcmp(payload, "good", 4u) != 0)
      goto cleanup;
  }
  nf = 0u;
  if (forward_case) {
    if (cnet_sctp_session_send(client, 20u, sid, 1,
            "drop", 4u, first, sizeof(first), &nf) !=
            CNET_SCTP_SESSION_OK || nf == 0u)
      goto cleanup;
  } else {
    if (cnet_sctp_session_send_reliable_tracked(client, 20u, sid,
            generation, 1, "drop", 4u, first,
            sizeof(first), &nf, &ticket) != CNET_SCTP_SESSION_OK ||
        nf == 0u || ticket == 0u)
      goto cleanup;
  }
  if (cnet_sctp_session_take_terminal_failure(client, &failed) !=
          CNET_SCTP_SESSION_EMPTY || failed.generation != 0u ||
      cnet_sctp_session_next_deadline(client, &due,
          &has_due) != CNET_SCTP_SESSION_OK ||
      !has_due || due <= 20u)
    goto cleanup;
  stage = 4;
  ns = 0u;
  if (cnet_sctp_session_advance(client, due, second,
          sizeof(second), &ns) != CNET_SCTP_SESSION_OK || ns == 0u)
    goto cleanup;
  if (forward_case) {
    if (second[12u] != 194u ||
        cnet_sctp_session_poll(client, due, &event, payload,
            sizeof(payload), third, sizeof(third),
            &nt) != CNET_SCTP_SESSION_OK ||
        event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED ||
        !event.has_tsn || event.generation != generation ||
        event.stream_id != sid || event.message_size != 4u ||
        cnet_sctp_session_take_forward_progress(client, &progress) !=
            CNET_SCTP_SESSION_EMPTY)
      goto cleanup;
    abandoned_tsn = event.last_tsn;
    if (cnet_sctp_session_next_deadline(client, &due, &has_due) !=
            CNET_SCTP_SESSION_OK || !has_due)
      goto cleanup;
    nt = 0u;
    if (cnet_sctp_session_advance(client, due, third,
            sizeof(third), &nt) != CNET_SCTP_SESSION_OK ||
        nt != ns || memcmp(second, third, ns) != 0)
      goto cleanup;
  } else {
    /* Preserve the earlier *real* cumulatively ACKed "good" receipt
     * unread. Its presence must not be treated as a failure of the new
     * "drop" DATA retransmission, nor may it be consumed before the later
     * terminal-exhaustion classification is checked. */
    if (ns != nf || memcmp(first, second, ns) != 0)
      goto cleanup;
  }
  stage = 5;
  if (cnet_sctp_session_next_deadline(client, &due, &has_due) !=
          CNET_SCTP_SESSION_OK || !has_due)
    goto cleanup;
  nq = sizeof(fourth);
  if (cnet_sctp_session_advance(client, due, fourth,
          sizeof(fourth), &nq) != CNET_SCTP_SESSION_TRANSPORT ||
      nq != 0u)
    goto cleanup;
  if (cnet_sctp_session_take_terminal_failure(client, &failed) !=
          CNET_SCTP_SESSION_OK ||
      failed.reason != CNET_SCTP_SESSION_TERMINAL_LOCAL_RETRY_EXHAUSTED ||
      failed.generation != generation || failed.stream_id != sid ||
      failed.ppid != 53u || failed.message_size != 4u ||
      !failed.binary || !failed.has_final_tsn ||
      failed.kind != (forward_case ?
          CNET_SCTP_SESSION_TERMINAL_FORWARD_UNCONFIRMED :
          CNET_SCTP_SESSION_TERMINAL_RELIABLE_UNACKED) ||
      failed.ticket != (forward_case ? 0u : ticket) ||
      (forward_case && failed.last_tsn != abandoned_tsn))
    goto cleanup;
  if (cnet_sctp_session_take_terminal_failure(client, &failed) !=
          CNET_SCTP_SESSION_EMPTY || failed.generation != 0u ||
      cnet_sctp_session_take_forward_progress(client, &progress) !=
          CNET_SCTP_SESSION_EMPTY)
    goto cleanup;
  if (forward_case) {
    if (cnet_sctp_session_take_reliable_ack(client, &ack) !=
            CNET_SCTP_SESSION_EMPTY)
      goto cleanup;
  } else {
    if (cnet_sctp_session_take_reliable_ack(client, &ack) !=
            CNET_SCTP_SESSION_OK ||
        ack.ticket != confirmed_ticket ||
        ack.ticket == ticket || ack.generation != generation ||
        ack.stream_id != sid || ack.message_size != 4u ||
        ack.ppid != 53u || !ack.binary ||
        cnet_sctp_session_take_reliable_ack(client, &ack) !=
            CNET_SCTP_SESSION_EMPTY || ack.ticket != 0u)
      goto cleanup;
  }
  result = 0;
cleanup:
  if (result != 0)
    fprintf(stderr, "installed terminal retry fixture %d failed stage %d\n",
            forward_case, stage);
  if (client) {
    if (cnet_sctp_session_stop(client) != CNET_SCTP_SESSION_OK ||
        !cnet_sctp_session_drained(client) ||
        cnet_sctp_session_destroy(client) != CNET_SCTP_SESSION_OK)
      result = 45;
  }
  if (server) {
    if (cnet_sctp_session_stop(server) != CNET_SCTP_SESSION_OK ||
        !cnet_sctp_session_drained(server) ||
        cnet_sctp_session_destroy(server) != CNET_SCTP_SESSION_OK)
      result = 46;
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
  {
    const int roundtrip = session_round_trip();
    if (roundtrip != 0) return roundtrip;
  }
  {
    const int progress = session_forward_progress();
    if (progress != 0) return progress;
  }
  {
    const int reliable_failure = session_terminal_retry_failure(0);
    if (reliable_failure != 0) return reliable_failure;
  }
  return session_terminal_retry_failure(1);
}
