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
/* Valid CRC32C on an incoming control with nonzero reserved flags,
 * without mutating the actual Owner-retained outgoing control. */
static void retag_control_packet(uint8_t *packet, size_t size,
    uint8_t flags) {
  uint8_t modified[CNET_SCTP_ASSOC_MAX_PACKET];
  cnet_sctp_wire_header h = {0};
  cnet_sctp_wire_decoder d = {0};
  cnet_sctp_wire_chunk ch = {0};
  size_t written = 0u;
  assert(packet && size <= sizeof(modified));
  assert(cnet_sctp_wire_decode(packet, size, &h, &d) == CNET_SCTP_WIRE_OK);
  assert(h.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&d, &ch) == CNET_SCTP_WIRE_OK);
  ch.flags = flags;
  assert(cnet_sctp_wire_encode(h.source_port, h.destination_port,
      h.verification_tag, &ch, 1u, modified, sizeof(modified),
      &written) == CNET_SCTP_WIRE_OK);
  assert(written == size);
  memcpy(packet, modified, written);
}

/* Output backpressure for a single accepted DATA Chunk must not move
 * the Association Owner clock while its bounded DATA TCB is unchanged.
 * The same packet/time and a larger SACK buffer must succeed once only. */
static void single_data_sack_capacity_preserves_input_watermark(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  const uint8_t message = 'w';
  uint8_t packet[1200], sack[1200], reply[1200], payload[8];
  cnet_sctp_data_message meta = {0};
  size_t packet_n = 0u, sack_n = 123u, reply_n = 0u, payload_n = 0u;
  size_t i;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 53u, 0,
      &message, 1u, packet, sizeof(packet), &packet_n) ==
      CNET_SCTP_ASSOC_OK);
  memcpy(&before, &b, sizeof(before));
  memset(sack, 0xCC, sizeof(sack));
  assert(cnet_sctp_assoc_input(&b, 7u, packet, packet_n,
      sack, 1u, &sack_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(sack_n == 0u && memcmp(&b, &before, sizeof(before)) == 0);
  for (i = 0u; i < sizeof(sack); ++i) assert(sack[i] == 0xCCu);
  assert(cnet_sctp_assoc_input(&b, 7u, packet, packet_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(sack_n > 0u && b.state == CNET_SCTP_ASSOC_ESTABLISHED &&
      b.last_time_ms == 7u && b.data.last_time_ms == 7u &&
      b.data.buffered_rx == 1u);
  assert(cnet_sctp_assoc_input(&a, 8u, sack, sack_n,
      reply, sizeof(reply), &reply_n) == CNET_SCTP_ASSOC_OK);
  assert(reply_n == 0u && a.data.in_flight == 0u);
  assert(cnet_sctp_assoc_take_message(&b, &meta, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(payload_n == 1u && payload[0] == message &&
      meta.stream_sequence == 0u && meta.stream_id == 0u);
  assert(cnet_sctp_assoc_take_message(&b, &meta, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_EMPTY);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
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
  /* RFC 9260 3.3.4: the peer transmits zero SACK flags, but the
   * receiving Association must ignore reserved flags. Re-encode the
   * already authenticated SACK with nonzero flags and a correct CRC. */
  {
    cnet_sctp_wire_header sack_header = {0};
    cnet_sctp_wire_decoder sack_decoder = {0};
    cnet_sctp_wire_chunk sack_chunk = {0};
    uint8_t flagged[1200];
    size_t flagged_n = 0u;
    assert(cnet_sctp_wire_decode(sack, ns, &sack_header, &sack_decoder) ==
        CNET_SCTP_WIRE_OK && sack_header.chunk_count == 1u);
    assert(cnet_sctp_wire_next_chunk(&sack_decoder, &sack_chunk) ==
        CNET_SCTP_WIRE_OK && sack_chunk.type == 3u);
    sack_chunk.flags = 0xA5u;
    assert(cnet_sctp_wire_encode(sack_header.source_port,
        sack_header.destination_port, sack_header.verification_tag,
        &sack_chunk, 1u, flagged, sizeof(flagged), &flagged_n) ==
        CNET_SCTP_WIRE_OK);
    assert(cnet_sctp_assoc_input(&a, 8u, flagged, flagged_n,
        answer, sizeof(answer), &nr) == CNET_SCTP_ASSOC_OK);
  }
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
  /* ACK bytes exist, but the peer channel is not open until SCTP accepts it. */
  assert(!cnet_sctp_dcep_ready(&server, 0u));
  assert(cnet_sctp_assoc_input(&a, 8u, sack, ns,
                               response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(a.data.in_flight == 0u && nr == 0u);
  /* Queue-capacity refusal must not consume the ACK or open the channel. */
  assert(cnet_sctp_assoc_send_message(&b, 9u, 0u, CNET_SCTP_DCEP_PPID, 0,
      ack_payload, nack, wire, 8u, &np) == CNET_SCTP_ASSOC_CAPACITY);
  assert(np == 0u && b.data.in_flight == 0u);
  assert(!cnet_sctp_dcep_ready(&server, 0u));
  assert(server.channels[0].state == 3u); /* ACK_PENDING remains retryable. */
  assert(cnet_sctp_assoc_send_message(&b, 9u, 0u, CNET_SCTP_DCEP_PPID, 0,
      ack_payload, nack, wire, sizeof(wire), &np) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_dcep_ack_admitted(&server, 0u) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_ready(&server, 0u));
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
  /* A valid output object is cleared even when the remaining arguments fail. */
  encoded.ppid = 999u;
  encoded.wire_bytes = fake_data;
  encoded.wire_size = sizeof(fake_data);
  encoded.logical_size = sizeof(fake_data);
  encoded.binary = 1u;
  assert(cnet_sctp_webrtc_prepare(2, fake_data, sizeof(fake_data), &encoded) ==
         CNET_SCTP_WEBRTC_INVALID);
  assert(encoded.ppid == 0u && encoded.wire_bytes == NULL &&
         encoded.wire_size == 0u && encoded.logical_size == 0u &&
         encoded.binary == 0u);
  decoded.ppid = 999u;
  decoded.wire_bytes = fake_data;
  decoded.wire_size = sizeof(fake_data);
  decoded.logical_size = sizeof(fake_data);
  decoded.binary = 1u;
  assert(cnet_sctp_webrtc_prepare(0, NULL, 1u, &encoded) ==
         CNET_SCTP_WEBRTC_INVALID);
  assert(encoded.ppid == 0u && encoded.wire_bytes == NULL &&
         encoded.wire_size == 0u && encoded.logical_size == 0u &&
         encoded.binary == 0u);
  assert(cnet_sctp_webrtc_decode(51u, NULL, sizeof(fake_data), &decoded) ==
         CNET_SCTP_WEBRTC_INVALID);
  assert(decoded.ppid == 0u && decoded.wire_bytes == NULL &&
         decoded.wire_size == 0u && decoded.logical_size == 0u &&
         decoded.binary == 0u);
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
  uint8_t flagged_ack[1200];
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
  retag_control_packet(req, nq, 0x80u); /* SHUTDOWN reserved */
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
  memcpy(flagged_ack, ack, na);
  retag_control_packet(flagged_ack, na, 0x40u); /* SHUTDOWN ACK */
  assert(cnet_sctp_assoc_input(&a, 109u, flagged_ack, na,
      complete, 10u, &nc) == CNET_SCTP_ASSOC_CAPACITY);
  assert(nc == 0u && a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);
  assert(cnet_sctp_assoc_input(&a, 109u, flagged_ack, na,
      complete, sizeof(complete), &nc) == CNET_SCTP_ASSOC_OK);
  assert(nc == 16u && shutdown_chunk_type(complete, nc) == 14u);
  assert(a.state == CNET_SCTP_ASSOC_STOPPED && cnet_sctp_assoc_drained(&a));
  assert(cnet_sctp_assoc_advance(&b, saved_deadline, retry,
      sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == na && memcmp(retry, ack, na) == 0);
  retag_control_packet(complete, nc, 0x80u); /* COMPLETE T=0, reserved */
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
/* A SHUTDOWN-SENT endpoint that receives out-of-order DATA must bundle
 * SHUTDOWN with the gap SACK. The peer must process both chunks atomically:
 * the SACK retires the second ticket, while the missing first ticket remains
 * in flight until its DATA arrives. */
static void shutdown_sent_bundles_gap_sack(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t payloads[2] = {'a', 'b'};
  uint8_t first[1200], second[1200], shutdown[1200];
  uint8_t response[1200], ack[1200], complete[1200];
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  size_t nfirst = 0u, nsecond = 0u, nshutdown = 0u;
  size_t nresponse = 0u, nack = 0u, ncomplete = 0u;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&b, 6u, 0u, 53u, 0,
      &payloads[0], 1u, first, sizeof(first), &nfirst) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_send_message(&b, 7u, 0u, 53u, 0,
      &payloads[1], 1u, second, sizeof(second), &nsecond) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_shutdown(&a, 8u, shutdown,
      sizeof(shutdown), &nshutdown) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 9u, shutdown, nshutdown,
      ack, sizeof(ack), &nack) == CNET_SCTP_ASSOC_OK);
  assert(nack == 0u && b.state == CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED &&
      b.data.in_flight == 2u);

  /* Receive TSN+1 first. Its gap block cannot be represented by SHUTDOWN's
   * cumulative TSN, so the response contains both SHUTDOWN and SACK. */
  assert(cnet_sctp_assoc_input(&a, 10u, second, nsecond,
      response, sizeof(response), &nresponse) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_wire_decode(response, nresponse, &header, &decoder) ==
      CNET_SCTP_WIRE_OK && header.chunk_count == 2u);
  /* SHUTDOWN+SACK has a separate Association preflight. Prove that
   * ignored SACK flag bits do not reject a well-formed legal bundle. */
  {
    cnet_sctp_wire_chunk flagged_chunks[2];
    uint8_t flagged_packet[1200];
    size_t flagged_n = 0u;
    assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) ==
        CNET_SCTP_WIRE_OK && chunk.type == 7u);
    flagged_chunks[0] = chunk;
    assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) ==
        CNET_SCTP_WIRE_OK && chunk.type == 3u);
    flagged_chunks[1] = chunk;
    flagged_chunks[1].flags = 0x5Au;
    assert(cnet_sctp_wire_encode(header.source_port,
        header.destination_port, header.verification_tag,
        flagged_chunks, 2u, flagged_packet, sizeof(flagged_packet),
        &flagged_n) == CNET_SCTP_WIRE_OK);
    assert(cnet_sctp_assoc_input(&b, 11u, flagged_packet, flagged_n,
        ack, sizeof(ack), &nack) == CNET_SCTP_ASSOC_OK);
  }
  assert(nack == 0u && b.state == CNET_SCTP_ASSOC_SHUTDOWN_RECEIVED &&
      b.data.in_flight == 1u);

  /* The missing first TSN closes the gap. A plain SHUTDOWN now cumulatively
   * acknowledges it and completes the close without waiting for T2. */
  assert(cnet_sctp_assoc_input(&a, 12u, first, nfirst,
      response, sizeof(response), &nresponse) == CNET_SCTP_ASSOC_OK);
  assert(shutdown_chunk_type(response, nresponse) == 7u);
  assert(cnet_sctp_assoc_input(&b, 13u, response, nresponse,
      ack, sizeof(ack), &nack) == CNET_SCTP_ASSOC_OK);
  assert(shutdown_chunk_type(ack, nack) == 8u &&
      b.state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT &&
      b.data.in_flight == 0u);
  assert(cnet_sctp_assoc_input(&a, 14u, ack, nack,
      complete, sizeof(complete), &ncomplete) == CNET_SCTP_ASSOC_OK);
  assert(shutdown_chunk_type(complete, ncomplete) == 14u);
  assert(cnet_sctp_assoc_input(&b, 15u, complete, ncomplete,
      first, sizeof(first), &nfirst) == CNET_SCTP_ASSOC_OK);
  assert(a.state == CNET_SCTP_ASSOC_STOPPED &&
      b.state == CNET_SCTP_ASSOC_STOPPED);
  /* Both DATA messages were accepted; drain the receive queue before
   * asserting that the association owner can be destroyed. */
  {
    cnet_sctp_data_message meta = {0};
    uint8_t payload[8] = {0};
    size_t bytes = 0u;
    assert(cnet_sctp_assoc_take_message(&a, &meta, payload,
        sizeof(payload), &bytes) == CNET_SCTP_ASSOC_OK);
    assert(bytes == 1u && payload[0] == payloads[0]);
    assert(cnet_sctp_assoc_take_message(&a, &meta, payload,
        sizeof(payload), &bytes) == CNET_SCTP_ASSOC_OK);
    assert(bytes == 1u && payload[0] == payloads[1]);
    assert(cnet_sctp_assoc_drained(&a) && cnet_sctp_assoc_drained(&b));
  }
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

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
  assert(cnet_sctp_assoc_input(&b, 13u, sack, ns,
      ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
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
  /* DATA without user bytes is malformed. Reserved/I bits are NOT. */
  broken[2].value_size = 12u;
  assert(cnet_sctp_wire_encode(a.config.local_port,
      a.config.remote_port, a.peer_tag, broken, 3u,
      combined, sizeof(combined), &n) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_assoc_input(&b, 10u, combined, n,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(ns == 0u && b.data.buffered_rx == 0u &&
      b.data.cumulative_rx == ca.tsn - 1u);
  /* The private sender transmits clean flags, while the receiver must
   * accept DATA reserved bits and I=1 inside a legal DATA bundle. */
  chunks[1].flags = (uint8_t)(chunks[1].flags | 0xF8u);
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


/* RFC 9260 §9.2 optional T5 is absolute, not a T2 retry deadline.
 * Repeated T2 retries cannot extend it; output-capacity refusal is atomic. */
static void shutdown_t5_absolute_guard(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  uint8_t shutdown[1200], retry[1200];
  size_t ns = 0u, nr = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;
  ready_pair(&a, &b, &ca, &cb);
  a.config.t5_shutdown_guard_ms = 250u;
  assert(cnet_sctp_assoc_shutdown(&a, 6u, shutdown,
      sizeof(shutdown), &ns) == CNET_SCTP_ASSOC_OK);
  assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
      a.t5_shutdown_deadline_ms == 256u);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 106u);
  assert(cnet_sctp_assoc_advance(&a, 106u, retry,
      sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == ns && a.deadline_ms == 306u &&
      a.t5_shutdown_deadline_ms == 256u);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 256u);
  assert(cnet_sctp_assoc_advance(&a, 256u, retry, 8u, &nr) ==
      CNET_SCTP_ASSOC_CAPACITY);
  assert(nr == 0u && a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
      a.deadline_ms == 306u && a.t5_shutdown_deadline_ms == 256u);
  assert(cnet_sctp_assoc_advance(&a, 256u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_OK);
  assert(nr == 16u && shutdown_chunk_type(retry, nr) == 6u &&
      a.state == CNET_SCTP_ASSOC_FAILED &&
      a.deadline_ms == 0u && a.t5_shutdown_deadline_ms == 0u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* A packet arriving after the absolute T5 deadline cannot race past
 * the overdue Owner timer and complete a graceful shutdown. */
static void shutdown_t5_preempts_late_ack(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  uint8_t shutdown[1200], ack[1200], response[1200];
  size_t ns = 0u, na = 0u, nr = 0u;
  ready_pair(&a, &b, &ca, &cb);
  a.config.t5_shutdown_guard_ms = 250u;
  assert(cnet_sctp_assoc_shutdown(&a, 6u, shutdown,
      sizeof(shutdown), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, shutdown, ns,
      ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(na == 16u && shutdown_chunk_type(ack, na) == 8u);
  assert(cnet_sctp_assoc_input(&a, 256u, ack, na,
      response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 16u && shutdown_chunk_type(response, nr) == 6u &&
      a.state == CNET_SCTP_ASSOC_FAILED &&
      a.t5_shutdown_deadline_ms == 0u);
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
  /* The rejected duplicate SHUTDOWN must not partially publish its
   * receiver clock before the saved ACK is actually emitted. */
  assert(nr == 0u && b.retransmits == 0u && b.deadline_ms == 107u &&
      b.current_rto_ms == 100u && b.last_time_ms == 7u);
  assert(cnet_sctp_assoc_input(&b, 50u, shutdown, ns,
      retry, sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == na && memcmp(retry, shutdown_ack, na) == 0 &&
      b.retransmits == 0u && b.deadline_ms == 150u &&
      b.last_time_ms == 50u);
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
  /* A duplicate SHUTDOWN in SHUTDOWN-ACK-SENT retransmits the ACK and
   * restarts T2, but is not evidence of DATA progress and cannot reset the
   * bounded retry budget. Exercise this after RTO has backed off to 400 ms. */
  assert(cnet_sctp_assoc_input(&b, 400u, shutdown, ns,
      retry, sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == na && memcmp(retry, shutdown_ack, na) == 0 &&
      b.retransmits == 2u && b.current_rto_ms == 400u &&
      b.deadline_ms == 800u && b.last_time_ms == 400u);
  assert(cnet_sctp_assoc_advance(&b, 800u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_OK);
  assert(b.retransmits == 3u && b.deadline_ms == 1200u);
  assert(cnet_sctp_assoc_advance(&b, 1200u, retry, sizeof(retry), &nr) ==
      CNET_SCTP_ASSOC_EXPIRED);
  assert(nr == 0u && b.state == CNET_SCTP_ASSOC_FAILED &&
      b.deadline_ms == 0u && b.saved_size == 0u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* A duplicate DATA TSN is ACKed while SHUTDOWN-SENT and restarts T2,
 * but it must not replenish the bounded shutdown retry budget. */
static void shutdown_sent_duplicate_data_does_not_reset_budget(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t payload[] = {'d'};
  uint8_t data[1200], sack[1200], shutdown[1200], response[1200], retry[1200];
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  size_t nd = 0u, ns = 0u, nshutdown = 0u, nr = 0u;
  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&b, 6u, 0u, 53u, 0,
      payload, sizeof(payload), data, sizeof(data), &nd) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 7u, data, nd,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 8u, sack, ns,
      response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 0u && b.data.in_flight == 0u);
  assert(cnet_sctp_assoc_shutdown(&a, 9u, shutdown,
      sizeof(shutdown), &nshutdown) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_advance(&a, 109u, retry,
      sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(a.retransmits == 1u && a.deadline_ms == 309u);
  assert(cnet_sctp_assoc_advance(&a, 309u, retry,
      sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(a.retransmits == 2u && a.deadline_ms == 709u &&
      a.current_rto_ms == 400u);

  /* The exact same DATA TSN was already admitted before shutdown. */
  assert(cnet_sctp_assoc_input(&a, 310u, data, nd,
      response, sizeof(response), &nr) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_wire_decode(response, nr, &header, &decoder) ==
      CNET_SCTP_WIRE_OK && header.chunk_count == 2u);
  assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT &&
      a.data.buffered_rx == 1u && a.retransmits == 2u &&
      a.deadline_ms == 710u);
  assert(cnet_sctp_assoc_advance(&a, 710u, retry,
      sizeof(retry), &nr) == CNET_SCTP_ASSOC_OK);
  assert(a.retransmits == 3u && a.deadline_ms == 1110u);
  assert(cnet_sctp_assoc_advance(&a, 1110u, retry,
      sizeof(retry), &nr) == CNET_SCTP_ASSOC_EXPIRED);
  assert(a.state == CNET_SCTP_ASSOC_FAILED && a.retransmits == 3u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* Exercise the relaxed Duplicate TSN list bound through the full Owner
 * Association path, including packet preflight and T3 transaction rules. */
static void association_accepts_sack_duplicate_list_beyond_rx_slots(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t payload = 'd';
  uint8_t data_packet[1200], body[12u + 4u * (CNET_SCTP_DATA_RX_SLOTS + 1u)];
  uint8_t sack_packet[256], reply[1200];
  cnet_sctp_wire_chunk sack = {0};
  size_t data_size = 0u, sack_size = 0u, reply_size = 123u, i;
  uint32_t cumulative, duplicate_tsn;
  uint64_t prior_t3;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 51u, 0, &payload, 1u,
      data_packet, sizeof(data_packet), &data_size) == CNET_SCTP_ASSOC_OK);
  assert(data_size != 0u && a.data.tx[0].used);
  cumulative = a.data.last_ack;
  duplicate_tsn = a.data.tx[0].tsn;
  prior_t3 = a.data.t3_deadline_ms;

  memset(body, 0, sizeof(body));
  body[0] = (uint8_t)(cumulative >> 24);
  body[1] = (uint8_t)(cumulative >> 16);
  body[2] = (uint8_t)(cumulative >> 8);
  body[3] = (uint8_t)cumulative;
  body[4] = 0u; body[5] = 0u; body[6] = 0x20u; body[7] = 0u; /* rwnd=8192 */
  body[10] = 0u;
  body[11] = (uint8_t)(CNET_SCTP_DATA_RX_SLOTS + 1u);
  for (i = 0u; i < CNET_SCTP_DATA_RX_SLOTS + 1u; ++i) {
    uint8_t *p = body + 12u + 4u * i;
    p[0] = (uint8_t)(duplicate_tsn >> 24);
    p[1] = (uint8_t)(duplicate_tsn >> 16);
    p[2] = (uint8_t)(duplicate_tsn >> 8);
    p[3] = (uint8_t)duplicate_tsn;
  }
  sack.type = 3u;
  sack.flags = 0xA5u;
  sack.value = body;
  sack.value_size = sizeof(body);
  assert(cnet_sctp_wire_encode(b.config.local_port, b.config.remote_port,
      a.local_tag, &sack, 1u, sack_packet, sizeof(sack_packet),
      &sack_size) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_assoc_input(&a, 7u, sack_packet, sack_size, reply,
      sizeof(reply), &reply_size) == CNET_SCTP_ASSOC_OK);
  assert(reply_size == 0u && a.last_time_ms == 7u &&
      a.data.last_time_ms == 7u);
  assert(a.data.last_ack == cumulative && a.data.in_flight == 1u &&
      a.data.tx[0].used && a.data.tx[0].tsn == duplicate_tsn);
  assert(a.data.t3_deadline_ms == prior_t3);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* A real established Association publishes the peer ABORT event with the
 * pre-teardown cumulative ACK / last-sent TSNs, even across TSN wrap. */
static void peer_abort_event_captures_data_tsn_before_teardown(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t payload = 'e';
  static const uint8_t cause12[] = {0u, 12u, 0u, 8u, 'p', 'e', 'e', 'r'};
  cnet_sctp_assoc_peer_abort_info info = {0};
  cnet_sctp_wire_chunk abort_chunk = {6u, 0u, cause12, sizeof(cause12)};
  uint8_t data_packet[1200], abort_packet[1200], reply[1200], reason[8];
  size_t data_n = 0u, abort_n = 0u, reply_n = 123u, reason_n = 0u;
  uint32_t initial_last_ack, actual_last_sent;

  ready_pair(&a, &b, &ca, &cb);
  initial_last_ack = a.data.last_ack;
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 51u, 0,
      &payload, 1u, data_packet, sizeof(data_packet), &data_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(a.data_ready && data_n > 0u && a.data.tx[0].used);
  actual_last_sent = a.data.next_tsn - 1u;
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag,
      &abort_chunk, 1u, abort_packet, sizeof(abort_packet), &abort_n) ==
      CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_assoc_input(&a, 7u, abort_packet, abort_n,
      reply, 0u, &reply_n) == CNET_SCTP_ASSOC_OK);
  assert(reply_n == 0u && !a.data_ready &&
      a.state == CNET_SCTP_ASSOC_FAILED);
  assert(cnet_sctp_assoc_take_peer_abort(&a, &info, reason,
      sizeof(reason), &reason_n) == CNET_SCTP_ASSOC_OK);
  assert(info.has_tsn && info.has_user_reason &&
      info.last_acked_tsn == initial_last_ack &&
      info.last_sent_tsn == actual_last_sent);
  assert(reason_n == 4u && memcmp(reason, "peer", 4u) == 0);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* A real, cumulative SACK before a terminal ABORT changes the TSN that
 * the upper layer sees as last acknowledged. The live DATA TCB is never
 * partially mutated: the terminal path still destroys it without sending
 * any reply (including at a pending T5 timeout). */
static void peer_abort_prefix_sack_updates_ulp_ack_snapshot(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint8_t payloads[2] = {'a', 'b'};
  static const uint8_t abort_reason[8] = {0u, 12u, 0u, 8u, 'p', 'e', 'e', 'r'};
  uint8_t data[2][1200], sack[1200], combined[1200], response[1200], reason[8];
  cnet_sctp_wire_header sack_header = {0};
  cnet_sctp_wire_decoder sack_decoder = {0};
  cnet_sctp_wire_chunk chunks[2] = {{0}};
  cnet_sctp_assoc_peer_abort_info info = {0};
  size_t data_n[2] = {0u}, sack_n = 0u, combined_n = 0u;
  size_t reply_n = 123u, reason_n = 0u;
  uint32_t original_ack, first_sent, last_sent;
  unsigned int i;

  ready_pair(&a, &b, &ca, &cb);
  original_ack = a.data.last_ack;
  for (i = 0u; i < 2u; ++i) {
    assert(cnet_sctp_assoc_send_message(&a, 6u + i, 0u, 51u, 0,
        payloads + i, 1u, data[i], sizeof(data[i]), &data_n[i]) ==
        CNET_SCTP_ASSOC_OK);
  }
  first_sent = a.initial_tsn;
  last_sent = a.data.next_tsn - 1u;
  assert(a.data.last_ack == original_ack && original_ack != first_sent);
  assert(cnet_sctp_assoc_input(&b, 8u, data[0], data_n[0],
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_wire_decode(sack, sack_n, &sack_header,
      &sack_decoder) == CNET_SCTP_WIRE_OK);
  assert(sack_header.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&sack_decoder, &chunks[0]) ==
      CNET_SCTP_WIRE_OK && chunks[0].type == 3u);
  chunks[1].type = 6u;
  chunks[1].flags = 0u;
  chunks[1].value = abort_reason;
  chunks[1].value_size = sizeof(abort_reason);
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks, 2u,
      combined, sizeof(combined), &combined_n) == CNET_SCTP_WIRE_OK);

  /* A valid terminal packet with an authentic ACK is silent with cap=0. */
  assert(cnet_sctp_assoc_input(&a, 9u, combined, combined_n,
      response, 0u, &reply_n) == CNET_SCTP_ASSOC_OK);
  assert(reply_n == 0u && a.state == CNET_SCTP_ASSOC_FAILED &&
      !a.data_ready && a.peer_abort_pending);
  assert(cnet_sctp_assoc_take_peer_abort(&a, &info, reason,
      sizeof(reason), &reason_n) == CNET_SCTP_ASSOC_OK);
  assert(info.has_tsn && info.has_user_reason &&
      info.last_acked_tsn == first_sent &&
      info.last_acked_tsn != original_ack &&
      info.last_sent_tsn == last_sent);
  assert(reason_n == 4u && memcmp(reason, "peer", 4u) == 0);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* A shape-valid but semantically impossible SACK before ABORT must not
 * falsely increase the ULP last-acked TSN. The authenticated ABORT itself
 * remains terminal, and no response packet is sent. */
static void peer_abort_prefix_invalid_sack_does_not_forge_ack(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  static const uint8_t payload = 'x';
  uint8_t data[1200], combined[1200], response[32];
  uint8_t sack_body[12] = {0};
  cnet_sctp_wire_chunk chunks[2] = {{0}};
  cnet_sctp_assoc_peer_abort_info info = {0};
  size_t data_n = 0u, combined_n = 0u, reply_n = 123u, reason_n = 123u;
  uint32_t old_ack, future;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 51u, 0,
      &payload, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  old_ack = a.data.last_ack;
  future = a.data.next_tsn; /* Claims an unsent TSN. */
  sack_body[0] = (uint8_t)(future >> 24);
  sack_body[1] = (uint8_t)(future >> 16);
  sack_body[2] = (uint8_t)(future >> 8);
  sack_body[3] = (uint8_t)future;
  sack_body[4] = 0u; sack_body[5] = 0u;
  sack_body[6] = 0x20u; sack_body[7] = 0u; /* rwnd = 8192 */
  chunks[0].type = 3u;
  chunks[0].flags = 0u;
  chunks[0].value = sack_body;
  chunks[0].value_size = sizeof(sack_body);
  chunks[1].type = 6u;
  chunks[1].flags = 0u;
  chunks[1].value = NULL;
  chunks[1].value_size = 0u;
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag,
      chunks, 2u, combined, sizeof(combined), &combined_n) ==
      CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_assoc_input(&a, 7u, combined, combined_n,
      response, 0u, &reply_n) == CNET_SCTP_ASSOC_OK);
  assert(reply_n == 0u && a.state == CNET_SCTP_ASSOC_FAILED);
  assert(cnet_sctp_assoc_take_peer_abort(&a, &info, NULL, 0u,
      &reason_n) == CNET_SCTP_ASSOC_OK);
  assert(reason_n == 0u && info.has_tsn &&
      info.last_acked_tsn == old_ack && !info.has_user_reason);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* ABORT followed by an otherwise valid cumulative SACK is terminal at
 * the first ABORT. The trailing SACK must not advance the Owner-pulled
 * last-acked TSN, even when the local T5 deadline is exactly due. */
static void peer_abort_trailing_sack_does_not_change_ulp_ack(void) {
  size_t reflected, i;
  for (reflected = 0u; reflected < 2u; ++reflected) {
    cnet_sctp_assoc a, b;
    test_context ca = {0}, cb = {0};
    const uint8_t payload = 't';
    static const uint8_t reason12[8] = {0u, 12u, 0u, 8u, 'l', 'a', 't', 'e'};
    uint8_t data[1200], sack[1200], combined[1200], reply[32], reason[8];
    cnet_sctp_wire_header h = {0};
    cnet_sctp_wire_decoder decoder = {0};
    cnet_sctp_wire_chunk chunks[2] = {{0}};
    cnet_sctp_assoc_peer_abort_info info = {0};
    size_t data_n = 0u, sack_n = 0u, combined_n = 0u;
    size_t reply_n = 123u, reason_n = 0u;
    uint32_t original_ack, last_sent;

    ready_pair(&a, &b, &ca, &cb);
    assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 51u, 0,
        &payload, 1u, data, sizeof(data), &data_n) ==
        CNET_SCTP_ASSOC_OK);
    original_ack = a.data.last_ack;
    last_sent = a.data.next_tsn - 1u;
    assert(cnet_sctp_assoc_input(&b, 7u, data, data_n,
        sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_wire_decode(sack, sack_n, &h, &decoder) ==
        CNET_SCTP_WIRE_OK);
    chunks[0].type = 6u;
    chunks[0].flags = (uint8_t)reflected;
    chunks[0].value = reason12;
    chunks[0].value_size = sizeof(reason12);
    assert(cnet_sctp_wire_next_chunk(&decoder, &chunks[1]) ==
        CNET_SCTP_WIRE_OK && chunks[1].type == 3u);
    assert(cnet_sctp_wire_encode(5001u, 5000u,
        reflected ? a.peer_tag : a.local_tag, chunks, 2u,
        combined, sizeof(combined), &combined_n) == CNET_SCTP_WIRE_OK);

    a.state = CNET_SCTP_ASSOC_SHUTDOWN_SENT;
    a.t5_shutdown_deadline_ms = 1000u;
    a.deadline_ms = 1000u;
    memset(reply, 0xCC, sizeof(reply));
    assert(cnet_sctp_assoc_input(&a, 1000u, combined, combined_n,
        reply, 0u, &reply_n) == CNET_SCTP_ASSOC_OK);
    assert(reply_n == 0u && a.state == CNET_SCTP_ASSOC_FAILED &&
        a.t5_shutdown_deadline_ms == 0u && a.peer_abort_pending);
    for (i = 0u; i < sizeof(reply); ++i) assert(reply[i] == 0xCCu);
    assert(cnet_sctp_assoc_take_peer_abort(&a, &info, reason,
        sizeof(reason), &reason_n) == CNET_SCTP_ASSOC_OK);
    assert(info.has_tsn && info.has_user_reason &&
        info.last_acked_tsn == original_ack &&
        info.last_acked_tsn != last_sent &&
        info.last_sent_tsn == last_sent);
    assert(reason_n == 4u && memcmp(reason, "late", 4u) == 0);
    assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
  }
}

static uint16_t read16_test(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read32_test(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
      ((uint32_t)p[2] << 8) | p[3];
}

static void write16_test(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static void write32_test(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static size_t encode_incoming_reset_request(const cnet_sctp_assoc *sender,
    uint32_t sequence, const uint16_t *streams, size_t stream_count,
    uint8_t *packet, size_t capacity) {
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[8u + 2u * CNET_SCTP_DATA_STREAMS];
  size_t i, written = 0u;
  assert(sender && packet && stream_count <= CNET_SCTP_DATA_STREAMS &&
      (stream_count == 0u || streams));
  write16_test(value, 14u);
  write16_test(value + 2u, (uint16_t)(8u + 2u * stream_count));
  write32_test(value + 4u, sequence);
  for (i = 0u; i < stream_count; ++i)
    write16_test(value + 8u + 2u * i, streams[i]);
  chunk.type = 130u;
  chunk.value = value;
  chunk.value_size = 8u + 2u * stream_count;
  assert(cnet_sctp_wire_encode(sender->config.local_port,
      sender->config.remote_port, sender->peer_tag, &chunk, 1u,
      packet, capacity, &written) == CNET_SCTP_WIRE_OK);
  return written;
}

static uint32_t incoming_reset_request_sequence(
    const uint8_t *packet, size_t size) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 8u &&
      read16_test(chunk.value) == 14u);
  return read32_test(chunk.value + 4u);
}

static size_t incoming_reset_parameter_size(
    const uint8_t *packet, size_t size) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 8u &&
      read16_test(chunk.value) == 14u);
  return chunk.value_size;
}

static uint16_t incoming_reset_stream_mask(
    const uint8_t *packet, size_t size, uint16_t inbound_streams) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  uint16_t mask = 0u;
  size_t position;
  assert(inbound_streams > 0u && inbound_streams <= CNET_SCTP_DATA_STREAMS);
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 8u &&
      read16_test(chunk.value) == 14u);
  if (chunk.value_size == 8u)
    return inbound_streams == CNET_SCTP_DATA_STREAMS ? UINT16_MAX :
        (uint16_t)((UINT16_C(1) << inbound_streams) - 1u);
  for (position = 8u; position < chunk.value_size; position += 2u) {
    const uint16_t stream = read16_test(chunk.value + position);
    assert(stream < inbound_streams);
    mask |= (uint16_t)(UINT16_C(1) << stream);
  }
  return mask;
}

static size_t encode_add_outgoing_request(const cnet_sctp_assoc *sender,
    uint32_t sequence, uint16_t stream_count, uint16_t reserved,
    uint8_t *packet, size_t capacity) {
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[12u];
  size_t written = 0u;
  write16_test(value, 17u);
  write16_test(value + 2u, (uint16_t)sizeof(value));
  write32_test(value + 4u, sequence);
  write16_test(value + 8u, stream_count);
  write16_test(value + 10u, reserved);
  chunk.type = 130u;
  chunk.value = value;
  chunk.value_size = sizeof(value);
  assert(cnet_sctp_wire_encode(sender->config.local_port,
      sender->config.remote_port, sender->peer_tag, &chunk, 1u,
      packet, capacity, &written) == CNET_SCTP_WIRE_OK);
  return written;
}

static void assert_add_outgoing_request(const uint8_t *packet, size_t size,
    uint32_t sequence, uint16_t stream_count) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size == 12u &&
      read16_test(chunk.value) == 17u &&
      read16_test(chunk.value + 2u) == 12u &&
      read32_test(chunk.value + 4u) == sequence &&
      read16_test(chunk.value + 8u) == stream_count &&
      read16_test(chunk.value + 10u) == 0u);
}

static size_t encode_add_incoming_request(const cnet_sctp_assoc *sender,
    uint32_t sequence, uint16_t stream_count, uint16_t reserved,
    uint8_t *packet, size_t capacity) {
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[12u];
  size_t written = 0u;
  write16_test(value, 18u);
  write16_test(value + 2u, (uint16_t)sizeof(value));
  write32_test(value + 4u, sequence);
  write16_test(value + 8u, stream_count);
  write16_test(value + 10u, reserved);
  chunk.type = 130u;
  chunk.value = value;
  chunk.value_size = sizeof(value);
  assert(cnet_sctp_wire_encode(sender->config.local_port,
      sender->config.remote_port, sender->peer_tag, &chunk, 1u,
      packet, capacity, &written) == CNET_SCTP_WIRE_OK);
  return written;
}

static void assert_add_incoming_request(const uint8_t *packet, size_t size,
    uint32_t sequence, uint16_t stream_count) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size == 12u &&
      read16_test(chunk.value) == 18u &&
      read16_test(chunk.value + 2u) == 12u &&
      read32_test(chunk.value + 4u) == sequence &&
      read16_test(chunk.value + 8u) == stream_count &&
      read16_test(chunk.value + 10u) == 0u);
}

static size_t encode_add_streams_request(const cnet_sctp_assoc *sender,
    uint32_t outgoing_sequence, uint16_t outgoing_stream_count,
    uint32_t incoming_sequence, uint16_t incoming_stream_count,
    uint8_t *packet, size_t capacity) {
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[24u] = {0};
  size_t written = 0u;
  write16_test(value, 17u);
  write16_test(value + 2u, 12u);
  write32_test(value + 4u, outgoing_sequence);
  write16_test(value + 8u, outgoing_stream_count);
  write16_test(value + 12u, 18u);
  write16_test(value + 14u, 12u);
  write32_test(value + 16u, incoming_sequence);
  write16_test(value + 20u, incoming_stream_count);
  chunk.type = 130u;
  chunk.value = value;
  chunk.value_size = sizeof(value);
  assert(cnet_sctp_wire_encode(sender->config.local_port,
      sender->config.remote_port, sender->peer_tag, &chunk, 1u,
      packet, capacity, &written) == CNET_SCTP_WIRE_OK);
  return written;
}

static void assert_add_streams_request(const uint8_t *packet, size_t size,
    uint32_t outgoing_sequence, uint16_t outgoing_stream_count,
    uint32_t incoming_sequence, uint16_t incoming_stream_count) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size == 24u &&
      read16_test(chunk.value) == 17u &&
      read16_test(chunk.value + 2u) == 12u &&
      read32_test(chunk.value + 4u) == outgoing_sequence &&
      read16_test(chunk.value + 8u) == outgoing_stream_count &&
      read16_test(chunk.value + 10u) == 0u &&
      read16_test(chunk.value + 12u) == 18u &&
      read16_test(chunk.value + 14u) == 12u &&
      read32_test(chunk.value + 16u) == incoming_sequence &&
      read16_test(chunk.value + 20u) == incoming_stream_count &&
      read16_test(chunk.value + 22u) == 0u);
}

static void assert_reset_response_pair(const uint8_t *packet, size_t size,
    uint32_t first_sequence, cnet_sctp_assoc_stream_reset_result first_result,
    uint32_t second_sequence,
    cnet_sctp_assoc_stream_reset_result second_result) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size == 24u &&
      read16_test(chunk.value) == 16u &&
      read16_test(chunk.value + 2u) == 12u &&
      read32_test(chunk.value + 4u) == first_sequence &&
      read32_test(chunk.value + 8u) == (uint32_t)first_result &&
      read16_test(chunk.value + 12u) == 16u &&
      read16_test(chunk.value + 14u) == 12u &&
      read32_test(chunk.value + 16u) == second_sequence &&
      read32_test(chunk.value + 20u) == (uint32_t)second_result);
}

static size_t encode_reset_response_packet(const cnet_sctp_assoc *sender,
    uint32_t sequence, cnet_sctp_assoc_stream_reset_result result,
    uint8_t *packet, size_t capacity) {
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[12u];
  size_t written = 0u;
  write16_test(value, 16u);
  write16_test(value + 2u, sizeof(value));
  write32_test(value + 4u, sequence);
  write32_test(value + 8u, (uint32_t)result);
  chunk.type = 130u;
  chunk.value = value;
  chunk.value_size = sizeof(value);
  assert(cnet_sctp_wire_encode(sender->config.local_port,
      sender->config.remote_port, sender->peer_tag, &chunk, 1u,
      packet, capacity, &written) == CNET_SCTP_WIRE_OK);
  return written;
}

static size_t encode_association_reset_request(
    const cnet_sctp_assoc *sender, uint32_t sequence,
    uint8_t *packet, size_t capacity) {
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[8u];
  size_t written = 0u;
  write16_test(value, 15u);
  write16_test(value + 2u, sizeof(value));
  write32_test(value + 4u, sequence);
  chunk.type = 130u;
  chunk.value = value;
  chunk.value_size = sizeof(value);
  assert(cnet_sctp_wire_encode(sender->config.local_port,
      sender->config.remote_port, sender->peer_tag, &chunk, 1u,
      packet, capacity, &written) == CNET_SCTP_WIRE_OK);
  return written;
}

static size_t encode_association_reset_response(
    const cnet_sctp_assoc *sender, uint32_t sequence,
    cnet_sctp_assoc_stream_reset_result result,
    uint32_t sender_next_tsn, uint32_t receiver_next_tsn,
    uint8_t *packet, size_t capacity) {
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[20u];
  size_t written = 0u;
  write16_test(value, 16u);
  write16_test(value + 2u, sizeof(value));
  write32_test(value + 4u, sequence);
  write32_test(value + 8u, (uint32_t)result);
  write32_test(value + 12u, sender_next_tsn);
  write32_test(value + 16u, receiver_next_tsn);
  chunk.type = 130u;
  chunk.value = value;
  chunk.value_size = sizeof(value);
  assert(cnet_sctp_wire_encode(sender->config.local_port,
      sender->config.remote_port, sender->peer_tag, &chunk, 1u,
      packet, capacity, &written) == CNET_SCTP_WIRE_OK);
  return written;
}

static void assert_association_reset_request(const uint8_t *packet,
    size_t size, uint32_t sequence) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size == 8u &&
      read16_test(chunk.value) == 15u &&
      read16_test(chunk.value + 2u) == 8u &&
      read32_test(chunk.value + 4u) == sequence);
}

static void assert_association_reset_response(const uint8_t *packet,
    size_t size, uint32_t sequence,
    cnet_sctp_assoc_stream_reset_result result,
    uint32_t sender_next_tsn, uint32_t receiver_next_tsn) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size == 20u &&
      read16_test(chunk.value) == 16u &&
      read16_test(chunk.value + 2u) == 20u &&
      read32_test(chunk.value + 4u) == sequence &&
      read32_test(chunk.value + 8u) == (uint32_t)result &&
      read32_test(chunk.value + 12u) == sender_next_tsn &&
      read32_test(chunk.value + 16u) == receiver_next_tsn);
}

