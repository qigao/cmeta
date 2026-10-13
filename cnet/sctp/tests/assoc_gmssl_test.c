#include "sctp_assoc_internal.h"
#include "sctp_gmssl_mac_internal.h"

#include <gmssl/rand.h>

#ifdef NDEBUG
#undef NDEBUG /* Release test checks are not compiled out. */
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct peer_context {
  cnet_sctp_gmssl_mac mac;
  int on_owner;
  unsigned int calls;
  unsigned int entropy_calls;
  int deny_entropy;
} peer_context;

static void clear_bytes(void *data, size_t n) {
  volatile uint8_t *bytes = (volatile uint8_t *)data;
  while (n--) *bytes++ = 0u;
}

static int owned(void *ctx) {
  return ((peer_context *)ctx)->on_owner;
}

static int sign_cookie(void *ctx, const uint8_t *message, size_t size,
                       uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  peer_context *peer = (peer_context *)ctx;
  ++peer->calls;
  return cnet_sctp_gmssl_mac_sign(&peer->mac, message, size, out);
}

/* C11 GmSSL OS-backed RNG: no fixed tag/TSN or insecure fallback. */
static int fresh_entropy(void *ctx, uint8_t *output, size_t size) {
  peer_context *peer = (peer_context *)ctx;
  ++peer->entropy_calls;
  if (peer->deny_entropy || size != CNET_SCTP_ASSOC_ENTROPY_BYTES)
    return -1;
  return rand_bytes(output, size) == 1 ? 0 : -1;
}

static int check_cookie(void *ctx, const uint8_t *message, size_t size,
                        const uint8_t claimed[CNET_SCTP_ASSOC_MAC_BYTES]) {
  peer_context *peer = (peer_context *)ctx;
  ++peer->calls;
  return cnet_sctp_gmssl_mac_verify(&peer->mac, message, size, claimed);
}

static cnet_sctp_assoc_config setup_config(uint16_t local, peer_context *peer) {
  cnet_sctp_assoc_config config = {0};
  config.local_port = local;
  config.remote_port = (uint16_t)(local == 5000u ? 5001u : 5000u);
  config.receive_window = 32768u;
  config.outbound_streams = 4u;
  config.inbound_streams = 8u;
  config.t1_ms = 100u;
  config.t1_max_ms = 400u;
  config.cookie_lifetime_ms = 500u;
  config.peer_binding = UINT64_C(0xbadc0ffee0ddfeed);
  config.max_retransmits = 3u;
  config.is_owner = owned;
  config.entropy = fresh_entropy;
  config.mac = sign_cookie;
  config.verify_mac = check_cookie;
  config.provider_context = peer;
  return config;
}

static void init_peer(peer_context *peer) {
  uint8_t key[CNET_SCTP_GMSSL_KEY_BYTES] = {0};
  memset(peer, 0, sizeof(*peer));
  peer->on_owner = 1;
  /* Linux-only test job: draws real fresh bytes from the GmSSL RNG.
   * Production randomness/provider availability on other targets is OPEN.
   */
  assert(rand_bytes(key, sizeof(key)) == 1);
  assert(cnet_sctp_gmssl_mac_init(&peer->mac, key, sizeof(key)) == 0);
  clear_bytes(key, sizeof(key));
}

static void test_gmssl_random_tag_tsn(void) {
  cnet_sctp_assoc first = {0}, second = {0}, rejected = {0};
  cnet_sctp_assoc_config config;
  peer_context ctx;
  init_peer(&ctx);
  config = setup_config(5000u, &ctx);
  ctx.deny_entropy = 1;
  assert(cnet_sctp_assoc_init(&rejected, &config, 0u) == CNET_SCTP_ASSOC_ENTROPY);
  assert(rejected.magic == 0u && rejected.local_tag == 0u);
  ctx.deny_entropy = 0;
  assert(cnet_sctp_assoc_init(&first, &config, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(&second, &config, 0u) == CNET_SCTP_ASSOC_OK);
  assert(first.local_tag != 0u && second.local_tag != 0u);
  assert(ctx.entropy_calls == 3u);
  /* A random 64-bit collision is possible but overwhelmingly unlikely. */
  assert(first.local_tag != second.local_tag || first.initial_tsn != second.initial_tsn);
  assert(cnet_sctp_assoc_stop(&first) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&second) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&first) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&second) == CNET_SCTP_ASSOC_OK);
  cnet_sctp_gmssl_mac_retire(&ctx.mac);
}

