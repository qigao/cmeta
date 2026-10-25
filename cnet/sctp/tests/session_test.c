#include "sctp_session_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Deliberately non-cryptographic fixture; the separate GmSSL lane qualifies
 * the actual cookie provider. This test isolates the Owner/ULP transaction. */
typedef struct test_context {
  uint32_t tag, tsn, key;
  int owner;
} test_context;

static int owned(void *value) { return ((test_context *)value)->owner; }
static int entropy(void *value, uint8_t *bytes, size_t size) {
  test_context *context = (test_context *)value;
  if (size != 8u) return -1;
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
static int fake_mac(void *value, const uint8_t *bytes, size_t size,
    uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  test_context *context = (test_context *)value;
  uint32_t state = context->key;
  size_t i;
  for (i = 0u; i < size; ++i) state = state * UINT32_C(65599) + bytes[i];
  for (i = 0u; i < CNET_SCTP_ASSOC_MAC_BYTES; ++i) {
    state = state * UINT32_C(1664525) + UINT32_C(1013904223);
    out[i] = (uint8_t)(state >> 24);
  }
  return 0;
}
static int fake_verify(void *value, const uint8_t *bytes, size_t size,
    const uint8_t claimed[CNET_SCTP_ASSOC_MAC_BYTES]) {
  uint8_t actual[CNET_SCTP_ASSOC_MAC_BYTES] = {0};
  uint8_t different = 0u;
  size_t i;
  if (fake_mac(value, bytes, size, actual) != 0) return -1;
  for (i = 0u; i < sizeof(actual); ++i)
    different |= (uint8_t)(actual[i] ^ claimed[i]);
  return different == 0u ? 0 : -1;
}

static cnet_sctp_session_config config(uint16_t port, int client,
    test_context *context) {
  cnet_sctp_session_config result = {0};
  result.association.local_port = port;
  result.association.remote_port = port == 5000u ? 5001u : 5000u;
  result.association.receive_window = 8192u;
  result.association.outbound_streams = 4u;
  result.association.inbound_streams = 4u;
  result.association.t1_ms = 100u;
  result.association.t1_max_ms = 400u;
  result.association.cookie_lifetime_ms = 1000u;
  result.association.peer_binding = UINT64_C(0x0123456789abcdef);
  result.association.max_retransmits = 3u;
  result.association.is_owner = owned;
  result.association.entropy = entropy;
  result.association.mac = fake_mac;
  result.association.verify_mac = fake_verify;
  result.association.provider_context = context;
  result.is_dtls_client = (uint8_t)client;
  return result;
}

static void ready_pair(cnet_sctp_session *a, cnet_sctp_session *b,
    test_context *ca, test_context *cb) {
  cnet_sctp_session_config ac, bc;
  uint8_t first[1200], second[1200], third[1200], fourth[1200];
  size_t nf = 0u, ns = 0u, nt = 0u, nq = 0u;
  ca->tag = UINT32_C(0x11223344);
  ca->tsn = UINT32_C(0x10203040);
  ca->key = UINT32_C(0x13579bdf);
  ca->owner = 1;
  cb->tag = UINT32_C(0x55667788);
  cb->tsn = UINT32_C(0x50607080);
  cb->key = ca->key;
  cb->owner = 1;
  ac = config(5000u, 1, ca);
  bc = config(5001u, 0, cb);
  assert(cnet_sctp_session_init(a, &ac, 0u) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_init(b, &bc, 0u) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_start(a, 1u, first, sizeof(first), &nf) ==
      CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_input(b, 2u, first, nf,
      second, sizeof(second), &ns) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_input(a, 3u, second, ns,
      third, sizeof(third), &nt) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_input(b, 4u, third, nt,
      fourth, sizeof(fourth), &nq) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_input(a, 5u, fourth, nq,
      first, sizeof(first), &nf) == CNET_SCTP_SESSION_OK && nf == 0u);
}

static void deliver(cnet_sctp_session *receiver, cnet_sctp_session *sender,
    uint64_t now, const uint8_t *packet, size_t packet_size) {
  uint8_t reply[1200], final[1200];
  size_t reply_size = 0u, final_size = 0u;
  assert(cnet_sctp_session_input(receiver, now, packet, packet_size,
      reply, sizeof(reply), &reply_size) == CNET_SCTP_SESSION_OK);
  if (reply_size != 0u)
    assert(cnet_sctp_session_input(sender, now + 1u, reply, reply_size,
        final, sizeof(final), &final_size) == CNET_SCTP_SESSION_OK);
  assert(final_size == 0u);
}

static void open_channel_with_policy(cnet_sctp_session *client,
    cnet_sctp_session *server, uint64_t now,
    uint16_t stream, uint8_t type, uint32_t reliability,
    uint64_t *client_generation, uint64_t *server_generation) {
  cnet_sctp_session_event event = {0};
  uint8_t packet[1200], reply[1200], payload[512];
  size_t packet_size = 0u, reply_size = 0u;
  assert(cnet_sctp_session_open_channel(client, now, stream, type,
      0u, reliability, "data", 4u, NULL, 0u,
      packet, 1u, &packet_size) == CNET_SCTP_SESSION_CAPACITY);
  assert(client->dcep.channels[stream].state == 0u);
  assert(cnet_sctp_session_open_channel(client, now, stream, type,
      0u, reliability, "data", 4u, NULL, 0u,
      packet, sizeof(packet), &packet_size) == CNET_SCTP_SESSION_OK);
  deliver(server, client, now + 1u, packet, packet_size);
  assert(cnet_sctp_session_poll(server, now + 3u, &event,
      payload, sizeof(payload), reply, sizeof(reply), &reply_size) ==
      CNET_SCTP_SESSION_OK);
  assert(event.kind == CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN &&
      !event.locally_initiated && reply_size != 0u);
  *server_generation = event.generation;
  deliver(client, server, now + 4u, reply, reply_size);
  assert(cnet_sctp_session_poll(client, now + 6u, &event,
      payload, sizeof(payload), packet, sizeof(packet), &packet_size) ==
      CNET_SCTP_SESSION_OK);
  assert(event.kind == CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN &&
      event.locally_initiated && packet_size == 0u);
  *client_generation = event.generation;
}

static void open_channel(cnet_sctp_session *client,
    cnet_sctp_session *server, uint64_t now,
    uint64_t *client_generation, uint64_t *server_generation) {
  open_channel_with_policy(client, server, now, 0u, 0x80u, 0u,
      client_generation, server_generation);
}

static void session_open_message_reset_reuse(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_event event = {0};
  uint8_t packet[1200], response[1200], final[1200], payload[512];
  size_t packet_size = 0u, response_size = 0u, final_size = 0u;
  uint64_t a_first = 0u, b_first = 0u, close_generation = 0u;
  uint64_t a_second = 0u, b_second = 0u;

  ready_pair(&a, &b, &ca, &cb);
  open_channel(&a, &b, 6u, &a_first, &b_first);
  assert(a_first != 0u && b_first != 0u);

  assert(cnet_sctp_session_send(&a, 20u, 0u, 1,
      "one", 3u, packet, sizeof(packet), &packet_size) ==
      CNET_SCTP_SESSION_OK);
  deliver(&b, &a, 21u, packet, packet_size);
  assert(cnet_sctp_session_poll(&b, 23u, &event,
      payload, sizeof(payload), response, sizeof(response), &response_size) ==
      CNET_SCTP_SESSION_OK);
  assert(event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE && event.binary &&
      event.stream_id == 0u && event.generation == b_first &&
      event.message_size == 3u && event.ppid == 53u && event.interleaved &&
      memcmp(payload, "one", 3u) == 0);

  assert(cnet_sctp_session_close_channel(&a, 24u, 0u,
      packet, 1u, &packet_size, &close_generation) ==
      CNET_SCTP_SESSION_CAPACITY);
  assert(a.dcep.channels[0].state == 2u);
  assert(cnet_sctp_session_close_channel(&a, 24u, 0u,
      packet, sizeof(packet), &packet_size, &close_generation) ==
      CNET_SCTP_SESSION_OK);
  assert(close_generation == a_first);
  assert(cnet_sctp_session_input(&b, 25u, packet, packet_size,
      response, sizeof(response), &response_size) == CNET_SCTP_SESSION_OK);
  assert(response_size != 0u);
  /* First peer event starts the matching DCEP retirement generation. */
  assert(cnet_sctp_session_poll(&b, 26u, &event,
      payload, sizeof(payload), final, sizeof(final), &final_size) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(b.closing_generation[0] == b_first);
  assert(cnet_sctp_session_input(&a, 27u, response, response_size,
      final, sizeof(final), &final_size) == CNET_SCTP_SESSION_OK);
  assert(final_size != 0u);
  assert(cnet_sctp_session_poll(&a, 28u, &event,
      payload, sizeof(payload), packet, sizeof(packet), &packet_size) ==
      CNET_SCTP_SESSION_OK);
  assert(event.kind == CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSED &&
      event.generation == a_first);
  assert(cnet_sctp_session_input(&b, 29u, final, final_size,
      packet, sizeof(packet), &packet_size) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_poll(&b, 30u, &event,
      payload, sizeof(payload), response, sizeof(response), &response_size) ==
      CNET_SCTP_SESSION_OK);
  assert(event.kind == CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSED &&
      event.generation == b_first);

  open_channel(&a, &b, 31u, &a_second, &b_second);
  assert(a_second != a_first && b_second != b_first);
  assert(cnet_sctp_dcep_close_reset_result(&a.dcep, 0u, a_first, 0, 1) ==
      CNET_SCTP_DCEP_STALE);
  assert(cnet_sctp_dcep_ready(&a.dcep, 0u));

  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_drained(&a) && cnet_sctp_session_drained(&b));
  assert(cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}

static void partial_loss_reports_abandonment_then_reliable_continues(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_event event = {0};
  cnet_sctp_session_tx_usage usage = {0};
  cnet_sctp_session_forward_progress progress = {0};
  cnet_sctp_session_unsent_cancelled cancelled = {0};
  uint8_t lost[1200], forward[1200], sack[1200], payload[32];
  uint8_t reliable[1200], response[1200];
  size_t lost_size = 0u, forward_size = 0u, sack_size = 0u;
  size_t reliable_size = 0u, response_size = 0u;
  uint64_t reliable_a = 0u, reliable_b = 0u;
  uint64_t partial_a = 0u, partial_b = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  open_channel_with_policy(&a, &b, 6u, 0u, 0x80u, 0u,
      &reliable_a, &reliable_b);
  open_channel_with_policy(&a, &b, 14u, 2u, 0x01u, 0u,
      &partial_a, &partial_b);

  /* Drop the admitted maxRetransmits=0 message. The first T3 expiry must
   * publish FORWARD-TSN and an exactly-once Owner event, not retransmit DATA. */
  assert(cnet_sctp_session_send(&a, 22u, 2u, 1, "drop", 4u,
      lost, sizeof(lost), &lost_size) == CNET_SCTP_SESSION_OK);
  assert(lost_size != 0u);
  /* A real PR-SCTP DATA TSN was already emitted, even though there is no
   * remaining pending copy. Local pre-TSN cancellation must return BUSY,
   * not EMPTY, and preserve the actual retransmission-budget policy. */
  assert(cnet_sctp_session_cancel_unsent_message(&a, 23u, 2u,
      partial_a, &cancelled) == CNET_SCTP_SESSION_BUSY);
  assert(cancelled.ticket == 0u && cancelled.generation == 0u);
  assert(cnet_sctp_session_get_tx_usage(&a, 2u, partial_a, &usage) ==
      CNET_SCTP_SESSION_OK);
  assert(usage.generation == partial_a && usage.retained_bytes == 4u &&
      usage.retained_fragments == 1u && usage.abandoned_bytes == 0u &&
      usage.gap_acked_bytes == 0u && usage.queued_bytes == 0u &&
      !usage.association_forward_pending);
  /* The unrelated reliable SID must not inherit the PR send ticket. */
  assert(cnet_sctp_session_get_tx_usage(&a, 0u, reliable_a, &usage) ==
      CNET_SCTP_SESSION_OK && usage.retained_bytes == 0u);
  assert(cnet_sctp_session_next_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_SESSION_OK && has_deadline && deadline > 22u);
  assert(cnet_sctp_session_advance(&a, deadline, forward, sizeof(forward),
      &forward_size) == CNET_SCTP_SESSION_OK);
  assert(forward_size != 0u && forward[12] == 194u);
  assert(cnet_sctp_session_get_tx_usage(&a, 2u, partial_a, &usage) ==
      CNET_SCTP_SESSION_OK);
  /* RFC3758 forward admission transactionally transfers the abandoned
   * DATA ticket into ONE retained FORWARD-TSN control ticket. Per-SID DATA
   * payload bytes become zero immediately, but the conservative
   * association-wide flag MUST remain set until a real peer SACK arrives.
   * Neither zero DATA bytes nor the abandonment event is ACK evidence. */
  assert(usage.retained_bytes == 0u && usage.abandoned_bytes == 0u &&
      usage.retained_fragments == 0u && usage.gap_acked_bytes == 0u &&
      usage.association_forward_pending);
  /* DATA slots have transferred into association-owned FORWARD control.
   * Even with zero retained DATA bytes, refusal remains conservatively BUSY
   * until the peer cumulatively ACKs the forward. */
  assert(cnet_sctp_session_cancel_unsent_message(&a, deadline, 2u,
      partial_a, &cancelled) == CNET_SCTP_SESSION_BUSY);
  assert(cancelled.ticket == 0u && cancelled.generation == 0u);
  /* Abandonment is a LOCAL decision, visible exactly once before the peer
   * has ever received or acknowledged the FORWARD control. It is NOT yet
   * evidence that the remote TSN stream has advanced. */
  assert(cnet_sctp_session_poll(&a, deadline, &event,
      payload, sizeof(payload), response, sizeof(response), &response_size) ==
      CNET_SCTP_SESSION_OK);
  assert(event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
      event.stream_id == 2u && event.generation == partial_a &&
      event.message_size == 4u && event.message_identifier == 1u &&
      event.ppid == 53u && event.binary &&
      event.first_tsn == event.last_tsn && event.has_tsn &&
      !event.unordered && event.interleaved && event.locally_initiated &&
      event.reliability.mode == CNET_SCTP_SESSION_RETRANSMITS &&
      event.reliability.value == 0u &&
      event.abandon_reason == CNET_SCTP_SESSION_ABANDON_RETRANSMITS);
  assert(cnet_sctp_session_poll(&a, deadline, &event,
      payload, sizeof(payload), response, sizeof(response), &response_size) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(progress.generation == 0u && progress.last_tsn == 0u);
  assert(cnet_sctp_session_input(&b, deadline + 1u, forward, forward_size,
      sack, sizeof(sack), &sack_size) == CNET_SCTP_SESSION_OK);
  assert(sack_size != 0u);
  assert(cnet_sctp_session_input(&a, deadline + 2u, sack, sack_size,
      response, sizeof(response), &response_size) == CNET_SCTP_SESSION_OK);
  assert(response_size == 0u);
  assert(cnet_sctp_session_get_tx_usage(&a, 2u, partial_a, &usage) ==
      CNET_SCTP_SESSION_OK);
  assert(usage.retained_bytes == 0u && usage.abandoned_bytes == 0u &&
      usage.retained_fragments == 0u &&
      !usage.association_forward_pending);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_OK);
  assert(progress.generation == partial_a &&
      progress.stream_id == 2u && progress.message_size == 4u &&
      progress.first_tsn == progress.last_tsn &&
      progress.ppid == 53u && progress.message_identifier == 1u &&
      progress.binary && !progress.unordered && progress.interleaved &&
      progress.reliability.mode == CNET_SCTP_SESSION_RETRANSMITS &&
      progress.reliability.value == 0u &&
      progress.abandon_reason == CNET_SCTP_SESSION_ABANDON_RETRANSMITS);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(progress.generation == 0u && progress.last_tsn == 0u);
  /* Once the forward barrier is genuinely retired, the strict pre-TSN
   * pending-cancel query can distinguish absence from an in-flight send. */
  assert(cnet_sctp_session_cancel_unsent_message(&a, deadline + 2u, 2u,
      partial_a, &cancelled) == CNET_SCTP_SESSION_EMPTY);
  assert(cancelled.ticket == 0u && cancelled.generation == 0u);
  /* The FORWARD SACK retires the control but MUST NOT re-emit the already
   * consumed local abandonment event, nor fabricate a cancellation. */
  assert(cnet_sctp_session_poll(&a, deadline + 4u, &event,
      payload, sizeof(payload), response, sizeof(response), &response_size) ==
      CNET_SCTP_SESSION_EMPTY);

  /* The same association must remain usable for a later reliable channel. */
  assert(cnet_sctp_session_send(&a, deadline + 5u, 0u, 0,
      "after", 5u, reliable, sizeof(reliable), &reliable_size) ==
      CNET_SCTP_SESSION_OK);
  deliver(&b, &a, deadline + 6u, reliable, reliable_size);
  assert(cnet_sctp_session_poll(&b, deadline + 8u, &event,
      payload, sizeof(payload), response, sizeof(response), &response_size) ==
      CNET_SCTP_SESSION_OK);
  assert(event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE && !event.binary &&
      event.stream_id == 0u && event.generation == reliable_b &&
      event.message_size == 5u && event.ppid == 51u && event.interleaved &&
      memcmp(payload, "after", 5u) == 0);

  assert(partial_b != 0u && reliable_a != 0u);
  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}


/* Saturate the distinct Owner-held peer-progress table, not DATA's own
 * abandonment FIFO. The 17th local event must remain in DATA until the
 * application takes an existing cumulative-progress result. No false peer
 * ACK, lost event, new worker, or dynamic queue growth is permitted. */
static void bounded_forward_progress_preserves_local_events(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_event event = {0};
  cnet_sctp_session_forward_progress progress = {0};
  uint8_t wire[1200], forward[1200], sack[1200], response[1200];
  uint8_t message[32];
  size_t wire_size = 0u, forward_size = 0u, sack_size = 0u;
  size_t response_size = 0u;
  uint64_t generation_a = 0u, generation_b = 0u;
  uint64_t now_ms = 20u, deadline = 0u;
  size_t i;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  open_channel_with_policy(&a, &b, 6u, 2u, 0x01u, 0u,
      &generation_a, &generation_b);
  assert(generation_a != 0u && generation_b != 0u);

  /* Each maxRetransmits=0 message is lost once; one Owner FORWARD and
   * one independent peer SACK cumulatively advance the real TSN. */
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS + 1u; ++i) {
    wire_size = 0u;
    assert(cnet_sctp_session_send(&a, now_ms, 2u, 1,
        "f", 1u, wire, sizeof(wire), &wire_size) ==
        CNET_SCTP_SESSION_OK && wire_size != 0u);
    assert(cnet_sctp_session_next_deadline(&a, &deadline,
        &has_deadline) == CNET_SCTP_SESSION_OK &&
        has_deadline && deadline > now_ms);
    forward_size = 0u;
    assert(cnet_sctp_session_advance(&a, deadline, forward,
        sizeof(forward), &forward_size) == CNET_SCTP_SESSION_OK &&
        forward_size != 0u && forward[12u] == 194u);
    sack_size = 0u;
    assert(cnet_sctp_session_input(&b, deadline + 1u,
        forward, forward_size, sack, sizeof(sack),
        &sack_size) == CNET_SCTP_SESSION_OK && sack_size != 0u);
    response_size = 0u;
    assert(cnet_sctp_session_input(&a, deadline + 2u,
        sack, sack_size, response, sizeof(response),
        &response_size) == CNET_SCTP_SESSION_OK && response_size == 0u);

    if (i < CNET_SCTP_DATA_TX_SLOTS) {
      assert(cnet_sctp_session_poll(&a, deadline + 3u, &event,
          message, sizeof(message), response, sizeof(response),
          &response_size) == CNET_SCTP_SESSION_OK &&
          event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
          event.generation == generation_a && event.stream_id == 2u &&
          event.has_tsn && event.message_size == 1u);
    } else {
      /* Poll must NOT pop or misattribute the 17th original event. */
      assert(cnet_sctp_session_poll(&a, deadline + 3u, &event,
          message, sizeof(message), response, sizeof(response),
          &response_size) == CNET_SCTP_SESSION_CAPACITY &&
          event.kind == CNET_SCTP_SESSION_EVENT_NONE);
      assert(a.association.data.abandon_event_count == 1u);
    }
    now_ms = deadline + 6u;
  }

  /* The result remains untouched if the Owner is denied. */
  ca.owner = 0;
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_OWNER && progress.generation == 0u);
  ca.owner = 1;
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_OK &&
      progress.generation == generation_a &&
      progress.stream_id == 2u && progress.message_size == 1u &&
      progress.first_tsn == progress.last_tsn);

  /* One read frees one slot. The 17th local event is still available, with
   * the same original generation and already validated cumulative progress. */
  assert(cnet_sctp_session_poll(&a, now_ms, &event,
      message, sizeof(message), response, sizeof(response),
      &response_size) == CNET_SCTP_SESSION_OK &&
      event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
      event.generation == generation_a && event.message_size == 1u &&
      a.association.data.abandon_event_count == 0u);
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
        CNET_SCTP_SESSION_OK &&
        progress.generation == generation_a &&
        progress.stream_id == 2u && progress.message_size == 1u &&
        progress.first_tsn == progress.last_tsn);
  }
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_EMPTY && progress.generation == 0u);
  assert(cnet_sctp_session_poll(&a, now_ms + 1u, &event,
      message, sizeof(message), response, sizeof(response),
      &response_size) == CNET_SCTP_SESSION_EMPTY);

  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_drained(&a) && cnet_sctp_session_drained(&b));
  assert(cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}


