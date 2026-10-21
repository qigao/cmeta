#include "sctp_assoc_internal.h"

#ifdef NDEBUG
#undef NDEBUG /* Runtime checks remain enabled for release CI. */
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct test_context {
  int on_owner;
  int fail_mac;
  int attempt_reentry;
  int reentry_called;
  int mac_calls;
  int entropy_calls;
  int fail_entropy;
  int zero_tag;
  int attempt_entropy_reentry;
  int attempt_entropy_init_reentry;
  int entropy_reentry_result;
  int entropy_init_reentry_result;
  uint32_t tag;
  uint32_t initial_tsn;
  uint32_t test_key;
  cnet_sctp_assoc *association;
} test_context;

static int is_owner(void *v) {
  return ((test_context *)v)->on_owner;
}

/* NON-CRYPTOGRAPHIC test double; production requires provider-backed HMAC.
 * It verifies that the engine calls the opaque provider consistently, and
 * catches tampering in this test only. It must never be used in production. */
static int fake_mac(void *v, const uint8_t *message, size_t size,
                    uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  test_context *ctx = (test_context *)v;
  uint32_t acc = ctx->test_key;
  size_t i;
  ++ctx->mac_calls;
  if (ctx->attempt_reentry && ctx->association) {
    ctx->reentry_called = cnet_sctp_assoc_stop(ctx->association);
  }
  if (ctx->fail_mac) return -1;
  for (i = 0u; i < size; ++i) {
    acc = acc * UINT32_C(65599) + (uint32_t)message[i];
  }
  for (i = 0u; i < CNET_SCTP_ASSOC_MAC_BYTES; ++i) {
    acc = acc * UINT32_C(1664525) + UINT32_C(1013904223);
    out[i] = (uint8_t)(acc >> 24);
  }
  return 0;
}

/* DELIBERATELY NON-CRYPTOGRAPHIC; deterministic mock for control flow.
 * Never provide this in published or shipping Salts builds. */
static int fake_entropy(void *v, uint8_t *out, size_t n) {
  test_context *ctx = (test_context *)v;
  uint32_t tag = ctx->zero_tag ? 0u : ctx->tag;
  ++ctx->entropy_calls;
  if (ctx->attempt_entropy_reentry && ctx->association) {
    ctx->entropy_reentry_result = cnet_sctp_assoc_stop(ctx->association);
  }
  if (ctx->attempt_entropy_init_reentry && ctx->association) {
    ctx->entropy_init_reentry_result = cnet_sctp_assoc_init(
        ctx->association, &ctx->association->config, 0u);
  }
  if (ctx->fail_entropy) return -1;
  if (n != CNET_SCTP_ASSOC_ENTROPY_BYTES) return -1;
  out[0] = (uint8_t)(tag >> 24);
  out[1] = (uint8_t)(tag >> 16);
  out[2] = (uint8_t)(tag >> 8);
  out[3] = (uint8_t)tag;
  out[4] = (uint8_t)(ctx->initial_tsn >> 24);
  out[5] = (uint8_t)(ctx->initial_tsn >> 16);
  out[6] = (uint8_t)(ctx->initial_tsn >> 8);
  out[7] = (uint8_t)ctx->initial_tsn;
  return 0;
}

/* NON-CRYPTOGRAPHIC test verifier: mirrors the engine's private contract.
 * Actual two-key verification is tested separately with GmSSL. */
static int fake_verify(void *v, const uint8_t *message, size_t size,
                       const uint8_t claimed[CNET_SCTP_ASSOC_MAC_BYTES]) {
  uint8_t computed[CNET_SCTP_ASSOC_MAC_BYTES] = {0};
  uint8_t diff = 0u;
  size_t i;
  if (fake_mac(v, message, size, computed) != 0) return -1;
  for (i = 0u; i < sizeof(computed); ++i) diff |= (uint8_t)(computed[i] ^ claimed[i]);
  memset(computed, 0, sizeof(computed));
  return diff == 0u ? 0 : -1;
}

static cnet_sctp_assoc_config config(uint16_t local, uint32_t tag,
                                      test_context *ctx) {
  cnet_sctp_assoc_config c = {0};
  c.local_port = local;
  c.remote_port = local == 5000u ? 5001u : 5000u;
  ctx->tag = tag;
  ctx->initial_tsn = local == 5000u ? 0x0f0e0d0cu : 0x11223344u;
  c.receive_window = 8192u;
  c.outbound_streams = 8u;
  c.inbound_streams = 8u;
  c.t1_ms = 100u;
  c.t1_max_ms = 400u;
  c.cookie_lifetime_ms = 500u;
  c.peer_binding = UINT64_C(0x123456789abcdef0);
  c.max_retransmits = 2u;
  c.is_owner = is_owner;
  c.entropy = fake_entropy;
  c.mac = fake_mac;
  c.verify_mac = fake_verify;
  c.provider_context = ctx;
  return c;
}

static void setup(cnet_sctp_assoc *a, cnet_sctp_assoc *b,
                  test_context *ca, test_context *cb) {
  cnet_sctp_assoc_config ac;
  cnet_sctp_assoc_config bc;
  memset(a, 0, sizeof(*a));
  memset(b, 0, sizeof(*b));
  memset(ca, 0, sizeof(*ca));
  memset(cb, 0, sizeof(*cb));
  ca->on_owner = cb->on_owner = 1;
  ca->test_key = cb->test_key = UINT32_C(0x10293847);
  ca->association = a; cb->association = b;
  ac = config(5000u, UINT32_C(0x12345678), ca);
  bc = config(5001u, UINT32_C(0x87654321), cb);
  assert(cnet_sctp_assoc_init(a, &ac, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(b, &bc, 0u) == CNET_SCTP_ASSOC_OK);
}

static void entropy_init_fail_closed(void) {
  cnet_sctp_assoc a = {0};
  test_context ctx = {0};
  cnet_sctp_assoc_config c;
  uint8_t packet[1200];
  size_t packet_size = 0u;
  ctx.on_owner = 1;
  ctx.association = &a;
  c = config(5000u, UINT32_C(0x12345678), &ctx);
  c.verify_mac = NULL;
  assert(cnet_sctp_assoc_init(&a, &c, 0u) == CNET_SCTP_ASSOC_INVALID);
  c.verify_mac = fake_verify;
  c.entropy = NULL;
  assert(cnet_sctp_assoc_init(&a, &c, 0u) == CNET_SCTP_ASSOC_INVALID);
  assert(ctx.entropy_calls == 0);
  c.entropy = fake_entropy;
  ctx.on_owner = 0;
  assert(cnet_sctp_assoc_init(&a, &c, 0u) == CNET_SCTP_ASSOC_INVALID);
  assert(ctx.entropy_calls == 0);
  ctx.on_owner = 1;
  ctx.fail_entropy = 1;
  assert(cnet_sctp_assoc_init(&a, &c, 0u) == CNET_SCTP_ASSOC_ENTROPY);
  assert(ctx.entropy_calls == 1);
  assert(a.magic == 0u && a.local_tag == 0u && a.initial_tsn == 0u);
  assert(cnet_sctp_assoc_start(&a, 0u, packet, sizeof(packet), &packet_size) ==
         CNET_SCTP_ASSOC_INVALID);
  ctx.fail_entropy = 0;
  ctx.zero_tag = 1;
  assert(cnet_sctp_assoc_init(&a, &c, 0u) == CNET_SCTP_ASSOC_ENTROPY);
  assert(a.magic == 0u && a.local_tag == 0u);
  ctx.zero_tag = 0;
  ctx.attempt_entropy_reentry = 1;
  ctx.attempt_entropy_init_reentry = 1;
  assert(cnet_sctp_assoc_init(&a, &c, 0u) == CNET_SCTP_ASSOC_OK);
  assert(ctx.entropy_reentry_result == CNET_SCTP_ASSOC_BUSY);
  assert(ctx.entropy_init_reentry_result == CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_init(&a, &c, 0u) == CNET_SCTP_ASSOC_BUSY);
  assert(a.local_tag == UINT32_C(0x12345678));
  assert(a.initial_tsn == UINT32_C(0x0f0e0d0c));
  assert(ctx.entropy_calls == 3);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
}

/* Re-encode one borrowed Chunk with incoming ignored flag bits and a
 * fresh checksum without altering the sender's retained wire packet. */
static void retag_single_chunk_flags(uint8_t *packet, size_t size,
    uint8_t flags) {
  uint8_t revised[CNET_SCTP_ASSOC_MAX_PACKET];
  cnet_sctp_wire_header h = {0};
  cnet_sctp_wire_decoder d = {0};
  cnet_sctp_wire_chunk ch = {0};
  size_t written = 0u;
  assert(packet && size <= sizeof(revised));
  assert(cnet_sctp_wire_decode(packet, size, &h, &d) == CNET_SCTP_WIRE_OK);
  assert(h.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&d, &ch) == CNET_SCTP_WIRE_OK);
  ch.flags = flags;
  assert(cnet_sctp_wire_encode(h.source_port, h.destination_port,
      h.verification_tag, &ch, 1u, revised, sizeof(revised),
      &written) == CNET_SCTP_WIRE_OK);
  assert(written == size);
  memcpy(packet, revised, written);
}

/* A stateful caller is permitted to retry the identical INIT at the same
 * monotonic time when the INIT ACK does not fit. A stateless response must
 * not silently commit the Association time watermark on CAPACITY. */