static void test_hmac_sm3_known_vector(void) {
  static const uint8_t expected[] = {
    0x90u, 0x30u, 0x0cu, 0xd1u, 0xb6u, 0x48u, 0xb2u, 0xccu,
    0x33u, 0x72u, 0xf7u, 0xa5u, 0x9cu, 0xedu, 0xc2u, 0x1au,
    0x1eu, 0x21u, 0xa9u, 0xecu, 0x71u, 0x9du, 0xabu, 0x00u,
    0x38u, 0xa6u, 0x73u, 0x0au, 0x59u, 0x56u, 0x13u, 0x3eu
  };
  cnet_sctp_gmssl_mac provider = {0};
  uint8_t key[CNET_SCTP_GMSSL_KEY_BYTES];
  uint8_t mac[CNET_SCTP_ASSOC_MAC_BYTES];
  memset(key, 0x0bu, sizeof(key));
  memset(mac, 0xeeu, sizeof(mac));
  assert(cnet_sctp_gmssl_mac_init(&provider, key, sizeof(key) - 1u) == -1);
  assert(cnet_sctp_gmssl_mac_init(&provider, key, sizeof(key)) == 0);
  assert(cnet_sctp_gmssl_mac_init(&provider, key, sizeof(key)) == -1);
  clear_bytes(key, sizeof(key)); /* prove the key is copied and retained privately */
  assert(cnet_sctp_gmssl_mac_sign(&provider, (const uint8_t *)"abc", 3u, mac) == 0);
  assert(memcmp(mac, expected, sizeof(expected)) == 0);
  cnet_sctp_gmssl_mac_retire(&provider);
  assert(provider.active == 0u);
  {
    size_t i;
    for (i = 0u; i < sizeof(provider.key); ++i) assert(provider.key[i] == 0u);
  }
  memset(mac, 0x5au, sizeof(mac));
  assert(cnet_sctp_gmssl_mac_sign(&provider, (const uint8_t *)"abc", 3u, mac) == -1);
  assert(mac[0] == 0x5au && mac[31] == 0x5au);
}

static void seal_packet(uint8_t *packet, size_t packet_size) {
  uint32_t crc;
  memset(packet + 8u, 0, 4u);
  crc = cnet_sctp_wire_crc32c(packet, packet_size);
  packet[8] = (uint8_t)crc;
  packet[9] = (uint8_t)(crc >> 8);
  packet[10] = (uint8_t)(crc >> 16);
  packet[11] = (uint8_t)(crc >> 24);
}

static void test_two_epoch_rotation_bounds(void) {
  cnet_sctp_gmssl_mac mac = {0};
  uint8_t first_key[CNET_SCTP_GMSSL_KEY_BYTES];
  uint8_t second_key[CNET_SCTP_GMSSL_KEY_BYTES];
  uint8_t third_key[CNET_SCTP_GMSSL_KEY_BYTES];
  uint8_t old_mac[CNET_SCTP_ASSOC_MAC_BYTES];
  uint8_t new_mac[CNET_SCTP_ASSOC_MAC_BYTES];
  static const uint8_t message[] = "two-epoch-cnet-cookie";
  size_t i;
  memset(first_key, 0x11, sizeof(first_key));
  memset(second_key, 0x22, sizeof(second_key));
  memset(third_key, 0x33, sizeof(third_key));
  assert(cnet_sctp_gmssl_mac_init(&mac, first_key, sizeof(first_key)) == 0);
  assert(cnet_sctp_gmssl_mac_sign(&mac, message, sizeof(message), old_mac) == 0);
  assert(cnet_sctp_gmssl_mac_verify(&mac, message, sizeof(message), old_mac) == 0);
  assert(cnet_sctp_gmssl_mac_rotate(&mac, first_key, sizeof(first_key)) == -1);
  assert(mac.previous_active == 0u);
  assert(cnet_sctp_gmssl_mac_rotate(&mac, second_key, sizeof(second_key)) == 0);
  assert(mac.previous_active == 1u);
  assert(cnet_sctp_gmssl_mac_rotate(&mac, third_key, sizeof(third_key)) == -1);
  assert(mac.previous_active == 1u); /* cannot silently evict outstanding epoch */
  assert(cnet_sctp_gmssl_mac_verify(&mac, message, sizeof(message), old_mac) == 0);
  assert(cnet_sctp_gmssl_mac_sign(&mac, message, sizeof(message), new_mac) == 0);
  assert(memcmp(old_mac, new_mac, sizeof(old_mac)) != 0);
  assert(cnet_sctp_gmssl_mac_verify(&mac, message, sizeof(message), new_mac) == 0);
  cnet_sctp_gmssl_mac_retire_previous(&mac);
  assert(mac.previous_active == 0u);
  for (i = 0; i < sizeof(mac.previous_key); ++i) assert(mac.previous_key[i] == 0u);
  assert(cnet_sctp_gmssl_mac_verify(&mac, message, sizeof(message), old_mac) == -1);
  assert(cnet_sctp_gmssl_mac_verify(&mac, message, sizeof(message), new_mac) == 0);
  assert(cnet_sctp_gmssl_mac_rotate(&mac, third_key, sizeof(third_key)) == 0);
  assert(cnet_sctp_gmssl_mac_verify(&mac, message, sizeof(message), new_mac) == 0);
  cnet_sctp_gmssl_mac_retire(&mac);
  assert(cnet_sctp_gmssl_mac_verify(&mac, message, sizeof(message), new_mac) == -1);
  assert(cnet_sctp_gmssl_mac_rotate(&mac, first_key, sizeof(first_key)) == -1);
  clear_bytes(first_key, sizeof(first_key));
  clear_bytes(second_key, sizeof(second_key));
  clear_bytes(third_key, sizeof(third_key));
}