/* Emit a real RTX0 PR-SCTP FORWARD, discard it and its single permitted
 * Owner retransmission, and then exceed the genuine local retry budget.
 * The already-consumed local abandonment must NOT become confirmed peer
 * progress, and the terminal record must retain its original generation. */
static void forward_retry_exhaustion_is_terminal_not_peer_progress(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_event local = {0};
  cnet_sctp_session_forward_progress progress = {0};
  cnet_sctp_session_terminal_failure failure = {0};
  uint8_t lost[1200], first[1200], second[1200], payload[32], reply[1200];
  size_t lost_size = 0u, first_size = 0u, second_size = 0u;
  size_t reply_size = 0u;
  uint64_t ga = 0u, gb = 0u, due = 0u;
  uint32_t original_last_tsn = 0u;
  int has_due = 0;

  ready_pair(&a, &b, &ca, &cb);
  open_channel_with_policy(&a, &b, 6u, 2u, 0x01u, 0u, &ga, &gb);
  /* This fixture changes only the local Owner's bounded retry budget. No
   * simulated ACK, protocol clock tampering or external message delivery. */
  a.association.config.max_retransmits = 1u;
  a.association.data.config.max_retransmits = 1u;
  assert(cnet_sctp_session_send(&a, 20u, 2u, 1, "lost", 4u,
      lost, sizeof(lost), &lost_size) == CNET_SCTP_SESSION_OK);
  assert(lost_size != 0u);
  assert(cnet_sctp_session_next_deadline(&a, &due, &has_due) ==
      CNET_SCTP_SESSION_OK && has_due && due > 20u);
  assert(cnet_sctp_session_advance(&a, due, first,
      sizeof(first), &first_size) == CNET_SCTP_SESSION_OK &&
      first_size != 0u && first[12u] == 194u);
  assert(cnet_sctp_session_poll(&a, due, &local,
      payload, sizeof(payload), reply, sizeof(reply),
      &reply_size) == CNET_SCTP_SESSION_OK &&
      local.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
      local.stream_id == 2u && local.generation == ga &&
      local.message_size == 4u && local.has_tsn);
  original_last_tsn = local.last_tsn;
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_EMPTY && failure.generation == 0u);
  assert(cnet_sctp_session_next_deadline(&a, &due, &has_due) ==
      CNET_SCTP_SESSION_OK && has_due);
  assert(cnet_sctp_session_advance(&a, due, second,
      sizeof(second), &second_size) == CNET_SCTP_SESSION_OK &&
      second_size == first_size && memcmp(second, first, first_size) == 0);
  assert(cnet_sctp_session_next_deadline(&a, &due, &has_due) ==
      CNET_SCTP_SESSION_OK && has_due);
  reply_size = sizeof(reply);
  assert(cnet_sctp_session_advance(&a, due, reply,
      sizeof(reply), &reply_size) == CNET_SCTP_SESSION_TRANSPORT &&
      reply_size == 0u);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_EMPTY && progress.generation == 0u);
  assert(cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_OK &&
      failure.kind == CNET_SCTP_SESSION_TERMINAL_FORWARD_UNCONFIRMED &&
      failure.reason == CNET_SCTP_SESSION_TERMINAL_LOCAL_RETRY_EXHAUSTED &&
      failure.generation == ga && failure.stream_id == 2u &&
      failure.ticket == 0u && failure.has_final_tsn &&
      failure.last_tsn == original_last_tsn && failure.ppid == 53u &&
      failure.message_size == 4u && failure.binary);
  assert(cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_EMPTY && failure.generation == 0u);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}

