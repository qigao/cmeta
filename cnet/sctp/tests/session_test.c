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

static void open_channel(cnet_sctp_session *client,
    cnet_sctp_session *server, uint64_t now,
    uint64_t *client_generation, uint64_t *server_generation) {
  cnet_sctp_session_event event = {0};
  uint8_t packet[1200], reply[1200], payload[512];
  size_t packet_size = 0u, reply_size = 0u;
  assert(cnet_sctp_session_open_channel(client, now, 0u, 0x80u,
      0u, 0u, "data", 4u, NULL, 0u,
      packet, 1u, &packet_size) == CNET_SCTP_SESSION_CAPACITY);
  assert(client->dcep.channels[0].state == 0u);
  assert(cnet_sctp_session_open_channel(client, now, 0u, 0x80u,
      0u, 0u, "data", 4u, NULL, 0u,
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
      event.message_size == 3u && memcmp(payload, "one", 3u) == 0);

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
  assert(cnet_sctp_session_destroy(&a) == CNET_SCTP_SESSION_OK);
  assert(cnet_sctp_session_destroy(&b) == CNET_SCTP_SESSION_OK);
}

int main(void) {
  session_open_message_reset_reuse();
  puts("C11 Owner-scoped SCTP/DCEP session lifecycle PASS");
  return 0;
}
