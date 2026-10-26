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
      assert(a.association.data.abandon_event_count == 1u &&
          a.association.data.abandon_events[
              a.association.data.abandon_event_head].admission_generation ==
                  generation_a);
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

  /* A test-only wrong SID reuse after 17 REAL PR messages were admitted
   * cannot re-attribute the unread 17th event to the current DCEP lifetime.
   * The actual RFC6525 Owner would ordinarily fence this reuse. */
  a.dcep.channels[2u].generation = generation_a + 901u;
  /* One read frees one slot. The 17th local event is still available, with
   * the ORIGINAL generation and validated cumulative peer progress. */
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
  unsigned int scenario;
  /* 0: invalid ordinary SACK; 1: valid ordinary SACK; 2: valid SACK
   * bundled ahead of a reflected-tag ABORT. Only scenario 1 may ACK. */
  for (scenario = 0u; scenario < 3u; ++scenario) {
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
    uint32_t unexpected, original_ack;

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
    original_ack = a.association.data.last_ack;
    if (scenario == 0u) {
      /* Beyond the original DATA send high-water mark. Framing and CRC
       * remain valid, but this SACK cannot confirm any local ticket. */
      unexpected = a.association.data.next_tsn + 100u;
      copied_sack[0] = (uint8_t)(unexpected >> 24);
      copied_sack[1] = (uint8_t)(unexpected >> 16);
      copied_sack[2] = (uint8_t)(unexpected >> 8);
      copied_sack[3] = (uint8_t)unexpected;
    }
    chunks[1].type = 6u;
    chunks[1].flags = (uint8_t)(scenario == 2u);
    chunks[1].value = cause;
    chunks[1].value_size = sizeof(cause);
    assert(cnet_sctp_wire_encode(5001u, 5000u,
        scenario == 2u ? a.association.peer_tag : a.association.local_tag,
        chunks, 2u, bundled, sizeof(bundled), &bundle_n) ==
        CNET_SCTP_WIRE_OK);
    assert(cnet_sctp_session_input(&a, 22u, bundled, bundle_n,
        reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OK &&
        reply_n == 0u && !a.association.data_ready && a.peer_abort_seen);
    assert(cnet_sctp_session_take_peer_abort(&a, &abort_info,
        reason, sizeof(reason), &reason_n) == CNET_SCTP_SESSION_OK &&
        abort_info.has_tsn && abort_info.has_user_reason &&
        reason_n == 4u && memcmp(reason, "sack", 4u) == 0 &&
        (scenario == 1u ?
            abort_info.last_acked_tsn != original_ack :
            abort_info.last_acked_tsn == original_ack));
    if (scenario == 1u) {
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

/* An already emitted local PR-SCTP abandonment is neither a new peer ABORT
 * nor a new LOCAL_RETRY_EXHAUSTED result. It must survive a real ABORT that
 * destroys DATA, even when the original Owner has not yet polled it.
 * Mode 0: plain ABORT without SACK plus saturated unconfirmed forward slots.
 * Mode 1: verified cumulative SACK before ordinary ABORT (real progress).
 * Mode 2: same SACK before reflected T=1 ABORT (no false progress).
 * Mode 3: explicit stop discards unread copied events. */
static void peer_abort_preserves_unread_local_abandonment(void) {
  unsigned int mode;
  for (mode = 0u; mode < 4u; ++mode) {
    cnet_sctp_session a = {0}, b = {0};
    test_context ca = {0}, cb = {0};
    cnet_sctp_session_event local = {0};
    cnet_sctp_session_forward_progress progress = {0};
    cnet_sctp_session_peer_abort abort_info = {0};
    cnet_sctp_session_terminal_failure failure = {0};
    static const uint8_t cause[] = {
        0u, 12u, 0u, 8u, 'h', 'a', 'l', 't'
    };
    cnet_sctp_wire_chunk chunks[2] = {{0}};
    cnet_sctp_wire_header sack_header = {0};
    cnet_sctp_wire_decoder sack_decoder = {0};
    uint8_t data[1200], forward[1200], sack[1200];
    uint8_t abort_packet[1200], output[1200], message[32], reason[8];
    size_t data_n = 0u, forward_n = 0u, sack_n = 0u;
    size_t abort_n = 0u, output_n = 0u, reason_n = 0u, i;
    uint64_t ga = 0u, gb = 0u, deadline = 0u;
    uint32_t abandoned_tsn;
    int has_deadline = 0;

    ready_pair(&a, &b, &ca, &cb);
    open_channel_with_policy(&a, &b, 6u, 2u, 0x01u, 0u, &ga, &gb);
    assert(cnet_sctp_session_send(&a, 20u, 2u, 1,
        "drop", 4u, data, sizeof(data), &data_n) ==
        CNET_SCTP_SESSION_OK && data_n != 0u);
    assert(cnet_sctp_session_next_deadline(&a, &deadline,
        &has_deadline) == CNET_SCTP_SESSION_OK && has_deadline);
    assert(cnet_sctp_session_advance(&a, deadline, forward,
        sizeof(forward), &forward_n) == CNET_SCTP_SESSION_OK &&
        forward_n > 12u && forward[12u] == 194u);
    assert(a.association.data.abandon_event_count == 1u);
    abandoned_tsn = a.association.data.abandon_events[
        a.association.data.abandon_event_head].last_tsn;
    /* Deliberately leave the original local event unread across ABORT.
     * Deliberately reuse the DCEP generation WITHOUT performing a real
     * reset: a test-only invariant violation that proves ABORT may not
     * misattribute the already-admitted original message to a new SID
     * lifetime. Real RFC6525 reuse has stricter Owner fences. */
    assert(a.association.data.abandon_events[
        a.association.data.abandon_event_head].admission_generation == ga);
    if (mode == 0u) {
      a.dcep.channels[2].generation = ga + 900u;
    }
    if (mode == 0u) {
      /* A full table of UNCONFIRMED prior forward placeholders can never
       * complete after ABORT and must not make the original event stuck. */
      for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
        a.forward_tracks[i].used = 1u;
        a.forward_tracks[i].result.last_tsn =
            a.association.data.next_tsn + 100u;
      }
    }
    if (mode == 1u || mode == 2u) {
      assert(cnet_sctp_session_input(&b, deadline + 1u, forward,
          forward_n, sack, sizeof(sack), &sack_n) ==
          CNET_SCTP_SESSION_OK && sack_n != 0u);
      assert(cnet_sctp_wire_decode(sack, sack_n, &sack_header,
          &sack_decoder) == CNET_SCTP_WIRE_OK &&
          sack_header.chunk_count == 1u);
      assert(cnet_sctp_wire_next_chunk(&sack_decoder, &chunks[0]) ==
          CNET_SCTP_WIRE_OK && chunks[0].type == 3u);
    }
    chunks[(mode == 1u || mode == 2u) ? 1u : 0u].type = 6u;
    chunks[(mode == 1u || mode == 2u) ? 1u : 0u].flags =
        (uint8_t)(mode == 2u);
    chunks[(mode == 1u || mode == 2u) ? 1u : 0u].value = cause;
    chunks[(mode == 1u || mode == 2u) ? 1u : 0u].value_size =
        sizeof(cause);
    assert(cnet_sctp_wire_encode(5001u, 5000u,
        mode == 2u ? a.association.peer_tag : a.association.local_tag,
        chunks, (mode == 1u || mode == 2u) ? 2u : 1u,
        abort_packet, sizeof(abort_packet), &abort_n) ==
        CNET_SCTP_WIRE_OK);
    /* A wrong-tag attempt must be transactionally refused, leaving DATA
     * and its unread abandonment FIFO completely intact. */
    if (mode == 0u) {
      size_t invalid_n = 0u;
      assert(cnet_sctp_wire_encode(5001u, 5000u,
          a.association.local_tag + 1u, chunks, 1u,
          abort_packet, sizeof(abort_packet), &invalid_n) ==
          CNET_SCTP_WIRE_OK);
      output_n = 123u;
      assert(cnet_sctp_session_input(&a, deadline + 2u,
          abort_packet, invalid_n, output, sizeof(output), &output_n) ==
          CNET_SCTP_SESSION_PROTOCOL && output_n == 0u &&
          a.association.data_ready &&
          a.association.data.abandon_event_count == 1u &&
          a.abort_abandoned_count == 0u);
      assert(cnet_sctp_wire_encode(5001u, 5000u,
          a.association.local_tag, chunks, 1u,
          abort_packet, sizeof(abort_packet), &abort_n) ==
          CNET_SCTP_WIRE_OK);
    }
    output_n = 123u;
    assert(cnet_sctp_session_input(&a, deadline + 2u,
        abort_packet, abort_n, output, sizeof(output), &output_n) ==
        CNET_SCTP_SESSION_OK && output_n == 0u &&
        a.peer_abort_seen && !a.association.data_ready &&
        a.abort_abandoned_count == 1u &&
        a.abort_abandoned[0].data.last_tsn == abandoned_tsn &&
        a.abort_abandoned[0].generation == ga);
    if (mode == 0u) {
      for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
        assert(!a.forward_tracks[i].used);
    }
    if (mode == 3u) {
      assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK &&
          !a.peer_abort_seen && a.abort_abandoned_count == 0u);
      for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
        assert(a.abort_abandoned[i].generation == 0u &&
            a.abort_abandoned[i].data.message_size == 0u);
    } else {
      /* The ABORT reason and original local event are distinct receipts;
       * taking the ABORT reason first cannot destroy the copied FIFO. */
      assert(cnet_sctp_session_take_peer_abort(&a, &abort_info,
          reason, sizeof(reason), &reason_n) == CNET_SCTP_SESSION_OK &&
          reason_n == 4u && abort_info.has_user_reason &&
          memcmp(reason, "halt", 4u) == 0 &&
          a.abort_abandoned_count == 1u);
      ca.owner = 0;
      output_n = 123u;
      assert(cnet_sctp_session_poll(&a, deadline + 2u, &local,
          message, sizeof(message), output, sizeof(output), &output_n) ==
          CNET_SCTP_SESSION_OWNER && output_n == 0u &&
          local.kind == CNET_SCTP_SESSION_EVENT_NONE &&
          a.abort_abandoned_count == 1u);
      ca.owner = 1;
      output_n = 123u;
      assert(cnet_sctp_session_poll(&a, deadline + 2u, &local,
          message, sizeof(message), output, sizeof(output), &output_n) ==
          CNET_SCTP_SESSION_OK && output_n == 0u &&
          local.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
          local.stream_id == 2u && local.generation == ga &&
          local.ppid == 53u && local.message_size == 4u &&
          local.has_tsn && local.last_tsn == abandoned_tsn &&
          local.abandon_reason == CNET_SCTP_SESSION_ABANDON_RETRANSMITS &&
          a.abort_abandoned_count == 0u);
      if (mode == 1u) {
        assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
            CNET_SCTP_SESSION_OK && progress.generation == ga &&
            progress.last_tsn == abandoned_tsn &&
            progress.stream_id == 2u &&
            cnet_sctp_session_take_forward_progress(&a, &progress) ==
            CNET_SCTP_SESSION_EMPTY);
      } else {
        assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
            CNET_SCTP_SESSION_EMPTY && progress.generation == 0u);
      }
      assert(cnet_sctp_session_take_terminal_failure(&a, &failure) ==
          CNET_SCTP_SESSION_EMPTY && failure.generation == 0u &&
          cnet_sctp_session_take_peer_abort(&a, &abort_info,
              reason, sizeof(reason), &reason_n) ==
          CNET_SCTP_SESSION_EMPTY && reason_n == 0u);
      /* Ordinary EOF: no MORE saved events after a real peer ABORT.
       * CLOSED would obscure a fully drained result queue. */
      output_n = 123u;
      assert(cnet_sctp_session_poll(&a, deadline + 2u, &local,
          message, sizeof(message), output, sizeof(output), &output_n) ==
          CNET_SCTP_SESSION_EMPTY && output_n == 0u &&
          local.kind == CNET_SCTP_SESSION_EVENT_NONE);
      assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK);
    }
    assert(gb != 0u);
    assert(cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK);
    assert(cnet_sctp_session_destroy_in_place(&a) ==
        CNET_SCTP_SESSION_OK);
    assert(cnet_sctp_session_destroy_in_place(&b) ==
        CNET_SCTP_SESSION_OK);
  }
}