static void init_ack_capacity_preserves_input_watermark(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca, cb;
  uint8_t packet[1200], output[1200];
  size_t packet_n = 0u, output_n = 123u, i;
  const uint64_t now = 20u;

  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 10u, packet, sizeof(packet),
      &packet_n) == CNET_SCTP_ASSOC_OK);
  assert(packet_n == 32u && b.state == CNET_SCTP_ASSOC_CLOSED);
  memcpy(&before, &b, sizeof(before));
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&b, now, packet, packet_n, output, 8u,
      &output_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(output_n == 0u && memcmp(&b, &before, sizeof(before)) == 0);
  for (i = 0u; i < sizeof(output); ++i) assert(output[i] == 0xCCu);
  assert(cb.mac_calls == 0); /* early output capacity gate, no signing */

  assert(cnet_sctp_assoc_input(&b, now, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_OK);
  assert(output_n > 0u && b.state == CNET_SCTP_ASSOC_CLOSED &&
      b.last_time_ms == now && cb.mac_calls > 0);
  output_n = 123u;
  assert(cnet_sctp_assoc_input(&b, now - 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_CLOCK);
  assert(output_n == 0u && b.last_time_ms == now);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void normal_handshake(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  uint8_t wire_init[1200], wire_ack[1200], wire_echo[1200], wire_ok[1200];
  uint8_t forged[1200];
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder decoder;
  cnet_sctp_wire_chunk echo;
  size_t n_init = 0, n_ack = 0, n_echo = 0, n_ok = 0, n_forged = 0;
  uint64_t deadline;
  int has_deadline;
  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 10u, wire_init, 8u, &n_init) ==
         CNET_SCTP_ASSOC_CAPACITY);
  assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_start(&a, 10u, wire_init, sizeof(wire_init), &n_init) ==
         CNET_SCTP_ASSOC_OK);
  assert(n_init == 32u && cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_COOKIE_WAIT);
  retag_single_chunk_flags(wire_init, n_init, 0x80u); /* INIT */
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
         CNET_SCTP_ASSOC_OK);
  assert(has_deadline && deadline == 110u);
  assert(cnet_sctp_assoc_input(&b, 20u, wire_init, n_init,
                               wire_ack, 8u, &n_ack) == CNET_SCTP_ASSOC_CAPACITY);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_input(&b, 20u, wire_init, n_init,
                               wire_ack, sizeof(wire_ack), &n_ack) == CNET_SCTP_ASSOC_OK);
  assert(n_ack == 124u && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  retag_single_chunk_flags(wire_ack, n_ack, 0xA0u); /* INIT ACK */
  assert(cnet_sctp_assoc_input(&a, 30u, wire_ack, n_ack,
                               wire_echo, 4u, &n_echo) == CNET_SCTP_ASSOC_CAPACITY);
  assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_COOKIE_WAIT);
  assert(cnet_sctp_assoc_input(&a, 30u, wire_ack, n_ack,
                               wire_echo, sizeof(wire_echo), &n_echo) == CNET_SCTP_ASSOC_OK);
  assert(n_echo == 104u && cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_COOKIE_ECHOED);
  retag_single_chunk_flags(wire_echo, n_echo, 0xF0u); /* COOKIE ECHO */
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
         CNET_SCTP_ASSOC_OK);
  assert(has_deadline && deadline == 130u);
  assert(cnet_sctp_wire_decode(wire_echo, n_echo, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_wire_next_chunk(&decoder, &echo) == CNET_SCTP_WIRE_OK);
  assert(echo.type == 10u && echo.value_size == CNET_SCTP_ASSOC_COOKIE_BYTES);
  {
    uint8_t tampered[CNET_SCTP_ASSOC_COOKIE_BYTES];
    cnet_sctp_wire_chunk bad_echo = echo;
    memcpy(tampered, echo.value, sizeof(tampered));
    tampered[24u] ^= 1u; /* tamper peer TSN without breaking framing/CRC */
    bad_echo.value = tampered;
    assert(cnet_sctp_wire_encode(5000u, 5001u, UINT32_C(0x87654321),
             &bad_echo, 1u, forged, sizeof(forged), &n_forged) == CNET_SCTP_WIRE_OK);
  }
  assert(cnet_sctp_assoc_input(&b, 40u, forged, n_forged,
                               wire_ok, sizeof(wire_ok), &n_ok) == CNET_SCTP_ASSOC_AUTH);
  assert(n_ok == 0u && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_input(&b, 40u, wire_echo, n_echo,
                               wire_ok, 8u, &n_ok) == CNET_SCTP_ASSOC_CAPACITY);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  cb.attempt_reentry = 1;
  assert(cnet_sctp_assoc_input(&b, 40u, wire_echo, n_echo,
                               wire_ok, sizeof(wire_ok), &n_ok) == CNET_SCTP_ASSOC_OK);
  cb.attempt_reentry = 0;
  assert(cb.reentry_called == CNET_SCTP_ASSOC_BUSY);
  assert(n_ok == 16u && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_ESTABLISHED);
  /* COOKIE ACK loss: another COOKIE ECHO must ACK without opening twice. */
  assert(cnet_sctp_assoc_input(&b, 50u, wire_echo, n_echo,
                               wire_ack, sizeof(wire_ack), &n_ack) == CNET_SCTP_ASSOC_OK);
  assert(n_ack == n_ok && memcmp(wire_ack, wire_ok, n_ack) == 0);
  retag_single_chunk_flags(wire_ok, n_ok, 0xC0u); /* COOKIE ACK */
  assert(cnet_sctp_assoc_input(&a, 55u, wire_ok, n_ok,
                               forged, sizeof(forged), &n_forged) == CNET_SCTP_ASSOC_OK);
  assert(n_forged == 0u && cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) == CNET_SCTP_ASSOC_OK);
  assert(!has_deadline);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_drained(&a) && cnet_sctp_assoc_drained(&b));
  assert(cnet_sctp_assoc_input(&a, 60u, wire_ok, n_ok,
                               forged, sizeof(forged), &n_forged) ==
         CNET_SCTP_ASSOC_CLOSED_ERROR);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void clock_owner_and_timeout(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  uint8_t init[1200], out[1200];
  size_t init_n = 0, n = 0;
  uint64_t deadline = 0;
  int exists = 0;
  setup(&a, &b, &ca, &cb);
  ca.on_owner = 0;
  n = 99u;
  assert(cnet_sctp_assoc_input(&a, 0u, init, sizeof(init), out, sizeof(out), &n) ==
         CNET_SCTP_ASSOC_OWNER);
  assert(n == 0u);
  n = 99u;
  assert(cnet_sctp_assoc_advance(&a, 0u, out, sizeof(out), &n) ==
         CNET_SCTP_ASSOC_OWNER);
  assert(n == 0u);
  assert(cnet_sctp_assoc_start(&a, 0u, init, sizeof(init), &init_n) ==
         CNET_SCTP_ASSOC_OWNER);
  ca.on_owner = 1;
  assert(cnet_sctp_assoc_start(&a, 0u, init, sizeof(init), &init_n) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_BUSY);
  assert(cnet_sctp_assoc_advance(&a, 90u, out, sizeof(out), &n) == CNET_SCTP_ASSOC_OK);
  assert(n == 0u);
  assert(cnet_sctp_assoc_advance(&a, 100u, out, 2u, &n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(n == 0u);
  assert(cnet_sctp_assoc_advance(&a, 100u, out, sizeof(out), &n) == CNET_SCTP_ASSOC_OK);
  assert(n == init_n && memcmp(out, init, n) == 0);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &exists) == CNET_SCTP_ASSOC_OK);
  assert(exists && deadline == 300u);
  assert(cnet_sctp_assoc_advance(&a, 299u, out, sizeof(out), &n) == CNET_SCTP_ASSOC_OK);
  assert(n == 0u);
  assert(cnet_sctp_assoc_advance(&a, 300u, out, sizeof(out), &n) == CNET_SCTP_ASSOC_OK);
  assert(n == init_n && memcmp(out, init, n) == 0);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &exists) == CNET_SCTP_ASSOC_OK);
  assert(exists && deadline == 700u);
  assert(cnet_sctp_assoc_advance(&a, 700u, out, sizeof(out), &n) == CNET_SCTP_ASSOC_EXPIRED);
  assert(n == 0u && cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_FAILED);
  assert(cnet_sctp_assoc_advance(&a, 701u, out, sizeof(out), &n) ==
         CNET_SCTP_ASSOC_CLOSED_ERROR);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void stale_cookie_and_wrong_mac(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  uint8_t init[1200], ack[1200], echo[1200], out[1200];
  size_t init_n = 0, ack_n = 0, echo_n = 0, n = 0;
  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 2u, init, sizeof(init), &init_n) == CNET_SCTP_ASSOC_OK);
  cb.fail_mac = 1;
  assert(cnet_sctp_assoc_input(&b, 4u, init, init_n, ack, sizeof(ack), &ack_n) ==
         CNET_SCTP_ASSOC_AUTH);
  assert(ack_n == 0 && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  cb.fail_mac = 0;
  assert(cnet_sctp_assoc_input(&b, 4u, init, init_n, ack, sizeof(ack), &ack_n) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 5u, ack, ack_n, echo, sizeof(echo), &echo_n) ==
         CNET_SCTP_ASSOC_OK);
  cb.test_key ^= 0x11002200u;
  assert(cnet_sctp_assoc_input(&b, 10u, echo, echo_n, out, sizeof(out), &n) ==
         CNET_SCTP_ASSOC_AUTH);
  cb.test_key ^= 0x11002200u;
  assert(cnet_sctp_assoc_input(&b, 505u, echo, echo_n, out, sizeof(out), &n) ==
         CNET_SCTP_ASSOC_EXPIRED);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_input(&b, 10u, echo, echo_n, out, sizeof(out), &n) ==
         CNET_SCTP_ASSOC_CLOCK);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}


static void binding_vtag_and_invalid_input(void) {
  cnet_sctp_assoc a, b, wrong_generation = {0};
  test_context ca, cb, cg;
  cnet_sctp_assoc_config cfg;
  cnet_sctp_wire_chunk cookie_ack = {11u, 0u, NULL, 0u};
  uint8_t init[1200], ack[1200], echo[1200], out[1200], bad_ack[1200];
  size_t init_n = 0u, ack_n = 0u, echo_n = 0u, out_n = 0u, bad_n = 0u;
  setup(&a, &b, &ca, &cb);
  memset(&cg, 0, sizeof(cg));
  cg.on_owner = 1;
  cg.test_key = cb.test_key;
  cg.association = &wrong_generation;
  cfg = config(5001u, UINT32_C(0x87654321), &cg);
  cfg.peer_binding ^= UINT64_C(0x100);
  assert(cnet_sctp_assoc_init(&wrong_generation, &cfg, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_start(&a, 3u, init, sizeof(init), &init_n) == CNET_SCTP_ASSOC_OK);
  cb.on_owner = 0;
  assert(cnet_sctp_assoc_input(&b, 4u, init, init_n, ack, sizeof(ack), &ack_n) ==
         CNET_SCTP_ASSOC_OWNER);
  assert(cb.mac_calls == 0);
  cb.on_owner = 1;
  assert(cnet_sctp_assoc_input(&b, 4u, init, init_n, ack, sizeof(ack), &ack_n) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 5u, ack, ack_n, echo, sizeof(echo), &echo_n) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&wrong_generation, 6u, echo, echo_n,
                               out, sizeof(out), &out_n) == CNET_SCTP_ASSOC_AUTH);
  assert(out_n == 0u && cnet_sctp_assoc_state_get(&wrong_generation) ==
         CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_wire_encode(5001u, 5000u, UINT32_C(0xfefe0001),
      &cookie_ack, 1u, bad_ack, sizeof(bad_ack), &bad_n) == CNET_SCTP_WIRE_OK);
  {
    uint64_t last_valid_time = a.last_time_ms;
    assert(cnet_sctp_assoc_input(&a, 7u, bad_ack, bad_n, out, sizeof(out), &out_n) ==
           CNET_SCTP_ASSOC_PROTOCOL);
    assert(a.last_time_ms == last_valid_time);
    assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_COOKIE_ECHOED);
    assert(cnet_sctp_assoc_input(&a, 8u, bad_ack, bad_n - 1u,
                                 out, sizeof(out), &out_n) ==
           CNET_SCTP_ASSOC_PROTOCOL);
    assert(a.last_time_ms == last_valid_time);
  }
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&wrong_generation) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&wrong_generation) == CNET_SCTP_ASSOC_OK);
}

static void cookie_echo_t1_retransmission(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  uint8_t init[1200], ack[1200], echo[1200], repeated[1200], confirm[1200];
  size_t init_n = 0u, ack_n = 0u, echo_n = 0u;
  size_t repeated_n = 0u, confirm_n = 0u;
  uint64_t deadline = 0u;
  int exists = 0;
  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &init_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, init, init_n, ack, sizeof(ack), &ack_n) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 3u, ack, ack_n, echo, sizeof(echo), &echo_n) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &exists) == CNET_SCTP_ASSOC_OK);
  assert(exists && deadline == 103u);
  assert(cnet_sctp_assoc_advance(&a, 103u, repeated, sizeof(repeated), &repeated_n) ==
         CNET_SCTP_ASSOC_OK);
  assert(repeated_n == echo_n && memcmp(repeated, echo, echo_n) == 0);
  assert(cnet_sctp_assoc_input(&b, 104u, repeated, repeated_n,
                               confirm, sizeof(confirm), &confirm_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 105u, confirm, confirm_n,
                               repeated, sizeof(repeated), &repeated_n) == CNET_SCTP_ASSOC_OK);
  assert(repeated_n == 0u && cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &exists) == CNET_SCTP_ASSOC_OK);
  assert(!exists);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}


/* End-to-end test through actual SCTP wire CRC, one-chunk parser and
 * Association state machine; these are not only direct TLV helper tests. */