static void test_rotated_cookie_inflight_handshake(void) {
  cnet_sctp_assoc a = {0}, b = {0};
  peer_context ca, cb;
  cnet_sctp_assoc_config ac, bc;
  uint8_t init[1200], ack[1200], echo[1200], answer[1200], no_output[1200];
  uint8_t replacement[CNET_SCTP_GMSSL_KEY_BYTES];
  size_t ni = 0u, na = 0u, ne = 0u, nr = 0u;
  init_peer(&ca); init_peer(&cb);
  ac = setup_config(5000u, &ca);
  bc = setup_config(5001u, &cb);
  assert(cnet_sctp_assoc_init(&a, &ac, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(&b, &bc, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &ni) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, init, ni, ack, sizeof(ack), &na) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 3u, ack, na, echo, sizeof(echo), &ne) ==
         CNET_SCTP_ASSOC_OK);
  assert(rand_bytes(replacement, sizeof(replacement)) == 1);
  assert(cnet_sctp_gmssl_mac_rotate(&cb.mac, replacement, sizeof(replacement)) == 0);
  clear_bytes(replacement, sizeof(replacement));
  /* Old Cookie is still valid under retained epoch after active-key rotation. */
  assert(cnet_sctp_assoc_input(&b, 4u, echo, ne,
                              answer, sizeof(answer), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 16u && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_input(&a, 5u, answer, nr,
                              no_output, sizeof(no_output), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 0u && cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
  /* Owner explicitly retires prior material only after associated work drains. */
  cnet_sctp_gmssl_mac_retire_previous(&cb.mac);
  cnet_sctp_gmssl_mac_retire(&ca.mac);
  cnet_sctp_gmssl_mac_retire(&cb.mac);
}

static void test_independent_provider_cookie(void) {
  cnet_sctp_assoc a = {0}, b = {0}, impostor = {0}, wrong_generation = {0};
  peer_context ca, cb, ci, cg;
  cnet_sctp_assoc_config ac, bc, ic, gc;
  uint8_t init[1200], init_ack[1200], echo[1200], reply[1200], tampered[1200];
  size_t n_init = 0u, n_ack = 0u, n_echo = 0u, n_reply = 0u;
  init_peer(&ca); init_peer(&cb); init_peer(&ci); init_peer(&cg);
  ac = setup_config(5000u, &ca);
  bc = setup_config(5001u, &cb);
  ic = setup_config(5001u, &ci);
  gc = setup_config(5001u, &cg);
  gc.peer_binding ^= 1u;
  assert(cnet_sctp_assoc_init(&a, &ac, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(&b, &bc, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(&impostor, &ic, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(&wrong_generation, &gc, 0u) == CNET_SCTP_ASSOC_OK);
  /* Only the test harness copies responder verification attributes so
   * wrong-key and wrong-DTLS-generation probes reach MAC verification.
   * Production never accepts caller-defined verification tags or TSNs. */
  impostor.local_tag = b.local_tag;
  impostor.initial_tsn = b.initial_tsn;
  wrong_generation.local_tag = b.local_tag;
  wrong_generation.initial_tsn = b.initial_tsn;
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &n_init) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, init, n_init, init_ack, sizeof(init_ack), &n_ack) ==
         CNET_SCTP_ASSOC_OK);
  assert(cb.calls == 1u && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_input(&a, 3u, init_ack, n_ack, echo, sizeof(echo), &n_echo) ==
         CNET_SCTP_ASSOC_OK);

  assert(cnet_sctp_assoc_input(&impostor, 4u, echo, n_echo,
                               reply, sizeof(reply), &n_reply) == CNET_SCTP_ASSOC_AUTH);
  assert(n_reply == 0u && cnet_sctp_assoc_state_get(&impostor) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_input(&wrong_generation, 4u, echo, n_echo,
                               reply, sizeof(reply), &n_reply) == CNET_SCTP_ASSOC_AUTH);
  assert(n_reply == 0u);
  memcpy(tampered, echo, n_echo);
  tampered[16u + 36u] ^= 1u; /* authenticated rwnd claim, not packet checksum */
  seal_packet(tampered, n_echo);
  assert(cnet_sctp_assoc_input(&b, 4u, tampered, n_echo,
                               reply, sizeof(reply), &n_reply) == CNET_SCTP_ASSOC_AUTH);
  assert(n_reply == 0u && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);

  cb.on_owner = 0;
  {
    unsigned int before = cb.calls;
    assert(cnet_sctp_assoc_input(&b, 4u, echo, n_echo,
                                 reply, sizeof(reply), &n_reply) == CNET_SCTP_ASSOC_OWNER);
    assert(cb.calls == before);
  }
  cb.on_owner = 1;
  assert(cnet_sctp_assoc_input(&b, 4u, echo, n_echo, reply, 3u, &n_reply) ==
         CNET_SCTP_ASSOC_CAPACITY);
  assert(cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_input(&b, 4u, echo, n_echo,
                               reply, sizeof(reply), &n_reply) == CNET_SCTP_ASSOC_OK);
  assert(n_reply == 16u && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(cnet_sctp_assoc_input(&a, 5u, reply, n_reply,
                               tampered, sizeof(tampered), &n_reply) == CNET_SCTP_ASSOC_OK);
  assert(n_reply == 0u && cnet_sctp_assoc_state_get(&a) == CNET_SCTP_ASSOC_ESTABLISHED);
  /* Real GmSSL HMAC/OS randomness now gate a fully established association
   * followed by actual DATA/SACK and synchronous PPID/stream delivery. */
  {
    static const uint8_t app_bytes[] = {'c','n','e','t'};
    cnet_sctp_data_message app = {0};
    size_t n_data = 0u, n_sack = 0u, n_event = 0u;
    assert(a.data_ready && b.data_ready);
    assert(cnet_sctp_assoc_send_message(&a, 6u, 0u, 51u, 0,
        app_bytes, sizeof(app_bytes), init, sizeof(init), &n_data) ==
        CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_input(&b, 7u, init, n_data, init_ack,
                                 sizeof(init_ack), &n_sack) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_assoc_take_message(&b, &app, tampered,
                                        sizeof(tampered), &n_event) == CNET_SCTP_ASSOC_OK);
    assert(app.stream_id == 0u && app.ppid == 51u && app.stream_sequence == 0u);
    assert(n_event == sizeof(app_bytes) &&
           memcmp(tampered, app_bytes, sizeof(app_bytes)) == 0);
    assert(cnet_sctp_assoc_input(&a, 8u, init_ack, n_sack, reply,
                                 sizeof(reply), &n_reply) == CNET_SCTP_ASSOC_OK);
    assert(n_reply == 0u && a.data.in_flight == 0u);
  }

  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&impostor) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&wrong_generation) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_drained(&a) && cnet_sctp_assoc_drained(&b));
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&impostor) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&wrong_generation) == CNET_SCTP_ASSOC_OK);
  cnet_sctp_gmssl_mac_retire(&ca.mac);
  cnet_sctp_gmssl_mac_retire(&cb.mac);
  cnet_sctp_gmssl_mac_retire(&ci.mac);
  cnet_sctp_gmssl_mac_retire(&cg.mac);
}