static size_t encode_reset_response_pair_packet(
    const cnet_sctp_assoc *sender,
    uint32_t first_sequence, cnet_sctp_assoc_stream_reset_result first_result,
    uint32_t second_sequence,
    cnet_sctp_assoc_stream_reset_result second_result,
    uint8_t *packet, size_t capacity) {
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[24u];
  size_t written = 0u;
  write16_test(value, 16u);
  write16_test(value + 2u, 12u);
  write32_test(value + 4u, first_sequence);
  write32_test(value + 8u, (uint32_t)first_result);
  write16_test(value + 12u, 16u);
  write16_test(value + 14u, 12u);
  write32_test(value + 16u, second_sequence);
  write32_test(value + 20u, (uint32_t)second_result);
  chunk.type = 130u;
  chunk.value = value;
  chunk.value_size = sizeof(value);
  assert(cnet_sctp_wire_encode(sender->config.local_port,
      sender->config.remote_port, sender->peer_tag, &chunk, 1u,
      packet, capacity, &written) == CNET_SCTP_WIRE_OK);
  return written;
}

static uint32_t outgoing_reset_response_sequence(
    const uint8_t *packet, size_t size) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 16u &&
      read16_test(chunk.value) == 13u);
  return read32_test(chunk.value + 8u);
}