static size_t append_parameter_to_sctp_packet(const uint8_t *input, size_t n,
    uint16_t type, const uint8_t *payload, size_t payload_size,
    uint8_t *out, size_t cap) {
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder decoder;
  cnet_sctp_wire_chunk chunk;
  uint8_t value[1200];
  size_t written = 0u;
  const size_t length = payload_size + 4u;
  const size_t padded = (length + 3u) & ~(size_t)3u;
  size_t offset;
  assert(payload_size <= UINT16_MAX - 4u);
  assert(cnet_sctp_wire_decode(input, n, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  assert(chunk.value_size + padded <= sizeof(value));
  offset = chunk.value_size;
  memcpy(value, chunk.value, offset);
  value[offset] = (uint8_t)(type >> 8);
  value[offset + 1u] = (uint8_t)type;
  value[offset + 2u] = (uint8_t)(length >> 8);
  value[offset + 3u] = (uint8_t)length;
  if (payload_size != 0u) memcpy(value + offset + 4u, payload, payload_size);
  memset(value + offset + length, 0, padded - length);
  chunk.value = value;
  chunk.value_size = offset + padded;
  assert(cnet_sctp_wire_encode(header.source_port, header.destination_port,
      header.verification_tag, &chunk, 1u, out, cap, &written) == CNET_SCTP_WIRE_OK);
  return written;
}

static void reseal_sctp_checksum(uint8_t *packet, size_t size) {
  uint32_t crc;
  assert(packet != NULL && size >= 16u);
  memset(packet + 8u, 0, 4u);
  crc = cnet_sctp_wire_crc32c(packet, size);
  packet[8] = (uint8_t)crc;
  packet[9] = (uint8_t)(crc >> 8);
  packet[10] = (uint8_t)(crc >> 16);
  packet[11] = (uint8_t)(crc >> 24);
}

static void init_tlv_wire_gate(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  uint8_t init[1200], ack[1200], encoded[1200], echo[1200], confirm[1200], scratch[1200];
  uint8_t dummy_ipv4[4] = {127u, 0u, 0u, 1u};
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder decoder;
  cnet_sctp_wire_chunk chunk;
  size_t init_n = 0u, ack_n = 0u, encoded_n = 0u, echo_n = 0u, confirm_n = 0u;
  unsigned int prior_signs;

  /* Unknown 01 on INIT: stop+report inside INIT ACK, then normal
   * COOKIE ECHO/ACK. Capacity refusal must happen before MAC signing. */
  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &init_n) == CNET_SCTP_ASSOC_OK);
  encoded_n = append_parameter_to_sctp_packet(init, init_n, 0x4003u,
                                               NULL, 0u, encoded, sizeof(encoded));
  prior_signs = (unsigned int)cb.mac_calls;
  assert(cnet_sctp_assoc_input(&b, 2u, encoded, encoded_n, ack, 32u, &ack_n) ==
         CNET_SCTP_ASSOC_CAPACITY);
  assert(ack_n == 0u && cb.mac_calls == (int)prior_signs);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_input(&b, 2u, encoded, encoded_n, ack,
                               sizeof(ack), &ack_n) == CNET_SCTP_ASSOC_OK);
  assert(ack_n != 0u && cb.mac_calls == (int)prior_signs + 1);
  assert(cnet_sctp_wire_decode(ack, ack_n, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u && header.verification_tag == a.local_tag);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  assert(chunk.type == 2u && chunk.value_size >= 116u);
  /* 108 bytes fixed INIT ACK + Cookie followed by Unrecognized Parameter. */
  assert(chunk.value[108u] == 0u && chunk.value[109u] == 8u);
  assert(chunk.value[110u] == 0u && chunk.value[111u] == 8u);
  assert(chunk.value[112u] == 0x40u && chunk.value[113u] == 3u);
  assert(cnet_sctp_assoc_input(&a, 3u, ack, ack_n, echo,
                               sizeof(echo), &echo_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 4u, echo, echo_n, confirm,
                               sizeof(confirm), &confirm_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 5u, confirm, confirm_n, scratch,
                               sizeof(scratch), &encoded_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_ESTABLISHED);
  /* A framed but illegal multi-control bundle must be rejected by full
   * Association preflight before the monotonic watermark or TCB changes. */
  {
    cnet_sctp_wire_chunk illegal[2] = {
      {11u, 0u, NULL, 0u}, {11u, 0u, NULL, 0u}
    };
    uint64_t prior_time = a.last_time_ms;
    size_t illegal_n = 0u;
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, illegal, 2u,
        encoded, sizeof(encoded), &illegal_n) == CNET_SCTP_WIRE_OK);
    assert(cnet_sctp_assoc_input(&a, 6u, encoded, illegal_n, scratch,
        sizeof(scratch), &encoded_n) == CNET_SCTP_ASSOC_PROTOCOL);
    assert(encoded_n == 0u && a.last_time_ms == prior_time);
    assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_ESTABLISHED);
  }
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);

  /* Unknown 11 on INIT ACK: COOKIE ECHO and ERROR cause 8 are bundled.
   * Both must be covered by CRC32C and the entire saved T1 retransmission. */
  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &init_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, init, init_n, ack,
                               sizeof(ack), &ack_n) == CNET_SCTP_ASSOC_OK);
  encoded_n = append_parameter_to_sctp_packet(ack, ack_n, 0xc002u,
                                               NULL, 0u, encoded, sizeof(encoded));
  assert(cnet_sctp_assoc_input(&a, 3u, encoded, encoded_n, echo,
                               16u, &echo_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(echo_n == 0u && cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_COOKIE_WAIT);
  assert(cnet_sctp_assoc_input(&a, 3u, encoded, encoded_n, echo,
                               sizeof(echo), &echo_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_wire_decode(echo, echo_n, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 2u && header.verification_tag == b.local_tag);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  assert(chunk.type == 10u && chunk.value_size == CNET_SCTP_ASSOC_COOKIE_BYTES);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  assert(chunk.type == 9u && chunk.value_size == 8u);
  assert(chunk.value[0] == 0u && chunk.value[1] == 8u);
  assert(chunk.value[2] == 0u && chunk.value[3] == 8u);
  assert(chunk.value[4] == 0xc0u && chunk.value[5] == 2u);
  /* A syntactically broken advisory ERROR MUST NOT admit the accompanying
   * COOKIE ECHO, even when the outer packet checksum has been recomputed. */
  {
    uint64_t prior_time = b.last_time_ms;
    memcpy(scratch, echo, echo_n);
    scratch[111u] ^= 1u; /* ERROR cause length 8 -> 9 */
    reseal_sctp_checksum(scratch, echo_n);
    assert(cnet_sctp_assoc_input(&b, 4u, scratch, echo_n, confirm,
                                 sizeof(confirm), &confirm_n) == CNET_SCTP_ASSOC_PROTOCOL);
    assert(confirm_n == 0u &&
           cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
    assert(b.last_time_ms == prior_time);
  }
  assert(cnet_sctp_assoc_advance(&a, 103u, scratch,
                                 sizeof(scratch), &encoded_n) == CNET_SCTP_ASSOC_OK);
  assert(encoded_n == echo_n && memcmp(echo, scratch, echo_n) == 0);
  /* The saved T1 packet remains byte-identical. A separate on-wire copy
   * has ignored flags on both COOKIE ECHO and advisory ERROR. */
  {
    cnet_sctp_wire_header framed = {0};
    cnet_sctp_wire_decoder reader = {0};
    cnet_sctp_wire_chunk flagged[2];
    uint8_t modified[CNET_SCTP_ASSOC_MAX_PACKET];
    size_t changed = 0u;
    assert(cnet_sctp_wire_decode(scratch, encoded_n, &framed, &reader) ==
        CNET_SCTP_WIRE_OK && framed.chunk_count == 2u);
    assert(cnet_sctp_wire_next_chunk(&reader, &flagged[0]) ==
        CNET_SCTP_WIRE_OK);
    assert(cnet_sctp_wire_next_chunk(&reader, &flagged[1]) ==
        CNET_SCTP_WIRE_OK);
    assert(flagged[0].type == 10u && flagged[1].type == 9u);
    flagged[0].flags = 0xA0u;
    flagged[1].flags = 0xF0u;
    assert(cnet_sctp_wire_encode(framed.source_port,
        framed.destination_port, framed.verification_tag, flagged, 2u,
        modified, sizeof(modified), &changed) == CNET_SCTP_WIRE_OK);
    assert(changed == encoded_n);
    memcpy(scratch, modified, changed);
  }
  assert(cnet_sctp_assoc_input(&b, 104u, scratch, encoded_n, confirm,
                               sizeof(confirm), &confirm_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 105u, confirm, confirm_n, scratch,
                               sizeof(scratch), &encoded_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);

  /* Known IPv4 address remains explicitly unsupported in this constrained
   * single-path profile, and cannot trigger Cookie signing. */
  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &init_n) == CNET_SCTP_ASSOC_OK);
  encoded_n = append_parameter_to_sctp_packet(init, init_n, 5u,
                   dummy_ipv4, sizeof(dummy_ipv4), encoded, sizeof(encoded));
  assert(cnet_sctp_assoc_input(&b, 2u, encoded, encoded_n, ack,
                               sizeof(ack), &ack_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(ack_n == 0u && cb.mac_calls == 0);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);

  /* Host Name Address in INIT: RFC 9260 requires ABORT with peer's
   * Initiate Tag, without ever constructing a signed State Cookie. */
  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &init_n) == CNET_SCTP_ASSOC_OK);
  encoded_n = append_parameter_to_sctp_packet(init, init_n, 11u,
                        dummy_ipv4, sizeof(dummy_ipv4), encoded, sizeof(encoded));
  assert(cnet_sctp_assoc_input(&b, 2u, encoded, encoded_n, ack,
                               15u, &ack_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(ack_n == 0u && cb.mac_calls == 0);
  assert(cnet_sctp_assoc_input(&b, 2u, encoded, encoded_n, ack,
                               sizeof(ack), &ack_n) == CNET_SCTP_ASSOC_OK);
  assert(ack_n == 16u && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_wire_decode(ack, ack_n, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.verification_tag == a.local_tag && header.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  assert(chunk.type == 6u && chunk.flags == 0u && chunk.value_size == 0u);
  assert(cnet_sctp_assoc_input(&a, 3u, ack, ack_n, echo,
                               sizeof(echo), &echo_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_FAILED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);

  /* Host Name Address in INIT ACK must also ABORT. Outbound capacity error
   * must leave T1-init and COOKIE-WAIT unchanged until ABORT is emitted. */
  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &init_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, init, init_n, ack,
                               sizeof(ack), &ack_n) == CNET_SCTP_ASSOC_OK);
  encoded_n = append_parameter_to_sctp_packet(ack, ack_n, 11u,
                        dummy_ipv4, sizeof(dummy_ipv4), encoded, sizeof(encoded));
  assert(cnet_sctp_assoc_input(&a, 3u, encoded, encoded_n, echo,
                               15u, &echo_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_COOKIE_WAIT);
  assert(cnet_sctp_assoc_input(&a, 3u, encoded, encoded_n, echo,
                               sizeof(echo), &echo_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_FAILED);
  assert(cnet_sctp_wire_decode(echo, echo_n, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.verification_tag == b.local_tag);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  assert(chunk.type == 6u && chunk.flags == 0u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* RFC 9260 5.2.4: a MAC-authenticated duplicate Cookie matching the
 * current established TCB can be ACKed after its original expiration. An
 * expired Cookie with a different Peer Tag/TSN may not replace that TCB. */
static void established_expired_duplicate_cookie(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  uint8_t init[1200], init_ack[1200], cookie_echo[1200], cookie_ack[1200];
  uint8_t reply[1200], forged[1200], modified_cookie[CNET_SCTP_ASSOC_COOKIE_BYTES];
  cnet_sctp_wire_chunk echo_chunk = {
    10u, 0u, modified_cookie, sizeof(modified_cookie)
  };
  size_t ni = 0u, na = 0u, ne = 0u, nk = 0u, nr = 0u, nf = 0u;
  uint32_t original_peer_tag;
  uint32_t original_tsn;
  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &ni) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, init, ni, init_ack,
                               sizeof(init_ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 3u, init_ack, na, cookie_echo,
                               sizeof(cookie_echo), &ne) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 4u, cookie_echo, ne, cookie_ack,
                               sizeof(cookie_ack), &nk) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 5u, cookie_ack, nk, reply,
                               sizeof(reply), &nr) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_ESTABLISHED);
  original_peer_tag = b.peer_tag;
  original_tsn = b.peer_initial_tsn;
  /* Issued at t=2, lifetime 500; elapsed > 500 on duplicate receipt. */
  assert(cnet_sctp_assoc_input(&b, 503u, cookie_echo, ne, reply,
                               sizeof(reply), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == nk && memcmp(reply, cookie_ack, nk) == 0);
  assert(b.peer_tag == original_peer_tag && b.peer_initial_tsn == original_tsn);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_ESTABLISHED);

  memcpy(modified_cookie, cookie_echo + 16u, sizeof(modified_cookie));
  modified_cookie[16u] ^= 1u; /* authenticated Cookie's peer Initiate Tag */
  assert(fake_mac(&cb, modified_cookie, 56u, modified_cookie + 56u) == 0);
  assert(cnet_sctp_wire_encode(5000u, 5001u, b.local_tag, &echo_chunk, 1u,
                                forged, sizeof(forged), &nf) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_assoc_input(&b, 504u, forged, nf, reply,
                               sizeof(reply), &nr) == CNET_SCTP_ASSOC_EXPIRED);
  assert(nr == 0u && b.peer_tag == original_peer_tag);

  modified_cookie[16u] ^= 1u;
  modified_cookie[24u] ^= 1u; /* authenticated Cookie's peer TSN */
  assert(fake_mac(&cb, modified_cookie, 56u, modified_cookie + 56u) == 0);
  assert(cnet_sctp_wire_encode(5000u, 5001u, b.local_tag, &echo_chunk, 1u,
                                forged, sizeof(forged), &nf) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_assoc_input(&b, 505u, forged, nf, reply,
                               sizeof(reply), &nr) == CNET_SCTP_ASSOC_EXPIRED);
  assert(nr == 0u && b.peer_initial_tsn == original_tsn);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_ESTABLISHED);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 506u, cookie_echo, ne, reply,
                               sizeof(reply), &nr) == CNET_SCTP_ASSOC_CLOSED_ERROR);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}


/* Both sides start at once. INIT in COOKIE-WAIT or COOKIE-ECHOED MUST
 * preserve the original local Initiate Tag, unsent packet/T1 and Owner. */
static void simultaneous_init_keeps_owner_and_t1(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  uint8_t a_init[1200], b_init[1200], a_ack[1200], b_ack[1200];
  uint8_t a_echo[1200], b_echo[1200], a_cookie_ack[1200], b_cookie_ack[1200];
  uint8_t scratch[1200];
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder decoder;
  cnet_sctp_wire_chunk chunk;
  size_t ai = 0u, bi = 0u, aa = 0u, ba = 0u;
  size_t ae = 0u, be = 0u, ak = 0u, bk = 0u, out_n = 0u;
  uint64_t deadline = 0u;
  uint32_t a_tag, b_tag;
  int has_deadline = 0;
  setup(&a, &b, &ca, &cb);
  a_tag = a.local_tag;
  b_tag = b.local_tag;
  assert(cnet_sctp_assoc_start(&a, 1u, a_init, sizeof(a_init), &ai) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_start(&b, 1u, b_init, sizeof(b_init), &bi) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, a_init, ai, b_ack, 15u, &ba) ==
         CNET_SCTP_ASSOC_CAPACITY);
  assert(ba == 0u && cb.mac_calls == 0);
  assert(b.local_tag == b_tag && b.state == CNET_SCTP_ASSOC_COOKIE_WAIT);
  assert(b.saved_size == bi && memcmp(b.saved_packet, b_init, bi) == 0);
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) == CNET_SCTP_ASSOC_OK);
  assert(has_deadline && deadline == 101u);
  assert(cnet_sctp_assoc_input(&b, 2u, a_init, ai,
                               b_ack, sizeof(b_ack), &ba) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 2u, b_init, bi,
                               a_ack, sizeof(a_ack), &aa) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_COOKIE_WAIT);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_COOKIE_WAIT);
  assert(a.local_tag == a_tag && b.local_tag == b_tag);
  assert(a.saved_size == ai && memcmp(a.saved_packet, a_init, ai) == 0);
  assert(b.saved_size == bi && memcmp(b.saved_packet, b_init, bi) == 0);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) == CNET_SCTP_ASSOC_OK);
  assert(has_deadline && deadline == 101u);
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) == CNET_SCTP_ASSOC_OK);
  assert(has_deadline && deadline == 101u);
  assert(cnet_sctp_wire_decode(a_ack, aa, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.verification_tag == b_tag);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  assert(chunk.type == 2u && chunk.value[0] == (uint8_t)(a_tag >> 24));
  assert(cnet_sctp_wire_decode(b_ack, ba, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(header.verification_tag == a_tag);
  assert(cnet_sctp_wire_next_chunk(&decoder, &chunk) == CNET_SCTP_WIRE_OK);
  assert(chunk.type == 2u && chunk.value[0] == (uint8_t)(b_tag >> 24));

  assert(cnet_sctp_assoc_input(&a, 3u, b_ack, ba,
                               a_echo, sizeof(a_echo), &ae) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 3u, a_ack, aa,
                               b_echo, sizeof(b_echo), &be) == CNET_SCTP_ASSOC_OK);
  assert(a.state == CNET_SCTP_ASSOC_COOKIE_ECHOED && a.peer_tag == b_tag);
  assert(b.state == CNET_SCTP_ASSOC_COOKIE_ECHOED && b.peer_tag == a_tag);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) == CNET_SCTP_ASSOC_OK);
  assert(has_deadline && deadline == 103u);
  /* Retransmitted simultaneous INIT after COOKIE-ECHOED does not reset T1,
   * replace the saved ECHO packet, switch the peer or change Initiate Tag. */
  assert(cnet_sctp_assoc_input(&a, 4u, b_init, bi,
                               scratch, sizeof(scratch), &out_n) == CNET_SCTP_ASSOC_OK);
  assert(out_n > 0u && a.state == CNET_SCTP_ASSOC_COOKIE_ECHOED);
  assert(a.local_tag == a_tag && a.peer_tag == b_tag);
  assert(a.saved_size == ae && memcmp(a.saved_packet, a_echo, ae) == 0);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) == CNET_SCTP_ASSOC_OK);
  assert(has_deadline && deadline == 103u);

  assert(cnet_sctp_assoc_input(&a, 5u, b_echo, be,
                               a_cookie_ack, sizeof(a_cookie_ack), &ak) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 5u, a_echo, ae,
                               b_cookie_ack, sizeof(b_cookie_ack), &bk) ==
         CNET_SCTP_ASSOC_OK);
  assert(ak == 16u && bk == 16u);
  assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
         b.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(a.peer_tag == b_tag && b.peer_tag == a_tag);
  assert(a.saved_size == 0u && b.saved_size == 0u);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) == CNET_SCTP_ASSOC_OK);
  assert(!has_deadline);
  assert(cnet_sctp_assoc_next_deadline(&b, &deadline, &has_deadline) == CNET_SCTP_ASSOC_OK);
  assert(!has_deadline);
  /* Late COOKIE ACK is a harmless no-op, not a new ULP terminal event. */
  assert(cnet_sctp_assoc_input(&a, 6u, b_cookie_ack, bk,
                               scratch, sizeof(scratch), &out_n) == CNET_SCTP_ASSOC_OK);
  assert(out_n == 0u && a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_input(&b, 6u, a_cookie_ack, ak,
                               scratch, sizeof(scratch), &out_n) == CNET_SCTP_ASSOC_OK);
  assert(out_n == 0u && b.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* COOKIE ECHO may win while the local INIT still waits for INIT ACK:
 * authenticated Table 12 case B is a bounded Owner-only commit. */
static void simultaneous_cookie_echo_wins_before_init_ack(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  uint8_t a_init[1200], b_init[1200], a_init_ack[1200];
  uint8_t b_cookie_echo[1200], a_cookie_ack[1200], output[1200];
  size_t ai = 0u, bi = 0u, na = 0u, ne = 0u, nk = 0u, no = 0u;
  uint64_t deadline = 0u;
  uint32_t a_tag, b_tag;
  int active = 0;
  setup(&a, &b, &ca, &cb);
  a_tag = a.local_tag;
  b_tag = b.local_tag;
  assert(cnet_sctp_assoc_start(&a, 1u, a_init, sizeof(a_init), &ai) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_start(&b, 1u, b_init, sizeof(b_init), &bi) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 2u, b_init, bi,
                               a_init_ack, sizeof(a_init_ack), &na) ==
         CNET_SCTP_ASSOC_OK);
  assert(a.peer_tag == 0u && a.state == CNET_SCTP_ASSOC_COOKIE_WAIT);
  assert(cnet_sctp_assoc_input(&b, 3u, a_init_ack, na,
                               b_cookie_echo, sizeof(b_cookie_echo), &ne) ==
         CNET_SCTP_ASSOC_OK);
  assert(b.state == CNET_SCTP_ASSOC_COOKIE_ECHOED);
  /* A has not received its INIT ACK. Failed ACK admission must not stop T1. */
  assert(cnet_sctp_assoc_input(&a, 4u, b_cookie_echo, ne,
                               a_cookie_ack, 15u, &nk) == CNET_SCTP_ASSOC_CAPACITY);
  assert(nk == 0u && a.state == CNET_SCTP_ASSOC_COOKIE_WAIT);
  assert(a.peer_tag == 0u && a.local_tag == a_tag);
  assert(a.saved_size == ai && memcmp(a.saved_packet, a_init, ai) == 0);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &active) == CNET_SCTP_ASSOC_OK);
  assert(active && deadline == 101u);
  assert(cnet_sctp_assoc_input(&a, 4u, b_cookie_echo, ne,
                               a_cookie_ack, sizeof(a_cookie_ack), &nk) ==
         CNET_SCTP_ASSOC_OK);
  assert(nk == 16u && a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(a.local_tag == a_tag && a.peer_tag == b_tag);
  assert(a.peer_initial_tsn == b.initial_tsn);
  assert(a.deadline_ms == 0u && a.saved_size == 0u && a.current_rto_ms == 0u);
  assert(cnet_sctp_assoc_input(&b, 5u, a_cookie_ack, nk,
                               output, sizeof(output), &no) == CNET_SCTP_ASSOC_OK);
  assert(no == 0u && b.state == CNET_SCTP_ASSOC_ESTABLISHED);
  /* Established-state INIT is deliberately NOT a live restart. */
  {
    int signed_count = ca.mac_calls;
    assert(cnet_sctp_assoc_input(&a, 6u, b_init, bi,
                                 output, sizeof(output), &no) == CNET_SCTP_ASSOC_PROTOCOL);
    assert(no == 0u && ca.mac_calls == signed_count);
    assert(a.peer_tag == b_tag && a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  }
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}


/* Even with a valid MAC from our own test-only key, a COOKIE ECHO claiming
 * a different peer tag cannot bypass COOKIE-ECHOED / Table 12 case B into
 * live peer replacement. That transaction needs a Generation lease gate. */
static void simultaneous_signed_peer_change_denied(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  uint8_t a_init[1200], b_init[1200], a_ack[1200], b_ack[1200];
  uint8_t a_echo[1200], b_echo[1200], forged[1200], output[1200];
  uint8_t cookie[CNET_SCTP_ASSOC_COOKIE_BYTES];
  cnet_sctp_wire_header header;
  cnet_sctp_wire_decoder decoder;
  cnet_sctp_wire_chunk parsed, counterfeit;
  size_t ai = 0u, bi = 0u, aa = 0u, ba = 0u;
  size_t ae = 0u, be = 0u, nf = 0u, no = 0u;
  uint64_t deadline = 0u;
  uint32_t old_peer;
  int has_deadline = 0;
  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 1u, a_init, sizeof(a_init), &ai) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_start(&b, 1u, b_init, sizeof(b_init), &bi) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 2u, b_init, bi,
                               a_ack, sizeof(a_ack), &aa) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, a_init, ai,
                               b_ack, sizeof(b_ack), &ba) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 3u, b_ack, ba,
                               a_echo, sizeof(a_echo), &ae) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 3u, a_ack, aa,
                               b_echo, sizeof(b_echo), &be) == CNET_SCTP_ASSOC_OK);
  old_peer = a.peer_tag;
  assert(old_peer == b.local_tag && a.state == CNET_SCTP_ASSOC_COOKIE_ECHOED);
  assert(cnet_sctp_wire_decode(b_echo, be, &header, &decoder) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_wire_next_chunk(&decoder, &parsed) == CNET_SCTP_WIRE_OK);
  assert(parsed.type == 10u && parsed.value_size == sizeof(cookie));
  memcpy(cookie, parsed.value, sizeof(cookie));
  cookie[16u] ^= 1u; /* a different peer tag within authenticated claims */
  assert(fake_mac(&ca, cookie, 56u, cookie + 56u) == 0);
  counterfeit.type = 10u;
  counterfeit.flags = 0u;
  counterfeit.value = cookie;
  counterfeit.value_size = sizeof(cookie);
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &counterfeit, 1u,
                               forged, sizeof(forged), &nf) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_assoc_input(&a, 4u, forged, nf,
                               output, sizeof(output), &no) == CNET_SCTP_ASSOC_AUTH);
  assert(no == 0u && a.state == CNET_SCTP_ASSOC_COOKIE_ECHOED);
  assert(a.peer_tag == old_peer && a.saved_size == ae);
  assert(memcmp(a.saved_packet, a_echo, ae) == 0);
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) == CNET_SCTP_ASSOC_OK);
  assert(has_deadline && deadline == 103u);
  /* The legitimate signed Cookie still succeeds after rejecting the rogue. */
  assert(cnet_sctp_assoc_input(&a, 4u, b_echo, be,
                               output, sizeof(output), &no) == CNET_SCTP_ASSOC_OK);
  assert(no == 16u && a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(a.peer_tag == old_peer && a.saved_size == 0u);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void malformed_shutdown_bundle_does_not_preempt_t5(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  cnet_sctp_wire_chunk chunks[2];
  uint8_t shutdown_value[4] = {0u, 0u, 0u, 1u};
  uint8_t malformed_sack[8] = {0};
  uint8_t packet[256], output[256];
  size_t packet_n = 0u, output_n = 0u;
  uint64_t prior_time;

  setup(&a, &b, &ca, &cb);
  /* Drive the Association into the deadline edge without depending on a
   * peer's unrelated shutdown choreography. The input packet still uses the
   * actual wire encoder and this Association's expected Verification Tag. */
  a.state = CNET_SCTP_ASSOC_SHUTDOWN_SENT;
  a.t5_shutdown_deadline_ms = 1000u;
  a.deadline_ms = 1000u;
  a.last_time_ms = 10u;
  prior_time = a.last_time_ms;
  chunks[0].type = 7u; /* SHUTDOWN */
  chunks[0].flags = 0u;
  chunks[0].value = shutdown_value;
  chunks[0].value_size = sizeof(shutdown_value);
  chunks[1].type = 3u; /* SACK with an invalid, undersized fixed header */
  chunks[1].flags = 0u;
  chunks[1].value = malformed_sack;
  chunks[1].value_size = sizeof(malformed_sack);
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks, 2u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);

  assert(cnet_sctp_assoc_input(&a, 1000u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(output_n == 0u);
  assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);
  assert(a.t5_shutdown_deadline_ms == 1000u);
  assert(a.last_time_ms == prior_time);

  /* A well-framed SHUTDOWN+SACK bundle rejected by the Association state
   * gate must not commit the provisional packet time either. */
  {
    uint8_t valid_sack[12] = {0};
    a.state = CNET_SCTP_ASSOC_CLOSED;
    a.t5_shutdown_deadline_ms = 0u;
    a.last_time_ms = 10u;
    prior_time = a.last_time_ms;
    chunks[1].value = valid_sack;
    chunks[1].value_size = sizeof(valid_sack);
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks, 2u,
        packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
    assert(cnet_sctp_assoc_input(&a, 11u, packet, packet_n,
        output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
    assert(output_n == 0u);
    assert(a.state == CNET_SCTP_ASSOC_CLOSED);
    assert(a.last_time_ms == prior_time);
  }

  /* DATA bundles also require syntax validation for every member before
   * T5. The first DATA is well formed; the trailing DATA is too short. */
  {
    uint8_t valid_data[13] = {0u};
    uint8_t malformed_data[12] = {0u};
    a.state = CNET_SCTP_ASSOC_SHUTDOWN_SENT;
    a.t5_shutdown_deadline_ms = 1000u;
    a.deadline_ms = 1000u;
    a.last_time_ms = prior_time;
    chunks[0].type = 0u;
    chunks[0].flags = 3u;
    chunks[0].value = valid_data;
    chunks[0].value_size = sizeof(valid_data);
    chunks[1].type = 0u;
    chunks[1].flags = 3u;
    chunks[1].value = malformed_data;
    chunks[1].value_size = sizeof(malformed_data);
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks, 2u,
        packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
    memset(output, 0xCC, sizeof(output));
    assert(cnet_sctp_assoc_input(&a, 1000u, packet, packet_n,
        output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
    assert(output_n == 0u && output[0] == 0xCCu);
    assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);
    assert(a.t5_shutdown_deadline_ms == 1000u);
    assert(a.last_time_ms == prior_time);
  }

  /* Length >= 12 is not enough: a declared gap block must fit in the SACK
   * body before the absolute T5 action can preempt this malformed packet. */
  {
    uint8_t malformed_full_header_sack[12] = {0u};
    a.state = CNET_SCTP_ASSOC_SHUTDOWN_SENT;
    a.t5_shutdown_deadline_ms = 1000u;
    a.deadline_ms = 1000u;
    a.last_time_ms = prior_time;
    /* Restore the first Chunk's live storage after the prior block-scoped
     * DATA test; never carry a pointer beyond that test's lifetime. */
    chunks[0].type = 7u;
    chunks[0].flags = 0u;
    chunks[0].value = shutdown_value;
    chunks[0].value_size = sizeof(shutdown_value);
    malformed_full_header_sack[9] = 1u; /* one block, no block bytes */
    chunks[1].value = malformed_full_header_sack;
    chunks[1].value_size = sizeof(malformed_full_header_sack);
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks, 2u,
        packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
    memset(output, 0xCC, sizeof(output));
    assert(cnet_sctp_assoc_input(&a, 1000u, packet, packet_n,
        output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
    assert(output_n == 0u && output[0] == 0xCCu);
    assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);
    assert(a.t5_shutdown_deadline_ms == 1000u);
    assert(a.last_time_ms == prior_time);
  }

  /* ERROR carries one or more causes in this profile. An empty ERROR
   * is malformed and cannot take the packet-path T5 transition. */
  {
    cnet_sctp_wire_chunk empty_error = {9u, 0u, NULL, 0u};
    a.state = CNET_SCTP_ASSOC_SHUTDOWN_SENT;
    a.t5_shutdown_deadline_ms = 1000u;
    a.deadline_ms = 1000u;
    a.last_time_ms = prior_time;
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &empty_error, 1u,
        packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
    memset(output, 0xCC, sizeof(output));
    assert(cnet_sctp_assoc_input(&a, 1000u, packet, packet_n,
        output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
    assert(output_n == 0u && output[0] == 0xCCu);
    assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);
    assert(a.t5_shutdown_deadline_ms == 1000u);
    assert(a.last_time_ms == prior_time);
  }

  /* A registered control that this profile cannot validate (an empty
   * HEARTBEAT lacks the required Heartbeat Info parameter) also cannot
   * trigger packet-path T5. The independent Owner advance path still owns
   * the absolute deadline. */
  {
    cnet_sctp_wire_chunk heartbeat = {4u, 0u, NULL, 0u};
    a.state = CNET_SCTP_ASSOC_SHUTDOWN_SENT;
    a.t5_shutdown_deadline_ms = 1000u;
    a.deadline_ms = 1000u;
    a.last_time_ms = prior_time;
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &heartbeat, 1u,
        packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
    memset(output, 0xCC, sizeof(output));
    assert(cnet_sctp_assoc_input(&a, 1000u, packet, packet_n,
        output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
    assert(output_n == 0u && output[0] == 0xCCu);
    assert(a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);
    assert(a.t5_shutdown_deadline_ms == 1000u);
    assert(a.last_time_ms == prior_time);
  }

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* A valid checksum and individually well-formed Chunk payloads must not
 * grant a mixed unknown/recognized packet access to the private M2 dispatch.
 * Reject every action-bit family and position before touching a TCB, including
 * at the absolute T5 edge (a malformed/unsupported bundle cannot abort it). */
static void assert_mixed_bundle_rejected_without_commit(cnet_sctp_assoc *a,
    const cnet_sctp_wire_chunk *chunks, size_t count, uint64_t now) {
  cnet_sctp_assoc before;
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  uint8_t packet[256] = {0}, output[256];
  size_t packet_n = 0u, output_n = 123u, i;

  assert(cnet_sctp_wire_encode(5001u, 5000u, a->local_tag, chunks, count,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  assert(cnet_sctp_wire_decode(packet, packet_n, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == count);
  memcpy(&before, a, sizeof(before));
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(a, now, packet, packet_n, output,
      sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(output_n == 0u);
  assert(memcmp(a, &before, sizeof(before)) == 0);
  for (i = 0u; i < sizeof(output); ++i)
    assert(output[i] == 0xCCu);
}

static void mixed_unknown_known_bundles_fail_closed(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  static const uint8_t unknown_value[] = {0xA5u, 0x5Au, 0xC3u};
  /* Unlike the former 12-byte DATA fixture, these DATA chunks contain the
   * required 12-byte fixed header plus nonempty user data. */
  static const uint8_t data_value[13] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0xA5u
  };
  static const uint8_t sack_value[12] = {0u};
  static const uint8_t shutdown_value[4] = {0u, 0u, 0u, 1u};
  static const uint8_t error_value[4] = {0u, 1u, 0u, 4u};
  static const uint8_t unknown_types[] = {0x20u, 0x61u, 0xA2u, 0xE3u};
  const cnet_sctp_wire_chunk known[] = {
    {0u, 3u, data_value, sizeof(data_value)},     /* valid DATA */
    {3u, 0u, sack_value, sizeof(sack_value)},     /* valid empty SACK */
    {7u, 0u, shutdown_value, sizeof(shutdown_value)},
    {8u, 0u, NULL, 0u},                            /* SHUTDOWN ACK */
    {11u, 0u, NULL, 0u},                           /* COOKIE ACK */
    {9u, 0u, error_value, sizeof(error_value)},   /* valid ERROR cause */
    {6u, 0u, NULL, 0u}                             /* plain ABORT */
  };
  size_t due, known_i, type_i, unknown_pos, group_i;
  setup(&a, &b, &ca, &cb);
  a.peer_tag = b.local_tag;

  for (due = 0u; due < 2u; ++due) {
    /* Exercise ordinary established traffic and the T5 shutdown edge.
     * Each rejection must leave even the absolute T5 deadline unchanged. */
    a.state = due ? CNET_SCTP_ASSOC_SHUTDOWN_SENT :
        CNET_SCTP_ASSOC_ESTABLISHED;
    a.last_time_ms = 10u;
    a.t5_shutdown_deadline_ms = due ? 1000u : 0u;
    a.deadline_ms = due ? 1000u : 0u;
    for (known_i = 0u; known_i < sizeof(known) / sizeof(known[0]);
         ++known_i) {
      for (type_i = 0u; type_i < sizeof(unknown_types); ++type_i) {
        cnet_sctp_wire_chunk unknown = {
          unknown_types[type_i], 0x5Au, unknown_value,
          sizeof(unknown_value)
        };
        for (unknown_pos = 0u; unknown_pos < 2u; ++unknown_pos) {
          cnet_sctp_wire_chunk chunks[2];
          chunks[unknown_pos] = unknown;
          chunks[1u - unknown_pos] = known[known_i];
          assert_mixed_bundle_rejected_without_commit(&a, chunks, 2u,
              due ? 1000u : 11u);
        }
      }
    }

    /* Cover unknown Chunk insertion before, between or after otherwise
     * supported DATA+DATA and leading-SACK+DATA sequences. */
    for (group_i = 0u; group_i < 2u; ++group_i) {
      for (type_i = 0u; type_i < sizeof(unknown_types); ++type_i) {
        for (unknown_pos = 0u; unknown_pos < 3u; ++unknown_pos) {
          cnet_sctp_wire_chunk chunks[3];
          cnet_sctp_wire_chunk first = group_i == 0u ? known[0] : known[1];
          cnet_sctp_wire_chunk second = known[0];
          cnet_sctp_wire_chunk unknown = {
            unknown_types[type_i], 0x5Au, unknown_value,
            sizeof(unknown_value)
          };
          size_t pos, next_known = 0u;
          for (pos = 0u; pos < 3u; ++pos) {
            if (pos == unknown_pos) {
              chunks[pos] = unknown;
            } else {
              chunks[pos] = next_known++ == 0u ? first : second;
            }
          }
          assert_mixed_bundle_rejected_without_commit(&a, chunks, 3u,
              due ? 1000u : 11u);
        }
      }
    }
  }
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void unknown_chunks_do_not_preempt_t5(void) {
  size_t case_i;
  for (case_i = 0u; case_i < 2u; ++case_i) {
    cnet_sctp_assoc a, b;
    test_context ca, cb;
    static const uint8_t value[] = {0xA5u, 0x5Au, 0xC3u};
    cnet_sctp_wire_chunk chunks[2];
    uint8_t packet[128] = {0}, output[128];
    cnet_sctp_wire_header response_header = {0};
    cnet_sctp_wire_decoder response_decoder = {0};
    cnet_sctp_wire_chunk response_chunk = {0};
    size_t packet_n = 0u, output_n = 0u;
    size_t count = case_i == 0u ? 1u : 2u;
    setup(&a, &b, &ca, &cb);
    a.state = CNET_SCTP_ASSOC_SHUTDOWN_SENT;
    a.peer_tag = b.local_tag;
    a.t5_shutdown_deadline_ms = 1000u;
    a.deadline_ms = 1000u;
    a.last_time_ms = 10u;
    chunks[0].type = case_i == 0u ? 0x20u : 0xA2u;
    chunks[0].flags = 0u;
    chunks[0].value = value;
    chunks[0].value_size = sizeof(value);
    chunks[1].type = 0xE3u;
    chunks[1].flags = 0u;
    chunks[1].value = value;
    chunks[1].value_size = sizeof(value);
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks, count,
        packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
    memset(output, 0xCC, sizeof(output));
    /* At the absolute T5 deadline, neither single nor multi unknown-Chunk
     * fast paths may postpone publication of the required ABORT. */
    assert(cnet_sctp_assoc_input(&a, 1000u, packet, packet_n,
        output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_OK);
    assert(output_n >= 16u);
    assert(a.state == CNET_SCTP_ASSOC_FAILED);
    assert(a.t5_shutdown_deadline_ms == 0u);
    assert(a.last_time_ms == 1000u);
    assert(cnet_sctp_wire_decode(output, output_n, &response_header,
        &response_decoder) == CNET_SCTP_WIRE_OK);
    assert(response_header.chunk_count == 1u);
    assert(cnet_sctp_wire_next_chunk(&response_decoder, &response_chunk) ==
        CNET_SCTP_WIRE_OK);
    assert(response_chunk.type == 6u && response_chunk.value_size == 0u);
    assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
  }
}

/* Unknown-only packets cannot be admitted as normal Association DATA
 * before an authenticated COOKIE exchange. In particular, CLOSED and
 * COOKIE-WAIT have no peer Verification Tag to use in an ERROR response.
 * COOKIE-ECHOED knows the peer tag but still has not completed admission.
 * This is the private fail-closed subset, not complete RFC 9260 OOTB. */
static void unknown_pre_cookie_packets_fail_closed(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  static const uint8_t value[] = {0xA5u};
  static const uint8_t types[] = {0x20u, 0x61u, 0xA2u, 0xE3u};
  static const cnet_sctp_assoc_state states[] = {
    CNET_SCTP_ASSOC_CLOSED,
    CNET_SCTP_ASSOC_COOKIE_WAIT,
    CNET_SCTP_ASSOC_COOKIE_ECHOED
  };
  size_t state_i, type_i, count;
  setup(&a, &b, &ca, &cb);
  for (state_i = 0u; state_i < sizeof(states) / sizeof(states[0]);
       ++state_i) {
    for (type_i = 0u; type_i < sizeof(types); ++type_i) {
      for (count = 1u; count <= 2u; ++count) {
        cnet_sctp_wire_chunk chunks[2];
        uint8_t packet[64] = {0}, output[64];
        cnet_sctp_assoc before;
        size_t packet_n = 0u, output_n = 99u, i;
        a.state = states[state_i];
        a.peer_tag = state_i == 2u ? b.local_tag : 0u;
        chunks[0].type = types[type_i];
        chunks[0].flags = 0x5Au;
        chunks[0].value = value;
        chunks[0].value_size = sizeof(value);
        chunks[1] = chunks[0];
        assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag,
            chunks, count, packet, sizeof(packet), &packet_n) ==
            CNET_SCTP_WIRE_OK);
        memcpy(&before, &a, sizeof(before));
        memset(output, 0xCC, sizeof(output));
        assert(cnet_sctp_assoc_input(&a, a.last_time_ms + 1u,
            packet, packet_n, output, sizeof(output), &output_n) ==
            CNET_SCTP_ASSOC_PROTOCOL);
        assert(output_n == 0u && memcmp(&a, &before, sizeof(before)) == 0);
        for (i = 0u; i < sizeof(output); ++i)
          assert(output[i] == 0xCCu);
      }
    }
  }
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void unknown_chunk_actions_are_bounded_and_exact(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  static const uint8_t value[] = {0xA5u, 0x5Au, 0xC3u};
  const uint8_t types[] = {0x20u, 0x61u, 0xA2u, 0xE3u};
  size_t i;
  setup(&a, &b, &ca, &cb);
  /* Unknown-chunk actions are association traffic: give the response path
   * a valid peer Verification Tag instead of testing an unstarted endpoint. */
  a.state = CNET_SCTP_ASSOC_ESTABLISHED;
  a.peer_tag = b.local_tag;
  for (i = 0u; i < sizeof(types); ++i) {
    cnet_sctp_wire_chunk chunk = {types[i], 0x5Au, value, sizeof(value)};
    cnet_sctp_wire_header h = {0};
    cnet_sctp_wire_decoder d = {0};
    cnet_sctp_wire_chunk response_chunk = {0};
    uint8_t packet[64] = {0}, output[128], tiny[1] = {0xCCu};
    size_t packet_n = 0u, output_n = 0u;
    uint64_t prior_time = a.last_time_ms;
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &chunk, 1u,
        packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
    memset(output, 0xCC, sizeof(output));
    if (types[i] == 0x61u || types[i] == 0xE3u) {
      assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
          tiny, sizeof(tiny), &output_n) == CNET_SCTP_ASSOC_CAPACITY);
      assert(output_n == 0u && a.last_time_ms == prior_time);
      assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED && tiny[0] == 0xCCu);
    }
    assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
        output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_OK);
    assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED);
    assert(a.last_time_ms == prior_time + 1u);
    if (types[i] == 0x20u || types[i] == 0xA2u) {
      assert(output_n == 0u && output[0] == 0xCCu);
    } else {
      assert(output_n >= 24u);
      assert(cnet_sctp_wire_decode(output, output_n, &h, &d) == CNET_SCTP_WIRE_OK);
      assert(h.chunk_count == 1u);
      assert(cnet_sctp_wire_next_chunk(&d, &response_chunk) == CNET_SCTP_WIRE_OK);
      assert(response_chunk.type == 9u && response_chunk.flags == 0u);
      assert(response_chunk.value_size == sizeof(value) + 8u);
      assert(response_chunk.value[0] == 0u && response_chunk.value[1] == 6u);
      assert(response_chunk.value[2] == 0u &&
             response_chunk.value[3] == (uint8_t)(sizeof(value) + 8u));
      assert(memcmp(response_chunk.value + 4u, packet + 12u,
                    sizeof(value) + 4u) == 0);
    }
  }
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* The public Wire module can parse packets far above the private
 * Association's 1200-byte response ceiling. A single unknown REPORT Chunk
 * must not bypass that ceiling when its caller offers an oversized buffer.
 * Cause length = 8 + payload, ERROR size = 16 + padded cause length. */
static void unknown_single_report_packet_ceiling_is_transactional(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  uint8_t value[1177], packet[1200], output[2048];
  cnet_sctp_wire_chunk unknown = {0xE3u, 0x5Au, value, 1176u};
  cnet_sctp_wire_header response_header = {0};
  cnet_sctp_wire_decoder response_decoder = {0};
  cnet_sctp_wire_chunk response_chunk = {0};
  size_t packet_n = 0u, output_n = 123u, i;
  uint64_t previous_time;

  for (i = 0u; i < sizeof(value); ++i)
    value[i] = (uint8_t)(i ^ 0x5Au);
  setup(&a, &b, &ca, &cb);
  a.state = CNET_SCTP_ASSOC_ESTABLISHED;
  a.peer_tag = b.local_tag;
  previous_time = a.last_time_ms;
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &unknown, 1u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  assert(packet_n == 1192u);
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, previous_time + 1u, packet, packet_n,
      output, 1199u, &output_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(output_n == 0u && output[0] == 0xCCu &&
      output[1198] == 0xCCu && output[1199] == 0xCCu);
  assert(a.last_time_ms == previous_time &&
      a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_input(&a, previous_time + 1u, packet, packet_n,
      output, 1200u, &output_n) == CNET_SCTP_ASSOC_OK);
  assert(output_n == 1200u && output[1200] == 0xCCu);
  assert(a.last_time_ms == previous_time + 1u &&
      a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_wire_decode(output, output_n, &response_header,
      &response_decoder) == CNET_SCTP_WIRE_OK);
  assert(response_header.chunk_count == 1u &&
      response_header.verification_tag == a.peer_tag);
  assert(cnet_sctp_wire_next_chunk(&response_decoder, &response_chunk) ==
      CNET_SCTP_WIRE_OK);
  assert(response_chunk.type == 9u && response_chunk.flags == 0u &&
      response_chunk.value_size == 1184u);
  assert(response_chunk.value[0] == 0u &&
      response_chunk.value[1] == 6u &&
      response_chunk.value[2] == 0x04u &&
      response_chunk.value[3] == 0xA0u);
  assert(memcmp(response_chunk.value + 4u, packet + 12u,
      1180u) == 0);

  /* Just one payload byte more would require a 1204-byte ERROR. It must
   * fail even with a 2048-byte destination, without output or clock commit. */
  unknown.value_size = 1177u;
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &unknown, 1u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  assert(packet_n == 1196u);
  previous_time = a.last_time_ms;
  memset(output, 0xCC, sizeof(output));
  output_n = 123u;
  assert(cnet_sctp_assoc_input(&a, previous_time + 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(output_n == 0u && output[0] == 0xCCu &&
      output[1199] == 0xCCu && output[sizeof(output) - 1u] == 0xCCu);
  assert(a.last_time_ms == previous_time &&
      a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void unknown_multi_chunk_actions_are_transactional(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  static const uint8_t value[] = {0xA5u, 0x5Au, 0xC3u};
  static const uint8_t sequences[][3] = {
    {0x20u, 0xA2u, 0xE3u}, /* stop/discard at first Chunk */
    {0x61u, 0xA2u, 0xE3u}, /* stop/report at first Chunk */
    {0xA2u, 0x20u, 0xE3u}, /* skip, then stop/discard */
    {0xA2u, 0x61u, 0xE3u}, /* skip, then stop/report */
    {0xA2u, 0xE3u, 0xA2u}, /* skip/report in the middle */
    {0xE3u, 0x20u, 0xA2u}, /* preserve earlier report before stop/discard */
    {0xE3u, 0x61u, 0xA2u}, /* preserve earlier report and report stop/report */
    {0xE3u, 0xE3u, 0xA2u}, /* collect two reports */
    {0xA2u, 0xA2u, 0xA2u}  /* skip all */
  };
  static const uint8_t report_masks[] = {
    0u, 1u, 0u, 2u, 2u, 1u, 3u, 3u, 0u
  };
  size_t case_i;
  setup(&a, &b, &ca, &cb);
  a.state = CNET_SCTP_ASSOC_ESTABLISHED;
  a.peer_tag = b.local_tag;
  for (case_i = 0u; case_i < sizeof(sequences) / sizeof(sequences[0]);
       ++case_i) {
    cnet_sctp_wire_chunk chunks[3];
    uint8_t packet[128] = {0}, output[256], tiny[1] = {0xCCu};
    cnet_sctp_wire_header header = {0};
    cnet_sctp_wire_decoder decoder = {0};
    cnet_sctp_wire_chunk response_chunk = {0};
    size_t packet_n = 0u, output_n = 0u, report_count = 0u, i;
    uint64_t prior_time = a.last_time_ms;
    uint64_t now = prior_time + 1u;
    for (i = 0u; i < 3u; ++i) {
      chunks[i].type = sequences[case_i][i];
      chunks[i].flags = 0x5Au;
      chunks[i].value = value;
      chunks[i].value_size = sizeof(value);
    }
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks, 3u,
        packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
    memset(output, 0xCC, sizeof(output));
    if (case_i == 6u) {
      /* An ERROR with two causes cannot fit: no output byte, TCB field or
       * monotonic watermark may be committed; retry at the same time works. */
      assert(cnet_sctp_assoc_input(&a, now, packet, packet_n,
          tiny, sizeof(tiny), &output_n) == CNET_SCTP_ASSOC_CAPACITY);
      assert(output_n == 0u && tiny[0] == 0xCCu);
      assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED);
      assert(a.last_time_ms == prior_time);
    }
    assert(cnet_sctp_assoc_input(&a, now, packet, packet_n,
        output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_OK);
    assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED);
    assert(a.last_time_ms == now);
    for (i = 0u; i < 3u; ++i)
      if ((report_masks[case_i] & (uint8_t)(1u << i)) != 0u)
        ++report_count;
    if (report_count == 0u) {
      assert(output_n == 0u && output[0] == 0xCCu);
      continue;
    }
    assert(cnet_sctp_wire_decode(output, output_n, &header, &decoder) ==
        CNET_SCTP_WIRE_OK);
    assert(header.chunk_count == 1u);
    assert(cnet_sctp_wire_next_chunk(&decoder, &response_chunk) ==
        CNET_SCTP_WIRE_OK);
    assert(response_chunk.type == 9u && response_chunk.flags == 0u);
    assert(response_chunk.value_size == report_count * 12u);
    {
      size_t cause_pos = 0u;
      for (i = 0u; i < 3u; ++i) {
        const uint8_t *cause;
        if ((report_masks[case_i] & (uint8_t)(1u << i)) == 0u) continue;
        cause = response_chunk.value + cause_pos;
        assert(cause[0] == 0u && cause[1] == 6u);
        assert(cause[2] == 0u && cause[3] == 11u);
        assert(cause[4] == sequences[case_i][i]);
        assert(cause[5] == 0x5Au);
        assert(cause[6] == 0u && cause[7] == 7u);
        assert(memcmp(cause + 8u, value, sizeof(value)) == 0);
        assert(cause[11] == 0u); /* cause element padding */
        cause_pos += 12u;
      }
      assert(cause_pos == response_chunk.value_size);
    }
  }
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void unknown_report_variable_lengths_preserve_tlv_and_padding(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  static const uint8_t p0[] = {0xA1u};
  static const uint8_t p1[] = {0xB1u, 0xB2u, 0xB3u, 0xB4u};
  static const uint8_t p2[] = {0xC1u, 0xC2u, 0xC3u, 0xC4u, 0xC5u};
  const uint8_t *payloads[] = {p0, p1, p2};
  const size_t payload_sizes[] = {sizeof(p0), sizeof(p1), sizeof(p2)};
  const uint8_t types[] = {0xE3u, 0xE3u, 0x61u};
  const uint8_t flags[] = {0x11u, 0x22u, 0x33u};
  const size_t cause_sizes[] = {9u, 12u, 13u};
  const size_t padded_sizes[] = {12u, 12u, 16u};
  cnet_sctp_wire_chunk chunks[3];
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk response = {0};
  uint8_t packet[128] = {0}, output[256];
  size_t packet_n = 0u, output_n = 0u, i, cause_pos = 0u;
  uint64_t now;

  setup(&a, &b, &ca, &cb);
  a.state = CNET_SCTP_ASSOC_ESTABLISHED;
  a.peer_tag = b.local_tag;
  now = a.last_time_ms + 1u;
  for (i = 0u; i < 3u; ++i) {
    chunks[i].type = types[i];
    chunks[i].flags = flags[i];
    chunks[i].value = payloads[i];
    chunks[i].value_size = payload_sizes[i];
  }
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks, 3u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, now, packet, packet_n, output,
      sizeof(output), &output_n) == CNET_SCTP_ASSOC_OK);
  assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED && a.last_time_ms == now);
  assert(cnet_sctp_wire_decode(output, output_n, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u);
  assert(cnet_sctp_wire_next_chunk(&decoder, &response) == CNET_SCTP_WIRE_OK);
  assert(response.type == 9u && response.flags == 0u);
  assert(response.value_size == padded_sizes[0] + padded_sizes[1] +
      padded_sizes[2]);

  /* Exercise ERROR-cause lengths 9, 12 and 13: the first needs three
   * padding bytes, the second none, and the last is padded to 16 bytes. */
  for (i = 0u; i < 3u; ++i) {
    const uint8_t *cause = response.value + cause_pos;
    size_t raw_chunk_size = 4u + payload_sizes[i];
    size_t j;
    assert(cause[0] == 0u && cause[1] == 6u);
    assert(cause[2] == 0u && cause[3] == cause_sizes[i]);
    assert(cause[4] == types[i] && cause[5] == flags[i]);
    assert(cause[6] == 0u && cause[7] == raw_chunk_size);
    assert(memcmp(cause + 8u, payloads[i], payload_sizes[i]) == 0);
    for (j = cause_sizes[i]; j < padded_sizes[i]; ++j)
      assert(cause[j] == 0u);
    cause_pos += padded_sizes[i];
  }
  assert(cause_pos == response.value_size);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* Wire accepts at most 128 Chunks in this private bounded profile.
 * An all-unknown, all-SKIP packet must traverse complete Association
 * preflight without allocating a report or touching the DATA engine.
 * An independently appended 129th Chunk with a valid CRC fails before
 * dispatch, with no output or time/state commit. */
static void max_unknown_skip_chunk_count_is_transactional(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  cnet_sctp_wire_chunk chunks[CNET_SCTP_WIRE_MAX_CHUNKS];
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  uint8_t packet[12u + 4u * (CNET_SCTP_WIRE_MAX_CHUNKS + 1u)];
  uint8_t output[64];
  size_t packet_n = 0u, output_n = 17u, i;
  uint64_t previous_time;
  uint32_t checksum;

  setup(&a, &b, &ca, &cb);
  a.state = CNET_SCTP_ASSOC_ESTABLISHED;
  a.peer_tag = b.local_tag;
  for (i = 0u; i < CNET_SCTP_WIRE_MAX_CHUNKS; ++i) {
    chunks[i].type = 0xA2u;
    chunks[i].flags = 0x5Au;
    chunks[i].value = NULL;
    chunks[i].value_size = 0u;
  }
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks,
      CNET_SCTP_WIRE_MAX_CHUNKS, packet, sizeof(packet), &packet_n) ==
      CNET_SCTP_WIRE_OK);
  assert(packet_n == 12u + 4u * CNET_SCTP_WIRE_MAX_CHUNKS);
  memset(output, 0xCC, sizeof(output));
  previous_time = a.last_time_ms;
  assert(cnet_sctp_assoc_input(&a, previous_time + 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_OK);
  assert(output_n == 0u && output[0] == 0xCCu &&
      output[sizeof(output) - 1u] == 0xCCu);
  assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
      a.last_time_ms == previous_time + 1u);

  packet[packet_n] = 0xA2u;
  packet[packet_n + 1u] = 0x5Au;
  packet[packet_n + 2u] = 0u;
  packet[packet_n + 3u] = 4u;
  memset(packet + 8u, 0, 4u);
  checksum = cnet_sctp_wire_crc32c(packet, packet_n + 4u);
  packet[8] = (uint8_t)checksum;
  packet[9] = (uint8_t)(checksum >> 8);
  packet[10] = (uint8_t)(checksum >> 16);
  packet[11] = (uint8_t)(checksum >> 24);
  assert(cnet_sctp_wire_decode(packet, packet_n + 4u,
      &header, &decoder) == CNET_SCTP_WIRE_CAPACITY);

  previous_time = a.last_time_ms;
  output_n = 17u;
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, previous_time + 1u, packet,
      packet_n + 4u, output, sizeof(output), &output_n) ==
      CNET_SCTP_ASSOC_PROTOCOL);
  assert(output_n == 0u && output[0] == 0xCCu &&
      output[sizeof(output) - 1u] == 0xCCu);
  assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
      a.last_time_ms == previous_time);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

/* The Wire limit and private ERROR-cause limit are independent.
 * Exactly 128 four-byte unknown SKIP/REPORT chunks produce 128 complete
 * eight-byte Unrecognized-Chunk causes: one bounded 1040-byte ERROR.
 * A one-byte undersized caller buffer must publish nothing, and retry at
 * the same Owner time must produce an exact wire response. */
/* A 1200-byte unknown SKIP packet fits the private Association. The
 * public Wire parser also accepts a CRC-correct 1204-byte packet, but
 * Association input MUST reject it before unknown actions or time commit.
 * The inbound bound must not depend on whether a REPORT is requested. */
static void oversized_unknown_skip_packet_is_not_admitted(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca, cb;
  uint8_t value[1188], packet[1204], output[64];
  cnet_sctp_wire_header h = {0};
  cnet_sctp_wire_decoder d = {0};
  cnet_sctp_wire_chunk unknown = {0xA2u, 0x5Au, value, 1184u};
  size_t packet_n = 0u, output_n = 123u, i;

  for (i = 0u; i < sizeof(value); ++i) value[i] = (uint8_t)i;
  setup(&a, &b, &ca, &cb);
  a.state = CNET_SCTP_ASSOC_ESTABLISHED;
  a.peer_tag = b.local_tag;

  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &unknown,
      1u, packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  assert(packet_n == CNET_SCTP_ASSOC_MAX_PACKET);
  assert(cnet_sctp_wire_decode(packet, packet_n, &h, &d) == CNET_SCTP_WIRE_OK);
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, a.last_time_ms + 1u,
      packet, packet_n, output, sizeof(output), &output_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(output_n == 0u && a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
      a.last_time_ms == 1u);
  for (i = 0u; i < sizeof(output); ++i) assert(output[i] == 0xCCu);

  unknown.value_size = 1188u;
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &unknown,
      1u, packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  assert(packet_n == CNET_SCTP_ASSOC_MAX_PACKET + 4u);
  assert(cnet_sctp_wire_decode(packet, packet_n, &h, &d) == CNET_SCTP_WIRE_OK);

  memcpy(&before, &a, sizeof(before));
  memset(output, 0xCC, sizeof(output));
  output_n = 123u;
  assert(cnet_sctp_assoc_input(&a, a.last_time_ms + 1u,
      packet, packet_n, output, sizeof(output), &output_n) ==
      CNET_SCTP_ASSOC_CAPACITY);
  assert(output_n == 0u && memcmp(&a, &before, sizeof(before)) == 0);
  for (i = 0u; i < sizeof(output); ++i) assert(output[i] == 0xCCu);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void max_unknown_report_chunk_count_is_transactional(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  cnet_sctp_wire_chunk chunks[CNET_SCTP_WIRE_MAX_CHUNKS];
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk response_chunk = {0};
  uint8_t packet[12u + 4u * CNET_SCTP_WIRE_MAX_CHUNKS];
  uint8_t output[1041u];
  size_t packet_n = 0u, output_n = 17u, i, j;
  uint64_t prior_time;
  setup(&a, &b, &ca, &cb);
  a.state = CNET_SCTP_ASSOC_ESTABLISHED;
  a.peer_tag = b.local_tag;
  for (i = 0u; i < CNET_SCTP_WIRE_MAX_CHUNKS; ++i) {
    chunks[i].type = 0xE3u; /* RFC 9260 action=11: skip/report */
    chunks[i].flags = 0x5Au;
    chunks[i].value = NULL;
    chunks[i].value_size = 0u;
  }
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks,
      CNET_SCTP_WIRE_MAX_CHUNKS, packet, sizeof(packet), &packet_n) ==
      CNET_SCTP_WIRE_OK);
  assert(packet_n == sizeof(packet));
  prior_time = a.last_time_ms;
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
      output, 1039u, &output_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(output_n == 0u && a.last_time_ms == prior_time &&
      a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  for (j = 0u; j < sizeof(output); ++j)
    assert(output[j] == 0xCCu);
  assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
      output, 1040u, &output_n) == CNET_SCTP_ASSOC_OK);
  assert(output_n == 1040u && output[1040] == 0xCCu);
  assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
      a.last_time_ms == prior_time + 1u);
  assert(cnet_sctp_wire_decode(output, output_n, &header, &decoder) ==
      CNET_SCTP_WIRE_OK);
  assert(header.chunk_count == 1u &&
      header.verification_tag == a.peer_tag);
  assert(cnet_sctp_wire_next_chunk(&decoder, &response_chunk) ==
      CNET_SCTP_WIRE_OK);
  assert(response_chunk.type == 9u && response_chunk.flags == 0u &&
      response_chunk.value_size == 8u * CNET_SCTP_WIRE_MAX_CHUNKS);
  for (i = 0u; i < CNET_SCTP_WIRE_MAX_CHUNKS; ++i) {
    const uint8_t *cause = response_chunk.value + 8u * i;
    assert(cause[0] == 0u && cause[1] == 6u &&
        cause[2] == 0u && cause[3] == 8u);
    assert(cause[4] == 0xE3u && cause[5] == 0x5Au &&
        cause[6] == 0u && cause[7] == 4u);
  }
  assert(cnet_sctp_wire_next_chunk(&decoder, &response_chunk) ==
      CNET_SCTP_WIRE_END);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void unknown_report_dense_capacity_boundary(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  static const uint8_t value[] = {0xA1u, 0xB2u, 0xC3u, 0xD4u};
  static const size_t counts[] = {98u, 99u, 100u, 101u};
  cnet_sctp_wire_chunk chunks[101];
  uint8_t packet[1200], output[1201];
  size_t case_i, i;

  setup(&a, &b, &ca, &cb);
  a.state = CNET_SCTP_ASSOC_ESTABLISHED;
  a.peer_tag = b.local_tag;
  for (i = 0u; i < 101u; ++i) {
    chunks[i].type = 0xE3u; /* unknown: skip and report */
    chunks[i].flags = 0x5Au;
    chunks[i].value = value;
    chunks[i].value_size = sizeof(value);
  }
  for (case_i = 0u; case_i < sizeof(counts) / sizeof(counts[0]);
       ++case_i) {
    cnet_sctp_wire_header header = {0};
    cnet_sctp_wire_decoder decoder = {0};
    cnet_sctp_wire_chunk response = {0};
    const size_t count = counts[case_i];
    const uint64_t prior_time = a.last_time_ms;
    const uint64_t now = prior_time + 1u;
    size_t packet_n = 0u, output_n = 0u;

    /* 101 valid input Chunks occupy only 820 bytes; their report causes
     * exceed the bounded 1200-byte ERROR staging buffer. */
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, chunks,
        count, packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
    assert(packet_n == 12u + count * 8u);
    memset(output, 0xCC, sizeof(output));

    if (count == 98u) {
      const size_t response_size = 12u + 4u + 98u * 12u;
      /* The generated ERROR is 1192 bytes: reject 1191 without touching
       * output or association, then admit exactly 1192 on the same clock. */
      assert(response_size == 1192u);
      assert(cnet_sctp_assoc_input(&a, now, packet, packet_n,
          output, response_size - 1u, &output_n) == CNET_SCTP_ASSOC_CAPACITY);
      assert(output_n == 0u && output[0] == 0xCCu &&
          output[response_size - 2u] == 0xCCu);
      assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
          a.last_time_ms == prior_time);
      assert(cnet_sctp_assoc_input(&a, now, packet, packet_n,
          output, response_size, &output_n) == CNET_SCTP_ASSOC_OK);
      assert(output_n == response_size && output[response_size] == 0xCCu);
      assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
          a.last_time_ms == now);
      assert(cnet_sctp_wire_decode(output, output_n, &header, &decoder) ==
          CNET_SCTP_WIRE_OK);
      assert(header.chunk_count == 1u);
      assert(cnet_sctp_wire_next_chunk(&decoder, &response) ==
          CNET_SCTP_WIRE_OK);
      assert(response.type == 9u && response.flags == 0u);
      assert(response.value_size == 98u * 12u);
      for (i = 0u; i < 98u; ++i) {
        const uint8_t *cause = response.value + i * 12u;
        assert(cause[0] == 0u && cause[1] == 6u &&
            cause[2] == 0u && cause[3] == 12u);
        assert(cause[4] == 0xE3u && cause[5] == 0x5Au &&
            cause[6] == 0u && cause[7] == 8u);
        assert(memcmp(cause + 8u, value, sizeof(value)) == 0);
      }
    } else {
      /* 99/100 reports fit the causes array but exceed the response
       * packet; 101 exceeds the causes array itself. All fail closed. */
      assert(cnet_sctp_assoc_input(&a, now, packet, packet_n,
          output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_CAPACITY);
      assert(output_n == 0u && output[0] == 0xCCu &&
          output[sizeof(output) - 1u] == 0xCCu);
      assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
          a.last_time_ms == prior_time);
    }
  }

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void error_and_abort_never_create_response_loops(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  static const uint8_t error_value[] = {0u, 1u, 0u, 4u};
  cnet_sctp_wire_chunk chunk = {9u, 0u, error_value, sizeof(error_value)};
  uint8_t packet[64] = {0}, output[64];
  size_t packet_n = 0u, output_n = 0u;
  uint64_t prior_time;
  static const uint8_t data_value[12] = {0};
  setup(&a, &b, &ca, &cb);
  prior_time = a.last_time_ms;

  /* A structurally valid DATA chunk cannot be dispatched before COOKIE
   * admission. State rejection must not commit the provisional watermark. */
  chunk.type = 0u;
  chunk.value = data_value;
  chunk.value_size = sizeof(data_value);
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &chunk, 1u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(output_n == 0u && output[0] == 0xCCu);
  assert(a.state == CNET_SCTP_ASSOC_CLOSED);
  assert(a.last_time_ms == prior_time);

  /* SHUTDOWN COMPLETE is valid only in SHUTDOWN-ACK-SENT. A rejected
   * control in any other state must not commit the provisional watermark. */
  chunk.type = 14u;
  chunk.value = NULL;
  chunk.value_size = 0u;
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &chunk, 1u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(output_n == 0u && output[0] == 0xCCu);
  assert(a.state == CNET_SCTP_ASSOC_CLOSED);
  assert(a.last_time_ms == prior_time);

  /* Wrong-state COOKIE ACK reaches a handler after provisional dispatch
   * time is set; rejection must restore the original watermark. */
  chunk.type = 11u;
  chunk.value = NULL;
  chunk.value_size = 0u;
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &chunk, 1u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(output_n == 0u && output[0] == 0xCCu);
  assert(a.state == CNET_SCTP_ASSOC_CLOSED);
  assert(a.last_time_ms == prior_time);

  a.state = CNET_SCTP_ASSOC_ESTABLISHED;
  a.peer_tag = b.local_tag;
  chunk.type = 9u;
  chunk.value = error_value;
  chunk.value_size = sizeof(error_value);

  /* ERROR is recognized but not implemented by this profile. Reject it
   * without replying with another ERROR or mutating the association. */
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &chunk, 1u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(output_n == 0u && output[0] == 0xCCu);
  assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(a.last_time_ms == prior_time);

  /* An ABORT with a non-matching Verification Tag and T=0 must not
   * terminate the association or commit the monotonic time watermark. */
  chunk.type = 6u;
  chunk.value = NULL;
  chunk.value_size = 0u;
  assert(cnet_sctp_wire_encode(5001u, 5000u, UINT32_C(0xDEADBEEF), &chunk, 1u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(output_n == 0u && output[0] == 0xCCu);
  assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(a.last_time_ms == prior_time);

  /* T-bit ABORT is outside this profile's plain-ABORT subset. Reject it
   * transactionally rather than accidentally treating it as a valid abort. */
  chunk.flags = 1u;
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &chunk, 1u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(output_n == 0u && output[0] == 0xCCu);
  assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(a.last_time_ms == prior_time);

  /* A valid plain T=0 ABORT with ignored reserved flags terminates locally
   * without an ABORT response. T=1 remains rejected above. */
  chunk.flags = 0x80u;
  assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &chunk, 1u,
      packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
  memset(output, 0xCC, sizeof(output));
  assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
      output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_OK);
  assert(output_n == 0u && output[0] == 0xCCu);
  assert(a.state == CNET_SCTP_ASSOC_FAILED);
  assert(a.last_time_ms == prior_time + 1u);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void unsupported_control_preserves_time_watermark(void) {
  cnet_sctp_assoc a, b;
  test_context ca, cb;
  static const uint8_t value[] = {0xA5u, 0x5Au};
  /* Standard reserved/unsupported controls and every currently assigned
   * IANA extension type recognized by this bounded profile. */
  const uint8_t types[] = {
    4u, 5u, 12u, 13u, 15u, 64u, 65u, 128u, 130u, 132u, 192u, 193u, 194u
  };
  size_t i;
  setup(&a, &b, &ca, &cb);
  for (i = 0u; i < sizeof(types); ++i) {
    cnet_sctp_wire_chunk chunk = {types[i], 0u, value, sizeof(value)};
    uint8_t packet[64] = {0}, output[64];
    size_t packet_n = 0u, output_n = 0u;
    uint64_t prior_time = a.last_time_ms;
    memset(output, 0xCC, sizeof(output));
    assert(cnet_sctp_wire_encode(5001u, 5000u, a.local_tag, &chunk, 1u,
        packet, sizeof(packet), &packet_n) == CNET_SCTP_WIRE_OK);
    assert(cnet_sctp_assoc_input(&a, prior_time + 1u, packet, packet_n,
        output, sizeof(output), &output_n) == CNET_SCTP_ASSOC_PROTOCOL);
    assert(output_n == 0u);
    assert(a.state == CNET_SCTP_ASSOC_CLOSED);
    assert(a.last_time_ms == prior_time);
    assert(output[0] == 0xCCu);
  }
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void shutdown_capacity_retries_are_transactional(void) {
  cnet_sctp_assoc a, b, before;
  test_context ca, cb;
  uint8_t init[1200], init_ack[1200], echo[1200], cookie_ack[1200];
  uint8_t data_packet[1200], data_ack[1200];
  uint8_t shutdown_packet[1200], ack_packet[1200], tiny[1] = {0xCCu};
  size_t init_n = 0u, init_ack_n = 0u, echo_n = 0u, cookie_ack_n = 0u;
  size_t data_n = 0u, data_ack_n = 0u, shutdown_n = 0u, ack_n = 0u;
  uint64_t prior_deadline;

  setup(&a, &b, &ca, &cb);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &init_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, init, init_n, init_ack,
      sizeof(init_ack), &init_ack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 3u, init_ack, init_ack_n, echo,
      sizeof(echo), &echo_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 4u, echo, echo_n, cookie_ack,
      sizeof(cookie_ack), &cookie_ack_n) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 5u, cookie_ack, cookie_ack_n, ack_packet,
      sizeof(ack_packet), &ack_n) == CNET_SCTP_ASSOC_OK);
  /* COOKIE-ACK completes the active side locally; this M2 path emits no
   * fourth handshake packet. The passive side was established on COOKIE-ECHO. */
  assert(ack_n == 0u);
  assert(a.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(b.state == CNET_SCTP_ASSOC_ESTABLISHED);

  /* Leave one outbound DATA TSN retained at b, while a has received it and
   * generated (but not delivered) the SACK. The later SHUTDOWN cumulative TSN
   * therefore drains b's TX ticket only if the input transaction commits. */
  assert(cnet_sctp_assoc_send_message(&b, 6u, 0u, 51u, 0,
      "x", 1u, data_packet, sizeof(data_packet), &data_n) ==
      CNET_SCTP_ASSOC_OK);
  assert(data_n > 0u && b.data.tx[0].used);
  assert(cnet_sctp_assoc_input(&a, 6u, data_packet, data_n,
      data_ack, sizeof(data_ack), &data_ack_n) == CNET_SCTP_ASSOC_OK);
  assert(data_ack_n > 0u && a.data.buffered_rx == 1u);
  /* Deliberately do not deliver data_ack to b: SHUTDOWN will cumulatively
   * acknowledge this retained TSN on b, exercising candidate DATA-state
   * rollback when SHUTDOWN-ACK cannot be published. */

  /* First SHUTDOWN: capacity refusal must leave protocol state unchanged
   * so the same timestamp can be retried. */
  before = a;
  tiny[0] = 0xCCu;
  assert(cnet_sctp_assoc_shutdown(&a, 7u, tiny, sizeof(tiny), &shutdown_n) ==
      CNET_SCTP_ASSOC_CAPACITY);
  assert(shutdown_n == 0u && tiny[0] == 0xCCu);
  assert(a.state == before.state && a.last_time_ms == before.last_time_ms &&
      a.deadline_ms == before.deadline_ms &&
      a.t5_shutdown_deadline_ms == before.t5_shutdown_deadline_ms &&
      a.current_rto_ms == before.current_rto_ms && a.local_tag == before.local_tag &&
      a.initial_tsn == before.initial_tsn && a.peer_tag == before.peer_tag &&
      a.peer_initial_tsn == before.peer_initial_tsn &&
      a.saved_size == before.saved_size && a.retransmits == before.retransmits &&
      a.in_progress == before.in_progress && a.data_ready == before.data_ready &&
      memcmp(a.saved_packet, before.saved_packet, sizeof(a.saved_packet)) == 0);
  assert(cnet_sctp_assoc_shutdown(&a, 7u, shutdown_packet,
      sizeof(shutdown_packet), &shutdown_n) == CNET_SCTP_ASSOC_OK);
  assert(shutdown_n > 0u && a.state == CNET_SCTP_ASSOC_SHUTDOWN_SENT);

  /* Inbound SHUTDOWN needs to publish SHUTDOWN-ACK before committing the
   * candidate cumulative-ACK/data state, state transition, or T2 deadline. */
  before = b;
  tiny[0] = 0xCCu;
  assert(cnet_sctp_assoc_input(&b, 8u, shutdown_packet, shutdown_n,
      tiny, sizeof(tiny), &ack_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(ack_n == 0u && tiny[0] == 0xCCu);
  /* A capacity refusal has not accepted this SHUTDOWN or published its
   * ACK. The full protocol/clock transaction must remain retryable. */
  assert(b.last_time_ms == before.last_time_ms);
  assert(b.state == before.state &&
      b.deadline_ms == before.deadline_ms &&
      b.t5_shutdown_deadline_ms == before.t5_shutdown_deadline_ms &&
      b.current_rto_ms == before.current_rto_ms && b.local_tag == before.local_tag &&
      b.initial_tsn == before.initial_tsn && b.peer_tag == before.peer_tag &&
      b.peer_initial_tsn == before.peer_initial_tsn &&
      b.saved_size == before.saved_size && b.retransmits == before.retransmits &&
      b.in_progress == before.in_progress && b.data_ready == before.data_ready &&
      memcmp(b.saved_packet, before.saved_packet, sizeof(b.saved_packet)) == 0);
  assert(b.data.tx[0].used == before.data.tx[0].used &&
      b.data.tx[0].tsn == before.data.tx[0].tsn &&
      b.data.tx[0].packet_size == before.data.tx[0].packet_size &&
      memcmp(b.data.tx[0].packet, before.data.tx[0].packet,
          sizeof(b.data.tx[0].packet)) == 0 &&
      b.data.in_flight == before.data.in_flight &&
      b.data.next_tsn == before.data.next_tsn &&
      b.data.last_ack == before.data.last_ack &&
      b.data.cumulative_rx == before.data.cumulative_rx &&
      b.data.buffered_rx == before.data.buffered_rx);
  assert(cnet_sctp_assoc_input(&b, 8u, shutdown_packet, shutdown_n,
      ack_packet, sizeof(ack_packet), &ack_n) == CNET_SCTP_ASSOC_OK);
  assert(ack_n > 0u && b.state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT &&
      b.last_time_ms == 8u);
  prior_deadline = b.deadline_ms;

  /* A duplicate SHUTDOWN must not restart T2 or consume retry budget until
   * its saved ACK has been admitted to the caller's output buffer. */
  before = b;
  tiny[0] = 0xCCu;
  assert(cnet_sctp_assoc_input(&b, 8u, shutdown_packet, shutdown_n,
      tiny, sizeof(tiny), &ack_n) == CNET_SCTP_ASSOC_CAPACITY);
  assert(ack_n == 0u && tiny[0] == 0xCCu);
  assert(b.state == before.state && b.last_time_ms == before.last_time_ms &&
      b.deadline_ms == before.deadline_ms &&
      b.t5_shutdown_deadline_ms == before.t5_shutdown_deadline_ms &&
      b.current_rto_ms == before.current_rto_ms && b.local_tag == before.local_tag &&
      b.initial_tsn == before.initial_tsn && b.peer_tag == before.peer_tag &&
      b.peer_initial_tsn == before.peer_initial_tsn &&
      b.saved_size == before.saved_size && b.retransmits == before.retransmits &&
      b.in_progress == before.in_progress && b.data_ready == before.data_ready &&
      memcmp(b.saved_packet, before.saved_packet, sizeof(b.saved_packet)) == 0);
  assert(b.deadline_ms == prior_deadline);
  assert(cnet_sctp_assoc_input(&b, 8u, shutdown_packet, shutdown_n,
      ack_packet, sizeof(ack_packet), &ack_n) == CNET_SCTP_ASSOC_OK);
  assert(ack_n > 0u && b.state == CNET_SCTP_ASSOC_SHUTDOWN_ACK_SENT);
  assert(b.deadline_ms == 108u);

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

int main(void) {
  init_ack_capacity_preserves_input_watermark();
  shutdown_capacity_retries_are_transactional();
  malformed_shutdown_bundle_does_not_preempt_t5();
  mixed_unknown_known_bundles_fail_closed();
  unknown_chunks_do_not_preempt_t5();
  unknown_chunk_actions_are_bounded_and_exact();
  unknown_pre_cookie_packets_fail_closed();
  unknown_single_report_packet_ceiling_is_transactional();
  unknown_multi_chunk_actions_are_transactional();
  unknown_report_variable_lengths_preserve_tlv_and_padding();
  unknown_report_dense_capacity_boundary();
  max_unknown_skip_chunk_count_is_transactional();
  max_unknown_report_chunk_count_is_transactional();
  oversized_unknown_skip_packet_is_not_admitted();
  error_and_abort_never_create_response_loops();
  unsupported_control_preserves_time_watermark();
  entropy_init_fail_closed();
  normal_handshake();
  simultaneous_init_keeps_owner_and_t1();
  simultaneous_cookie_echo_wins_before_init_ack();
  simultaneous_signed_peer_change_denied();
  init_tlv_wire_gate();
  clock_owner_and_timeout();
  stale_cookie_and_wrong_mac();
  established_expired_duplicate_cookie();
  binding_vtag_and_invalid_input();
  cookie_echo_t1_retransmission();
  puts("C11 SCTP owner-local INIT/COOKIE research gates PASS (test-only MAC)");
  return 0;
}
