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
  assert(cnet_sctp_assoc_next_deadline(&a, &deadline, &has_deadline) ==
         CNET_SCTP_ASSOC_OK);
  assert(has_deadline && deadline == 110u);
  assert(cnet_sctp_assoc_input(&b, 20u, wire_init, n_init,
                               wire_ack, 8u, &n_ack) == CNET_SCTP_ASSOC_CAPACITY);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_input(&b, 20u, wire_init, n_init,
                               wire_ack, sizeof(wire_ack), &n_ack) == CNET_SCTP_ASSOC_OK);
  assert(n_ack == 124u && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_input(&a, 30u, wire_ack, n_ack,
                               wire_echo, 4u, &n_echo) == CNET_SCTP_ASSOC_CAPACITY);
  assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_COOKIE_WAIT);
  assert(cnet_sctp_assoc_input(&a, 30u, wire_ack, n_ack,
                               wire_echo, sizeof(wire_echo), &n_echo) == CNET_SCTP_ASSOC_OK);
  assert(n_echo == 104u && cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_COOKIE_ECHOED);
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
  assert(cnet_sctp_assoc_input(&a, 7u, bad_ack, bad_n, out, sizeof(out), &out_n) ==
         CNET_SCTP_ASSOC_PROTOCOL);
  assert(cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_COOKIE_ECHOED);
  assert(cnet_sctp_assoc_input(&a, 8u, bad_ack, bad_n - 1u, out, sizeof(out), &out_n) ==
         CNET_SCTP_ASSOC_PROTOCOL);
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
  assert(chunk.type == 9u && chunk.value_size == 12u);
  assert(chunk.value[0] == 0u && chunk.value[1] == 8u);
  assert(chunk.value[2] == 0u && chunk.value[3] == 12u);
  assert(chunk.value[4] == 0xc0u && chunk.value[5] == 2u);
  assert(cnet_sctp_assoc_advance(&a, 103u, scratch,
                                 sizeof(scratch), &encoded_n) == CNET_SCTP_ASSOC_OK);
  assert(encoded_n == echo_n && memcmp(echo, scratch, echo_n) == 0);
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

int main(void) {
  entropy_init_fail_closed();
  normal_handshake();
  init_tlv_wire_gate();
  clock_owner_and_timeout();
  stale_cookie_and_wrong_mac();
  binding_vtag_and_invalid_input();
  cookie_echo_t1_retransmission();
  puts("C11 SCTP owner-local INIT/COOKIE research gates PASS (test-only MAC)");
  return 0;
}