static void test_retired_key_fail_closed(void) {
  cnet_sctp_assoc a = {0}, b = {0};
  peer_context ca, cb;
  cnet_sctp_assoc_config ac, bc;
  uint8_t init[1200], ack[1200], echo[1200], response[1200];
  size_t ni = 0u, na = 0u, ne = 0u, nr = 0u;
  init_peer(&ca); init_peer(&cb);
  ac = setup_config(5000u, &ca);
  bc = setup_config(5001u, &cb);
  assert(cnet_sctp_assoc_init(&a, &ac, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(&b, &bc, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &ni) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, init, ni, ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 3u, ack, na, echo, sizeof(echo), &ne) == CNET_SCTP_ASSOC_OK);
  /* Test misuse rejection only: production must not retire an admitted key
   * before associations drain, and must never rely on this path for rotation. */
  cnet_sctp_gmssl_mac_retire(&cb.mac);
  assert(cnet_sctp_assoc_input(&b, 4u, echo, ne, response, sizeof(response), &nr) ==
         CNET_SCTP_ASSOC_AUTH);
  assert(nr == 0u && cnet_sctp_assoc_state_get(&b) == CNET_SCTP_ASSOC_CLOSED);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
  cnet_sctp_gmssl_mac_retire(&ca.mac);
}

int main(void) {
  test_gmssl_random_tag_tsn();
  test_hmac_sm3_known_vector();
  test_two_epoch_rotation_bounds();
  test_rotated_cookie_inflight_handshake();
  test_independent_provider_cookie();
  test_retired_key_fail_closed();
  puts("C11 GmSSL real SM3-HMAC Cookie MAC research gate PASS (not browser interoperability)");
  return 0;
}