/* A dropped tracked RELIABLE DATA record hits its actual T3 budget, never
 * becomes ACKed, and exposes one old-generation terminal failure ticket. */
static void reliable_retry_exhaustion_is_terminal_not_acked(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_reliable_ack ack = {0};
  cnet_sctp_session_terminal_failure failure = {0};
  uint8_t lost[1200], retry[1200];
  size_t lost_size = 0u, retry_size = 0u;
  uint64_t ga = 0u, gb = 0u, ticket = 0u, due = 0u;
  int has_due = 0;

  ready_pair(&a, &b, &ca, &cb);
  open_channel_with_policy(&a, &b, 6u, 0u, 0x80u, 0u, &ga, &gb);
  a.association.config.max_retransmits = 1u;
  a.association.data.config.max_retransmits = 1u;
  assert(cnet_sctp_session_send_reliable_tracked(&a, 20u, 0u, ga, 1,
      "lost", 4u, lost, sizeof(lost), &lost_size, &ticket) ==
      CNET_SCTP_SESSION_OK && ticket != 0u && lost_size != 0u);
  assert(cnet_sctp_session_take_reliable_ack(&a, &ack) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(cnet_sctp_session_next_deadline(&a, &due, &has_due) ==
      CNET_SCTP_SESSION_OK && has_due && due > 20u);
  assert(cnet_sctp_session_advance(&a, due, retry,
      sizeof(retry), &retry_size) == CNET_SCTP_SESSION_OK &&
      retry_size == lost_size && memcmp(retry, lost, retry_size) == 0);
  assert(cnet_sctp_session_next_deadline(&a, &due, &has_due) ==
      CNET_SCTP_SESSION_OK && has_due);
  retry_size = sizeof(retry);
  assert(cnet_sctp_session_advance(&a, due, retry,
      sizeof(retry), &retry_size) == CNET_SCTP_SESSION_TRANSPORT &&
      retry_size == 0u);
  assert(cnet_sctp_session_take_reliable_ack(&a, &ack) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_OK &&
      failure.kind == CNET_SCTP_SESSION_TERMINAL_RELIABLE_UNACKED &&
      failure.reason == CNET_SCTP_SESSION_TERMINAL_LOCAL_RETRY_EXHAUSTED &&
      failure.ticket == ticket && failure.generation == ga &&
      failure.stream_id == 0u && failure.has_final_tsn &&
      failure.ppid == 53u && failure.message_size == 4u &&
      failure.binary);
  assert(cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_EMPTY && failure.ticket == 0u);
  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}


/* A FORWARD retransmission budget may expire before ULP polls the ORIGINAL
 * PR-SCTP local abandonment. FAILED must preserve that bounded DATA event
 * and require its generation-bound collection BEFORE terminal failure can
 * report a result; no peer progress is fabricated for dropped FORWARDs. */
static void unread_abandonment_after_failed_is_not_lost(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_event local = {0};
  cnet_sctp_session_terminal_failure failure = {0};
  cnet_sctp_session_forward_progress progress = {0};
  uint8_t lost[1200], forward[1200], retry[1200], output[1200], payload[32];
  size_t lost_size = 0u, forward_size = 0u, retry_size = 0u;
  size_t output_size = 0u;
  uint64_t ga = 0u, gb = 0u, deadline = 0u;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  open_channel_with_policy(&a, &b, 6u, 2u, 0x01u, 0u, &ga, &gb);
  a.association.config.max_retransmits = 1u;
  a.association.data.config.max_retransmits = 1u;
  assert(cnet_sctp_session_send(&a, 20u, 2u, 1, "lost", 4u,
      lost, sizeof(lost), &lost_size) == CNET_SCTP_SESSION_OK &&
      lost_size != 0u);
  /* Admit a real peer DCEP OPEN on another SID, but make its ACK output too
   * small. FAILED must not retry this impossible ACK ahead of the original
   * local PR abandonment, nor fabricate a CHANNEL_OPEN notification. */
  {
    uint8_t remote_open[1200], sack[1200];
    size_t remote_size = 0u, sack_size = 0u;
    assert(cnet_sctp_session_open_channel(&b, 21u, 1u,
        CNET_SCTP_SESSION_CHANNEL_RELIABLE_ORDERED, 0u, 0u,
        "side", 4u, NULL, 0u, remote_open, sizeof(remote_open),
        &remote_size) == CNET_SCTP_SESSION_OK && remote_size != 0u);
    assert(cnet_sctp_session_input(&a, 22u, remote_open, remote_size,
        sack, sizeof(sack), &sack_size) == CNET_SCTP_SESSION_OK);
    output_size = 123u;
    assert(cnet_sctp_session_poll(&a, 23u, &local,
        payload, sizeof(payload), output, 1u, &output_size) ==
        CNET_SCTP_SESSION_CAPACITY);
    assert(output_size == 0u &&
        local.kind == CNET_SCTP_SESSION_EVENT_NONE &&
        a.pending_dcep_ack == (uint16_t)(UINT16_C(1) << 1u));
  }
  assert(cnet_sctp_session_next_deadline(&a, &deadline,
      &has_deadline) == CNET_SCTP_SESSION_OK && has_deadline);
  assert(cnet_sctp_session_advance(&a, deadline, forward,
      sizeof(forward), &forward_size) == CNET_SCTP_SESSION_OK &&
      forward_size != 0u && forward[12u] == 194u);
  /* Deliberately DO NOT poll MESSAGE_ABANDONED here. */
  assert(a.association.data.abandon_event_count == 1u);
  assert(cnet_sctp_session_next_deadline(&a, &deadline,
      &has_deadline) == CNET_SCTP_SESSION_OK && has_deadline);
  assert(cnet_sctp_session_advance(&a, deadline, retry,
      sizeof(retry), &retry_size) == CNET_SCTP_SESSION_OK &&
      retry_size == forward_size &&
      memcmp(retry, forward, forward_size) == 0);
  assert(cnet_sctp_session_next_deadline(&a, &deadline,
      &has_deadline) == CNET_SCTP_SESSION_OK && has_deadline);
  output_size = sizeof(output);
  assert(cnet_sctp_session_advance(&a, deadline, output,
      sizeof(output), &output_size) == CNET_SCTP_SESSION_TRANSPORT &&
      output_size == 0u);
  assert(a.association.state == CNET_SCTP_ASSOC_FAILED &&
      a.association.data_ready &&
      a.association.data.abandon_event_count == 1u &&
      a.pending_dcep_ack == (uint16_t)(UINT16_C(1) << 1u));
  /* Returning EMPTY here would let the consumer wrongly conclude all
   * generation-bound terminal events have been settled. */
  assert(cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_BUSY && failure.generation == 0u);
  output_size = 0u;
  assert(cnet_sctp_session_poll(&a, deadline, &local,
      payload, sizeof(payload), output, sizeof(output),
      &output_size) == CNET_SCTP_SESSION_OK &&
      local.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
      local.stream_id == 2u && local.generation == ga &&
      local.has_tsn && local.message_size == 4u &&
      local.abandon_reason == CNET_SCTP_SESSION_ABANDON_RETRANSMITS &&
      a.association.data.abandon_event_count == 0u &&
      a.pending_dcep_ack == (uint16_t)(UINT16_C(1) << 1u));
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_EMPTY &&
      cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_OK &&
      failure.kind == CNET_SCTP_SESSION_TERMINAL_FORWARD_UNCONFIRMED &&
      failure.generation == ga && failure.stream_id == 2u &&
      failure.last_tsn == local.last_tsn &&
      failure.message_size == 4u && failure.ticket == 0u &&
      cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}

/* Peer ABORT destroys the DATA TCB, NOT the already confirmed, copied
 * original-generation successes. The association-level ABORT reason is
 * independent and can be consumed before either success receipt. */
static void peer_abort_retains_unread_confirmed_results(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_event event = {0};
  cnet_sctp_session_reliable_ack ack = {0};
  cnet_sctp_session_forward_progress progress = {0};
  cnet_sctp_session_terminal_failure terminal = {0};
  cnet_sctp_session_peer_abort abort_info = {0};
  const uint8_t cause[] = {0u, 12u, 0u, 8u, 'l', 'a', 't', 'e'};
  const cnet_sctp_wire_chunk abort_chunk = {6u, 0u, cause, sizeof(cause)};
  uint8_t reliable[1200], pr[1200], forward[1200], sack[1200];
  uint8_t packet[1200], reply[1200], payload[32], reason[8] = {0};
  size_t reliable_n = 0u, pr_n = 0u, forward_n = 0u, sack_n = 0u;
  size_t packet_n = 0u, reply_n = 0u, reason_n = 0u;
  uint64_t ga = 0u, gb = 0u, gp = 0u, gpb = 0u;
  uint64_t ticket = 0u, deadline = 0u;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  open_channel_with_policy(&a, &b, 6u, 0u, 0x80u, 0u, &ga, &gb);
  open_channel_with_policy(&a, &b, 14u, 2u, 0x01u, 0u, &gp, &gpb);
  assert(cnet_sctp_session_send_reliable_tracked(&a, 22u, 0u, ga, 1,
      "good", 4u, reliable, sizeof(reliable), &reliable_n, &ticket) ==
      CNET_SCTP_SESSION_OK && ticket != 0u && reliable_n != 0u);
  deliver(&b, &a, 23u, reliable, reliable_n);
  assert(cnet_sctp_session_send(&a, 26u, 2u, 1,
      "drop", 4u, pr, sizeof(pr), &pr_n) == CNET_SCTP_SESSION_OK &&
      pr_n != 0u);
  assert(cnet_sctp_session_next_deadline(&a, &deadline,
      &has_deadline) == CNET_SCTP_SESSION_OK && has_deadline);
  assert(cnet_sctp_session_advance(&a, deadline, forward,
      sizeof(forward), &forward_n) == CNET_SCTP_SESSION_OK &&
      forward_n > 12u && forward[12u] == 194u);
  assert(cnet_sctp_session_poll(&a, deadline, &event, payload,
      sizeof(payload), reply, sizeof(reply), &reply_n) ==
      CNET_SCTP_SESSION_OK &&
      event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
      event.generation == gp && event.stream_id == 2u && event.has_tsn);
  assert(cnet_sctp_session_input(&b, deadline + 1u, forward,
      forward_n, sack, sizeof(sack), &sack_n) ==
      CNET_SCTP_SESSION_OK && sack_n != 0u);
  assert(cnet_sctp_session_input(&a, deadline + 2u, sack,
      sack_n, reply, sizeof(reply), &reply_n) ==
      CNET_SCTP_SESSION_OK && reply_n == 0u);
  /* Keep both success receipts unread through a REAL incoming ABORT. */
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.association.local_tag,
      &abort_chunk, 1u, packet, sizeof(packet), &packet_n) ==
      CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_session_input(&a, deadline + 3u, packet, packet_n,
      reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OK &&
      reply_n == 0u && !a.association.data_ready && a.peer_abort_seen);
  assert(cnet_sctp_session_take_peer_abort(&a, &abort_info, reason,
      sizeof(reason), &reason_n) == CNET_SCTP_SESSION_OK &&
      abort_info.has_tsn && abort_info.has_user_reason &&
      reason_n == 4u && memcmp(reason, "late", 4u) == 0);
  ca.owner = 0;
  assert(cnet_sctp_session_take_reliable_ack(&a, &ack) ==
      CNET_SCTP_SESSION_OWNER && ack.ticket == 0u);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_OWNER && progress.generation == 0u);
  ca.owner = 1;
  assert(cnet_sctp_session_take_reliable_ack(&a, &ack) ==
      CNET_SCTP_SESSION_OK && ack.ticket == ticket &&
      ack.generation == ga && ack.stream_id == 0u &&
      ack.message_size == 4u);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_OK && progress.generation == gp &&
      progress.stream_id == 2u && progress.message_size == 4u &&
      progress.abandon_reason == CNET_SCTP_SESSION_ABANDON_RETRANSMITS);
  assert(cnet_sctp_session_take_reliable_ack(&a, &ack) ==
      CNET_SCTP_SESSION_EMPTY && ack.ticket == 0u);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_EMPTY && progress.generation == 0u);
  assert(cnet_sctp_session_take_terminal_failure(&a, &terminal) ==
      CNET_SCTP_SESSION_EMPTY && terminal.generation == 0u);
  assert(gb != 0u && gpb != 0u);
  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}