/* Test-only DATA FIFO injection. Each fixed record has already been reserved
 * by the DATA layer in a valid active SID lifetime; this isolates the
 * full-capacity/wrapped-head ownership transfer without injecting network
 * events or pretending synthetic abandonment is genuine wire evidence. */
static void peer_abort_snapshot_full_wrapped_fifo(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_event event = {0};
  cnet_sctp_session_peer_abort abort_info = {0};
  cnet_sctp_session_forward_progress progress = {0};
  cnet_sctp_session_terminal_failure failure = {0};
  cnet_sctp_data *data;
  const uint8_t cause[] = {0u, 12u, 0u, 8u, 'f', 'i', 'f', 'o'};
  const cnet_sctp_wire_chunk abort_chunk = {6u, 0u, cause, sizeof(cause)};
  uint8_t wire[1200], reply[1200], msg[32], reason[8];
  size_t wire_n = 0u, reply_n = 0u, reason_n = 0u, i;
  uint64_t g0 = 0u, peer0 = 0u, g2 = 0u, peer2 = 0u;
  uint32_t base_tsn;

  ready_pair(&a, &b, &ca, &cb);
  open_channel_with_policy(&a, &b, 6u, 0u, 0x01u, 0u, &g0, &peer0);
  open_channel_with_policy(&a, &b, 14u, 2u, 0x01u, 0u, &g2, &peer2);
  data = &a.association.data;
  base_tsn = data->next_tsn + 10u;
  data->abandon_event_head = 13u;
  data->abandon_event_count = CNET_SCTP_DATA_TX_SLOTS;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    cnet_sctp_data_abandon_event *entry =
        &data->abandon_events[(13u + i) % CNET_SCTP_DATA_TX_SLOTS];
    memset(entry, 0, sizeof(*entry));
    entry->stream_id = (uint16_t)((i % 2u) ? 2u : 0u);
    entry->ppid = 53u;
    entry->message_identifier = (uint32_t)(100u + i);
    entry->admission_generation = (i % 2u) ? g2 : g0;
    entry->message_size = 7u + i;
    entry->first_tsn = base_tsn + (uint32_t)i;
    entry->last_tsn = entry->first_tsn;
    entry->reliability.mode = CNET_SCTP_DATA_RELIABILITY_TIMED;
    entry->reliability.value = 19u;
    entry->reason = CNET_SCTP_DATA_ABANDON_TIMED;
    entry->has_tsn = 1u;
  }
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.association.local_tag,
      &abort_chunk, 1u, wire, sizeof(wire), &wire_n) ==
      CNET_SCTP_WIRE_OK);
  reply_n = 123u;
  assert(cnet_sctp_session_input(&a, 23u, wire, wire_n,
      reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OK &&
      reply_n == 0u && !a.association.data_ready &&
      a.abort_abandoned_count == CNET_SCTP_DATA_TX_SLOTS &&
      a.abort_abandoned_head == 0u);
  ca.owner = 0;
  reply_n = 123u;
  assert(cnet_sctp_session_poll(&a, 23u, &event, msg, sizeof(msg),
      reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OWNER &&
      reply_n == 0u && event.kind == CNET_SCTP_SESSION_EVENT_NONE &&
      a.abort_abandoned_count == CNET_SCTP_DATA_TX_SLOTS);
  ca.owner = 1;

  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    reply_n = 123u;
    assert(cnet_sctp_session_poll(&a, 23u, &event, msg, sizeof(msg),
        reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OK &&
        reply_n == 0u &&
        event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
        event.stream_id == (uint16_t)((i % 2u) ? 2u : 0u) &&
        event.generation == ((i % 2u) ? g2 : g0) &&
        event.ppid == 53u && event.message_identifier == 100u + i &&
        event.message_size == i + 7u &&
        event.first_tsn == base_tsn + (uint32_t)i &&
        event.last_tsn == base_tsn + (uint32_t)i &&
        event.has_tsn &&
        event.abandon_reason == CNET_SCTP_SESSION_ABANDON_TIMED &&
        event.reliability.mode == CNET_SCTP_SESSION_TIMED &&
        event.reliability.value == 19u &&
        a.abort_abandoned_count == CNET_SCTP_DATA_TX_SLOTS - i - 1u &&
        cnet_sctp_session_take_forward_progress(&a, &progress) ==
        CNET_SCTP_SESSION_EMPTY);
  }
  reply_n = 123u;
  assert(cnet_sctp_session_poll(&a, 23u, &event, msg, sizeof(msg),
      reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_EMPTY &&
      reply_n == 0u && event.kind == CNET_SCTP_SESSION_EVENT_NONE);
  assert(cnet_sctp_session_take_peer_abort(&a, &abort_info,
      reason, sizeof(reason), &reason_n) == CNET_SCTP_SESSION_OK &&
      abort_info.has_user_reason && reason_n == 4u &&
      memcmp(reason, "fifo", 4u) == 0);
  assert(cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_EMPTY && failure.generation == 0u);
  assert(peer0 != 0u && peer2 != 0u);
  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}

/* Test-only saturated success ledger plus one original local PR record.
 * CAPACITY from poll must not consume the snapshot, modify output buffers
 * or overwrite any confirmed receipt. Owner then releases a slot and retries.
 * The synthetic retained record was already cumulatively acknowledged
 * before the test's real ABORT; this only isolates result-slot pressure. */
static void peer_abort_snapshot_progress_capacity_is_atomic(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_event event = {0};
  cnet_sctp_session_forward_progress progress = {0};
  cnet_sctp_data_abandon_event *pending;
  uint8_t wire[1200], reply[1200], msg[32];
  const cnet_sctp_wire_chunk abort_chunk = {6u, 0u, NULL, 0u};
  size_t wire_n = 0u, reply_n = 0u, i;
  uint64_t ga = 0u, gb = 0u;

  ready_pair(&a, &b, &ca, &cb);
  open_channel_with_policy(&a, &b, 6u, 2u, 0x01u, 0u, &ga, &gb);
  a.association.data.abandon_event_head = 0u;
  a.association.data.abandon_event_count = 1u;
  pending = &a.association.data.abandon_events[0u];
  memset(pending, 0, sizeof(*pending));
  pending->stream_id = 2u;
  pending->ppid = 53u;
  pending->admission_generation = ga;
  pending->message_size = 9u;
  pending->has_tsn = 1u;
  pending->first_tsn = a.association.data.last_ack;
  pending->last_tsn = a.association.data.last_ack;
  pending->reliability.mode = CNET_SCTP_DATA_RELIABILITY_TIMED;
  pending->reliability.value = 1u;
  pending->reason = CNET_SCTP_DATA_ABANDON_TIMED;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    a.forward_tracks[i].used = 1u;
    a.forward_tracks[i].cumul_confirmed = 1u;
    a.forward_tracks[i].result.generation = ga;
    a.forward_tracks[i].result.stream_id = 2u;
    a.forward_tracks[i].result.message_size = 100u + i;
  }
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.association.local_tag,
      &abort_chunk, 1u, wire, sizeof(wire), &wire_n) ==
      CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_session_input(&a, 20u, wire, wire_n, reply,
      sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OK &&
      reply_n == 0u && a.abort_abandoned_count == 1u);
  memset(reply, 0xCC, sizeof(reply));
  memset(msg, 0xCC, sizeof(msg));
  reply_n = 123u;
  assert(cnet_sctp_session_poll(&a, 20u, &event, msg, sizeof(msg),
      reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_CAPACITY &&
      reply_n == 0u && event.kind == CNET_SCTP_SESSION_EVENT_NONE &&
      a.abort_abandoned_count == 1u);
  for (i = 0u; i < sizeof(reply); ++i) assert(reply[i] == 0xCCu);
  for (i = 0u; i < sizeof(msg); ++i) assert(msg[i] == 0xCCu);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_OK && progress.message_size == 100u);
  reply_n = 123u;
  assert(cnet_sctp_session_poll(&a, 20u, &event, msg, sizeof(msg),
      reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OK &&
      reply_n == 0u &&
      event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
      event.stream_id == 2u && event.generation == ga &&
      event.message_size == 9u && event.has_tsn &&
      a.abort_abandoned_count == 0u);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_OK && progress.message_size == 9u &&
      progress.generation == ga && progress.stream_id == 2u);
  assert(gb != 0u);
  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}

