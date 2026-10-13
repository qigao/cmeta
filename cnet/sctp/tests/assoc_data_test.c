#include "sctp_assoc_internal.h"
#include "sctp_dcep_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Deliberately non-cryptographic fixture. Actual HMAC and RNG are qualified
 * separately in the pinned GmSSL test; this checks the real Association path. */
typedef struct test_context {
  uint32_t tag, tsn, test_key;
  int owner;
} test_context;
static int admitted(void *v) { return ((test_context *)v)->owner; }
static int entropy(void *v, uint8_t *bytes, size_t n) {
  test_context *c = (test_context *)v;
  if (n != 8u) return -1;
  bytes[0] = (uint8_t)(c->tag >> 24);
  bytes[1] = (uint8_t)(c->tag >> 16);
  bytes[2] = (uint8_t)(c->tag >> 8);
  bytes[3] = (uint8_t)c->tag;
  bytes[4] = (uint8_t)(c->tsn >> 24);
  bytes[5] = (uint8_t)(c->tsn >> 16);
  bytes[6] = (uint8_t)(c->tsn >> 8);
  bytes[7] = (uint8_t)c->tsn;
  return 0;
}
static int fake_mac(void *v, const uint8_t *p, size_t n,
                    uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  test_context *ctx = (test_context *)v;
  uint32_t s = ctx->test_key;
  size_t i;
  for (i = 0u; i < n; ++i) s = s * UINT32_C(65599) + p[i];
  for (i = 0u; i < CNET_SCTP_ASSOC_MAC_BYTES; ++i) {
    s = s * UINT32_C(1664525) + UINT32_C(1013904223);
    out[i] = (uint8_t)(s >> 24);
  }
  return 0;
}
static int fake_verify(void *v, const uint8_t *p, size_t n,
                       const uint8_t mac[CNET_SCTP_ASSOC_MAC_BYTES]) {
  uint8_t computed[CNET_SCTP_ASSOC_MAC_BYTES] = {0};
  size_t i;
  uint8_t diff = 0u;
  if (fake_mac(v, p, n, computed) != 0) return -1;
  for (i = 0u; i < sizeof(computed); ++i) diff |= (uint8_t)(computed[i] ^ mac[i]);
  return diff == 0u ? 0 : -1;
}
static cnet_sctp_assoc_config config(uint16_t port, test_context *ctx) {
  cnet_sctp_assoc_config c = {0};
  c.local_port = port;
  c.remote_port = port == 5000u ? 5001u : 5000u;
  c.receive_window = 8192u;
  c.outbound_streams = 4u;
  c.inbound_streams = 4u;
  c.t1_ms = 100u;
  c.t1_max_ms = 400u;
  c.cookie_lifetime_ms = 1000u;
  c.peer_binding = UINT64_C(0x0123456789abcdef);
  c.max_retransmits = 3u;
  c.is_owner = admitted;
  c.entropy = entropy;
  c.mac = fake_mac;
  c.verify_mac = fake_verify;
  c.provider_context = ctx;
  return c;
}
static void ready_pair(cnet_sctp_assoc *a, cnet_sctp_assoc *b,
                       test_context *ca, test_context *cb) {
  cnet_sctp_assoc_config ac, bc;
  uint8_t init[1200], ack[1200], echo[1200], final_ack[1200];
  size_t ni = 0u, na = 0u, ne = 0u, nk = 0u;
  memset(a, 0, sizeof(*a));
  memset(b, 0, sizeof(*b));
  ca->tag = UINT32_C(0x11223344);
  ca->tsn = UINT32_C(0xfffffffe);
  ca->test_key = UINT32_C(0x13579bdf);
  ca->owner = 1;
  cb->tag = UINT32_C(0x55667788);
  cb->tsn = 1000u;
  cb->test_key = ca->test_key;
  cb->owner = 1;
  ac = config(5000u, ca);
  bc = config(5001u, cb);
  assert(cnet_sctp_assoc_init(a, &ac, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(b, &bc, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_start(a, 1u, init, sizeof(init), &ni) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(b, 2u, init, ni, ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(a, 3u, ack, na, echo, sizeof(echo), &ne) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(b, 4u, echo, ne, final_ack, sizeof(final_ack), &nk) ==
         CNET_SCTP_ASSOC_OK);
  assert(b->state == CNET_SCTP_ASSOC_ESTABLISHED && b->data_ready);
  assert(b->data.next_tsn == 1000u && b->data.cumulative_rx == UINT32_C(0xfffffffd));
  assert(cnet_sctp_assoc_input(a, 5u, final_ack, nk,
                               init, sizeof(init), &ni) == CNET_SCTP_ASSOC_OK);
  assert(ni == 0u && a->state == CNET_SCTP_ASSOC_ESTABLISHED && a->data_ready);
  assert(a->data.next_tsn == UINT32_C(0xfffffffe));
  assert(a->data.cumulative_rx == 999u);
}
static void real_assoc_send_sack_and_t3(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t text[] = {'h','e','l','l','o'};
  const uint8_t pong[] = {'p','o','n','g'};
  uint8_t wire[1200], sack[1200], data[1200], answer[1200];
  uint8_t event[1024];
  cnet_sctp_data_message meta = {0};
  size_t nw = 0u, ns = 0u, ne = 0u, nr = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 51u, 0,
      text, sizeof(text), wire, 8u, &nw) == CNET_SCTP_ASSOC_CAPACITY);
  assert(nw == 0u && a.data.next_tsn == UINT32_C(0xfffffffe));
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 51u, 0,
      text, sizeof(text), wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK);
  assert(a.data.next_tsn == UINT32_C(0xffffffff) && a.data.in_flight == sizeof(text));
  assert(cnet_sctp_assoc_input(&b, 7u, wire, nw, sack, sizeof(sack), &ns) ==
         CNET_SCTP_ASSOC_OK);
  assert(ns != 0u);
  assert(cnet_sctp_assoc_take_message(&b, &meta, event, sizeof(event), &ne) ==
         CNET_SCTP_ASSOC_OK);
  assert(meta.stream_id == 0u && !meta.unordered && meta.stream_sequence == 0u);
  assert(meta.ppid == 51u && ne == sizeof(text));
  assert(memcmp(event, text, sizeof(text)) == 0);
  assert(cnet_sctp_assoc_take_message(&b, &meta, event, sizeof(event), &ne) ==
         CNET_SCTP_ASSOC_EMPTY);
  assert(cnet_sctp_assoc_input(&a, 8u, sack, ns, answer, sizeof(answer), &nr) ==
         CNET_SCTP_ASSOC_OK);
  assert(nr == 0u && a.data.in_flight == 0u);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
         CNET_SCTP_ASSOC_OK);
  assert(!has_deadline);

  assert(cnet_sctp_assoc_send_message(&b, 9u, 1u, 53u, 1,
      pong, sizeof(pong), data, sizeof(data), &ne) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 10u, data, ne, sack, sizeof(sack), &ns) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&a, &meta, event, sizeof(event), &nr) ==
         CNET_SCTP_ASSOC_OK);
  assert(meta.stream_id == 1u && meta.unordered && meta.ppid == 53u);
  assert(nr == sizeof(pong) && memcmp(event, pong, sizeof(pong)) == 0);
  assert(cnet_sctp_assoc_input(&b, 11u, sack, ns, answer, sizeof(answer), &nr) ==
         CNET_SCTP_ASSOC_OK);
  assert(b.data.in_flight == 0u);

  assert(cnet_sctp_assoc_send_message(&a, 12u, 0u, 51u, 0,
      text, sizeof(text), wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
         CNET_SCTP_ASSOC_OK);
  assert(has_deadline && deadline == 112u);
  assert(cnet_sctp_assoc_advance(&a, 112u, answer, sizeof(answer), &nr) ==
         CNET_SCTP_ASSOC_OK);
  assert(nr == nw && memcmp(answer, wire, nw) == 0);
  assert(cnet_sctp_assoc_input(&b, 113u, answer, nr, sack, sizeof(sack), &ns) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 114u, sack, ns, answer, sizeof(answer), &nr) ==
         CNET_SCTP_ASSOC_OK);
  assert(a.data.in_flight == 0u);
  ca.owner = 0;
  assert(cnet_sctp_assoc_send_message(&a, 115u, 0u, 51u, 0,
      text, sizeof(text), wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_OWNER);
  ca.owner = 1;
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(!a.data_ready && !b.data_ready);
  assert(cnet_sctp_assoc_take_message(&a, &meta, event, sizeof(event), &nr) ==
         CNET_SCTP_ASSOC_CLOSED_ERROR);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* Real DATA wire path: RFC 8832 OPEN/ACK on PPID 50, fixed Owner,
 * separate SACKs, followed by user text/binary data on the established SID.
 * Application-level DCEP owns channels; the SCTP Association owns TSN/T3.
 * No implicit second transport, background work or accepted message replay.
 */
static void post_handshake_dcep_wire(void) {
  cnet_sctp_assoc a, b;
  cnet_sctp_dcep client = {0}, server = {0};
  test_context ca = {0}, cb = {0};
  const uint8_t label[] = {'c','h','a','t'};
  const uint8_t protocol[] = {'j','s','o','n'};
  const uint8_t text[] = {'h','i'};
  uint8_t open[300], ack_payload[8], wire[1200], sack[1200], response[1200];
  uint8_t received[1024];
  cnet_sctp_data_message meta = {0};
  size_t nopen = 0u, nack = 0u, np = 0u, ns = 0u, nm = 0u, nr = 0u;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_dcep_init(&client, a.data.config.outbound_streams,
                             1, admitted, &ca) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_init(&server, b.data.config.inbound_streams,
                             0, admitted, &cb) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_open(&client, 0u, 0u, 100u, 0u,
      label, sizeof(label), protocol, sizeof(protocol),
      open, sizeof(open), &nopen) == CNET_SCTP_DCEP_OK);
  assert(nopen == 20u && !cnet_sctp_dcep_ready(&client, 0u));
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, CNET_SCTP_DCEP_PPID, 0,
      open, nopen, wire, 8u, &np) == CNET_SCTP_ASSOC_CAPACITY);
  assert(np == 0u && a.data.in_flight == 0u);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, CNET_SCTP_DCEP_PPID, 0,
      open, nopen, wire, sizeof(wire), &np) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, wire, np,
                               sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&b, &meta, received,
                                      sizeof(received), &nm) == CNET_SCTP_ASSOC_OK);
  assert(meta.stream_id == 0u && !meta.unordered &&
         meta.ppid == CNET_SCTP_DCEP_PPID && meta.stream_sequence == 0u);
  assert(nm == nopen && memcmp(received, open, nopen) == 0);
  cb.owner = 0;
  assert(cnet_sctp_dcep_input(&server, 0u, received, nm,
      ack_payload, sizeof(ack_payload), &nack) == CNET_SCTP_DCEP_OWNER);
  assert(server.channels[0].state == 0u);
  cb.owner = 1;
  assert(cnet_sctp_dcep_input(&server, 0u, received, nm,
      ack_payload, 0u, &nack) == CNET_SCTP_DCEP_CAPACITY);
  assert(server.channels[0].state == 0u && nack == 0u);
  assert(cnet_sctp_dcep_input(&server, 0u, received, nm,
      ack_payload, sizeof(ack_payload), &nack) == CNET_SCTP_DCEP_OK);
  assert(nack == 1u && ack_payload[0] == 2u);
  assert(cnet_sctp_dcep_ready(&server, 0u));
  assert(cnet_sctp_assoc_input(&a, 8u, sack, ns,
                               response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(a.data.in_flight == 0u && nr == 0u);
  assert(cnet_sctp_assoc_send_message(&b, 9u, 0u, CNET_SCTP_DCEP_PPID, 0,
      ack_payload, nack, wire, sizeof(wire), &np) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 10u, wire, np,
                               sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&a, &meta, received,
                                      sizeof(received), &nm) == CNET_SCTP_ASSOC_OK);
  assert(meta.ppid == CNET_SCTP_DCEP_PPID && meta.stream_id == 0u);
  assert(nm == 1u && received[0] == 2u);
  assert(cnet_sctp_dcep_input(&client, 0u, received, nm,
      response, sizeof(response), &nr) == CNET_SCTP_DCEP_OK);
  assert(nr == 0u && cnet_sctp_dcep_ready(&client, 0u));
  assert(cnet_sctp_assoc_input(&b, 11u, sack, ns,
                               response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(b.data.in_flight == 0u);
  /* Application PPID 51 text shares the same SID after DCEP, but never
   * masquerades as a DCEP control message. */
  assert(cnet_sctp_assoc_send_message(&a, 12u, 0u, 51u, 0,
      text, sizeof(text), wire, sizeof(wire), &np) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 13u, wire, np,
                               sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&b, &meta, received,
                                      sizeof(received), &nm) == CNET_SCTP_ASSOC_OK);
  assert(meta.stream_id == 0u && meta.stream_sequence == 1u &&
         meta.ppid == 51u && !meta.unordered);
  assert(nm == sizeof(text) && memcmp(received, text, nm) == 0);
  assert(cnet_sctp_assoc_input(&a, 14u, sack, ns,
                               response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(a.data.in_flight == 0u);
  assert(cnet_sctp_dcep_stop(&client) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_stop(&server) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* Post-handshake >16KiB stream delivery through the SAME Association,
 * with first fragment lost and recovered by T3 after Gap SACKs. This is the
 * actual private INIT/COOKIE -> DATA fragment -> SACK -> take() path.
 */
static void post_handshake_twenty_kib_fragments(void) {
  enum { MESSAGE = 20480, FRAGMENTS = 20 };
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  uint8_t original[MESSAGE], expected[MESSAGE], reassembled[MESSAGE];
  uint8_t wire[1200], sack[1200], response[1200];
  cnet_sctp_data_message metadata = {0};
  size_t nw = 0u, ns = 0u, nr = 0u;
  uint64_t now = 0u, deadline = 0u;
  int has_deadline = 0;
  unsigned int i;
  ready_pair(&a, &b, &ca, &cb);
  for (i = 0u; i < MESSAGE; ++i)
    original[i] = (uint8_t)(i * 17u + 5u);
  memcpy(expected, original, sizeof(original));
  assert(cnet_sctp_assoc_queue_message(&a, 6u, 2u, 53u, 0,
      original, sizeof(original)) == CNET_SCTP_ASSOC_OK);
  memset(original, 0u, sizeof(original)); /* sender owns queued bytes now */
  assert(cnet_sctp_assoc_pump_message(&a, 7u, wire, 10u, &nw) ==
         CNET_SCTP_ASSOC_CAPACITY);
  assert(nw == 0u && a.data.pending.offset == 0u);
  assert(cnet_sctp_assoc_pump_message(&a, 7u, wire,
                                    sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK);
  /* Drop first B-flag fragment. Sender must retain it as a T3 ticket. */
  for (i = 1u; i < FRAGMENTS; ++i) {
    now = 7u + (uint64_t)i * 3u;
    assert(cnet_sctp_assoc_pump_message(&a, now, wire, sizeof(wire), &nw) ==
           CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_input(&b, now + 1u, wire, nw,
                                 sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_take_message(&b, &metadata, reassembled,
                                        sizeof(reassembled), &nr) ==
           CNET_SCTP_ASSOC_EMPTY);
    assert(cnet_sctp_assoc_input(&a, now + 2u, sack, ns,
                                 response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
    assert(nr == 0u);
  }
  assert(!a.data.pending.active && a.data.tx_ssn[2u] == 1u);
  assert(b.data.buffered_rx == MESSAGE - CNET_SCTP_DATA_PAYLOAD_MAX);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
         CNET_SCTP_ASSOC_OK);
  assert(has_deadline);
  assert(cnet_sctp_assoc_advance(&a, deadline, wire,
                                 sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, deadline + 1u, wire, nw,
                               sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&b, &metadata, reassembled,
                                      1024u, &nr) == CNET_SCTP_ASSOC_CAPACITY);
  assert(nr == 0u && b.data.buffered_rx == MESSAGE);
  assert(cnet_sctp_assoc_take_message(&b, &metadata, reassembled,
                                      sizeof(reassembled), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == MESSAGE && metadata.size == MESSAGE);
  assert(metadata.stream_id == 2u && metadata.ppid == 53u &&
         metadata.stream_sequence == 0u && !metadata.unordered);
  assert(memcmp(reassembled, expected, MESSAGE) == 0);
  assert(cnet_sctp_assoc_input(&a, deadline + 2u, sack, ns,
                               response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(a.data.in_flight == 0u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(!a.data_ready && !b.data_ready);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

int main(void) {
  real_assoc_send_sack_and_t3();
  post_handshake_dcep_wire();
  post_handshake_twenty_kib_fragments();
  puts("C11 SCTP post-COOKIE Association DATA/SACK/T3 stream test PASS");
  return 0;
}