static uint32_t outgoing_reset_request_sequence(
    const uint8_t *packet, size_t size) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 16u &&
      read16_test(chunk.value) == 13u);
  return read32_test(chunk.value + 4u);
}

static size_t encode_outgoing_incoming_reset_pair(
    const cnet_sctp_assoc *sender,
    const uint8_t *outgoing_packet, size_t outgoing_size,
    uint32_t incoming_sequence, const uint16_t *streams, size_t stream_count,
    uint8_t *packet, size_t capacity) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk outgoing = {0}, combined = {0};
  uint8_t value[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t outgoing_padded, value_size, i, written = 0u;
  assert(sender && outgoing_packet && packet &&
      stream_count <= CNET_SCTP_DATA_STREAMS &&
      (stream_count == 0u || streams));
  assert(cnet_sctp_wire_decode(outgoing_packet, outgoing_size,
      &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &outgoing) == CNET_SCTP_WIRE_OK &&
      outgoing.type == 130u && outgoing.value_size >= 16u &&
      read16_test(outgoing.value) == 13u);
  outgoing_padded = (outgoing.value_size + 3u) & ~(size_t)3u;
  value_size = outgoing_padded + 8u + 2u * stream_count;
  assert(value_size <= sizeof(value));
  memset(value, 0, value_size);
  memcpy(value, outgoing.value, outgoing.value_size);
  write16_test(value + outgoing_padded, 14u);
  write16_test(value + outgoing_padded + 2u,
      (uint16_t)(8u + 2u * stream_count));
  write32_test(value + outgoing_padded + 4u, incoming_sequence);
  for (i = 0u; i < stream_count; ++i)
    write16_test(value + outgoing_padded + 8u + 2u * i, streams[i]);
  combined.type = 130u;
  combined.value = value;
  combined.value_size = value_size;
  assert(cnet_sctp_wire_encode(sender->config.local_port,
      sender->config.remote_port, sender->peer_tag, &combined, 1u,
      packet, capacity, &written) == CNET_SCTP_WIRE_OK);
  return written;
}

static void assert_outgoing_incoming_reset_pair(const uint8_t *packet,
    size_t size, uint32_t outgoing_sequence, uint32_t incoming_sequence,
    uint16_t expected_outgoing_mask, uint16_t expected_incoming_mask,
    uint16_t outbound_streams, uint16_t inbound_streams) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  uint16_t outgoing_mask = 0u, incoming_mask = 0u;
  const uint16_t all_outgoing = outbound_streams == CNET_SCTP_DATA_STREAMS ?
      UINT16_MAX : (uint16_t)((UINT16_C(1) << outbound_streams) - 1u);
  const uint16_t all_incoming = inbound_streams == CNET_SCTP_DATA_STREAMS ?
      UINT16_MAX : (uint16_t)((UINT16_C(1) << inbound_streams) - 1u);
  size_t outgoing_size, incoming_offset, incoming_size, position;
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 24u &&
      read16_test(chunk.value) == 13u &&
      read32_test(chunk.value + 4u) == outgoing_sequence);
  outgoing_size = read16_test(chunk.value + 2u);
  assert(outgoing_size >= 16u && outgoing_size <= chunk.value_size);
  if (outgoing_size == 16u) {
    outgoing_mask = all_outgoing;
  } else {
    for (position = 16u; position < outgoing_size; position += 2u) {
      const uint16_t stream = read16_test(chunk.value + position);
      assert(stream < outbound_streams);
      outgoing_mask |= (uint16_t)(UINT16_C(1) << stream);
    }
  }
  incoming_offset = (outgoing_size + 3u) & ~(size_t)3u;
  assert(incoming_offset + 8u <= chunk.value_size &&
      read16_test(chunk.value + incoming_offset) == 14u &&
      read32_test(chunk.value + incoming_offset + 4u) == incoming_sequence);
  incoming_size = read16_test(chunk.value + incoming_offset + 2u);
  assert(incoming_size >= 8u &&
      incoming_offset + incoming_size == chunk.value_size);
  if (incoming_size == 8u) {
    incoming_mask = all_incoming;
  } else {
    for (position = incoming_offset + 8u;
         position < incoming_offset + incoming_size; position += 2u) {
      const uint16_t stream = read16_test(chunk.value + position);
      assert(stream < inbound_streams);
      incoming_mask |= (uint16_t)(UINT16_C(1) << stream);
    }
  }
  assert(outgoing_mask == expected_outgoing_mask &&
      incoming_mask == expected_incoming_mask);
}

static void assert_response_outgoing_pair(const uint8_t *packet, size_t size,
    uint32_t response_sequence, uint32_t outgoing_response_sequence_value,
    uint16_t expected_stream_mask, uint16_t outbound_streams) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  size_t second;
  uint16_t mask = 0u;
  size_t position;
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 28u &&
      read16_test(chunk.value) == 16u &&
      read16_test(chunk.value + 2u) == 12u &&
      read32_test(chunk.value + 4u) == response_sequence);
  second = 12u;
  assert(read16_test(chunk.value + second) == 13u &&
      read32_test(chunk.value + second + 8u) ==
          outgoing_response_sequence_value);
  if (read16_test(chunk.value + second + 2u) == 16u) {
    mask = outbound_streams == CNET_SCTP_DATA_STREAMS ? UINT16_MAX :
        (uint16_t)((UINT16_C(1) << outbound_streams) - 1u);
  } else {
    for (position = second + 16u;
         position < second + read16_test(chunk.value + second + 2u);
         position += 2u) {
      const uint16_t stream = read16_test(chunk.value + position);
      mask |= (uint16_t)(UINT16_C(1) << stream);
    }
  }
  assert(mask == expected_stream_mask);
}

static uint16_t outgoing_reset_stream_mask(
    const uint8_t *packet, size_t size, uint16_t outbound_streams) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  uint16_t mask = 0u;
  size_t position;
  assert(outbound_streams > 0u &&
      outbound_streams <= CNET_SCTP_DATA_STREAMS);
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 16u &&
      read16_test(chunk.value) == 13u);
  if (chunk.value_size == 16u)
    return outbound_streams == CNET_SCTP_DATA_STREAMS ? UINT16_MAX :
        (uint16_t)((UINT16_C(1) << outbound_streams) - 1u);
  for (position = 16u; position < chunk.value_size; position += 2u) {
    const uint16_t stream = read16_test(chunk.value + position);
    assert(stream < outbound_streams);
    mask |= (uint16_t)(UINT16_C(1) << stream);
  }
  return mask;
}

static size_t outgoing_reset_parameter_size(
    const uint8_t *packet, size_t size) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 16u &&
      read16_test(chunk.value) == 13u);
  return chunk.value_size;
}

static void rewrite_reset_last_tsn(uint8_t *packet, size_t size,
    uint32_t last_tsn) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t rewritten[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t written = 0u;
  assert(packet && size <= sizeof(rewritten));
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 16u &&
      read16_test(chunk.value) == 13u);
  memcpy(value, chunk.value, chunk.value_size);
  write32_test(value + 12u, last_tsn);
  chunk.value = value;
  assert(cnet_sctp_wire_encode(header.source_port, header.destination_port,
      header.verification_tag, &chunk, 1u, rewritten, sizeof(rewritten),
      &written) == CNET_SCTP_WIRE_OK && written == size);
  memcpy(packet, rewritten, written);
}