/* Exercise the real Session -> Association -> DATA admission-generation
 * propagation, not merely a synthetic injected abandonment FIFO.
 * 0: direct maxRTX0 on negotiated I-DATA; 1: copied 2100-byte I-DATA
 * with two emitted fragments and an unsent suffix; 2: TIMED copied
 * message abandoned BEFORE any TSN; 3: test-only forced legacy DATA
 * framing on the bounded sender, NOT independent-peer negotiation evidence.
 * Capacity/Owner refusals must not reserve identity or consume a DATA TSN.
 * Test-only DCEP generation mutation simulates forbidden old-SID reuse after
 * successful admission: original DATA and the resulting abandonment event
 * must retain the old generation, never requery the current SID. */
static void pr_admission_generation_is_immutable(void) {
  unsigned int mode;
  for (mode = 0u; mode < 4u; ++mode) {
    cnet_sctp_session a = {0}, b = {0};
    test_context ca = {0}, cb = {0};
    cnet_sctp_session_event event = {0};
    uint8_t body[2100], wire[1200], forward[1200], payload[32];
    size_t wire_n = 0u, forward_n = 0u, i;
    uint64_t ga = 0u, gb = 0u, deadline = 0u;
    uint32_t original_next_tsn;
    int has_deadline = 0, found_tx = 0;
    const uint8_t channel_type = mode == 2u ? 0x02u : 0x01u;
    const uint32_t policy_value = mode == 2u ? 5u : 0u;
    size_t size = mode == 1u ? sizeof(body) : 4u;

    for (i = 0u; i < sizeof(body); ++i) body[i] = (uint8_t)(i ^ 0x5Au);
    ready_pair(&a, &b, &ca, &cb);
    open_channel_with_policy(&a, &b, 6u, 2u, channel_type, policy_value,
        &ga, &gb);
    if (mode == 3u)
      a.association.data.config.interleaving_supported = 0u;
    original_next_tsn = a.association.data.next_tsn;

    /* No fallback on a full fixed 16-entry PR event reservation ledger. */
    a.association.data.abandon_event_count = CNET_SCTP_DATA_TX_SLOTS;
    wire_n = 123u;
    memset(wire, 0xCC, sizeof(wire));
    if (mode == 0u || mode == 3u)
      assert(cnet_sctp_session_send(&a, 20u, 2u, 1, body, size,
          wire, sizeof(wire), &wire_n) == CNET_SCTP_SESSION_CAPACITY &&
          wire_n == 0u);
    else
      assert(cnet_sctp_session_queue_message(&a, 20u, 2u, 1,
          body, size) == CNET_SCTP_SESSION_CAPACITY);
    assert(a.association.data.next_tsn == original_next_tsn &&
        !a.association.data.pending.active &&
        a.association.data.abandon_event_count == CNET_SCTP_DATA_TX_SLOTS);
    for (i = 0u; i < sizeof(wire); ++i) assert(wire[i] == 0xCCu);
    a.association.data.abandon_event_count = 0u;
    a.association.data.abandon_event_head = 0u;

    ca.owner = 0;
    if (mode == 0u || mode == 3u)
      assert(cnet_sctp_session_send(&a, 20u, 2u, 1, body, size,
          wire, sizeof(wire), &wire_n) == CNET_SCTP_SESSION_OWNER);
    else
      assert(cnet_sctp_session_queue_message(&a, 20u, 2u, 1,
          body, size) == CNET_SCTP_SESSION_OWNER);
    ca.owner = 1;
    assert(a.association.data.next_tsn == original_next_tsn &&
        !a.association.data.pending.active);

    if (mode == 0u || mode == 3u) {
      assert(cnet_sctp_session_send(&a, 20u, 2u, 1, body, size,
          wire, sizeof(wire), &wire_n) == CNET_SCTP_SESSION_OK &&
          wire_n != 0u);
    } else {
      assert(cnet_sctp_session_queue_message(&a, 20u, 2u, 1,
          body, size) == CNET_SCTP_SESSION_OK &&
          a.association.data.pending.active &&
          a.association.data.pending.admission_generation == ga);
      memset(body, 0xEE, sizeof(body)); /* no borrowed application payload */
      assert(cnet_sctp_session_queue_message(&a, 20u, 2u, 1,
          body, size) == CNET_SCTP_SESSION_CAPACITY &&
          a.association.data.pending.admission_generation == ga);
      if (mode == 1u) {
        assert(cnet_sctp_session_pump_message(&a, 21u,
            wire, sizeof(wire), &wire_n) == CNET_SCTP_SESSION_OK &&
            wire_n > 32u && wire[12u] == 64u &&
            a.association.data.pending.active);
        assert(cnet_sctp_session_pump_message(&a, 22u,
            wire, sizeof(wire), &wire_n) == CNET_SCTP_SESSION_OK &&
            wire_n > 32u && a.association.data.pending.active);
      }
    }
    for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
      const cnet_sctp_data_tx *tx = &a.association.data.tx[i];
      if (!tx->used || tx->stream_id != 2u || tx->ppid != 53u)
        continue;
      found_tx = 1;
      assert(tx->admission_generation == ga);
    }
    assert(found_tx == (mode != 2u));
    a.dcep.channels[2u].generation = ga + 700u; /* injected old-SID reuse */
    assert(cnet_sctp_session_next_deadline(&a, &deadline, &has_deadline) ==
        CNET_SCTP_SESSION_OK && has_deadline && deadline >= 20u);
    /* The egress guard must refuse this artificial stale lifetime BEFORE
     * mutating DATA or writing a network packet, not merely preserve the
     * resulting event's old generation while transmitting under the wrong
     * one. Restore the original SID lifetime before real Owner advance. */
    {
      cnet_sctp_data before;
      memcpy(&before, &a.association.data, sizeof(before));
      memset(forward, 0xA5, sizeof(forward));
      forward_n = 123u;
      assert(cnet_sctp_session_advance(&a, deadline, forward,
          sizeof(forward), &forward_n) == CNET_SCTP_SESSION_STALE &&
          forward_n == 0u &&
          memcmp(&before, &a.association.data, sizeof(before)) == 0);
      for (i = 0u; i < sizeof(forward); ++i)
        assert(forward[i] == 0xA5u);
    }
    a.dcep.channels[2u].generation = ga;
    forward_n = 0u;
    assert(cnet_sctp_session_advance(&a, deadline, forward,
        sizeof(forward), &forward_n) == CNET_SCTP_SESSION_OK);
    /* Original generation belongs to the admitted message, not the
     * current channel. Mutation after real abandonment may not relabel it
     * when the application eventually polls its copied local event. */
    a.dcep.channels[2u].generation = ga + 700u;
    if (mode == 2u)
      assert(forward_n == 0u);
    else
      assert(forward_n != 0u && forward[12u] ==
          (uint8_t)(mode == 3u ? 192u : 194u));
    assert(a.association.data.abandon_event_count == 1u &&
        a.association.data.abandon_events[
            a.association.data.abandon_event_head].admission_generation ==
                ga);
    assert(cnet_sctp_session_poll(&a, deadline, &event, payload,
        sizeof(payload), wire, sizeof(wire), &wire_n) ==
        CNET_SCTP_SESSION_OK &&
        event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
        event.generation == ga && event.stream_id == 2u &&
        event.message_size == size && event.ppid == 53u &&
        event.reliability.mode == (mode == 2u ?
            CNET_SCTP_SESSION_TIMED : CNET_SCTP_SESSION_RETRANSMITS) &&
        event.has_tsn == (mode != 2u) &&
        event.interleaved == (mode != 3u));
    assert(gb != 0u &&
        cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
  }
}


