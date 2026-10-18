#include "sctp_assoc_internal.h"
#include "sctp_dcep_internal.h"
#include "sctp_webrtc_internal.h"

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
 * with first fragment lost and repaired by Fast Retransmit after Gap SACKs. This is the
 * actual private INIT/COOKIE -> DATA fragment -> SACK -> take() path.
 */
static void post_handshake_twenty_kib_fragments(void) {
  enum { MESSAGE = 20480, FRAGMENTS = 20 };
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  uint8_t original[MESSAGE], expected[MESSAGE], reassembled[MESSAGE];
  uint8_t wire[1200], sack[1200], response[1200], first_wire[1200];
  cnet_sctp_data_message metadata = {0};
  size_t nw = 0u, ns = 0u, nr = 0u, first_size = 0u;
  uint64_t now = 7u, deadline = 0u;
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
  first_size = nw;
  memcpy(first_wire, wire, nw);
  /* Drop first B-flag fragment. All subsequent Gap ACKs are revocable:
   * keep their tickets and exercise backpressure through the Association. */
  for (i = 1u; i < FRAGMENTS; ++i) {
    now += 3u;
    if (i == 4u) {
      const size_t offset = a.data.pending.offset;
      assert(cnet_sctp_assoc_pump_message(&a, now, wire,
          sizeof(wire), &nw) == CNET_SCTP_ASSOC_RETRANSMIT_PENDING);
      assert(nw == 0u && offset == i * CNET_SCTP_DATA_PAYLOAD_MAX);
      assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
             CNET_SCTP_ASSOC_OK);
      assert(has_deadline && deadline <= now);
      assert(cnet_sctp_assoc_advance(&a, now, wire,
          sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK);
      assert(nw == first_size && memcmp(wire, first_wire, nw) == 0);
      assert(cnet_sctp_assoc_input(&b, now + 1u, wire, nw,
          sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
      assert(cnet_sctp_assoc_input(&a, now + 2u, sack, ns,
          response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
      assert(nr == 0u && a.data.in_flight == 0u &&
          a.data.pending.offset == offset);
      now += 3u;
    }
    assert(cnet_sctp_assoc_pump_message(&a, now, wire, sizeof(wire), &nw) ==
           CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_input(&b, now + 1u, wire, nw,
                                 sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
    if (i + 1u < FRAGMENTS)
      assert(cnet_sctp_assoc_take_message(&b, &metadata, reassembled,
                                          sizeof(reassembled), &nr) ==
             CNET_SCTP_ASSOC_EMPTY);
    assert(cnet_sctp_assoc_input(&a, now + 2u, sack, ns,
                                 response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
    assert(nr == 0u);
  }
  assert(!a.data.pending.active && a.data.tx_ssn[2u] == 1u);
  assert(b.data.buffered_rx == MESSAGE);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
         CNET_SCTP_ASSOC_OK);
  assert(!has_deadline && a.data.in_flight == 0u);
  assert(cnet_sctp_assoc_take_message(&b, &metadata, reassembled,
                                      1024u, &nr) == CNET_SCTP_ASSOC_CAPACITY);
  assert(nr == 0u && b.data.buffered_rx == MESSAGE);
  assert(cnet_sctp_assoc_take_message(&b, &metadata, reassembled,
                                      sizeof(reassembled), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == MESSAGE && metadata.size == MESSAGE);
  assert(metadata.stream_id == 2u && metadata.ppid == 53u &&
         metadata.stream_sequence == 0u && !metadata.unordered &&
         metadata.tsn == ca.tsn);
  assert(memcmp(reassembled, expected, MESSAGE) == 0);
  assert(cnet_sctp_assoc_take_message(&b, &metadata, reassembled,
                                      sizeof(reassembled), &nr) == CNET_SCTP_ASSOC_EMPTY);
  assert(nr == 0u && b.data.buffered_rx == 0u && b.data.rx_ssn[2u] == 1u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(!a.data_ready && !b.data_ready);
  assert(cnet_sctp_assoc_drained(&a) && cnet_sctp_assoc_drained(&b));
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* RFC 8831 WebRTC empty text/binary user messages: PPID 56/57 and
 * exactly one zero byte on SCTP DATA, then logical zero bytes to the ULP.
 * Application framing cannot accidentally reinterpret DCEP/partial PPIDs. */
static void post_handshake_webrtc_empty_ppids(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_webrtc_payload encoded = {0}, decoded = {0};
  cnet_sctp_data_message meta = {0};
  uint8_t packet[1200], sack[1200], response[1200], app[1200];
  static const uint8_t malformed[] = {0u, 0u};
  static const uint8_t fake_data[] = {'t','e','s','t'};
  size_t np = 0u, ns = 0u, nr = 0u;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_webrtc_prepare(0, NULL, 0u, &encoded) ==
         CNET_SCTP_WEBRTC_OK);
  assert(encoded.ppid == 56u && encoded.wire_size == 1u &&
         encoded.wire_bytes[0] == 0u && encoded.logical_size == 0u);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, encoded.ppid, 0,
      encoded.wire_bytes, encoded.wire_size,
      packet, sizeof(packet), &np) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, packet, np,
                               sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&b, &meta, app,
                                      sizeof(app), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 1u && meta.ppid == 56u);
  assert(cnet_sctp_webrtc_decode(meta.ppid, app, nr, &decoded) ==
         CNET_SCTP_WEBRTC_OK);
  assert(decoded.logical_size == 0u && !decoded.binary);
  assert(cnet_sctp_assoc_input(&a, 8u, sack, ns,
                               response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_webrtc_prepare(1, NULL, 0u, &encoded) ==
         CNET_SCTP_WEBRTC_OK);
  assert(encoded.ppid == 57u && encoded.wire_size == 1u);
  assert(cnet_sctp_assoc_send_message(&b, 9u, 2u, encoded.ppid, 1,
      encoded.wire_bytes, encoded.wire_size,
      packet, sizeof(packet), &np) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 10u, packet, np,
                               sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&a, &meta, app,
                                      sizeof(app), &nr) == CNET_SCTP_ASSOC_OK);
  assert(meta.ppid == 57u && nr == 1u);
  assert(cnet_sctp_webrtc_decode(meta.ppid, app, nr, &decoded) ==
         CNET_SCTP_WEBRTC_OK);
  assert(decoded.logical_size == 0u && decoded.binary);
  assert(cnet_sctp_assoc_input(&b, 11u, sack, ns,
                               response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_webrtc_prepare(0, fake_data, sizeof(fake_data), &encoded) ==
         CNET_SCTP_WEBRTC_OK);
  assert(encoded.ppid == 51u && encoded.wire_size == 4u);
  assert(cnet_sctp_webrtc_decode(51u, fake_data, 4u, &decoded) ==
         CNET_SCTP_WEBRTC_OK);
  assert(decoded.logical_size == 4u && !decoded.binary);
  assert(cnet_sctp_webrtc_decode(56u, malformed, 2u, &decoded) ==
         CNET_SCTP_WEBRTC_INVALID);
  assert(cnet_sctp_webrtc_decode(57u, fake_data, 1u, &decoded) ==
         CNET_SCTP_WEBRTC_INVALID);
  assert(cnet_sctp_webrtc_decode(50u, fake_data, 4u, &decoded) ==
         CNET_SCTP_WEBRTC_UNSUPPORTED);
  assert(cnet_sctp_webrtc_decode(52u, fake_data, 4u, &decoded) ==
         CNET_SCTP_WEBRTC_UNSUPPORTED);
  assert(cnet_sctp_webrtc_decode(54u, fake_data, 4u, &decoded) ==
         CNET_SCTP_WEBRTC_UNSUPPORTED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* Window updates are also delivered over the established, private
 * Association boundary: one Owner, CRC-correct DATA and returned SACKs. */
static void post_handshake_window_update(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  uint8_t body[1024] = {0};
  uint8_t wire[1200], sack[1200], response[1200];
  cnet_sctp_data_message meta = {0};
  size_t nw = 0u, ns = 0u, nr = 0u;
  unsigned int i;
  ready_pair(&a, &b, &ca, &cb);
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS; ++i) {
    const uint64_t now = 6u + i;
    assert(cnet_sctp_assoc_send_message(&a, now, 0u, 53u, 0,
        body, sizeof(body), wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_input(&b, now, wire, nw,
        sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_input(&a, now, sack, ns,
        response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
    assert(nr == 0u);
  }
  assert(b.data.buffered_rx == CNET_SCTP_DATA_RECEIVE_CEILING);
  assert(a.data.peer_rwnd == 0u && b.data.last_advertised_rwnd == 0u);
  assert(cnet_sctp_assoc_take_message(&b, &meta, body,
      sizeof(body), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == sizeof(body) && b.data.window_update_pending);
  cb.owner = 0;
  assert(cnet_sctp_assoc_window_update(&b, 40u, sack,
      sizeof(sack), &ns) == CNET_SCTP_ASSOC_OWNER);
  cb.owner = 1;
  {
    cnet_sctp_data old;
    memcpy(&old, &b.data, sizeof(old));
    assert(cnet_sctp_assoc_window_update(&b, 40u, sack,
        0u, &ns) == CNET_SCTP_ASSOC_CAPACITY);
    assert(ns == 0u && memcmp(&old, &b.data, sizeof(old)) == 0);
  }
  assert(cnet_sctp_assoc_window_update(&b, 40u, sack,
      sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(!b.data.window_update_pending);
  assert(cnet_sctp_assoc_input(&a, 40u, sack, ns,
      response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(a.data.peer_rwnd == CNET_SCTP_DATA_PAYLOAD_MAX);
  assert(cnet_sctp_assoc_window_update(&b, 40u, sack,
      sizeof(sack), &ns) == CNET_SCTP_ASSOC_EMPTY);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* RFC 9260 9.2 private shutdown tests use a real two-sided
 * INIT/COOKIE association.  No direct injection of shutdown states. */
static uint8_t shutdown_chunk_type(const uint8_t *wire, size_t bytes) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(wire, bytes, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  return chunk.type;
}
static void shutdown_no_inflight_and_t2(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  uint8_t req[1200], ack[1200], complete[1200], retry[1200];
  size_t nq = 0u, na = 0u, nc = 0u, nr = 0u;
  uint64_t deadline = 0u, saved_deadline;
  int has_deadline = 0;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_shutdown(&a, 6u, req, 10u, &nq) ==
      CNET_SCTP_ASSOC_CAPACITY);
  assert(nq == 0u && a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_shutdown(&a, 6u, req, sizeof(req), &nq) ==
      CNET_SCTP_ASSOC_OK);
  assert(nq == 20u && shutdown_chunk_type(req, nq) == 7u);
  assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 106u);
  assert(cnet_sctp_assoc_advance(&a, 105u, retry,
      sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK && nr == 0u);
  assert(cnet_sctp_assoc_advance(&a, 106u, retry,
      sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == nq && memcmp(req, retry, nr) == 0 &&
      a.deadline_ms == 306u);
  assert(cnet_sctp_assoc_input(&b, 107u, req, nq,
      ack, 10u, &na) == CNET_SCTP_ASSOC_CAPACITY);
  assert(na == 0u && b.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_input(&b, 107u, req, nq,
      ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(na == 16u && shutdown_chunk_type(ack, na) == 8u);
  assert(b.state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT);
  saved_deadline = b.deadline_ms;
  assert(cnet_sctp_assoc_input(&b, 108u, retry, nr,
      complete, sizeof(complete), &nc) == CNET_SCTP_ASSOC_OK);
  assert(nc == na && memcmp(complete, ack, na) == 0 &&
      b.deadline_ms == saved_deadline + 1u);
  saved_deadline = b.deadline_ms; /* duplicate SHUTDOWN restarted T2 */
  assert(cnet_sctp_assoc_input(&a, 109u, ack, na,
      complete, 10u, &nc) == CNET_SCTP_ASSOC_CAPACITY);
  assert(nc == 0u && a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);
  assert(cnet_sctp_assoc_input(&a, 109u, ack, na,
      complete, sizeof(complete), &nc) == CNET_SCTP_ASSOC_OK);
  assert(nc == 16u && shutdown_chunk_type(complete, nc) == 14u);
  assert(a.state == CNET_SCTP_ASSOC_STOPPED && cnet_sctp_assoc_drained(&a));
  assert(cnet_sctp_assoc_advance(&b, saved_deadline, retry,
      sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == na && memcmp(retry, ack, na) == 0);
  assert(cnet_sctp_assoc_input(&b, saved_deadline + 1u,
      complete, nc, retry, sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 0u && b.state == CNET_SCTP_ASSOC_STOPPED &&
      cnet_sctp_assoc_drained(&b));
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}
static void shutdown_pending_data_and_cumulative_ack(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t one[] = {0x32u};
  uint8_t wire[1200], sack[1200], response[1200], ack[1200];
  size_t nw = 0u, ns = 0u, nr = 0u, na = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 53u, 0,
      one, sizeof(one), wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK);
  assert(a.data.in_flight == 1u);
  assert(cnet_sctp_assoc_shutdown(&a, 7u, response,
      sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 0u && a.state == CNET_SCTP_ASSOC_SHUTDOWN_PENDING);
  assert(cnet_sctp_assoc_send_message(&a, 8u, 0u, 53u, 0,
      one, sizeof(one), response, sizeof(response), &nr) ==
      CNET_SCTP_ASSOC_CLOSED_ERROR);
  assert(cnet_sctp_assoc_input(&b, 8u, wire, nw,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 9u, sack, ns,
      response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(a.data.in_flight == 0u);
  assert(cnet_sctp_assoc_send_message(&b, 9u, 0u, 53u, 0,
      one, sizeof(one), wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK);
  assert(b.data.in_flight == 1u);
  assert(cnet_sctp_assoc_input(&a, 10u, wire, nw,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == a.last_time_ms);
  assert(cnet_sctp_assoc_advance(&a, 11u, response, 10u, &nr) ==
      CNET_SCTP_ASSOC_CAPACITY);
  assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_PENDING);
  assert(cnet_sctp_assoc_advance(&a, 11u,
      response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 20u && shutdown_chunk_type(response, nr) == 7u);
  assert(cnet_sctp_assoc_input(&b, 12u, response, nr,
      ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(b.data.in_flight == 0u && b.state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT);
  assert(na == 16u && shutdown_chunk_type(ack, na) == 8u);
  assert(cnet_sctp_assoc_input(&a, 13u, ack, na,
      response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 16u && shutdown_chunk_type(response, nr) == 14u);
  assert(cnet_sctp_assoc_input(&b, 14u, response, nr,
      wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK);
  assert(a.state == CNET_SCTP_ASSOC_STOPPED &&
      b.state == CNET_SCTP_ASSOC_STOPPED);
  /* Each side still owns one byte acknowledged before wire termination. */
  assert(!cnet_sctp_assoc_drained(&a) && !cnet_sctp_assoc_drained(&b));
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_BUSY);
  {
    cnet_sctp_data_message meta = {0};
    uint8_t received[8] = {0};
    size_t bytes = 0u;
    assert(cnet_sctp_assoc_take_message(&a, &meta,
        received, 0u, &bytes) == CNET_SCTP_ASSOC_CAPACITY);
    assert(bytes == 0u && !cnet_sctp_assoc_drained(&a));
    assert(cnet_sctp_assoc_take_message(&a, &meta,
        received, sizeof(received), &bytes) == CNET_SCTP_ASSOC_OK);
    assert(bytes == 1u && received[0] == one[0]);
    assert(cnet_sctp_assoc_take_message(&b, &meta,
        received, sizeof(received), &bytes) == CNET_SCTP_ASSOC_OK);
    assert(bytes == 1u && received[0] == one[0]);
  }
  assert(cnet_sctp_assoc_drained(&a) && cnet_sctp_assoc_drained(&b));
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}
/* A late peer DATA arriving while SHUTDOWN-SENT advances our receive TSN.
 * The T2 retransmitted SHUTDOWN must carry the updated cumulative value.
 * Output-capacity failure may not mutate the saved packet/deadline/clock. */
/* A packet already in flight may contain multiple DATA chunks even
 * after the local endpoint sent SHUTDOWN; it must still be admitted and
 * cumulatively acknowledged while T2 is running. */
static void shutdown_sent_accepts_bundled_data(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t payloads[2] = {'m', 'n'};
  uint8_t shutdown[1200], packets[2][1200], combined[1200], sack[1200];
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunks[2];
  cnet_sctp_data_message meta = {0};
  cnet_sctp_data before;
  uint8_t body[8] = {0}, saved_before[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t saved_size_before;
  uint64_t deadline_before, deadline_after_retry;
  uint32_t retries_before;
  size_t nshutdown = 0u, np[2] = {0u, 0u}, ncombined = 0u;
  size_t nsack = 0u, nbody = 0u, i;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_shutdown(&a, 6u, shutdown,
      sizeof(shutdown), &nshutdown) == CNET_SCTP_ASSOC_OK);
  assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);
  for (i = 0u; i < 2u; ++i) {
    assert(cnet_sctp_assoc_send_message(&b, 6u + i, 0u, 53u, 0,
        &payloads[i], 1u, packets[i], sizeof(packets[i]), &np[i]) ==
        CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_wire_decode(packets[i], np[i], &header, &decoder) ==
        CNET_SCTP_WIRE_OK);
    assert(header.chunk_count == 1u);
    assert(cnet_sctp_wire_next_chunk(&decoder, &chunks[i]) ==
        CNET_SCTP_WIRE_OK && chunks[i].type == 0u);
  }
  assert(cnet_sctp_wire_encode(b.config.local_port, b.config.remote_port,
      b.peer_tag, chunks, 2u, combined, sizeof(combined), &ncombined) ==
      CNET_SCTP_WIRE_OK);
  deadline_before = a.deadline_ms;
  assert(cnet_sctp_assoc_advance(&a, deadline_before, shutdown,
      sizeof(shutdown), &nshutdown) == CNET_SCTP_ASSOC_OK);
  assert(a.retransmits == 1u && a.deadline_ms > deadline_before);
  deadline_after_retry = a.deadline_ms;
  memcpy(&before, &a.data, sizeof(before));
  memcpy(saved_before, a.saved_packet, sizeof(saved_before));
  saved_size_before = a.saved_size;
  retries_before = a.retransmits;
  nsack = 99u;
  assert(cnet_sctp_assoc_input(&a, deadline_before + 1u, combined, ncombined,
      sack, 1u, &nsack) == CNET_SCTP_ASSOC_CAPACITY);
  assert(nsack == 0u && memcmp(&before, &a.data, sizeof(before)) == 0 &&
      a.saved_size == saved_size_before &&
      memcmp(saved_before, a.saved_packet, sizeof(saved_before)) == 0 &&
      a.deadline_ms == deadline_after_retry && a.retransmits == retries_before);
  assert(cnet_sctp_assoc_input(&a, deadline_before + 2u, combined,
      ncombined, sack, sizeof(sack), &nsack) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_wire_decode(sack, nsack, &header, &decoder) ==
      CNET_SCTP_WIRE_OK && header.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunks[0]) ==
      CNET_SCTP_WIRE_OK && chunks[0].type == 7u);
  assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
      a.deadline_ms == deadline_before + 2u + a.current_rto_ms && a.retransmits == 0u &&
      a.data.buffered_rx == 2u && a.data.cumulative_rx == cb.tsn + 1u);
  for (i = 0u; i < 2u; ++i) {
    assert(cnet_sctp_assoc_take_message(&a, &meta, body,
        sizeof(body), &nbody) == CNET_SCTP_ASSOC_OK);
    assert(nbody == 1u && body[0] == payloads[i]);
  }
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* RFC 9260 §6: SACK is not processed in SHUTDOWN-SENT. Reuse a valid SACK
 * captured before shutdown and verify it cannot settle or mutate TX state. */
static void shutdown_sent_does_not_process_sack(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t body[] = {'p'};
  uint8_t data[1200], sack[1200], reply[1200], shutdown[1200];
  cnet_sctp_data before;
  size_t ndata = 0u, nsack = 0u, nreply = 0u, nshutdown = 0u;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 53u, 0,
      body, sizeof(body), data, sizeof(data), &ndata) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, data, ndata,
      sack, sizeof(sack), &nsack) == CNET_SCTP_ASSOC_OK);
  assert(nsack != 0u && shutdown_chunk_type(sack, nsack) == 3u);
  assert(cnet_sctp_assoc_input(&a, 8u, sack, nsack,
      reply, sizeof(reply), &nreply) == CNET_SCTP_ASSOC_OK);
  assert(a.data.in_flight == 0u);
  assert(cnet_sctp_assoc_shutdown(&a, 9u, shutdown,
      sizeof(shutdown), &nshutdown) == CNET_SCTP_ASSOC_OK);
  assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);
  memcpy(&before, &a.data, sizeof(before));
  assert(cnet_sctp_assoc_input(&a, 10u, sack, nsack,
      reply, sizeof(reply), &nreply) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(nreply == 0u && a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
      memcmp(&before, &a.data, sizeof(before)) == 0);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void shutdown_t2_refreshes_cumulative(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t user[] = {'x'};
  uint8_t initial[1200], data[1200], sack[1200], update[1200];
  uint8_t ack[1200], complete[1200];
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  size_t ninit = 0u, nd = 0u, ns = 0u, nu = 0u, na = 0u, nc = 0u;
  uint64_t old_deadline;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_shutdown(&a, 6u, initial,
      sizeof(initial), &ninit) == CNET_SCTP_ASSOC_OK);
  assert(ninit == 20u && shutdown_chunk_type(initial, ninit) == 7u);
  assert(cnet_sctp_assoc_send_message(&b, 7u, 0u, 53u, 0,
      user, sizeof(user), data, sizeof(data), &nd) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 8u, data, nd,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(a.data.cumulative_rx == cb.tsn);
  old_deadline = a.deadline_ms;
  assert(cnet_sctp_assoc_advance(&a, old_deadline, update,
      10u, &nu) == CNET_SCTP_ASSOC_CAPACITY);
  assert(nu == 0u && a.deadline_ms == old_deadline &&
      a.last_time_ms == 8u && a.saved_size == ninit &&
      memcmp(a.saved_packet, initial, ninit) != 0);
  assert(cnet_sctp_assoc_advance(&a, old_deadline, update,
      sizeof(update), &nu) == CNET_SCTP_ASSOC_OK);
  assert(nu == ninit && memcmp(update, initial, ninit) != 0);
  assert(cnet_sctp_wire_decode(update, nu, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  assert(chunk.type == 7u && chunk.value_size == 4u);
  assert(chunk.value[0] == 0u && chunk.value[1] == 0u &&
      chunk.value[2] == (uint8_t)(cb.tsn >> 8) &&
      chunk.value[3] == (uint8_t)cb.tsn);
  assert(cnet_sctp_assoc_input(&b, old_deadline + 1u, update, nu,
      ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(b.data.in_flight == 0u && b.state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT);
  assert(cnet_sctp_assoc_input(&a, old_deadline + 2u, ack, na,
      complete, sizeof(complete), &nc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, old_deadline + 3u,
      complete, nc, data, sizeof(data), &nd) == CNET_SCTP_ASSOC_OK);
  assert(!cnet_sctp_assoc_drained(&a) && cnet_sctp_assoc_drained(&b));
  {
    cnet_sctp_data_message meta = {0};
    uint8_t payload[8] = {0};
    size_t bytes = 0u;
    assert(cnet_sctp_assoc_take_message(&a, &meta, payload,
        sizeof(payload), &bytes) == CNET_SCTP_ASSOC_OK);
    assert(bytes == 1u && payload[0] == user[0]);
  }
  assert(cnet_sctp_assoc_drained(&a) && cnet_sctp_assoc_drained(&b));
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}
/* Receiver cannot SHUTDOWN ACK until all its outbound DATA is settled.
 * The SHUTDOWN cumulative TSN confirms only the first of two peer tickets;
 * a later real DATA/SACK exchange releases the second through the same Owner.
 */
static void shutdown_received_waits_for_final_sack(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t body[] = {0x4au};
  uint8_t first[1200], second[1200], sack[1200];
  uint8_t shutdown[1200], ack[1200], complete[1200];
  size_t nf = 0u, nsnd = 0u, ns = 0u, nq = 0u, na = 0u, nc = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&b, 6u, 0u, 53u, 0,
      body, sizeof(body), first, sizeof(first), &nf) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_send_message(&b, 7u, 0u, 53u, 0,
      body, sizeof(body), second, sizeof(second), &nsnd) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 8u, first, nf,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 9u, sack, ns,
      ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(b.data.in_flight == 1u);
  assert(cnet_sctp_assoc_shutdown(&a, 10u, shutdown,
      sizeof(shutdown), &nq) == CNET_SCTP_ASSOC_OK);
  assert(shutdown_chunk_type(shutdown, nq) == 7u);
  assert(cnet_sctp_assoc_input(&b, 11u, shutdown, nq,
      ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(na == 0u && b.state == CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED);
  assert(b.data.in_flight == 1u);
  assert(cnet_sctp_assoc_send_message(&b, 12u, 0u, 53u, 0,
      body, sizeof(body), ack, sizeof(ack), &na) ==
      CNET_SCTP_ASSOC_CLOSED_ERROR);
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline > 11u);
  assert(cnet_sctp_assoc_input(&a, 12u, second, nsnd,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  /* The SHUTDOWN response to the second DATA cumulatively acknowledges
   * the last ticket. The peer must immediately answer with SHUTDOWN ACK;
   * waiting for a later timer advance would delay the RFC 9260 transition. */
  assert(b.data.in_flight == 0u &&
      b.state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT);
  assert(shutdown_chunk_type(ack, na) == 8u);
  assert(cnet_sctp_assoc_input(&a, 14u, ack, na,
      complete, sizeof(complete), &nc) == CNET_SCTP_ASSOC_OK);
  assert(shutdown_chunk_type(complete, nc) == 14u);
  assert(cnet_sctp_assoc_input(&b, 15u, complete, nc,
      first, sizeof(first), &nf) == CNET_SCTP_ASSOC_OK);
  assert(a.state == CNET_SCTP_ASSOC_STOPPED &&
      b.state == CNET_SCTP_ASSOC_STOPPED);
  assert(!cnet_sctp_assoc_drained(&a) && cnet_sctp_assoc_drained(&b));
  {
    cnet_sctp_data_message meta = {0};
    uint8_t payload[8] = {0};
    size_t bytes = 0u;
    assert(cnet_sctp_assoc_take_message(&a, &meta, payload,
        sizeof(payload), &bytes) == CNET_SCTP_ASSOC_OK);
    assert(bytes == 1u && payload[0] == body[0] && meta.stream_sequence == 0u);
    assert(cnet_sctp_assoc_take_message(&a, &meta, payload,
        sizeof(payload), &bytes) == CNET_SCTP_ASSOC_OK);
    assert(bytes == 1u && payload[0] == body[0] && meta.stream_sequence == 1u);
  }
  assert(cnet_sctp_assoc_drained(&a));
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}
static void shutdown_simultaneous(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  uint8_t aq[1200], bq[1200], aa[1200], ba[1200], out[1200];
  size_t na = 0u, nb = 0u, naa = 0u, nba = 0u, nc = 0u;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_shutdown(&a, 6u, aq, sizeof(aq), &na) ==
      CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_shutdown(&b, 6u, bq, sizeof(bq), &nb) ==
      CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 7u, bq, nb,
      aa, sizeof(aa), &naa) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, aq, na,
      ba, sizeof(ba), &nba) == CNET_SCTP_ASSOC_OK);
  assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT &&
      b.state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT);
  assert(shutdown_chunk_type(aa, naa) == 8u &&
      shutdown_chunk_type(ba, nba) == 8u);
  assert(cnet_sctp_assoc_input(&a, 8u, ba, nba,
      out, sizeof(out), &nc) == CNET_SCTP_ASSOC_OK);
  assert(shutdown_chunk_type(out, nc) == 14u &&
      a.state == CNET_SCTP_ASSOC_STOPPED);
  assert(cnet_sctp_assoc_input(&b, 8u, aa, naa,
      out, sizeof(out), &nc) == CNET_SCTP_ASSOC_OK);
  assert(shutdown_chunk_type(out, nc) == 14u &&
      b.state == CNET_SCTP_ASSOC_STOPPED);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* RFC 9260 6.9 multi-chunk traffic through the actual authenticated
 * INIT/COOKIE Association, including TSN wrap and all-or-none refusal. */
static void post_cookie_bundled_data(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  uint8_t frames[3][1200], combined[1200], sack[1200], response[1200];
  const uint8_t bytes[3] = {'u', 'v', 'w'};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunks[3], broken[3];
  cnet_sctp_data_message meta = {0};
  uint8_t body[8] = {0};
  size_t sizes[3], n = 0u, ns = 0u, nr = 0u, i;
  ready_pair(&a, &b, &ca, &cb);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_assoc_send_message(&a, 6u + i, 0u, 53u, 0,
        &bytes[i], 1u, frames[i], sizeof(frames[i]), &sizes[i]) ==
        CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_wire_decode(frames[i], sizes[i], &header,
        &decoder) == CNET_SCTP_WIRE_OK);
    assert(header.chunk_count == 1u);
    assert(cnet_sctp_wire_next_chunk(&decoder, &chunks[i]) ==
        CNET_SCTP_WIRE_OK);
    assert(chunks[i].type == 0u);
  }
  assert(a.data.next_tsn == ca.tsn + 3u); /* rollover: fffffffe -> 1 */
  memcpy(broken, chunks, sizeof(broken));
  broken[2].flags = 0x80u;
  assert(cnet_sctp_wire_encode(a.config.local_port,
      a.config.remote_port, a.peer_tag, broken, 3u,
      combined, sizeof(combined), &n) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_assoc_input(&b, 10u, combined, n,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(ns == 0u && b.data.buffered_rx == 0u &&
      b.data.cumulative_rx == ca.tsn - 1u);
  assert(cnet_sctp_wire_encode(a.config.local_port,
      a.config.remote_port, a.peer_tag, chunks, 3u,
      combined, sizeof(combined), &n) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_assoc_input(&b, 10u, combined, n,
      sack, 8u, &ns) == CNET_SCTP_ASSOC_CAPACITY);
  assert(ns == 0u && b.data.buffered_rx == 0u);
  assert(cnet_sctp_assoc_input(&b, 10u, combined, n,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(shutdown_chunk_type(sack, ns) == 3u);
  assert(b.data.cumulative_rx == ca.tsn + 2u &&
      b.data.buffered_rx == 3u);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_assoc_take_message(&b, &meta, body,
        sizeof(body), &nr) == CNET_SCTP_ASSOC_OK);
    assert(nr == 1u && body[0] == bytes[i] && meta.stream_sequence == (uint16_t)i);
  }
  assert(cnet_sctp_assoc_input(&a, 11u, sack, ns,
      response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 0u && a.data.in_flight == 0u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}


/* T2 is bounded by the configured retransmit budget. Capacity failures at
 * a due deadline are retryable and must not consume a retry or advance time;
 * after the budget is exhausted, the association fails closed. */
static void shutdown_t2_retry_budget(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  uint8_t shutdown[1200], retry[1200], shutdown_ack[1200];
  size_t ns = 0u, nr = 0u, na = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_shutdown(&a, 6u, shutdown,
      sizeof(shutdown), &ns) == CNET_SCTP_ASSOC_OK);
  assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT && ns == 20u);
  assert(cnet_sctp_assoc_input(&b, 7u, shutdown, ns,
      shutdown_ack, sizeof(shutdown_ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(b.state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT &&
      na == 16u && shutdown_chunk_type(shutdown_ack, na) == 8u);
  /* A duplicate SHUTDOWN retransmits the ACK and restarts T2, but a
   * capacity failure must leave the timer and retry budget untouched. */
  assert(cnet_sctp_assoc_input(&b, 50u, shutdown, ns,
      retry, 8u, &nr) == CNET_SCTP_ASSOC_CAPACITY);
  assert(nr == 0u && b.retransmits == 0u && b.deadline_ms == 107u &&
      b.current_rto_ms == 100u && b.last_time_ms == 50u);
  assert(cnet_sctp_assoc_input(&b, 50u, shutdown, ns,
      retry, sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == na && memcmp(retry, shutdown_ack, na) == 0 &&
      b.retransmits == 0u && b.deadline_ms == 150u);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 106u);
  assert(cnet_sctp_assoc_advance(&a, 106u, retry, 8u, &nr) ==
      CNET_SCTP_ASSOC_CAPACITY);
  assert(nr == 0u && a.retransmits == 0u && a.deadline_ms == 106u &&
      a.last_time_ms == 6u);
  assert(cnet_sctp_assoc_advance(&a, 106u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_OK);
  assert(nr == ns && memcmp(retry, shutdown, ns) == 0 &&
      a.retransmits == 1u && a.deadline_ms == 306u);
  assert(cnet_sctp_assoc_advance(&a, 306u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_OK);
  assert(a.retransmits == 2u && a.deadline_ms == 706u);
  assert(cnet_sctp_assoc_advance(&a, 706u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_OK);
  assert(a.retransmits == 3u && a.deadline_ms == 1106u);
  assert(cnet_sctp_assoc_advance(&a, 1106u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_EXPIRED);
  assert(nr == 0u && a.state == CNET_SCTP_ASSOC_FAILED &&
      a.deadline_ms == 0u && a.saved_size == 0u);
  assert(cnet_sctp_assoc_input(&a, 1107u, shutdown, ns,
      retry, sizeof(retry), &nr) == CNET_SCTP_ASSOC_CLOSED_ERROR);
  /* The responder's T2 budget for SHUTDOWN ACK is independently bounded. */
  assert(cnet_sctp_assoc_advance(&b, 150u, retry, 8u, &nr) ==
      CNET_SCTP_ASSOC_CAPACITY);
  assert(nr == 0u && b.retransmits == 0u && b.deadline_ms == 150u &&
      b.last_time_ms == 50u);
  assert(cnet_sctp_assoc_advance(&b, 150u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_OK);
  assert(nr == na && memcmp(retry, shutdown_ack, na) == 0 &&
      b.retransmits == 1u && b.deadline_ms == 350u);
  assert(cnet_sctp_assoc_advance(&b, 350u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_OK);
  assert(b.retransmits == 2u && b.deadline_ms == 750u);
  assert(cnet_sctp_assoc_advance(&b, 750u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_OK);
  assert(b.retransmits == 3u && b.deadline_ms == 1150u);
  assert(cnet_sctp_assoc_advance(&b, 1150u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_EXPIRED);
  assert(nr == 0u && b.state == CNET_SCTP_ASSOC_FAILED &&
      b.deadline_ms == 0u && b.saved_size == 0u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

int main(void) {
  post_cookie_bundled_data();
  shutdown_no_inflight_and_t2();
  shutdown_t2_retry_budget();
  shutdown_pending_data_and_cumulative_ack();
  shutdown_t2_refreshes_cumulative();
  shutdown_sent_accepts_bundled_data();
  shutdown_sent_does_not_process_sack();
  shutdown_received_waits_for_final_sack();
  shutdown_simultaneous();
  post_handshake_window_update();
  real_assoc_send_sack_and_t3();
  post_handshake_dcep_wire();
  post_handshake_twenty_kib_fragments();
  post_handshake_webrtc_empty_ppids();
  puts("C11 SCTP post-COOKIE Association DATA/SACK/T3 stream test PASS");
  return 0;
}