/* RFC 9260 control-before-ABORT: a semantically admissible cumulative
 * SACK must be latched for the ULP before DATA is destroyed, but a forged
 * out-of-range SACK before an otherwise valid ABORT must NOT create success. */
static void peer_abort_prefix_sack_is_not_a_fake_ack(void) {
  unsigned int admissible;
  for (admissible = 0u; admissible < 2u; ++admissible) {
    cnet_sctp_session a = {0}, b = {0};
    test_context ca = {0}, cb = {0};
    cnet_sctp_session_reliable_ack ack = {0};
    cnet_sctp_session_peer_abort abort_info = {0};
    cnet_sctp_wire_header header = {0};
    cnet_sctp_wire_decoder cursor = {0};
    cnet_sctp_wire_chunk chunks[2] = {{0}};
    const uint8_t cause[] = {0u, 12u, 0u, 8u, 's', 'a', 'c', 'k'};
    uint8_t data[1200], sack[1200], bundled[1200], reply[1200];
    uint8_t copied_sack[32], reason[8] = {0};
    size_t data_n = 0u, sack_n = 0u, bundle_n = 0u, reply_n = 0u;
    size_t reason_n = 0u;
    uint64_t ga = 0u, gb = 0u, ticket = 0u;
    uint32_t unexpected;

    ready_pair(&a, &b, &ca, &cb);
    open_channel_with_policy(&a, &b, 6u, 0u, 0x80u, 0u, &ga, &gb);
    assert(cnet_sctp_session_send_reliable_tracked(&a, 20u, 0u,
        ga, 1, "sent", 4u, data, sizeof(data), &data_n, &ticket) ==
        CNET_SCTP_SESSION_OK && ticket != 0u && data_n != 0u);
    /* The genuine peer receives the packet and generates its real SACK;
     * withhold that SACK until it is bundled immediately ahead of ABORT. */
    assert(cnet_sctp_session_input(&b, 21u, data, data_n,
        sack, sizeof(sack), &sack_n) ==
        CNET_SCTP_SESSION_OK && sack_n != 0u);
    assert(cnet_sctp_wire_decode(sack, sack_n, &header, &cursor) ==
        CNET_SCTP_WIRE_OK && header.chunk_count == 1u);
    assert(cnet_sctp_wire_next_chunk(&cursor, &chunks[0]) ==
        CNET_SCTP_WIRE_OK && chunks[0].type == 3u &&
        chunks[0].value_size == 12u);
    memcpy(copied_sack, chunks[0].value, chunks[0].value_size);
    chunks[0].value = copied_sack;
    if (!admissible) {
      /* Beyond the original DATA send high-water mark. Framing and CRC
       * remain valid, but this SACK cannot confirm any local ticket. */
      unexpected = a.association.data.next_tsn + 100u;
      copied_sack[0] = (uint8_t)(unexpected >> 24);
      copied_sack[1] = (uint8_t)(unexpected >> 16);
      copied_sack[2] = (uint8_t)(unexpected >> 8);
      copied_sack[3] = (uint8_t)unexpected;
    }
    chunks[1].type = 6u;
    chunks[1].flags = 0u;
    chunks[1].value = cause;
    chunks[1].value_size = sizeof(cause);
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.association.local_tag,
        chunks, 2u, bundled, sizeof(bundled), &bundle_n) ==
        CNET_SCTP_WIRE_OK);
    assert(cnet_sctp_session_input(&a, 22u, bundled, bundle_n,
        reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OK &&
        reply_n == 0u && !a.association.data_ready && a.peer_abort_seen);
    assert(cnet_sctp_session_take_peer_abort(&a, &abort_info,
        reason, sizeof(reason), &reason_n) == CNET_SCTP_SESSION_OK &&
        abort_info.has_tsn && abort_info.has_user_reason &&
        reason_n == 4u && memcmp(reason, "sack", 4u) == 0);
    if (admissible) {
      assert(cnet_sctp_session_take_reliable_ack(&a, &ack) ==
          CNET_SCTP_SESSION_OK && ack.ticket == ticket &&
          ack.generation == ga && ack.stream_id == 0u);
    } else {
      assert(cnet_sctp_session_take_reliable_ack(&a, &ack) ==
          CNET_SCTP_SESSION_EMPTY && ack.ticket == 0u);
    }
    assert(cnet_sctp_session_take_reliable_ack(&a, &ack) ==
        CNET_SCTP_SESSION_EMPTY && ack.ticket == 0u);
    assert(gb != 0u);
    assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK);
    assert(cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK);
    assert(cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK);
    assert(cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
  }
}

int main(void) {
  session_open_message_reset_reuse();
  partial_loss_reports_abandonment_then_reliable_continues();
  bounded_forward_progress_preserves_local_events();
  forward_retry_exhaustion_is_terminal_not_peer_progress();
  reliable_retry_exhaustion_is_terminal_not_acked();
  unread_abandonment_after_failed_is_not_lost();
  peer_abort_retains_unread_confirmed_results();
  peer_abort_prefix_sack_is_not_a_fake_ack();
  puts("C11 Owner-scoped SCTP/DCEP session lifecycle PASS");
  return 0;
}