/* A copied message is owned by its original generation at queue admission.
 * Under test-only incorrect DCEP SID reuse, neither a caller using the new
 * generation nor one using the stale old token may silently cancel it.
 * Restore the real generation and the Owner can cancel exactly once,
 * including its original tracked reliable ticket where applicable. */
static void queued_cancel_rejects_reused_dcep_generation(void) {
  unsigned int reliable;
  for (reliable = 0u; reliable < 2u; ++reliable) {
    cnet_sctp_session a = {0}, b = {0};
    test_context ca = {0}, cb = {0};
    cnet_sctp_session_unsent_cancelled cancelled = {0};
    cnet_sctp_session_reliable_ack ack = {0};
    uint16_t sid = reliable ? 0u : 2u;
    uint64_t ga = 0u, gb = 0u, ticket = 0u;
    uint32_t before_tsn;

    ready_pair(&a, &b, &ca, &cb);
    open_channel_with_policy(&a, &b, 6u, sid,
        reliable ? 0x80u : 0x02u, reliable ? 0u : 1000u, &ga, &gb);
    before_tsn = a.association.data.next_tsn;
    if (reliable)
      assert(cnet_sctp_session_queue_reliable_tracked(&a, 20u,
          sid, ga, 1, "copy", 4u, &ticket) == CNET_SCTP_SESSION_OK &&
          ticket != 0u);
    else
      assert(cnet_sctp_session_queue_message(&a, 20u, sid, 1,
          "copy", 4u) == CNET_SCTP_SESSION_OK && ticket == 0u);
    assert(a.association.data.pending.active &&
        a.association.data.pending.admission_generation == ga &&
        a.association.data.next_tsn == before_tsn);

    a.dcep.channels[sid].generation = ga + 11u;
    assert(cnet_sctp_session_cancel_unsent_message(&a, 21u, sid,
        ga + 11u, &cancelled) == CNET_SCTP_SESSION_STALE &&
        cancelled.generation == 0u && cancelled.ticket == 0u &&
        a.association.data.pending.active &&
        a.association.data.pending.admission_generation == ga &&
        a.association.data.next_tsn == before_tsn);
    assert(cnet_sctp_session_cancel_unsent_message(&a, 21u, sid,
        ga, &cancelled) == CNET_SCTP_SESSION_STALE &&
        a.association.data.pending.active);
    a.dcep.channels[sid].generation = ga;

    ca.owner = 0;
    assert(cnet_sctp_session_cancel_unsent_message(&a, 21u, sid,
        ga, &cancelled) == CNET_SCTP_SESSION_OWNER &&
        cancelled.generation == 0u);
    ca.owner = 1;
    assert(cnet_sctp_session_cancel_unsent_message(&a, 21u, sid,
        ga, &cancelled) == CNET_SCTP_SESSION_OK &&
        cancelled.generation == ga && cancelled.stream_id == sid &&
        cancelled.ticket == ticket && cancelled.ppid == 53u &&
        cancelled.message_size == 4u &&
        !a.association.data.pending.active &&
        a.association.data.next_tsn == before_tsn);
    if (reliable)
      assert(cnet_sctp_session_take_reliable_ack(&a, &ack) ==
          CNET_SCTP_SESSION_EMPTY && ack.ticket == 0u);
    assert(gb != 0u &&
        cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
  }
}


/* Test-only deliberately unbound lower-DATA record. A Session must not
 * recover a missing original generation by reading the currently open DCEP
 * SID; the result stays in the copied fixed FIFO and returns STALE until
 * explicit abortive stop, rather than being consumed or misattributed. */
static void peer_abort_unbound_generation_fails_closed(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_event event = {0};
  cnet_sctp_data_abandon_event *unbound;
  uint8_t wire[1200], reply[1200], msg[16];
  const cnet_sctp_wire_chunk abort_chunk = {6u, 0u, NULL, 0u};
  size_t wire_n = 0u, reply_n = 0u;
  uint64_t ga = 0u, gb = 0u;

  ready_pair(&a, &b, &ca, &cb);
  open_channel_with_policy(&a, &b, 6u, 2u, 0x01u, 0u, &ga, &gb);
  a.association.data.abandon_event_head = 0u;
  a.association.data.abandon_event_count = 1u;
  unbound = &a.association.data.abandon_events[0];
  memset(unbound, 0, sizeof(*unbound));
  unbound->stream_id = 2u;
  unbound->ppid = 53u;
  unbound->message_size = 4u;
  unbound->admission_generation = 0u; /* deliberately invalid */
  unbound->reason = CNET_SCTP_DATA_ABANDON_RETRANSMITS;
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.association.local_tag,
      &abort_chunk, 1u, wire, sizeof(wire), &wire_n) ==
      CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_session_input(&a, 20u, wire, wire_n,
      reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OK &&
      reply_n == 0u && !a.association.data_ready &&
      a.abort_abandoned_count == 1u &&
      a.abort_abandoned[0].generation == 0u);
  reply_n = 123u;
  assert(cnet_sctp_session_poll(&a, 20u, &event, msg, sizeof(msg),
      reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_STALE &&
      reply_n == 0u && event.kind == CNET_SCTP_SESSION_EVENT_NONE &&
      a.abort_abandoned_count == 1u && a.abort_abandoned_head == 0u);
  assert(ga != 0u && gb != 0u);
  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK &&
      a.abort_abandoned_count == 0u &&
      cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}


/* The read-only public TX snapshot must not attribute bytes or fragment
 * tickets admitted by SID generation N to generation N+1. Even if another
 * layer violates the normal RFC6525 SID-reuse fences, the response is STALE
 * with zeroed output (never falsely zero retained bytes of the new lifetime).
 * Real DATA admission is used for direct, copied unsent and partly pumped PR. */
static void tx_usage_refuses_old_generation_tickets(void) {
  unsigned int mode;
  for (mode = 0u; mode < 3u; ++mode) {
    cnet_sctp_session a = {0}, b = {0};
    test_context ca = {0}, cb = {0};
    cnet_sctp_session_tx_usage usage = {0}, zero = {0};
    uint8_t packet[1200], message[1800];
    size_t packet_n = 0u, expected_queued = 0u, expected_retained = 0u;
    uint64_t original = 0u, peer = 0u;
    uint32_t original_next_tsn;
    size_t i;

    memset(message, 0x5A, sizeof(message));
    ready_pair(&a, &b, &ca, &cb);
    open_channel_with_policy(&a, &b, 6u, 2u, 0x01u, 0u,
        &original, &peer);
    if (mode == 0u) {
      assert(cnet_sctp_session_send(&a, 20u, 2u, 1, "data", 4u,
          packet, sizeof(packet), &packet_n) == CNET_SCTP_SESSION_OK &&
          packet_n != 0u);
      expected_retained = 4u;
    } else {
      assert(cnet_sctp_session_queue_message(&a, 20u, 2u, 1,
          message, sizeof(message)) == CNET_SCTP_SESSION_OK);
      if (mode == 2u) {
        assert(cnet_sctp_session_pump_message(&a, 21u,
            packet, sizeof(packet), &packet_n) == CNET_SCTP_SESSION_OK &&
            packet_n != 0u);
        expected_retained = CNET_SCTP_DATA_PAYLOAD_MAX;
      }
      expected_queued = sizeof(message) - expected_retained;
    }
    original_next_tsn = a.association.data.next_tsn;
    assert(cnet_sctp_session_get_tx_usage(&a, 2u,
        original, &usage) == CNET_SCTP_SESSION_OK &&
        usage.generation == original &&
        usage.queued_bytes == expected_queued &&
        usage.retained_bytes == expected_retained);
    for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
      const cnet_sctp_data_tx *tx = &a.association.data.tx[i];
      if (tx->used && tx->stream_id == 2u && tx->ppid == 53u)
        assert(tx->admission_generation == original);
    }
    if (a.association.data.pending.active)
      assert(a.association.data.pending.admission_generation == original);

    /* Deliberately corrupt only the DCEP lifetime (test-only). */
    a.dcep.channels[2u].generation = original + 99u;
    memset(&usage, 0xCC, sizeof(usage));
    assert(cnet_sctp_session_get_tx_usage(&a, 2u,
        original + 99u, &usage) == CNET_SCTP_SESSION_STALE &&
        memcmp(&usage, &zero, sizeof(usage)) == 0 &&
        a.association.data.next_tsn == original_next_tsn);
    memset(&usage, 0xCC, sizeof(usage));
    assert(cnet_sctp_session_get_tx_usage(&a, 2u, original,
        &usage) == CNET_SCTP_SESSION_STALE &&
        memcmp(&usage, &zero, sizeof(usage)) == 0 &&
        a.association.data.next_tsn == original_next_tsn);
    a.dcep.channels[2u].generation = original;
    assert(cnet_sctp_session_get_tx_usage(&a, 2u,
        original, &usage) == CNET_SCTP_SESSION_OK &&
        usage.generation == original &&
        usage.queued_bytes == expected_queued &&
        usage.retained_bytes == expected_retained);
    assert(peer != 0u);
    assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
  }
}