static void rewrite_reset_sequence(uint8_t *packet, size_t size,
    uint32_t sequence) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  uint8_t value[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t rewritten[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t written = 0u;
  assert(packet && size <= sizeof(rewritten));
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 16u &&
      read16_test(chunk.value) == 13u);
  memcpy(value, chunk.value, chunk.value_size);
  write32_test(value + 4u, sequence);
  chunk.value = value;
  assert(cnet_sctp_wire_encode(header.source_port, header.destination_port,
      header.verification_tag, &chunk, 1u, rewritten, sizeof(rewritten),
      &written) == CNET_SCTP_WIRE_OK && written == size);
  memcpy(packet, rewritten, written);
}

static uint32_t reset_response_result(const uint8_t *packet, size_t size) {
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  assert(cnet_sctp_wire_decode(packet, size, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 130u && chunk.value_size >= 12u &&
      read16_test(chunk.value) == 16u &&
      read16_test(chunk.value + 2u) == 12u);
  return read32_test(chunk.value + 8u);
}

static void add_outgoing_streams_roundtrip_enables_new_sid(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_change_event event = {0};
  cnet_sctp_data_message message = {0};
  const uint8_t byte = 'g';
  uint8_t request[1200], response[1200], duplicate[1200];
  uint8_t data[1200], sack[1200], payload[8];
  size_t request_n = 0u, response_n = 0u, duplicate_n = 0u;
  size_t data_n = 0u, sack_n = 0u, payload_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  sequence = a.next_reset_request;
  assert(cnet_sctp_assoc_add_outgoing_streams(&a, 6u, 2u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(request_n != 0u && a.reset_active &&
      a.reconfig_kind == CNET_SCTP_RECONFIG_ADD_OUTGOING &&
      a.data.config.outbound_streams == 4u &&
      a.config.outbound_streams == 4u);
  assert_add_outgoing_request(request, request_n, sequence, 2u);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 4u, 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_INVALID);

  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(response_n != 0u && reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(b.data.config.inbound_streams == 6u &&
      b.config.inbound_streams == 4u);
  assert(cnet_sctp_assoc_input(&b, 8u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0 &&
      b.data.config.inbound_streams == 6u);

  assert(cnet_sctp_assoc_input(&a, 9u, response, response_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == 0u && !a.reset_active &&
      a.data.config.outbound_streams == 6u &&
      a.config.outbound_streams == 4u);
  assert(cnet_sctp_assoc_take_stream_change(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 6u &&
      event.outbound_streams == 4u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_take_stream_change(&b, &event) ==
      CNET_SCTP_ASSOC_EMPTY);
  assert(cnet_sctp_assoc_take_stream_change(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 4u &&
      event.outbound_streams == 6u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);

  assert(cnet_sctp_assoc_send_message(&a, 10u, 5u, 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(a.data.tx_ssn[5] == 1u);
  assert(cnet_sctp_assoc_input(&b, 11u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_id == 5u && message.stream_sequence == 0u &&
      payload_n == 1u && payload[0] == byte && b.data.rx_ssn[5] == 1u);
  assert(cnet_sctp_assoc_input(&a, 12u, sack, sack_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_reset_stream(&a, 13u, 5u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void add_outgoing_streams_capacity_and_event_backpressure(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_change_event event = {0};
  uint8_t request[1200], response[1200], duplicate[1200];
  size_t request_n, response_n = 0u, duplicate_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_add_outgoing_streams(&a, 6u, 1u,
      request, 1u, &request_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(request_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  assert(cnet_sctp_assoc_add_outgoing_streams(&a, 6u, 13u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(request_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);

  sequence = b.expected_peer_reset_request;
  request_n = encode_add_outgoing_request(&a, sequence, 1u,
      UINT16_C(0xbeef), request, sizeof(request));
  memcpy(&before, &b, sizeof(before));
  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      response, 1u, &response_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(response_n == 0u && memcmp(&b, &before, sizeof(b)) == 0);
  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_PERFORMED &&
      b.data.config.inbound_streams == 5u &&
      b.expected_peer_reset_request == sequence + 1u);

  request_n = encode_add_outgoing_request(&a, sequence + 1u, 1u, 0u,
      request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 8u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS &&
      b.data.config.inbound_streams == 5u &&
      b.expected_peer_reset_request == sequence + 1u);
  assert(cnet_sctp_assoc_take_stream_change(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 5u);
  assert(cnet_sctp_assoc_input(&b, 9u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_PERFORMED &&
      b.data.config.inbound_streams == 6u &&
      b.expected_peer_reset_request == sequence + 2u);
  assert(cnet_sctp_assoc_take_stream_change(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 6u);

  request_n = encode_add_outgoing_request(&a, sequence + 2u, 11u, 0u,
      request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 10u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_DENIED &&
      b.data.config.inbound_streams == 6u &&
      b.expected_peer_reset_request == sequence + 3u);
  assert(cnet_sctp_assoc_input(&b, 11u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0 &&
      cnet_sctp_assoc_take_stream_change(&b, &event) ==
          CNET_SCTP_ASSOC_EMPTY);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void add_outgoing_streams_denial_and_timeout_are_nonterminal(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_change_event event = {0};
  uint8_t request[1200], response[1200], retry[1200];
  size_t request_n = 0u, response_n, retry_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  sequence = a.next_reset_request;
  assert(cnet_sctp_assoc_add_outgoing_streams(&a, 6u, 2u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  response_n = encode_reset_response_packet(&b, sequence,
      CNET_SCTP_STREAM_RESET_DENIED, response, sizeof(response));
  assert(cnet_sctp_assoc_input(&a, 7u, response, response_n,
      retry, sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK);
  assert(retry_n == 0u && !a.reset_active &&
      a.data.config.outbound_streams == 4u);
  assert(cnet_sctp_assoc_take_stream_change(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.outbound_streams == 4u &&
      event.result == CNET_SCTP_STREAM_RESET_DENIED);

  assert(cnet_sctp_assoc_add_outgoing_streams(&a, 8u, 1u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_advance(&a, 108u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 308u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 708u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 1108u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK && retry_n == 0u);
  assert(!a.reset_active && a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
      a.data.config.outbound_streams == 4u);
  assert(cnet_sctp_assoc_take_stream_change(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 4u &&
      event.outbound_streams == 4u &&
      event.result == CNET_SCTP_STREAM_RESET_TIMEOUT);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void add_incoming_streams_roundtrip_enables_new_sid(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_change_event event = {0};
  cnet_sctp_data_message message = {0};
  const uint8_t byte = 'i';
  uint8_t request[1200], corresponding[1200], duplicate[1200];
  uint8_t response[1200], data[1200], sack[1200], payload[8];
  size_t request_n = 0u, corresponding_n = 0u, duplicate_n = 0u;
  size_t response_n = 0u, data_n = 0u, sack_n = 0u, payload_n = 0u;
  uint32_t request_sequence, corresponding_sequence;

  ready_pair(&a, &b, &ca, &cb);
  request_sequence = a.next_reset_request;
  corresponding_sequence = b.next_reset_request;
  assert(cnet_sctp_assoc_add_incoming_streams(&a, 6u, 2u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(request_n != 0u && a.reset_active &&
      a.reconfig_kind == CNET_SCTP_RECONFIG_ADD_INCOMING &&
      a.data.config.inbound_streams == 4u &&
      a.config.inbound_streams == 4u);
  assert_add_incoming_request(request, request_n, request_sequence, 2u);
  assert(cnet_sctp_assoc_send_message(&b, 6u, 4u, 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_INVALID);

  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      corresponding, sizeof(corresponding), &corresponding_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(corresponding_n != 0u && b.reset_active &&
      b.reconfig_kind == CNET_SCTP_RECONFIG_ADD_OUTGOING &&
      b.add_outgoing_is_reciprocal &&
      b.data.config.outbound_streams == 4u);
  assert_add_outgoing_request(corresponding, corresponding_n,
      corresponding_sequence, 2u);
  assert(cnet_sctp_assoc_input(&b, 8u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == corresponding_n &&
      memcmp(duplicate, corresponding, corresponding_n) == 0);

  assert(cnet_sctp_assoc_input(&a, 9u, corresponding, corresponding_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(response_n != 0u && reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_PERFORMED && !a.reset_active &&
      a.data.config.inbound_streams == 6u &&
      a.config.inbound_streams == 4u);
  assert(cnet_sctp_assoc_take_stream_change(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 6u &&
      event.outbound_streams == 4u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);

  assert(cnet_sctp_assoc_input(&b, 10u, response, response_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == 0u && !b.reset_active &&
      b.data.config.outbound_streams == 6u &&
      b.config.outbound_streams == 4u);
  assert(cnet_sctp_assoc_take_stream_change(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 4u &&
      event.outbound_streams == 6u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);

  assert(cnet_sctp_assoc_input(&b, 11u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == corresponding_n &&
      memcmp(duplicate, corresponding, corresponding_n) == 0 &&
      b.data.config.outbound_streams == 6u);
  assert(cnet_sctp_assoc_input(&a, 12u, duplicate, duplicate_n,
      data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(data_n == response_n && memcmp(data, response, response_n) == 0 &&
      a.data.config.inbound_streams == 6u &&
      cnet_sctp_assoc_take_stream_change(&a, &event) ==
          CNET_SCTP_ASSOC_EMPTY &&
      cnet_sctp_assoc_take_stream_change(&b, &event) ==
          CNET_SCTP_ASSOC_EMPTY);

  assert(cnet_sctp_assoc_send_message(&b, 13u, 5u, 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(b.data.tx_ssn[5] == 1u);
  assert(cnet_sctp_assoc_input(&a, 14u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&a, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_id == 5u && message.stream_sequence == 0u &&
      payload_n == 1u && payload[0] == byte && a.data.rx_ssn[5] == 1u);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void add_incoming_streams_capacity_duplicate_and_backpressure(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_change_event event = {0};
  uint8_t request[1200], response[1200], duplicate[1200];
  size_t request_n = 0u, response_n = 0u, duplicate_n = 0u;
  uint32_t peer_sequence, local_sequence;

  ready_pair(&a, &b, &ca, &cb);
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_add_incoming_streams(&a, 6u, 1u,
      request, 1u, &request_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(request_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  assert(cnet_sctp_assoc_add_incoming_streams(&a, 6u, 13u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(request_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);

  peer_sequence = b.expected_peer_reset_request;
  local_sequence = b.next_reset_request;
  request_n = encode_add_incoming_request(&a, peer_sequence, 1u,
      UINT16_C(0xbeef), request, sizeof(request));
  memcpy(&before, &b, sizeof(before));
  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      response, 1u, &response_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(response_n == 0u && memcmp(&b, &before, sizeof(b)) == 0);
  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert_add_outgoing_request(response, response_n, local_sequence, 1u);
  assert(b.reset_active && b.add_outgoing_is_reciprocal &&
      b.expected_peer_reset_request == peer_sequence + 1u);
  assert(cnet_sctp_assoc_input(&b, 8u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0);

  request_n = encode_add_incoming_request(&a, peer_sequence + 1u, 1u, 0u,
      request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 9u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(duplicate, duplicate_n) ==
      CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS &&
      b.expected_peer_reset_request == peer_sequence + 1u);

  request_n = encode_reset_response_packet(&a, local_sequence,
      CNET_SCTP_STREAM_RESET_DENIED, request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 10u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.data.config.outbound_streams == 4u);

  request_n = encode_add_incoming_request(&a, peer_sequence + 1u, 1u, 0u,
      request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 11u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS &&
      b.expected_peer_reset_request == peer_sequence + 1u);
  assert(cnet_sctp_assoc_take_stream_change(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.result == CNET_SCTP_STREAM_RESET_DENIED);

  request_n = encode_add_incoming_request(&a, peer_sequence + 1u, 13u, 0u,
      request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 12u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_DENIED &&
      b.expected_peer_reset_request == peer_sequence + 2u &&
      b.data.config.outbound_streams == 4u);
  assert(cnet_sctp_assoc_input(&b, 13u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void add_incoming_streams_denial_success_response_and_timeout(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_change_event event = {0};
  uint8_t request[1200], response[1200], retry[1200];
  size_t request_n = 0u, response_n, retry_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  sequence = a.next_reset_request;
  assert(cnet_sctp_assoc_add_incoming_streams(&a, 6u, 2u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  response_n = encode_reset_response_packet(&b, sequence,
      CNET_SCTP_STREAM_RESET_DENIED, response, sizeof(response));
  assert(cnet_sctp_assoc_input(&a, 7u, response, response_n,
      retry, sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK);
  assert(retry_n == 0u && !a.reset_active &&
      a.data.config.inbound_streams == 4u);
  assert(cnet_sctp_assoc_take_stream_change(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 4u &&
      event.result == CNET_SCTP_STREAM_RESET_DENIED);

  sequence = a.next_reset_request;
  assert(cnet_sctp_assoc_add_incoming_streams(&a, 8u, 1u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  response_n = encode_reset_response_packet(&b, sequence,
      CNET_SCTP_STREAM_RESET_PERFORMED, response, sizeof(response));
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_input(&a, 9u, response, response_n,
      retry, sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(retry_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);

  assert(cnet_sctp_assoc_advance(&a, 108u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 308u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 708u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 1108u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK && retry_n == 0u);
  assert(!a.reset_active && a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
      a.data.config.inbound_streams == 4u);
  assert(cnet_sctp_assoc_take_stream_change(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 4u &&
      event.outbound_streams == 4u &&
      event.result == CNET_SCTP_STREAM_RESET_TIMEOUT);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void add_streams_pair_roundtrip_completes_once(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_change_event event = {0};
  uint8_t request[1200], first_response[1200], corresponding[1200];
  uint8_t final_response[1200], duplicate[1200];
  size_t request_n = 0u, first_response_n = 0u, corresponding_n = 0u;
  size_t final_response_n = 0u, duplicate_n = 0u;
  uint32_t outgoing_sequence, incoming_sequence, corresponding_sequence;

  ready_pair(&a, &b, &ca, &cb);
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_add_streams(&a, 6u, 1u, 1u,
      request, 1u, &request_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(request_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  assert(cnet_sctp_assoc_add_streams(&a, 6u, 13u, 1u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(request_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  outgoing_sequence = a.next_reset_request;
  incoming_sequence = outgoing_sequence + 1u;
  corresponding_sequence = b.next_reset_request;
  assert(cnet_sctp_assoc_add_streams(&a, 6u, 2u, 1u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(request_n != 0u && a.reset_active &&
      a.reconfig_kind == CNET_SCTP_RECONFIG_ADD_STREAMS &&
      a.next_reset_request == outgoing_sequence + 2u &&
      a.data.config.inbound_streams == 4u &&
      a.data.config.outbound_streams == 4u);
  assert_add_streams_request(request, request_n,
      outgoing_sequence, 1u, incoming_sequence, 2u);

  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      first_response, sizeof(first_response), &first_response_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(first_response_n != 0u &&
      reset_response_result(first_response, first_response_n) ==
          CNET_SCTP_STREAM_RESET_PERFORMED &&
      b.data.config.inbound_streams == 5u &&
      b.data.config.outbound_streams == 4u && b.reset_active &&
      b.add_outgoing_is_reciprocal && b.add_outgoing_is_paired &&
      b.reset_needs_initial_publish);
  assert(cnet_sctp_assoc_take_stream_change(&b, &event) ==
      CNET_SCTP_ASSOC_EMPTY);

  assert(cnet_sctp_assoc_advance(&b, 8u, corresponding,
      sizeof(corresponding), &corresponding_n) == CNET_SCTP_ASSOC_OK);
  assert_add_outgoing_request(corresponding, corresponding_n,
      corresponding_sequence, 2u);
  assert(!b.reset_needs_initial_publish);
  assert(cnet_sctp_assoc_input(&a, 9u, corresponding, corresponding_n,
      final_response, sizeof(final_response), &final_response_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(final_response_n != 0u &&
      reset_response_result(final_response, final_response_n) ==
          CNET_SCTP_STREAM_RESET_PERFORMED &&
      a.reset_active && !a.reset_outgoing_complete &&
      a.reset_paired_incoming_complete &&
      a.data.config.inbound_streams == 6u &&
      a.data.config.outbound_streams == 4u &&
      cnet_sctp_assoc_take_stream_change(&a, &event) ==
          CNET_SCTP_ASSOC_EMPTY);

  assert(cnet_sctp_assoc_input(&a, 10u, first_response, first_response_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == 0u && !a.reset_active &&
      a.data.config.inbound_streams == 6u &&
      a.data.config.outbound_streams == 5u);
  assert(cnet_sctp_assoc_take_stream_change(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 6u &&
      event.outbound_streams == 5u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_take_stream_change(&a, &event) ==
      CNET_SCTP_ASSOC_EMPTY);

  assert(cnet_sctp_assoc_input(&b, 11u, final_response, final_response_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == 0u && !b.reset_active &&
      b.data.config.inbound_streams == 5u &&
      b.data.config.outbound_streams == 6u);
  assert(cnet_sctp_assoc_take_stream_change(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 5u &&
      event.outbound_streams == 6u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);

  assert(cnet_sctp_assoc_input(&b, 12u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == first_response_n &&
      memcmp(duplicate, first_response, first_response_n) == 0 &&
      b.data.config.inbound_streams == 5u &&
      b.data.config.outbound_streams == 6u);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void add_streams_pair_atomic_publish_and_backpressure(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_change_event event = {0};
  uint8_t request[1200], response[1200], duplicate[1200];
  size_t request_n, response_n = 0u, duplicate_n = 0u;
  uint32_t peer_sequence, local_sequence;

  ready_pair(&a, &b, &ca, &cb);
  peer_sequence = b.expected_peer_reset_request;
  local_sequence = b.next_reset_request;
  request_n = encode_add_streams_request(&a,
      peer_sequence, 1u, peer_sequence + 1u, 1u,
      request, sizeof(request));
  memcpy(&before, &b, sizeof(before));
  assert(cnet_sctp_assoc_input(&b, 6u, request, request_n,
      response, 1u, &response_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(response_n == 0u && memcmp(&b, &before, sizeof(b)) == 0);
  assert(cnet_sctp_assoc_input(&b, 6u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_PERFORMED &&
      b.expected_peer_reset_request == peer_sequence + 2u &&
      b.data.config.inbound_streams == 5u &&
      b.data.config.outbound_streams == 4u &&
      b.reset_needs_initial_publish);
  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0);

  memcpy(&before, &b, sizeof(before));
  assert(cnet_sctp_assoc_advance(&b, 8u, duplicate, 1u, &duplicate_n) ==
      CNET_SCTP_ASSOC_CAPACITY);
  assert(duplicate_n == 0u && memcmp(&b, &before, sizeof(b)) == 0);
  assert(cnet_sctp_assoc_advance(&b, 8u, duplicate,
      sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert_add_outgoing_request(duplicate, duplicate_n, local_sequence, 1u);

  request_n = encode_add_outgoing_request(&a, peer_sequence + 2u, 1u, 0u,
      request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 9u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS &&
      b.expected_peer_reset_request == peer_sequence + 2u);

  request_n = encode_reset_response_packet(&a, local_sequence,
      CNET_SCTP_STREAM_RESET_DENIED, request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 10u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.data.config.inbound_streams == 5u &&
      b.data.config.outbound_streams == 4u);

  request_n = encode_add_streams_request(&a,
      peer_sequence + 2u, 1u, peer_sequence + 3u, 1u,
      request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 11u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert_reset_response_pair(response, response_n,
      peer_sequence + 2u, CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS,
      peer_sequence + 3u, CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS);
  assert(b.expected_peer_reset_request == peer_sequence + 2u);
  assert(cnet_sctp_assoc_take_stream_change(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 5u &&
      event.outbound_streams == 4u &&
      event.result == CNET_SCTP_STREAM_RESET_DENIED);

  request_n = encode_add_streams_request(&a,
      peer_sequence + 2u, 13u, peer_sequence + 3u, 13u,
      request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 12u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert_reset_response_pair(response, response_n,
      peer_sequence + 2u, CNET_SCTP_STREAM_RESET_DENIED,
      peer_sequence + 3u, CNET_SCTP_STREAM_RESET_DENIED);
  assert(b.expected_peer_reset_request == peer_sequence + 4u &&
      b.data.config.inbound_streams == 5u &&
      b.data.config.outbound_streams == 4u && !b.reset_active);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void add_streams_pair_partial_success_timeout(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_change_event event = {0};
  uint8_t request[1200], response[1200], retry[1200];
  size_t request_n = 0u, response_n, retry_n = 0u;
  uint32_t outgoing_sequence, incoming_sequence;

  ready_pair(&a, &b, &ca, &cb);
  outgoing_sequence = a.next_reset_request;
  incoming_sequence = outgoing_sequence + 1u;
  assert(cnet_sctp_assoc_add_streams(&a, 6u, 1u, 1u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);

  response_n = encode_reset_response_packet(&b, incoming_sequence,
      CNET_SCTP_STREAM_RESET_PERFORMED, response, sizeof(response));
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_input(&a, 7u, response, response_n,
      retry, sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(retry_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);

  response_n = encode_reset_response_packet(&b, outgoing_sequence,
      CNET_SCTP_STREAM_RESET_PERFORMED, response, sizeof(response));
  assert(cnet_sctp_assoc_input(&a, 7u, response, response_n,
      retry, sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK);
  assert(retry_n == 0u && a.reset_active && a.reset_outgoing_complete &&
      a.data.config.outbound_streams == 5u &&
      a.data.config.inbound_streams == 4u);

  assert(cnet_sctp_assoc_advance(&a, 106u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 306u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 706u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 1106u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK && retry_n == 0u);
  assert(!a.reset_active && a.data.config.inbound_streams == 4u &&
      a.data.config.outbound_streams == 5u);
  assert(cnet_sctp_assoc_take_stream_change(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.inbound_streams == 4u &&
      event.outbound_streams == 5u &&
      event.result == CNET_SCTP_STREAM_RESET_TIMEOUT);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void stream_reset_roundtrip_reuse_and_duplicate(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  cnet_sctp_data_message message = {0};
  const uint8_t first = 'a', second = 'b', third = 'c';
  uint8_t data[1200], sack[1200], request[1200], response[1200];
  uint8_t duplicate[1200], reciprocal[1200], reciprocal_response[1200];
  uint8_t payload[8];
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  size_t data_n = 0u, sack_n = 0u, request_n = 0u, response_n = 0u;
  size_t duplicate_n = 0u, reciprocal_n = 0u;
  size_t reciprocal_response_n = 0u, payload_n = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  assert(a.peer_reconfig_supported && b.peer_reconfig_supported);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 2u, 51u, 0,
      &first, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_id == 2u && message.stream_sequence == 0u &&
      payload_n == 1u && payload[0] == first);
  assert(cnet_sctp_assoc_input(&a, 8u, sack, sack_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(a.data.tx_ssn[2] == 1u && b.data.rx_ssn[2] == 1u &&
      cnet_sctp_data_outbound_drained(&a.data));

  assert(cnet_sctp_assoc_reset_stream(&a, 9u, 2u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(request_n != 0u && a.reset_active);
  assert(cnet_sctp_assoc_send_message(&a, 9u, 2u, 51u, 0,
      &second, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_input(&b, 10u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(response_n != 0u && b.data.rx_ssn[2] == 0u &&
      (b.reciprocal_reset_pending & (UINT16_C(1) << 2u)) != 0u);

  /* RFC 6525 5.2.1: exact duplicate request gets the saved response and
   * cannot reset the stream or enqueue a second ULP event again. */
  assert(cnet_sctp_assoc_input(&b, 11u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK);
  assert(event.incoming && event.stream_id == 2u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_EMPTY);

  assert(cnet_sctp_assoc_input(&a, 12u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(sack_n == 0u && !a.reset_active && a.data.tx_ssn[2] == 0u &&
      (a.awaiting_peer_reciprocal & (UINT16_C(1) << 2u)) != 0u);
  assert(cnet_sctp_assoc_send_message(&a, 12u, 2u, 51u, 0,
      &second, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_BUSY);

  /* RFC 8831 6.7: B automatically resets its corresponding outgoing
   * direction. The pending bit makes this an immediate Owner deadline and
   * capacity rejection cannot consume it. */
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 11u);
  assert(cnet_sctp_assoc_advance(&b, 13u, reciprocal, 1u,
      &reciprocal_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(reciprocal_n == 0u && !b.reset_active &&
      (b.reciprocal_reset_pending & (UINT16_C(1) << 2u)) != 0u);
  assert(cnet_sctp_assoc_advance(&b, 13u, reciprocal,
      sizeof(reciprocal), &reciprocal_n) == CNET_SCTP_ASSOC_OK);
  assert(reciprocal_n != 0u && b.reset_active && b.reset_is_reciprocal &&
      (b.reciprocal_reset_pending & (UINT16_C(1) << 2u)) == 0u);
  assert(cnet_sctp_assoc_input(&a, 14u, reciprocal, reciprocal_n,
      reciprocal_response, sizeof(reciprocal_response),
      &reciprocal_response_n) == CNET_SCTP_ASSOC_OK);
  assert(reciprocal_response_n != 0u && a.data.rx_ssn[2] == 0u &&
      (a.awaiting_peer_reciprocal & (UINT16_C(1) << 2u)) == 0u &&
      a.reciprocal_reset_pending == 0u);
  assert(cnet_sctp_assoc_input(&b, 15u, reciprocal_response,
      reciprocal_response_n, sack, sizeof(sack), &sack_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(sack_n == 0u && !b.reset_active && b.data.tx_ssn[2] == 0u);

  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 2u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 2u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 2u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);

  assert(cnet_sctp_assoc_send_message(&a, 16u, 2u, 51u, 0,
      &second, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_wire_decode(data, data_n, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 0u && chunk.value_size >= 13u &&
      read16_test(chunk.value + 6u) == 0u);
  assert(cnet_sctp_assoc_input(&b, 17u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_sequence == 0u && payload_n == 1u &&
      payload[0] == second);
  assert(cnet_sctp_assoc_input(&a, 18u, sack, sack_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_send_message(&b, 19u, 2u, 51u, 0,
      &third, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_wire_decode(data, data_n, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 0u && read16_test(chunk.value + 6u) == 0u);
  assert(cnet_sctp_assoc_input(&a, 20u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&a, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_sequence == 0u && payload_n == 1u &&
      payload[0] == third);
  assert(cnet_sctp_assoc_input(&b, 21u, sack, sack_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void stream_reset_defers_until_buffered_delivery(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  cnet_sctp_data_message message = {0};
  const uint8_t byte = 'q';
  uint8_t data[1200], sack[1200], request[1200], response[1200], final[1200];
  uint8_t duplicate[1200], payload[8];
  size_t data_n = 0u, sack_n = 0u, request_n = 0u, response_n = 0u;
  size_t final_n = 0u, duplicate_n = 0u, payload_n = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 1u, 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 8u, sack, sack_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_reset_stream(&a, 9u, 1u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 10u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_IN_PROGRESS);
  assert(b.deferred_peer_reset_active &&
      b.deferred_peer_reset_streams == (UINT16_C(1) << 1u) &&
      b.data.rx_ssn[1] == 0u);
  assert(cnet_sctp_assoc_input(&b, 11u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0);
  assert(cnet_sctp_assoc_input(&a, 11u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_EMPTY);
  assert(a.reset_active && a.data.tx_ssn[1] == 1u);
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_sequence == 0u && payload_n == 1u &&
      payload[0] == byte && b.data.rx_ssn[1] == 1u);
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 11u);
  assert(cnet_sctp_assoc_advance(&b, 12u, final, 1u, &final_n) ==
      CNET_SCTP_ASSOC_CAPACITY);
  assert(final_n == 0u && b.deferred_peer_reset_active &&
      b.data.rx_ssn[1] == 1u);
  assert(cnet_sctp_assoc_advance(&b, 12u, final,
      sizeof(final), &final_n) == CNET_SCTP_ASSOC_OK);
  assert(final_n != 0u && !b.deferred_peer_reset_active &&
      b.data.rx_ssn[1] == 0u &&
      reset_response_result(final, final_n) ==
          CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_input(&a, 13u, final, final_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK);
  assert(event.result == CNET_SCTP_STREAM_RESET_PERFORMED &&
      a.data.tx_ssn[1] == 0u && b.data.rx_ssn[1] == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 1u);
  assert(cnet_sctp_assoc_advance(&b, 14u, request,
      sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(request_n != 0u && b.reset_active && b.reset_is_reciprocal);
  assert(cnet_sctp_assoc_input(&a, 15u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 16u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 1u);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 1u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(a.awaiting_peer_reciprocal == 0u &&
      b.reciprocal_reset_pending == 0u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void stream_reset_holds_post_barrier_data_until_completion(void) {
  cnet_sctp_assoc a, b, sender, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  cnet_sctp_data_message message = {0};
  const uint8_t old_byte = 'o', new_byte = 'n';
  uint8_t old_data[1200], new_data[1200], request[1200];
  uint8_t response[1200], duplicate[1200], next_request[1200];
  uint8_t collision[1200], sack[1200], payload[8];
  size_t old_n = 0u, new_n = 0u, request_n = 0u, response_n = 0u;
  size_t duplicate_n = 0u, collision_n = 0u, sack_n = 0u, payload_n = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  memcpy(&sender, &a, sizeof(sender));
  assert(cnet_sctp_assoc_send_message(&sender, 6u, 1u, 53u, 0,
      &old_byte, 1u, old_data, sizeof(old_data), &old_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_send_message(&sender, 7u, 1u, 53u, 1,
      &new_byte, 1u, new_data, sizeof(new_data), &new_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_reset_stream(&a, 6u, 1u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  rewrite_reset_last_tsn(request, request_n, UINT32_C(0xfffffffe));

  assert(cnet_sctp_assoc_input(&b, 8u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_IN_PROGRESS);
  assert(b.deferred_peer_reset_active &&
      b.deferred_peer_reset_last_tsn == UINT32_C(0xfffffffe));
  assert(cnet_sctp_assoc_input(&b, 9u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0);
  memcpy(next_request, request, request_n);
  rewrite_reset_sequence(next_request, request_n,
      b.expected_peer_reset_request);
  assert(cnet_sctp_assoc_input(&b, 9u, next_request, request_n,
      collision, sizeof(collision), &collision_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(collision, collision_n) ==
      CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS);
  assert(b.expected_peer_reset_request ==
      b.deferred_peer_reset_request + 1u);
  assert(cnet_sctp_assoc_send_message(&b, 9u, 1u, 53u, 0,
      &new_byte, 1u, collision, sizeof(collision), &collision_n) ==
      CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_shutdown(&b, 9u, collision,
      sizeof(collision), &collision_n) == CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_input(&a, 10u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK &&
      a.reset_active);

  /* The unordered post-barrier record would normally be immediately
   * deliverable. Deferred reset ownership keeps it buffered instead. */
  assert(cnet_sctp_assoc_input(&b, 11u, new_data, new_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_EMPTY);
  assert(cnet_sctp_assoc_input(&b, 12u, old_data, old_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(b.data.cumulative_rx == UINT32_C(0xffffffff));
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_id == 1u && !message.unordered &&
      message.tsn == UINT32_C(0xfffffffe) && payload_n == 1u &&
      payload[0] == old_byte);
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_EMPTY);
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 12u);

  memcpy(&before, &b, sizeof(before));
  assert(cnet_sctp_assoc_advance(&b, 13u, response, 1u, &response_n) ==
      CNET_SCTP_ASSOC_CAPACITY);
  assert(response_n == 0u && memcmp(&b, &before, sizeof(b)) == 0);
  assert(cnet_sctp_assoc_advance(&b, 13u, response,
      sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(!b.deferred_peer_reset_active && b.data.rx_ssn[1] == 0u);
  assert(cnet_sctp_assoc_input(&b, 14u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0);
  assert(cnet_sctp_assoc_input(&a, 14u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(!a.reset_active && a.data.tx_ssn[1] == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 1u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 1u);
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_id == 1u && message.unordered &&
      message.tsn == UINT32_C(0xffffffff) && payload_n == 1u &&
      payload[0] == new_byte);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void incoming_reset_waits_for_outbound_drain(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  cnet_sctp_data_message message = {0};
  const uint8_t byte = 'i';
  const uint16_t stream = 1u, other_stream = 2u;
  uint8_t data[1200], sack[1200], incoming[1200], duplicate[1200];
  uint8_t next[1200], reply[1200], outgoing[1200], response[1200];
  uint8_t payload[8];
  size_t data_n = 0u, sack_n = 0u, incoming_n, duplicate_n = 0u;
  size_t next_n, reply_n = 0u, outgoing_n = 0u, response_n = 0u;
  size_t payload_n = 0u;
  uint64_t deadline = 0u;
  uint32_t sequence;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&b, 6u, stream, 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  sequence = b.expected_peer_reset_request;
  incoming_n = encode_incoming_reset_request(&a,
      sequence, &stream, 1u,
      incoming, sizeof(incoming));
  assert(cnet_sctp_assoc_input(&b, 7u, incoming, incoming_n,
      reply, sizeof(reply), &reply_n) == CNET_SCTP_ASSOC_OK);
  assert(reply_n == 0u && b.peer_incoming_reset_pending &&
      b.peer_incoming_reset_streams ==
          (uint16_t)(UINT16_C(1) << stream) &&
      (b.reciprocal_reset_pending &
       (uint16_t)(UINT16_C(1) << stream)) != 0u);
  assert(cnet_sctp_assoc_send_message(&b, 7u, stream, 53u, 0,
      &byte, 1u, reply, sizeof(reply), &reply_n) == CNET_SCTP_ASSOC_BUSY);

  assert(cnet_sctp_assoc_input(&b, 8u, incoming, incoming_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == 0u && b.peer_incoming_reset_pending);
  next_n = encode_incoming_reset_request(&a,
      b.expected_peer_reset_request, &other_stream, 1u,
      next, sizeof(next));
  assert(cnet_sctp_assoc_input(&b, 8u, next, next_n,
      reply, sizeof(reply), &reply_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(reply, reply_n) ==
      CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS);
  assert(b.expected_peer_reset_request ==
      b.peer_incoming_reset_request + 1u);

  assert(cnet_sctp_assoc_input(&a, 9u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&a, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_id == stream && payload_n == 1u &&
      payload[0] == byte);
  assert(cnet_sctp_assoc_input(&b, 10u, sack, sack_n,
      reply, sizeof(reply), &reply_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_data_outbound_drained(&b.data));
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 10u);

  memcpy(&before, &b, sizeof(before));
  assert(cnet_sctp_assoc_advance(&b, 10u, outgoing, 1u,
      &outgoing_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(outgoing_n == 0u && memcmp(&b, &before, sizeof(b)) == 0);
  assert(cnet_sctp_assoc_advance(&b, 10u, outgoing,
      sizeof(outgoing), &outgoing_n) == CNET_SCTP_ASSOC_OK);
  assert(outgoing_n != 0u && b.reset_active && b.reset_is_reciprocal &&
      !b.peer_incoming_reset_pending &&
      outgoing_reset_response_sequence(outgoing, outgoing_n) ==
          sequence);
  assert(cnet_sctp_assoc_input(&b, 11u, incoming, incoming_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == outgoing_n &&
      memcmp(duplicate, outgoing, outgoing_n) == 0);

  assert(cnet_sctp_assoc_input(&a, 12u, outgoing, outgoing_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 13u, response, response_n,
      reply, sizeof(reply), &reply_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.data.tx_ssn[stream] == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming &&
      event.stream_id == stream &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void incoming_reset_immediate_is_capacity_atomic(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  const uint16_t stream = 0u;
  uint8_t incoming[1200], outgoing[1200];
  size_t incoming_n, outgoing_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  sequence = b.expected_peer_reset_request;
  incoming_n = encode_incoming_reset_request(&a, sequence, &stream, 1u,
      incoming, sizeof(incoming));
  memcpy(&before, &b, sizeof(before));
  assert(cnet_sctp_assoc_input(&b, 6u, incoming, incoming_n,
      outgoing, 1u, &outgoing_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(outgoing_n == 0u && memcmp(&b, &before, sizeof(b)) == 0);
  assert(cnet_sctp_assoc_input(&b, 6u, incoming, incoming_n,
      outgoing, sizeof(outgoing), &outgoing_n) == CNET_SCTP_ASSOC_OK);
  assert(outgoing_n != 0u && b.reset_active && b.reset_is_reciprocal &&
      outgoing_reset_response_sequence(outgoing, outgoing_n) == sequence);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void incoming_reset_overlap_is_nothing_to_do(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  const uint16_t stream = 2u;
  const uint16_t duplicate_streams[2] = {2u, 2u};
  uint8_t outgoing[1200], incoming[1200], response[1200];
  uint8_t duplicate[1200], next[1200], collision[1200], scratch[1200];
  size_t outgoing_n = 0u, incoming_n, response_n = 0u;
  size_t duplicate_n = 0u, next_n, collision_n = 0u, scratch_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_reset_stream(&b, 6u, stream,
      outgoing, sizeof(outgoing), &outgoing_n) == CNET_SCTP_ASSOC_OK);
  sequence = b.expected_peer_reset_request;
  incoming_n = encode_incoming_reset_request(&a, sequence, &stream, 1u,
      incoming, sizeof(incoming));
  assert(cnet_sctp_assoc_input(&b, 7u, incoming, incoming_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(response, response_n) ==
      CNET_SCTP_STREAM_RESET_NOTHING_TO_DO);
  assert(b.reset_active && b.reset_streams ==
      (uint16_t)(UINT16_C(1) << stream) &&
      b.expected_peer_reset_request == sequence + 1u);
  assert(cnet_sctp_assoc_input(&b, 8u, incoming, incoming_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0);

  assert(cnet_sctp_assoc_input(&a, 9u, outgoing, outgoing_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 10u, response, response_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.data.tx_ssn[stream] == 0u);
  next_n = encode_incoming_reset_request(&a,
      b.expected_peer_reset_request, duplicate_streams, 2u,
      next, sizeof(next));
  assert(cnet_sctp_assoc_input(&b, 11u, next, next_n,
      collision, sizeof(collision), &collision_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(collision, collision_n) ==
      CNET_SCTP_STREAM_RESET_WRONG_SSN);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void incoming_reset_all_streams_uses_empty_outgoing_list(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  uint8_t incoming[1200], outgoing[1200];
  size_t incoming_n, outgoing_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  sequence = b.expected_peer_reset_request;
  incoming_n = encode_incoming_reset_request(&a, sequence, NULL, 0u,
      incoming, sizeof(incoming));
  assert(cnet_sctp_assoc_input(&b, 6u, incoming, incoming_n,
      outgoing, sizeof(outgoing), &outgoing_n) == CNET_SCTP_ASSOC_OK);
  assert(outgoing_n != 0u && b.reset_active && b.reset_is_reciprocal &&
      b.reset_streams == UINT16_C(0x000f) &&
      outgoing_reset_response_sequence(outgoing, outgoing_n) == sequence &&
      outgoing_reset_parameter_size(outgoing, outgoing_n) == 16u &&
      outgoing_reset_stream_mask(outgoing, outgoing_n,
          b.config.outbound_streams) == UINT16_C(0x000f));
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void incoming_reset_multi_sid_roundtrip(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint16_t streams[2] = {0u, 2u};
  const uint16_t mask = (uint16_t)((UINT16_C(1) << 0u) |
      (UINT16_C(1) << 2u));
  const uint8_t byte = 'm';
  uint8_t incoming[1200], outgoing[1200], response[1200], scratch[1200];
  size_t incoming_n, outgoing_n = 0u, response_n = 0u, scratch_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  sequence = b.expected_peer_reset_request;
  incoming_n = encode_incoming_reset_request(&a, sequence, streams, 2u,
      incoming, sizeof(incoming));
  assert(cnet_sctp_assoc_input(&b, 6u, incoming, incoming_n,
      outgoing, sizeof(outgoing), &outgoing_n) == CNET_SCTP_ASSOC_OK);
  assert(outgoing_n != 0u && b.reset_active && b.reset_is_reciprocal &&
      b.reset_streams == mask &&
      outgoing_reset_response_sequence(outgoing, outgoing_n) == sequence &&
      outgoing_reset_stream_mask(outgoing, outgoing_n,
          b.config.outbound_streams) == mask);
  assert(cnet_sctp_assoc_send_message(&b, 6u, streams[0], 53u, 0,
      &byte, 1u, scratch, sizeof(scratch), &scratch_n) ==
      CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_send_message(&b, 6u, streams[1], 53u, 0,
      &byte, 1u, scratch, sizeof(scratch), &scratch_n) ==
      CNET_SCTP_ASSOC_BUSY);

  assert(cnet_sctp_assoc_input(&a, 7u, outgoing, outgoing_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 8u, response, response_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.data.tx_ssn[0] == 0u &&
      b.data.tx_ssn[2] == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 0u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 2u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void incoming_reset_partial_overlap_is_deferred(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint16_t active_stream = 1u;
  const uint16_t streams[2] = {1u, 2u};
  const uint16_t mask = (uint16_t)((UINT16_C(1) << 1u) |
      (UINT16_C(1) << 2u));
  uint8_t active[1200], incoming[1200], response[1200], scratch[1200];
  uint8_t deferred[1200], duplicate[1200];
  size_t active_n = 0u, incoming_n, response_n = 0u, scratch_n = 0u;
  size_t deferred_n = 0u, duplicate_n = 0u;
  uint64_t deadline = 0u;
  uint32_t sequence;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_reset_stream(&b, 6u, active_stream,
      active, sizeof(active), &active_n) == CNET_SCTP_ASSOC_OK);
  sequence = b.expected_peer_reset_request;
  incoming_n = encode_incoming_reset_request(&a, sequence, streams, 2u,
      incoming, sizeof(incoming));
  assert(cnet_sctp_assoc_input(&b, 7u, incoming, incoming_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(scratch_n == 0u && b.reset_active &&
      b.peer_incoming_reset_pending &&
      b.peer_incoming_reset_streams == mask &&
      b.expected_peer_reset_request == sequence + 1u);

  assert(cnet_sctp_assoc_input(&a, 8u, active, active_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming &&
      event.stream_id == active_stream);
  assert(cnet_sctp_assoc_input(&b, 9u, response, response_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.peer_incoming_reset_pending);
  assert(cnet_sctp_assoc_advance(&b, 9u, deferred,
      sizeof(deferred), &deferred_n) == CNET_SCTP_ASSOC_OK);
  assert(deferred_n == 0u && !b.reset_active &&
      b.peer_incoming_reset_pending &&
      b.peer_incoming_reset_streams == mask &&
      (b.outgoing_reset_events &
       (uint16_t)(UINT16_C(1) << active_stream)) != 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming &&
      event.stream_id == active_stream &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 9u);
  assert(cnet_sctp_assoc_advance(&b, 9u, deferred,
      sizeof(deferred), &deferred_n) == CNET_SCTP_ASSOC_OK);
  assert(deferred_n != 0u && b.reset_active && b.reset_streams == mask &&
      !b.peer_incoming_reset_pending &&
      outgoing_reset_response_sequence(deferred, deferred_n) == sequence &&
      outgoing_reset_stream_mask(deferred, deferred_n,
          b.config.outbound_streams) == mask);
  assert(cnet_sctp_assoc_input(&b, 10u, incoming, incoming_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == deferred_n &&
      memcmp(duplicate, deferred, deferred_n) == 0);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void paired_outgoing_incoming_reset_is_atomic(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint16_t incoming_stream = 1u;
  const uint16_t incoming_mask = (uint16_t)(UINT16_C(1) << 1u);
  uint8_t outgoing[1200], pair[1200], combined[1200];
  uint8_t duplicate[1200], final_response[1200], scratch[1200];
  size_t outgoing_n = 0u, pair_n, combined_n = 0u;
  size_t duplicate_n = 0u, final_n = 0u, scratch_n = 0u;
  uint32_t outgoing_sequence, incoming_sequence;

  ready_pair(&a, &b, &ca, &cb);
  b.data.rx_ssn[0] = 3u;
  b.data.tx_ssn[incoming_stream] = 4u;
  assert(cnet_sctp_assoc_reset_stream(&a, 6u, 0u,
      outgoing, sizeof(outgoing), &outgoing_n) == CNET_SCTP_ASSOC_OK);
  outgoing_sequence = outgoing_reset_request_sequence(outgoing, outgoing_n);
  incoming_sequence = outgoing_sequence + 1u;
  pair_n = encode_outgoing_incoming_reset_pair(&a,
      outgoing, outgoing_n, incoming_sequence,
      &incoming_stream, 1u, pair, sizeof(pair));

  memcpy(&before, &b, sizeof(before));
  assert(cnet_sctp_assoc_input(&b, 7u, pair, pair_n,
      combined, 1u, &combined_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(combined_n == 0u && memcmp(&b, &before, sizeof(b)) == 0);
  assert(cnet_sctp_assoc_input(&b, 7u, pair, pair_n,
      combined, sizeof(combined), &combined_n) == CNET_SCTP_ASSOC_OK);
  assert(combined_n != 0u && b.reset_active && b.reset_is_reciprocal &&
      b.reset_streams == incoming_mask && b.data.rx_ssn[0] == 0u &&
      b.expected_peer_reset_request == incoming_sequence + 1u);
  assert(reset_response_result(combined, combined_n) ==
      CNET_SCTP_STREAM_RESET_PERFORMED);
  assert_response_outgoing_pair(combined, combined_n,
      outgoing_sequence, incoming_sequence, incoming_mask,
      b.config.outbound_streams);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 0u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);

  assert(cnet_sctp_assoc_input(&b, 8u, pair, pair_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == combined_n &&
      memcmp(duplicate, combined, combined_n) == 0);
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_input(&a, 9u, combined, combined_n,
      final_response, 1u, &final_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(final_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  assert(cnet_sctp_assoc_input(&a, 9u, combined, combined_n,
      final_response, sizeof(final_response), &final_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(final_n != 0u && !a.reset_active &&
      a.data.tx_ssn[0] == 0u && a.data.rx_ssn[incoming_stream] == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming &&
      event.stream_id == incoming_stream);
  assert(cnet_sctp_assoc_input(&b, 10u, final_response, final_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.data.tx_ssn[incoming_stream] == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming &&
      event.stream_id == incoming_stream);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void local_paired_bidirectional_reset_roundtrip_is_atomic(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint16_t stream = 1u;
  const uint16_t mask = (uint16_t)(UINT16_C(1) << 1u);
  uint16_t a_incoming = 0u, a_outgoing = 0u;
  uint16_t b_incoming = 0u, b_outgoing = 0u;
  uint8_t pair[1200], combined[1200], response[1200], scratch[1200];
  size_t pair_n = 0u, combined_n = 0u, response_n = 0u, scratch_n = 0u;
  uint32_t outgoing_sequence, incoming_sequence;

  ready_pair(&a, &b, &ca, &cb);
  a.data.tx_ssn[stream] = 5u;
  a.data.rx_ssn[stream] = 6u;
  b.data.tx_ssn[stream] = 7u;
  b.data.rx_ssn[stream] = 8u;
  outgoing_sequence = a.next_reset_request;
  incoming_sequence = outgoing_sequence + 1u;
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_reset_streams_bidirectional(&a, 6u,
      &stream, 1u, pair, 1u, &pair_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(pair_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  assert(cnet_sctp_assoc_reset_streams_bidirectional(&a, 6u,
      &stream, 1u, pair, sizeof(pair), &pair_n) == CNET_SCTP_ASSOC_OK);
  assert(pair_n != 0u && a.reset_active &&
      a.reset_has_paired_incoming && !a.reset_outgoing_complete &&
      !a.reset_paired_incoming_complete && a.reset_streams == mask &&
      a.reset_paired_incoming_streams == mask &&
      a.reset_request == outgoing_sequence &&
      a.reset_paired_incoming_request == incoming_sequence &&
      a.next_reset_request == incoming_sequence + 1u);
  assert_outgoing_incoming_reset_pair(pair, pair_n,
      outgoing_sequence, incoming_sequence, mask, mask,
      a.config.outbound_streams, a.config.inbound_streams);

  assert(cnet_sctp_assoc_input(&b, 7u, pair, pair_n,
      combined, sizeof(combined), &combined_n) == CNET_SCTP_ASSOC_OK);
  assert(combined_n != 0u && b.reset_active && b.reset_is_reciprocal);
  assert_response_outgoing_pair(combined, combined_n,
      outgoing_sequence, incoming_sequence, mask,
      b.config.outbound_streams);
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_input(&a, 8u, combined, combined_n,
      response, 1u, &response_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(response_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  assert(cnet_sctp_assoc_input(&a, 8u, combined, combined_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(response_n != 0u && !a.reset_active &&
      a.data.tx_ssn[stream] == 0u && a.data.rx_ssn[stream] == 0u);
  while (cnet_sctp_assoc_take_stream_reset(&a, &event) ==
         CNET_SCTP_ASSOC_OK) {
    assert(event.stream_id == stream &&
        event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
    if (event.incoming) a_incoming |= mask;
    else a_outgoing |= mask;
  }
  assert(a_incoming == mask && a_outgoing == mask);

  assert(cnet_sctp_assoc_input(&b, 9u, response, response_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.data.tx_ssn[stream] == 0u &&
      b.data.rx_ssn[stream] == 0u);
  while (cnet_sctp_assoc_take_stream_reset(&b, &event) ==
         CNET_SCTP_ASSOC_OK) {
    assert(event.stream_id == stream &&
        event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
    if (event.incoming) b_incoming |= mask;
    else b_outgoing |= mask;
  }
  assert(b_incoming == mask && b_outgoing == mask);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void local_paired_reset_partial_completion_times_out_once(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint16_t stream = 2u;
  uint8_t pair[1200], response[1200], replay[1200], scratch[1200];
  size_t pair_n = 0u, response_n, replay_n = 0u, scratch_n = 123u;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_reset_streams_bidirectional(&a, 6u,
      &stream, 1u, pair, sizeof(pair), &pair_n) == CNET_SCTP_ASSOC_OK);
  response_n = encode_reset_response_packet(&b, a.reset_request,
      CNET_SCTP_STREAM_RESET_PERFORMED, response, sizeof(response));
  assert(cnet_sctp_assoc_input(&a, 7u, response, response_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(scratch_n == 0u && a.reset_active && a.reset_outgoing_complete &&
      !a.reset_paired_incoming_complete);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming &&
      event.stream_id == stream &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_advance(&a, 106u, replay,
      sizeof(replay), &replay_n) == CNET_SCTP_ASSOC_OK &&
      replay_n == pair_n && memcmp(replay, pair, pair_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 306u, replay,
      sizeof(replay), &replay_n) == CNET_SCTP_ASSOC_OK &&
      replay_n == pair_n);
  assert(cnet_sctp_assoc_advance(&a, 706u, replay,
      sizeof(replay), &replay_n) == CNET_SCTP_ASSOC_OK &&
      replay_n == pair_n);
  replay_n = 123u;
  assert(cnet_sctp_assoc_advance(&a, 1106u, replay,
      sizeof(replay), &replay_n) == CNET_SCTP_ASSOC_OK);
  assert(replay_n == 0u && !a.reset_active &&
      a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming &&
      event.stream_id == stream &&
      event.result == CNET_SCTP_STREAM_RESET_TIMEOUT);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_EMPTY);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void local_paired_reset_all_streams_and_denials_are_bounded(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint16_t all = UINT16_C(0x000f);
  uint16_t incoming = 0u, outgoing = 0u;
  uint8_t pair[1200], denials[1200], scratch[1200];
  size_t pair_n = 0u, denials_n, scratch_n = 123u;
  uint32_t outgoing_sequence, incoming_sequence;

  ready_pair(&a, &b, &ca, &cb);
  outgoing_sequence = a.next_reset_request;
  incoming_sequence = outgoing_sequence + 1u;
  assert(cnet_sctp_assoc_reset_streams_bidirectional(&a, 6u,
      NULL, 0u, pair, sizeof(pair), &pair_n) == CNET_SCTP_ASSOC_OK);
  assert_outgoing_incoming_reset_pair(pair, pair_n,
      outgoing_sequence, incoming_sequence, all, all,
      a.config.outbound_streams, a.config.inbound_streams);
  denials_n = encode_reset_response_pair_packet(&b,
      outgoing_sequence, CNET_SCTP_STREAM_RESET_DENIED,
      incoming_sequence, CNET_SCTP_STREAM_RESET_WRONG_SSN,
      denials, sizeof(denials));
  assert(cnet_sctp_assoc_input(&a, 7u, denials, denials_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(scratch_n == 0u && !a.reset_active);
  while (cnet_sctp_assoc_take_stream_reset(&a, &event) ==
         CNET_SCTP_ASSOC_OK) {
    const uint16_t bit = (uint16_t)(UINT16_C(1) << event.stream_id);
    if (event.incoming) {
      assert(event.result == CNET_SCTP_STREAM_RESET_WRONG_SSN);
      incoming |= bit;
    } else {
      assert(event.result == CNET_SCTP_STREAM_RESET_DENIED);
      outgoing |= bit;
    }
  }
  assert(incoming == all && outgoing == all);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void local_incoming_reset_encoding_is_capacity_atomic(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  const uint16_t streams[2] = {0u, 2u};
  const uint16_t mask = (uint16_t)((UINT16_C(1) << 0u) |
      (UINT16_C(1) << 2u));
  uint8_t request[1200], all_request[1200];
  size_t request_n = 0u, all_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  sequence = a.next_reset_request;
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_reset_incoming_streams(&a, 6u, streams, 2u,
      request, 1u, &request_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(request_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  assert(cnet_sctp_assoc_reset_incoming_streams(&a, 6u, streams, 2u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(request_n != 0u && a.reset_active &&
      a.reset_requests_incoming && !a.reset_is_reciprocal &&
      a.reset_streams == mask && a.reset_request == sequence &&
      a.next_reset_request == sequence + 1u &&
      incoming_reset_request_sequence(request, request_n) == sequence &&
      incoming_reset_parameter_size(request, request_n) == 12u &&
      incoming_reset_stream_mask(request, request_n,
          a.config.inbound_streams) == mask);

  sequence = b.next_reset_request;
  assert(cnet_sctp_assoc_reset_incoming_streams(&b, 6u, NULL, 0u,
      all_request, sizeof(all_request), &all_n) == CNET_SCTP_ASSOC_OK);
  assert(all_n != 0u && b.reset_streams == UINT16_C(0x000f) &&
      incoming_reset_request_sequence(all_request, all_n) == sequence &&
      incoming_reset_parameter_size(all_request, all_n) == 8u &&
      incoming_reset_stream_mask(all_request, all_n,
          b.config.inbound_streams) == UINT16_C(0x000f));
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void local_incoming_reset_roundtrip_and_outbound_independence(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint16_t streams[2] = {0u, 2u};
  const uint16_t mask = (uint16_t)((UINT16_C(1) << 0u) |
      (UINT16_C(1) << 2u));
  const uint8_t byte = 'i';
  uint8_t request[1200], data[1200], sack[1200];
  uint8_t outgoing[1200], response[1200], scratch[1200];
  size_t request_n = 0u, data_n = 0u, sack_n = 0u;
  size_t outgoing_n = 0u, response_n = 0u, scratch_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  a.data.rx_ssn[0] = 5u;
  a.data.rx_ssn[1] = 7u;
  a.data.rx_ssn[2] = 9u;
  sequence = a.next_reset_request;
  assert(cnet_sctp_assoc_reset_incoming_streams(&a, 6u, streams, 2u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  /* Resetting our receive direction does not block unrelated local DATA. */
  assert(cnet_sctp_assoc_send_message(&a, 6u, streams[0], 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 8u, sack, sack_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);

  assert(cnet_sctp_assoc_input(&b, 9u, request, request_n,
      outgoing, sizeof(outgoing), &outgoing_n) == CNET_SCTP_ASSOC_OK);
  assert(outgoing_n != 0u && b.reset_active && b.reset_is_reciprocal &&
      b.reset_streams == mask &&
      outgoing_reset_response_sequence(outgoing, outgoing_n) == sequence);
  assert(cnet_sctp_assoc_input(&a, 10u, outgoing, outgoing_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(response_n != 0u && !a.reset_active &&
      a.awaiting_local_incoming_reset == 0u &&
      a.reciprocal_reset_pending == 0u &&
      a.data.rx_ssn[0] == 0u && a.data.rx_ssn[1] == 7u &&
      a.data.rx_ssn[2] == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 0u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 2u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_EMPTY);
  assert(cnet_sctp_assoc_input(&b, 11u, response, response_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.data.tx_ssn[0] == 0u &&
      b.data.tx_ssn[2] == 0u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void local_incoming_reset_waits_for_peer_tsn_barrier(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint16_t streams[2] = {0u, 2u};
  const uint8_t byte = 'd';
  uint8_t request[1200], outgoing[1200], progress[1200];
  uint8_t data[1200], sack[1200], complete[1200];
  size_t request_n = 0u, outgoing_n = 0u, progress_n = 0u;
  size_t data_n = 0u, sack_n = 0u, complete_n = 0u;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_reset_incoming_streams(&a, 6u, streams, 2u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      outgoing, sizeof(outgoing), &outgoing_n) == CNET_SCTP_ASSOC_OK);
  /* Model a valid peer barrier whose final DATA is reordered after RE-CONFIG. */
  rewrite_reset_last_tsn(outgoing, outgoing_n, b.data.next_tsn);
  assert(cnet_sctp_assoc_input(&a, 8u, outgoing, outgoing_n,
      progress, sizeof(progress), &progress_n) == CNET_SCTP_ASSOC_OK);
  assert(reset_response_result(progress, progress_n) ==
      CNET_SCTP_STREAM_RESET_IN_PROGRESS);
  assert(!a.reset_active && a.awaiting_local_incoming_reset != 0u &&
      a.deferred_peer_reset_active &&
      a.deferred_peer_reset_answers_incoming);

  assert(cnet_sctp_assoc_send_message(&b, 9u, 1u, 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 10u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_advance(&a, 10u, complete,
      sizeof(complete), &complete_n) == CNET_SCTP_ASSOC_OK);
  assert(complete_n != 0u && reset_response_result(complete, complete_n) ==
      CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(!a.deferred_peer_reset_active &&
      a.awaiting_local_incoming_reset == 0u &&
      a.reciprocal_reset_pending == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 0u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 2u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_input(&b, 11u, complete, complete_n,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void local_incoming_reset_denial_is_reported(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint16_t stream = 1u;
  uint8_t request[1200], denial[1200], scratch[1200];
  size_t request_n = 0u, denial_n, scratch_n = 123u;

  ready_pair(&a, &b, &ca, &cb);
  a.data.rx_ssn[stream] = 4u;
  assert(cnet_sctp_assoc_reset_incoming_streams(&a, 6u, &stream, 1u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  denial_n = encode_reset_response_packet(&b, a.reset_request,
      CNET_SCTP_STREAM_RESET_DENIED, denial, sizeof(denial));
  assert(cnet_sctp_assoc_input(&a, 7u, denial, denial_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(scratch_n == 0u && !a.reset_active &&
      a.data.rx_ssn[stream] == 4u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming &&
      event.stream_id == stream &&
      event.result == CNET_SCTP_STREAM_RESET_DENIED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void local_incoming_reset_timeout_is_nonterminal(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint16_t stream = 3u;
  uint8_t request[1200], retry[1200];
  size_t request_n = 0u, retry_n = 0u;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_reset_incoming_streams(&a, 6u, &stream, 1u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_advance(&a, 106u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 306u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n);
  assert(cnet_sctp_assoc_advance(&a, 706u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK &&
      retry_n == request_n);
  retry_n = 123u;
  assert(cnet_sctp_assoc_advance(&a, 1106u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK);
  assert(retry_n == 0u && !a.reset_active &&
      a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming &&
      event.stream_id == stream &&
      event.result == CNET_SCTP_STREAM_RESET_TIMEOUT);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void stream_reset_timer_is_bounded_and_nonterminal(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint8_t byte = 'r';
  uint8_t request[1200], retry[1200];
  size_t request_n = 0u, retry_n = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_reset_stream(&a, 6u, 0u,
      request, 1u, &request_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(request_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  assert(cnet_sctp_assoc_reset_stream(&a, 6u, 0u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  retry_n = 123u;
  assert(cnet_sctp_assoc_shutdown(&a, 6u, retry, sizeof(retry), &retry_n) ==
      CNET_SCTP_ASSOC_BUSY);
  assert(retry_n == 0u && a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 106u);
  assert(cnet_sctp_assoc_advance(&a, 106u, retry, sizeof(retry), &retry_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(retry_n == request_n && memcmp(retry, request, request_n) == 0);
  assert(cnet_sctp_assoc_advance(&a, 306u, retry, sizeof(retry), &retry_n) ==
      CNET_SCTP_ASSOC_OK && retry_n == request_n);
  assert(cnet_sctp_assoc_advance(&a, 706u, retry, sizeof(retry), &retry_n) ==
      CNET_SCTP_ASSOC_OK && retry_n == request_n);
  retry_n = 123u;
  assert(cnet_sctp_assoc_advance(&a, 1106u, retry, sizeof(retry), &retry_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(retry_n == 0u && !a.reset_active &&
      a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 0u &&
      event.result == CNET_SCTP_STREAM_RESET_TIMEOUT);
  assert(a.awaiting_peer_reciprocal == 0u);
  assert(cnet_sctp_assoc_send_message(&a, 1106u, 0u, 51u, 0,
      &byte, 1u, request, sizeof(request), &request_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void stream_reset_reciprocal_waits_for_outbound_drain(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  cnet_sctp_data_message message = {0};
  const uint8_t byte = 'd';
  uint8_t data[1200], sack[1200], request[1200], response[1200];
  uint8_t reciprocal[1200], final_response[1200], payload[8];
  size_t data_n = 0u, sack_n = 0u, request_n = 0u, response_n = 0u;
  size_t reciprocal_n = 0u, final_response_n = 0u, payload_n = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&b, 6u, 3u, 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_reset_stream(&a, 6u, 3u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert((b.reciprocal_reset_pending & (UINT16_C(1) << 3u)) != 0u &&
      !cnet_sctp_data_outbound_drained(&b.data));
  assert(cnet_sctp_assoc_send_message(&b, 7u, 3u, 53u, 0,
      &byte, 1u, reciprocal, sizeof(reciprocal), &reciprocal_n) ==
      CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 106u);
  assert(cnet_sctp_assoc_input(&a, 8u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);

  assert(cnet_sctp_assoc_input(&a, 9u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&a, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_id == 3u && payload_n == 1u && payload[0] == byte);
  assert(cnet_sctp_assoc_input(&b, 10u, sack, sack_n,
      reciprocal, sizeof(reciprocal), &reciprocal_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_data_outbound_drained(&b.data));
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) ==
      CNET_SCTP_ASSOC_OK && has_deadline && deadline == 10u);
  assert(cnet_sctp_assoc_advance(&b, 10u, reciprocal,
      sizeof(reciprocal), &reciprocal_n) == CNET_SCTP_ASSOC_OK);
  assert(reciprocal_n != 0u && b.reset_active && b.reset_is_reciprocal);
  assert(cnet_sctp_assoc_input(&a, 11u, reciprocal, reciprocal_n,
      final_response, sizeof(final_response), &final_response_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 12u, final_response, final_response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.reciprocal_reset_pending == 0u &&
      a.awaiting_peer_reciprocal == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 3u);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 3u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void stream_reset_simultaneous_close_does_not_loop(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  uint8_t a_request[1200], b_request[1200], a_response[1200];
  uint8_t b_response[1200], scratch[1200];
  size_t a_request_n = 0u, b_request_n = 0u, a_response_n = 0u;
  size_t b_response_n = 0u, scratch_n = 0u;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_reset_stream(&a, 6u, 2u,
      a_request, sizeof(a_request), &a_request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_reset_stream(&b, 6u, 2u,
      b_request, sizeof(b_request), &b_request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 7u, b_request, b_request_n,
      a_response, sizeof(a_response), &a_response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, a_request, a_request_n,
      b_response, sizeof(b_response), &b_response_n) == CNET_SCTP_ASSOC_OK);
  assert(a.awaiting_peer_reciprocal == 0u &&
      b.awaiting_peer_reciprocal == 0u &&
      a.reciprocal_reset_pending == 0u &&
      b.reciprocal_reset_pending == 0u);
  assert(cnet_sctp_assoc_input(&a, 8u, b_response, b_response_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 8u, a_response, a_response_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(!a.reset_active && !b.reset_active &&
      a.reciprocal_reset_pending == 0u &&
      b.reciprocal_reset_pending == 0u);
  assert(cnet_sctp_assoc_advance(&a, 8u, scratch,
      sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK && scratch_n == 0u);
  assert(cnet_sctp_assoc_advance(&b, 8u, scratch,
      sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK && scratch_n == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 2u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 2u);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 2u);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 2u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void stream_reset_reciprocal_timeout_blocks_until_retry(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_stream_reset_event event = {0};
  const uint8_t byte = 'x';
  uint8_t request[1200], response[1200], reciprocal[1200], retry[1200];
  uint8_t original_reciprocal[1200];
  size_t request_n = 0u, response_n = 0u, reciprocal_n = 0u, retry_n = 0u;
  size_t original_reciprocal_n = 0u;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_reset_stream(&a, 6u, 0u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 0u);
  assert(cnet_sctp_assoc_input(&a, 8u, response, response_n,
      retry, sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 0u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_advance(&b, 9u, reciprocal,
      sizeof(reciprocal), &reciprocal_n) == CNET_SCTP_ASSOC_OK);
  assert(b.reset_active && b.reset_is_reciprocal);
  memcpy(original_reciprocal, reciprocal, reciprocal_n);
  original_reciprocal_n = reciprocal_n;
  assert(cnet_sctp_assoc_advance(&b, 109u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK && retry_n != 0u);
  assert(cnet_sctp_assoc_advance(&b, 309u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK && retry_n != 0u);
  assert(cnet_sctp_assoc_advance(&b, 709u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK && retry_n != 0u);
  assert(cnet_sctp_assoc_advance(&b, 1109u, retry,
      sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK && retry_n == 0u);
  assert(!b.reset_active &&
      (b.reciprocal_reset_failed & UINT16_C(1)) != 0u);
  assert(cnet_sctp_assoc_send_message(&b, 1109u, 0u, 51u, 0,
      &byte, 1u, retry, sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 0u &&
      event.result == CNET_SCTP_STREAM_RESET_TIMEOUT);
  assert(cnet_sctp_assoc_reset_stream(&b, 1110u, 0u,
      reciprocal, 1u, &reciprocal_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(reciprocal_n == 0u && !b.reset_active &&
      (b.reciprocal_reset_failed & UINT16_C(1)) != 0u);
  assert(cnet_sctp_assoc_reset_stream(&b, 1110u, 1u,
      retry, sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_BUSY);

  assert(cnet_sctp_assoc_reset_stream(&b, 1110u, 0u,
      reciprocal, sizeof(reciprocal), &reciprocal_n) == CNET_SCTP_ASSOC_OK);
  assert(b.reset_active && b.reset_is_reciprocal &&
      b.reciprocal_reset_failed == 0u &&
      reciprocal_n == original_reciprocal_n &&
      memcmp(reciprocal, original_reciprocal, reciprocal_n) == 0);
  assert(cnet_sctp_assoc_input(&a, 1111u, reciprocal, reciprocal_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(a.awaiting_peer_reciprocal == 0u &&
      a.reciprocal_reset_pending == 0u);
  assert(cnet_sctp_assoc_input(&b, 1112u, response, response_n,
      retry, sizeof(retry), &retry_n) == CNET_SCTP_ASSOC_OK);
  assert(!b.reset_active && b.reciprocal_reset_failed == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.incoming && event.stream_id == 0u);
  assert(cnet_sctp_assoc_take_stream_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.incoming && event.stream_id == 0u &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void association_reset_roundtrip_rebases_tsn_and_ssn(void) {
  cnet_sctp_assoc a, b;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_reset_event event = {0};
  cnet_sctp_data_message message = {0};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk = {0};
  cnet_sctp_data_bundle_item item = {0};
  const uint8_t byte = 'r';
  uint8_t data[1200], sack[1200], request[1200], response[1200];
  uint8_t duplicate[1200], payload[8];
  size_t data_n = 0u, sack_n = 0u, request_n = 0u, response_n = 0u;
  size_t duplicate_n = 0u, payload_n = 0u;
  uint32_t sequence, a_next, b_next;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 1u, 51u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 7u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 8u, sack, sack_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_send_message(&b, 8u, 2u, 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 9u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&a, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 9u, sack, sack_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(a.data.tx_ssn[1] == 1u && b.data.rx_ssn[1] == 1u &&
      b.data.tx_ssn[2] == 1u && a.data.rx_ssn[2] == 1u);

  sequence = a.next_reset_request;
  a_next = b.data.cumulative_rx + 1u + UINT32_C(0x80000000);
  b_next = b.data.next_tsn;
  assert(cnet_sctp_assoc_reset_association(&a, 10u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert_association_reset_request(request, request_n, sequence);
  assert(a.reset_active &&
      a.reconfig_kind == CNET_SCTP_RECONFIG_SSN_TSN_RESET);
  assert(cnet_sctp_assoc_send_message(&a, 10u, 0u, 51u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_queue_message(&a, 10u, 0u, 51u, 0,
      &byte, 1u) == CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_pump_message(&a, 10u,
      data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_BUSY);
  item.stream_id = 0u;
  item.ppid = 51u;
  item.payload = &byte;
  item.payload_size = 1u;
  assert(cnet_sctp_assoc_send_bundle(&a, 10u, &item, 1u,
      data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_BUSY);

  assert(cnet_sctp_assoc_input(&b, 11u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert_association_reset_response(response, response_n, sequence,
      CNET_SCTP_STREAM_RESET_PERFORMED, b_next, a_next);
  assert(b.data.next_tsn == b_next && b.data.cumulative_rx == a_next - 1u &&
      b.data.tx_ssn[2] == 0u && b.data.rx_ssn[1] == 0u);
  assert(cnet_sctp_assoc_input(&a, 12u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(!a.reset_active && a.data.next_tsn == a_next &&
      a.data.cumulative_rx == b_next - 1u &&
      a.data.tx_ssn[1] == 0u && a.data.rx_ssn[2] == 0u);

  assert(cnet_sctp_assoc_input(&b, 12u, request, request_n,
      duplicate, sizeof(duplicate), &duplicate_n) == CNET_SCTP_ASSOC_OK);
  assert(duplicate_n == response_n &&
      memcmp(duplicate, response, response_n) == 0);
  assert(cnet_sctp_assoc_take_association_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.initiated_locally &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED &&
      event.local_next_tsn == a_next && event.peer_next_tsn == b_next);
  assert(cnet_sctp_assoc_take_association_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.initiated_locally &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED &&
      event.local_next_tsn == b_next && event.peer_next_tsn == a_next);

  assert(cnet_sctp_assoc_send_message(&a, 13u, 1u, 51u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_wire_decode(data, data_n, &header, &decoder) ==
      CNET_SCTP_WIRE_OK &&
      cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK &&
      chunk.type == 0u && read32_test(chunk.value) == a_next &&
      read16_test(chunk.value + 6u) == 0u);
  assert(cnet_sctp_assoc_input(&b, 14u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK &&
      message.stream_sequence == 0u && message.tsn == a_next);
  assert(cnet_sctp_assoc_input(&a, 15u, sack, sack_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void association_reset_collision_and_minimum_interval(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_reset_event event = {0};
  uint8_t request_a[1200], request_b[1200];
  uint8_t response_a[1200], response_b[1200], scratch[1200];
  size_t request_a_n = 0u, request_b_n = 0u;
  size_t response_a_n = 0u, response_b_n = 0u, scratch_n = 0u;
  uint32_t a_sequence, b_sequence, a_next, b_next;
  uint32_t a_cumulative, b_cumulative;

  ready_pair(&a, &b, &ca, &cb);
  a_sequence = a.next_reset_request;
  b_sequence = b.next_reset_request;
  a_next = a.data.next_tsn;
  b_next = b.data.next_tsn;
  a_cumulative = a.data.cumulative_rx;
  b_cumulative = b.data.cumulative_rx;

  assert(cnet_sctp_assoc_reset_association(&a, 6u,
      request_a, sizeof(request_a), &request_a_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_reset_association(&b, 6u,
      request_b, sizeof(request_b), &request_b_n) == CNET_SCTP_ASSOC_OK);
  assert(a.last_association_reset_request_ms == 6u &&
      b.last_association_reset_request_ms == 6u);

  assert(cnet_sctp_assoc_input(&b, 7u, request_a, request_a_n,
      response_b, sizeof(response_b), &response_b_n) == CNET_SCTP_ASSOC_OK);
  assert_association_reset_response(response_b, response_b_n, a_sequence,
      CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS,
      b_next, b_cumulative + 1u + UINT32_C(0x80000000));
  assert(cnet_sctp_assoc_input(&a, 7u, request_b, request_b_n,
      response_a, sizeof(response_a), &response_a_n) == CNET_SCTP_ASSOC_OK);
  assert_association_reset_response(response_a, response_a_n, b_sequence,
      CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS,
      a_next, a_cumulative + 1u + UINT32_C(0x80000000));
  assert(a.expected_peer_reset_request == b_sequence + 1u &&
      b.expected_peer_reset_request == a_sequence + 1u);

  assert(cnet_sctp_assoc_input(&a, 8u, response_b, response_b_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 8u, response_a, response_a_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(!a.reset_active && !b.reset_active &&
      a.data.next_tsn == a_next && b.data.next_tsn == b_next &&
      a.data.cumulative_rx == a_cumulative &&
      b.data.cumulative_rx == b_cumulative);
  assert(cnet_sctp_assoc_take_association_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.initiated_locally &&
      event.result == CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS);
  assert(cnet_sctp_assoc_take_association_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && event.initiated_locally &&
      event.result == CNET_SCTP_STREAM_RESET_REQUEST_IN_PROGRESS);

  assert(cnet_sctp_assoc_reset_association(&a,
      6u + CNET_SCTP_ASSOC_RESET_MIN_INTERVAL_MS - 1u,
      request_a, sizeof(request_a), &request_a_n) == CNET_SCTP_ASSOC_BUSY);
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_reset_association(&a,
      6u + CNET_SCTP_ASSOC_RESET_MIN_INTERVAL_MS,
      request_a, 1u, &request_a_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(request_a_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  assert(cnet_sctp_assoc_reset_association(&a,
      6u + CNET_SCTP_ASSOC_RESET_MIN_INTERVAL_MS,
      request_a, sizeof(request_a), &request_a_n) == CNET_SCTP_ASSOC_OK);
  assert(a.last_association_reset_request_ms ==
      6u + CNET_SCTP_ASSOC_RESET_MIN_INTERVAL_MS);
  assert(cnet_sctp_assoc_input(&b,
      7u + CNET_SCTP_ASSOC_RESET_MIN_INTERVAL_MS,
      request_a, request_a_n, response_b, sizeof(response_b),
      &response_b_n) == CNET_SCTP_ASSOC_OK);
  assert_association_reset_response(response_b, response_b_n,
      a_sequence + 1u, CNET_SCTP_STREAM_RESET_PERFORMED,
      b_next, b_cumulative + 1u + UINT32_C(0x80000000));
  assert(cnet_sctp_assoc_input(&a,
      8u + CNET_SCTP_ASSOC_RESET_MIN_INTERVAL_MS,
      response_b, response_b_n, scratch, sizeof(scratch),
      &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_association_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.initiated_locally &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_take_association_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.initiated_locally &&
      event.result == CNET_SCTP_STREAM_RESET_PERFORMED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void association_reset_admission_and_backpressure_are_atomic(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_reset_event event = {0};
  cnet_sctp_data_message message = {0};
  const uint8_t byte = 'q';
  uint8_t data[1200], sack[1200], request[1200], response[1200], payload[8];
  size_t data_n = 0u, sack_n = 0u, request_n = 0u, response_n = 0u;
  size_t payload_n = 0u;
  uint32_t sequence;

  ready_pair(&a, &b, &ca, &cb);
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_reset_association(&a, 6u,
      request, 1u, &request_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(request_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);
  assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 51u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_reset_association(&a, 6u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_input(&b, 7u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 8u, sack, sack_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);

  sequence = a.next_reset_request;
  assert(cnet_sctp_assoc_reset_association(&a, 9u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 10u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert_association_reset_response(response, response_n, sequence,
      CNET_SCTP_STREAM_RESET_IN_PROGRESS,
      b.data.next_tsn,
      b.data.cumulative_rx + 1u + UINT32_C(0x80000000));
  assert(b.expected_peer_reset_request == sequence);
  assert(cnet_sctp_assoc_input(&a, 11u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(a.reset_active);
  assert(cnet_sctp_assoc_take_message(&b, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK);

  memcpy(&before, &b, sizeof(before));
  assert(cnet_sctp_assoc_input(&b, 12u, request, request_n,
      response, 1u, &response_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(response_n == 0u && memcmp(&b, &before, sizeof(b)) == 0);
  assert(cnet_sctp_assoc_input(&b, 12u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert_association_reset_response(response, response_n, sequence,
      CNET_SCTP_STREAM_RESET_PERFORMED, 1000u,
      UINT32_C(0x7fffffff));
  assert(cnet_sctp_assoc_input(&a, 13u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);

  request_n = encode_association_reset_request(&a,
      b.expected_peer_reset_request, request, sizeof(request));
  assert(cnet_sctp_assoc_input(&b, 14u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert_association_reset_response(response, response_n,
      b.expected_peer_reset_request,
      CNET_SCTP_STREAM_RESET_IN_PROGRESS,
      b.data.next_tsn,
      b.data.cumulative_rx + 1u + UINT32_C(0x80000000));
  assert(cnet_sctp_assoc_take_association_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.initiated_locally);
  assert(cnet_sctp_assoc_take_association_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.initiated_locally);
  assert(cnet_sctp_assoc_input(&b, 15u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert_association_reset_response(response, response_n,
      sequence + 1u, CNET_SCTP_STREAM_RESET_PERFORMED,
      1000u, UINT32_C(0xffffffff));
  assert(cnet_sctp_assoc_take_association_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.initiated_locally &&
      event.peer_next_tsn == UINT32_C(0xffffffff));
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void association_reset_denial_shape_and_timeout_are_nonterminal(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_reset_event event = {0};
  const uint8_t byte = 'n';
  uint8_t request[1200], response[1200], scratch[1200];
  size_t request_n = 0u, response_n = 0u, scratch_n = 0u;
  uint32_t original_next;

  ready_pair(&a, &b, &ca, &cb);
  original_next = a.data.next_tsn;
  assert(cnet_sctp_assoc_reset_association(&a, 6u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  response_n = encode_reset_response_packet(&b, a.reset_request,
      CNET_SCTP_STREAM_RESET_PERFORMED, response, sizeof(response));
  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_input(&a, 7u, response, response_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(scratch_n == 0u && memcmp(&a, &before, sizeof(a)) == 0);

  response_n = encode_association_reset_response(&b, a.reset_request,
      CNET_SCTP_STREAM_RESET_DENIED, 123u, 456u,
      response, sizeof(response));
  assert(cnet_sctp_assoc_input(&a, 7u, response, response_n,
      scratch, sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK);
  assert(!a.reset_active && a.data.next_tsn == original_next);
  assert(cnet_sctp_assoc_take_association_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.initiated_locally &&
      event.result == CNET_SCTP_STREAM_RESET_DENIED &&
      event.local_next_tsn == 0u && event.peer_next_tsn == 0u);
  assert(cnet_sctp_assoc_send_message(&a, 8u, 0u, 51u, 0,
      &byte, 1u, scratch, sizeof(scratch), &scratch_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);

  ready_pair(&a, &b, &ca, &cb);
  original_next = a.data.next_tsn;
  assert(cnet_sctp_assoc_reset_association(&a, 6u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_advance(&a, 106u, scratch,
      sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK && scratch_n != 0u);
  assert(a.last_association_reset_request_ms == 6u);
  assert(cnet_sctp_assoc_advance(&a, 306u, scratch,
      sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK && scratch_n != 0u);
  assert(a.last_association_reset_request_ms == 6u);
  assert(cnet_sctp_assoc_advance(&a, 706u, scratch,
      sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK && scratch_n != 0u);
  assert(a.last_association_reset_request_ms == 6u);
  assert(cnet_sctp_assoc_advance(&a, 1106u, scratch,
      sizeof(scratch), &scratch_n) == CNET_SCTP_ASSOC_OK && scratch_n == 0u);
  assert(!a.reset_active && a.data.next_tsn == original_next);
  assert(cnet_sctp_assoc_take_association_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.result == CNET_SCTP_STREAM_RESET_TIMEOUT);
  assert(cnet_sctp_assoc_send_message(&a, 1107u, 0u, 51u, 0,
      &byte, 1u, scratch, sizeof(scratch), &scratch_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void association_reset_response_preserves_unread_data(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca = {0}, cb = {0};
  cnet_sctp_assoc_reset_event event = {0};
  cnet_sctp_data_message message = {0};
  const uint8_t byte = 'u';
  uint8_t request[1200], data[1200], sack[1200], response[1200], payload[8];
  size_t request_n = 0u, data_n = 0u, sack_n = 0u, response_n = 0u;
  size_t payload_n = 0u;

  ready_pair(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_reset_association(&a, 6u,
      request, sizeof(request), &request_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_send_message(&b, 6u, 0u, 53u, 0,
      &byte, 1u, data, sizeof(data), &data_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 7u, data, data_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 8u, sack, sack_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 9u, request, request_n,
      response, sizeof(response), &response_n) == CNET_SCTP_ASSOC_OK);
  assert_association_reset_response(response, response_n, a.reset_request,
      CNET_SCTP_STREAM_RESET_PERFORMED, 1001u,
      UINT32_C(0x7ffffffe));

  memcpy(&before, &a, sizeof(before));
  assert(cnet_sctp_assoc_input(&a, 10u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_BUSY);
  assert(sack_n == 0u && memcmp(&a, &before, sizeof(a)) == 0 &&
      a.data.buffered_rx == 1u);
  assert(cnet_sctp_assoc_take_message(&a, &message, payload,
      sizeof(payload), &payload_n) == CNET_SCTP_ASSOC_OK &&
      payload_n == 1u && payload[0] == byte);
  assert(cnet_sctp_assoc_input(&a, 10u, response, response_n,
      sack, sizeof(sack), &sack_n) == CNET_SCTP_ASSOC_OK);
  assert(a.data.next_tsn == UINT32_C(0x7ffffffe) &&
      a.data.cumulative_rx == 1000u);
  assert(cnet_sctp_assoc_take_association_reset(&a, &event) ==
      CNET_SCTP_ASSOC_OK && event.initiated_locally);
  assert(cnet_sctp_assoc_take_association_reset(&b, &event) ==
      CNET_SCTP_ASSOC_OK && !event.initiated_locally);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

int main(void) {
  association_reset_roundtrip_rebases_tsn_and_ssn();
  association_reset_collision_and_minimum_interval();
  association_reset_admission_and_backpressure_are_atomic();
  association_reset_denial_shape_and_timeout_are_nonterminal();
  association_reset_response_preserves_unread_data();
  add_outgoing_streams_roundtrip_enables_new_sid();
  add_outgoing_streams_capacity_and_event_backpressure();
  add_outgoing_streams_denial_and_timeout_are_nonterminal();
  add_incoming_streams_roundtrip_enables_new_sid();
  add_incoming_streams_capacity_duplicate_and_backpressure();
  add_incoming_streams_denial_success_response_and_timeout();
  add_streams_pair_roundtrip_completes_once();
  add_streams_pair_atomic_publish_and_backpressure();
  add_streams_pair_partial_success_timeout();
  stream_reset_roundtrip_reuse_and_duplicate();
  stream_reset_defers_until_buffered_delivery();
  stream_reset_holds_post_barrier_data_until_completion();
  incoming_reset_waits_for_outbound_drain();
  incoming_reset_immediate_is_capacity_atomic();
  incoming_reset_overlap_is_nothing_to_do();
  incoming_reset_all_streams_uses_empty_outgoing_list();
  incoming_reset_multi_sid_roundtrip();
  incoming_reset_partial_overlap_is_deferred();
  paired_outgoing_incoming_reset_is_atomic();
  local_paired_bidirectional_reset_roundtrip_is_atomic();
  local_paired_reset_partial_completion_times_out_once();
  local_paired_reset_all_streams_and_denials_are_bounded();
  local_incoming_reset_encoding_is_capacity_atomic();
  local_incoming_reset_roundtrip_and_outbound_independence();
  local_incoming_reset_waits_for_peer_tsn_barrier();
  local_incoming_reset_denial_is_reported();
  local_incoming_reset_timeout_is_nonterminal();
  stream_reset_timer_is_bounded_and_nonterminal();
  stream_reset_reciprocal_waits_for_outbound_drain();
  stream_reset_simultaneous_close_does_not_loop();
  stream_reset_reciprocal_timeout_blocks_until_retry();
  single_data_sack_capacity_preserves_input_watermark();
  peer_abort_event_captures_data_tsn_before_teardown();
  peer_abort_prefix_sack_updates_ulp_ack_snapshot();
  peer_abort_prefix_invalid_sack_does_not_forge_ack();
  peer_abort_trailing_sack_does_not_change_ulp_ack();
  association_accepts_sack_duplicate_list_beyond_rx_slots();
  post_cookie_bundled_data();
  shutdown_no_inflight_and_t2();
  shutdown_t5_absolute_guard();
  shutdown_t5_preempts_late_ack();
  shutdown_t2_retry_budget();
  shutdown_sent_duplicate_data_does_not_reset_budget();
  shutdown_pending_data_and_cumulative_ack();
  shutdown_t2_refreshes_cumulative();
  shutdown_sent_accepts_bundled_data();
  shutdown_sent_does_not_process_sack();
  shutdown_sent_bundles_gap_sack();
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