/* A verified peer ABORT may preempt a real in-flight bilateral RFC6525 close
 * after peer FORWARD SACK, but before its reset transaction completes.
 * The already-local PR abandonment and confirmed FORWARD receipt retain the
 * original SID generation. Closing fails; no CHANNEL_CLOSED or locally
 * exhausted transport failure may be synthesized from the peer ABORT. */
static void peer_abort_preempts_reset_without_losing_pr_events(void) {
  cnet_sctp_session a = {0}, b = {0};
  test_context ca = {0}, cb = {0};
  cnet_sctp_session_event event = {0};
  cnet_sctp_session_forward_progress progress = {0};
  cnet_sctp_session_terminal_failure failure = {0};
  cnet_sctp_session_peer_abort aborted = {0};
  static const uint8_t cause[] = {
      0u, 12u, 0u, 8u, 's', 't', 'o', 'p'
  };
  const cnet_sctp_wire_chunk abort_chunk = {6u, 0u, cause, sizeof(cause)};
  uint8_t data[1200], forward[1200], sack[1200], reset[1200];
  uint8_t wire[1200], reply[1200], message[32], reason[8] = {0};
  size_t data_n = 0u, forward_n = 0u, sack_n = 0u, reset_n = 0u;
  size_t wire_n = 0u, reply_n = 0u, reason_n = 0u;
  uint64_t original = 0u, peer = 0u, closing = 0u, deadline = 0u;
  uint32_t last_abandoned_tsn = 0u;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  open_channel_with_policy(&a, &b, 6u, 2u, 0x01u, 0u,
      &original, &peer);
  assert(cnet_sctp_session_send(&a, 20u, 2u, 1, "lost", 4u,
      data, sizeof(data), &data_n) == CNET_SCTP_SESSION_OK &&
      data_n != 0u);
  assert(cnet_sctp_session_next_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_SESSION_OK && has_deadline);
  assert(cnet_sctp_session_advance(&a, deadline, forward,
      sizeof(forward), &forward_n) == CNET_SCTP_SESSION_OK &&
      forward_n != 0u && forward[12u] == 194u &&
      a.association.data.abandon_event_count == 1u);
  last_abandoned_tsn = a.association.data.abandon_events[
      a.association.data.abandon_event_head].last_tsn;
  assert(a.association.data.abandon_events[
      a.association.data.abandon_event_head].admission_generation == original);
  assert(cnet_sctp_session_input(&b, deadline + 1u, forward, forward_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_SESSION_OK &&
      sack_n != 0u);
  assert(cnet_sctp_session_input(&a, deadline + 2u, sack, sack_n,
      reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OK &&
      reply_n == 0u);
  /* A real reset request has been emitted; keep the original abandonment
   * and its cumulative-progress receipt unread during the whole overlap. */
  assert(cnet_sctp_session_close_channel(&a, deadline + 3u, 2u,
      reset, sizeof(reset), &reset_n, &closing) == CNET_SCTP_SESSION_OK &&
      closing == original && reset_n != 0u &&
      a.association.reset_active && a.closing_generation[2u] == original);
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.association.local_tag,
      &abort_chunk, 1u, wire, sizeof(wire), &wire_n) ==
      CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_session_input(&a, deadline + 4u, wire, wire_n,
      reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_OK &&
      reply_n == 0u && a.peer_abort_seen &&
      a.abort_abandoned_count == 1u && !a.association.data_ready &&
      a.abort_abandoned[0].generation == original);
  ca.owner = 0;
  assert(cnet_sctp_session_take_peer_abort(&a, &aborted, reason,
      sizeof(reason), &reason_n) == CNET_SCTP_SESSION_OWNER &&
      reason_n == 0u && !aborted.has_user_reason);
  ca.owner = 1;
  assert(cnet_sctp_session_poll(&a, deadline + 4u, &event,
      message, sizeof(message), reply, sizeof(reply), &reply_n) ==
      CNET_SCTP_SESSION_OK &&
      event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
      event.generation == original && event.stream_id == 2u &&
      event.last_tsn == last_abandoned_tsn && event.message_size == 4u &&
      a.abort_abandoned_count == 0u);
  assert(cnet_sctp_session_poll(&a, deadline + 4u, &event,
      message, sizeof(message), reply, sizeof(reply), &reply_n) ==
      CNET_SCTP_SESSION_OK &&
      event.kind == CNET_SCTP_SESSION_EVENT_CHANNEL_CLOSE_FAILED &&
      event.generation == original && event.stream_id == 2u);
  assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_OK && progress.generation == original &&
      progress.stream_id == 2u &&
      progress.last_tsn == last_abandoned_tsn &&
      cnet_sctp_session_take_forward_progress(&a, &progress) ==
      CNET_SCTP_SESSION_EMPTY);
  assert(cnet_sctp_session_take_terminal_failure(&a, &failure) ==
      CNET_SCTP_SESSION_EMPTY && failure.generation == 0u);
  assert(cnet_sctp_session_take_peer_abort(&a, &aborted, reason,
      sizeof(reason), &reason_n) == CNET_SCTP_SESSION_OK &&
      aborted.has_user_reason && reason_n == 4u &&
      memcmp(reason, "stop", 4u) == 0);
  assert(cnet_sctp_session_poll(&a, deadline + 4u, &event,
      message, sizeof(message), reply, sizeof(reply), &reply_n) ==
      CNET_SCTP_SESSION_EMPTY &&
      event.kind == CNET_SCTP_SESSION_EVENT_NONE);
  assert(peer != 0u);
  assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK &&
      cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
}


/* Egress, not only accounting/cancel, must retain the admission-time SID
 * generation. This test deliberately corrupts DCEP state after genuine
 * application DATA admission; real bilateral RFC6525 SID reuse is more
 * strongly fenced. All denied calls must leave the original DATA TCB,
 * packet buffer and ticket unchanged and allow normal recovery when the
 * original DCEP identity is restored.
 * mode 0: partly pumped 1.6 KiB PR I-DATA; mode 1: direct RTX0 PR T3;
 * mode 2: copied reliable tracked; mode 3: PR zero-window probe. */
static void stale_generation_denies_old_data_egress(void) {
  unsigned int mode;
  for (mode = 0u; mode < 4u; ++mode) {
    cnet_sctp_session a = {0}, b = {0};
    cnet_sctp_data before;
    test_context ca = {0}, cb = {0};
    cnet_sctp_session_event event = {0};
    const uint16_t sid = mode == 2u ? 0u : 2u;
    const uint8_t channel_type = mode == 2u ? 0x80u : 0x01u;
    uint8_t body[1600], packet[1200], answer[1200], wire[1200];
    uint8_t sack_fields[12] = {0};
    cnet_sctp_wire_chunk sack_chunk = {3u, 0u, sack_fields,
        sizeof(sack_fields)};
    size_t packet_n = 0u, answer_n = 0u, wire_n = 0u, i;
    uint64_t generation = 0u, peer = 0u, ticket = 0u, deadline = 0u;
    uint32_t cum;
    int has_deadline = 0;

    memset(body, 0x44, sizeof(body));
    ready_pair(&a, &b, &ca, &cb);
    open_channel_with_policy(&a, &b, 6u, sid, channel_type, 0u,
        &generation, &peer);
    if (mode == 3u) {
      /* Real wire SACK, not direct state mutation: the remote advertised
       * receive window is zero; Owner must use advance() to probe. */
      cum = a.association.data.last_ack;
      sack_fields[0] = (uint8_t)(cum >> 24);
      sack_fields[1] = (uint8_t)(cum >> 16);
      sack_fields[2] = (uint8_t)(cum >> 8);
      sack_fields[3] = (uint8_t)cum;
      assert(cnet_sctp_wire_encode(5001u, 5000u,
          a.association.local_tag, &sack_chunk, 1u, wire,
          sizeof(wire), &wire_n) == CNET_SCTP_WIRE_OK);
      assert(cnet_sctp_session_input(&a, 18u, wire, wire_n,
          answer, sizeof(answer), &answer_n) == CNET_SCTP_SESSION_OK &&
          answer_n == 0u && a.association.data.peer_rwnd == 0u);
    }
    if (mode == 1u) {
      assert(cnet_sctp_session_send(&a, 20u, sid, 1, body, 4u,
          packet, sizeof(packet), &packet_n) == CNET_SCTP_SESSION_OK &&
          packet_n != 0u);
    } else if (mode == 2u) {
      assert(cnet_sctp_session_queue_reliable_tracked(&a, 20u, sid,
          generation, 1, "reliable", 8u, &ticket) ==
          CNET_SCTP_SESSION_OK && ticket != 0u);
    } else {
      assert(cnet_sctp_session_queue_message(&a, 20u, sid, 1,
          body, mode == 0u ? sizeof(body) : 3u) ==
          CNET_SCTP_SESSION_OK && a.association.data.pending.active &&
          a.association.data.pending.admission_generation == generation);
      if (mode == 0u) {
        assert(cnet_sctp_session_pump_message(&a, 21u,
            packet, sizeof(packet), &packet_n) == CNET_SCTP_SESSION_OK &&
            packet_n != 0u && a.association.data.pending.active);
      }
    }
    if (mode == 1u || mode == 3u)
      assert(cnet_sctp_session_next_deadline(&a, &deadline,
          &has_deadline) == CNET_SCTP_SESSION_OK &&
          has_deadline && deadline > 20u);
    memcpy(&before, &a.association.data, sizeof(before));
    /* Simulated corrupt/reused channel lifetime. Do not let OLD DATA
     * or its retransmission leave before this mismatch is repaired. */
    a.dcep.channels[sid].generation = generation + 901u;
    memset(packet, 0xA5, sizeof(packet));
    packet_n = 123u;
    if (mode == 0u || mode == 2u)
      assert(cnet_sctp_session_pump_message(&a, 22u,
          packet, sizeof(packet), &packet_n) == CNET_SCTP_SESSION_STALE);
    else
      assert(cnet_sctp_session_advance(&a, deadline, packet,
          sizeof(packet), &packet_n) == CNET_SCTP_SESSION_STALE);
    assert(packet_n == 0u &&
        memcmp(&before, &a.association.data, sizeof(before)) == 0);
    for (i = 0u; i < sizeof(packet); ++i)
      assert(packet[i] == 0xA5u);
    /* A fresh send on the improperly reused same SID must fail too;
     * the original owner already has a retained or copied record. */
    packet_n = 123u;
    assert(cnet_sctp_session_send(&a, 22u, sid, 1,
        "new", 3u, packet, sizeof(packet), &packet_n) ==
        CNET_SCTP_SESSION_STALE && packet_n == 0u &&
        memcmp(&before, &a.association.data, sizeof(before)) == 0);
    ca.owner = 0;
    assert(cnet_sctp_session_pump_message(&a, 22u,
        packet, sizeof(packet), &packet_n) == CNET_SCTP_SESSION_OWNER);
    ca.owner = 1;
    assert(memcmp(&before, &a.association.data, sizeof(before)) == 0);
    a.dcep.channels[sid].generation = generation;
    if (mode == 1u) {
      assert(cnet_sctp_session_advance(&a, deadline, packet,
          sizeof(packet), &packet_n) == CNET_SCTP_SESSION_OK &&
          packet_n != 0u && packet[12u] == 194u);
      assert(cnet_sctp_session_poll(&a, deadline, &event, body,
          sizeof(body), answer, sizeof(answer), &answer_n) ==
          CNET_SCTP_SESSION_OK &&
          event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
          event.generation == generation);
    } else if (mode == 3u) {
      assert(cnet_sctp_session_advance(&a, deadline, packet,
          sizeof(packet), &packet_n) == CNET_SCTP_SESSION_OK &&
          packet_n != 0u && a.association.data.probe_active);
      for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
        if (a.association.data.tx[i].used &&
            a.association.data.tx[i].ppid == 53u)
          assert(a.association.data.tx[i].admission_generation ==
              generation);
    } else {
      assert(cnet_sctp_session_pump_message(&a, 22u,
          packet, sizeof(packet), &packet_n) == CNET_SCTP_SESSION_OK &&
          packet_n != 0u);
      if (mode == 2u) {
        assert(a.tracked[0].used && a.tracked[0].ack.ticket == ticket &&
            a.tracked[0].ack.generation == generation);
      }
    }
    assert(peer != 0u);
    assert(cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
  }
}


/* After local FORWARD retry exhaustion, Association FAILED is terminal:
 * even a valid late peer ABORT must be rejected without destroying DATA
 * or replacing the original local failure. Ordinary unread PR requires
 * terminal BUSY before poll. A full prior unconfirmed FORWARD ledger must
 * allow one prior LOCAL failure to free the only slot needed by poll(),
 * without returning premature EMPTY or fabricating an ABORT receipt.
 * The terminal state and new abandonment come from real DATA/FORWARD
 * packets and Owner deadlines; only the saturation fixture is injected. */
static void late_peer_abort_is_closed_after_local_retry_exhaustion(void) {
  unsigned int saturated;
  for (saturated = 0u; saturated < 2u; ++saturated) {
    cnet_sctp_session a = {0}, b = {0};
    test_context ca = {0}, cb = {0};
    cnet_sctp_session_event event = {0};
    cnet_sctp_session_terminal_failure terminal = {0};
    cnet_sctp_session_forward_progress progress = {0};
    cnet_sctp_session_peer_abort abort_info = {0};
    const uint8_t cause[] = {
        0u, 12u, 0u, 8u, 'l', 'a', 't', 'e'
    };
    const cnet_sctp_wire_chunk chunk = {6u, 0u, cause, sizeof(cause)};
    uint8_t lost[1200], forward[1200], retry[1200], wire[1200];
    uint8_t reply[1200], reason[8] = {0}, body[32];
    size_t lost_n = 0u, forward_n = 0u, retry_n = 0u;
    size_t wire_n = 0u, reply_n = 0u, reason_n = 0u, i;
    uint64_t ga = 0u, gb = 0u, deadline = 0u;
    uint32_t original_tsn = 0u;
    unsigned int terminal_count = 0u, original_failures = 0u;
    int has_deadline = 0;

    ready_pair(&a, &b, &ca, &cb);
    open_channel_with_policy(&a, &b, 6u, 2u, 0x01u, 0u, &ga, &gb);
    a.association.config.max_retransmits = 1u;
    a.association.data.config.max_retransmits = 1u;
    assert(cnet_sctp_session_send(&a, 20u, 2u, 1, "lost", 4u,
        lost, sizeof(lost), &lost_n) == CNET_SCTP_SESSION_OK &&
        lost_n != 0u);
    assert(cnet_sctp_session_next_deadline(&a, &deadline, &has_deadline) ==
        CNET_SCTP_SESSION_OK && has_deadline);
    assert(cnet_sctp_session_advance(&a, deadline, forward,
        sizeof(forward), &forward_n) == CNET_SCTP_SESSION_OK &&
        forward_n != 0u && forward[12u] == 194u &&
        a.association.data.abandon_event_count == 1u);
    original_tsn = a.association.data.abandon_events[
        a.association.data.abandon_event_head].last_tsn;
    assert(a.association.data.abandon_events[
        a.association.data.abandon_event_head].admission_generation == ga);
    assert(cnet_sctp_session_next_deadline(&a, &deadline, &has_deadline) ==
        CNET_SCTP_SESSION_OK && has_deadline);
    assert(cnet_sctp_session_advance(&a, deadline, retry,
        sizeof(retry), &retry_n) == CNET_SCTP_SESSION_OK &&
        retry_n == forward_n && memcmp(retry, forward, forward_n) == 0);
    assert(cnet_sctp_session_next_deadline(&a, &deadline, &has_deadline) ==
        CNET_SCTP_SESSION_OK && has_deadline);
    reply_n = sizeof(reply);
    assert(cnet_sctp_session_advance(&a, deadline, reply,
        sizeof(reply), &reply_n) == CNET_SCTP_SESSION_TRANSPORT &&
        reply_n == 0u && a.association.state == CNET_SCTP_ASSOC_FAILED &&
        a.association.data_ready && a.terminal_retry_exhausted &&
        a.association.data.abandon_event_count == 1u);
    assert(cnet_sctp_session_take_terminal_failure(&a, &terminal) ==
        CNET_SCTP_SESSION_BUSY && terminal.generation == 0u);

    if (saturated) {
      /* Test-only 16-entry saturation of pre-existing copied failure
       * records. The real local FAILED flag and the fresh PR abandonment
       * come from actual Owner timers/wire; injected older placeholders
       * solely exercise the bounded full-ledger escape path. */
      for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
        cnet_sctp_session_forward_track *slot = &a.forward_tracks[i];
        slot->used = 1u;
        slot->result.generation = ga;
        slot->result.stream_id = 2u;
        slot->result.ppid = 53u;
        slot->result.last_tsn = a.association.data.next_tsn + 100u +
            (uint32_t)i;
        slot->result.message_size = i + 1u;
      }
    }
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.association.local_tag,
        &chunk, 1u, wire, sizeof(wire), &wire_n) == CNET_SCTP_WIRE_OK);
    reply_n = 123u;
    /* The Association intentionally refuses ALL packets after an actual
     * LOCAL retry-budget FAILED transition, including a late, well-formed
     * peer ABORT. It cannot replace/erase the local terminal outcome. */
    assert(cnet_sctp_session_input(&a, deadline + 1u, wire, wire_n,
        reply, sizeof(reply), &reply_n) == CNET_SCTP_SESSION_CLOSED &&
        reply_n == 0u && a.association.data_ready &&
        a.association.state == CNET_SCTP_ASSOC_FAILED &&
        !a.peer_abort_seen && !a.association.peer_abort_pending &&
        a.terminal_retry_exhausted && a.abort_abandoned_count == 0u &&
        a.association.data.abandon_event_count == 1u &&
        a.association.data.abandon_events[
            a.association.data.abandon_event_head].last_tsn ==
                original_tsn);
    if (saturated) {
      /* An all-unconfirmed full ledger cannot be consumed through the
       * success API. One earlier legitimate failure must free capacity. */
      assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
          CNET_SCTP_SESSION_EMPTY &&
          cnet_sctp_session_take_terminal_failure(&a, &terminal) ==
          CNET_SCTP_SESSION_OK &&
          terminal.kind == CNET_SCTP_SESSION_TERMINAL_FORWARD_UNCONFIRMED &&
          terminal.generation == ga &&
          terminal.last_tsn != original_tsn &&
          a.association.data.abandon_event_count == 1u);
      ++terminal_count;
    } else {
      assert(cnet_sctp_session_take_terminal_failure(&a, &terminal) ==
          CNET_SCTP_SESSION_BUSY && terminal.generation == 0u);
    }
    ca.owner = 0;
    reply_n = 123u;
    assert(cnet_sctp_session_poll(&a, deadline + 1u, &event, body,
        sizeof(body), reply, sizeof(reply), &reply_n) ==
        CNET_SCTP_SESSION_OWNER && event.kind == CNET_SCTP_SESSION_EVENT_NONE &&
        reply_n == 0u &&
        a.association.data.abandon_event_count == 1u);
    ca.owner = 1;
    reply_n = 123u;
    assert(cnet_sctp_session_poll(&a, deadline + 1u, &event, body,
        sizeof(body), reply, sizeof(reply), &reply_n) ==
        CNET_SCTP_SESSION_OK && reply_n == 0u &&
        event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED &&
        event.generation == ga && event.stream_id == 2u &&
        event.message_size == 4u && event.has_tsn &&
        event.last_tsn == original_tsn &&
        a.association.data.abandon_event_count == 0u);
    assert(cnet_sctp_session_take_forward_progress(&a, &progress) ==
        CNET_SCTP_SESSION_EMPTY && progress.generation == 0u);
    for (;;) {
      cnet_sctp_session_status result =
          cnet_sctp_session_take_terminal_failure(&a, &terminal);
      if (result == CNET_SCTP_SESSION_EMPTY) break;
      assert(result == CNET_SCTP_SESSION_OK &&
          terminal.kind == CNET_SCTP_SESSION_TERMINAL_FORWARD_UNCONFIRMED &&
          terminal.reason == CNET_SCTP_SESSION_TERMINAL_LOCAL_RETRY_EXHAUSTED &&
          terminal.generation == ga && terminal.stream_id == 2u &&
          terminal.has_final_tsn);
      if (terminal.last_tsn == original_tsn) {
        assert(terminal.message_size == 4u && terminal.ppid == 53u);
        ++original_failures;
      }
      ++terminal_count;
      assert(terminal_count <= CNET_SCTP_DATA_TX_SLOTS + 1u);
    }
    assert(original_failures == 1u &&
        terminal_count == (saturated ? CNET_SCTP_DATA_TX_SLOTS + 1u : 1u));
    assert(cnet_sctp_session_take_peer_abort(&a, &abort_info, reason,
        sizeof(reason), &reason_n) == CNET_SCTP_SESSION_EMPTY &&
        !abort_info.has_user_reason && reason_n == 0u);
    assert(gb != 0u &&
        cnet_sctp_session_stop(&a) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_stop(&b) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_destroy_in_place(&a) == CNET_SCTP_SESSION_OK &&
        cnet_sctp_session_destroy_in_place(&b) == CNET_SCTP_SESSION_OK);
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
  peer_abort_preserves_unread_local_abandonment();
  peer_abort_snapshot_full_wrapped_fifo();
  peer_abort_snapshot_progress_capacity_is_atomic();
  pr_admission_generation_is_immutable();
  queued_cancel_rejects_reused_dcep_generation();
  peer_abort_unbound_generation_fails_closed();
  tx_usage_refuses_old_generation_tickets();
  peer_abort_preempts_reset_without_losing_pr_events();
  stale_generation_denies_old_data_egress();
  late_peer_abort_is_closed_after_local_retry_exhaustion();
  puts("C11 Owner-scoped SCTP/DCEP session lifecycle PASS");
  return 0;
}
