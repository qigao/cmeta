#include "../reliable.h"
#include "test_mac.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "%s:%d assertion: %s\n", __FILE__, __LINE__, #expr); \
  return 1; \
} } while (0)

typedef struct wire_item {
  uint8_t bytes[RL_MAX_WIRE_BYTES];
  size_t size;
  uint8_t destination;
} wire_item;

typedef struct fake_link {
  wire_item queued[128];
  size_t count;
  bool block_a;
  bool block_b;
  bool fatal_a;
} fake_link;

typedef struct node {
  rl_session session;
  rl_ticket ticket;
  fake_link *link;
  uint8_t side;
  uint32_t delivered_id[64];
  size_t delivered_size[64];
  unsigned char delivered_data[RL_MAX_MESSAGE_BYTES];
  unsigned delivers;
  unsigned terminals;
  uint64_t last_tag;
  rl_terminal last_terminal;
  bool reenter;
  rl_result reenter_status;
} node;

typedef struct pair {
  fake_link link;
  node a;
  node b;
  rl_config config_a;
  rl_config config_b;
} pair;

static int emit(void *user, const void *bytes, size_t size) {
  node *n = (node *)user;
  fake_link *link = n->link;
  if (n->side == 0 && link->fatal_a) return -1;
  if ((n->side == 0 && link->block_a) || (n->side == 1 && link->block_b) ||
      link->count == 128) return 0;
  assert(size <= RL_MAX_WIRE_BYTES);
  wire_item *item = &link->queued[link->count++];
  memcpy(item->bytes, bytes, size);
  item->size = size;
  item->destination = n->side == 0 ? 1u : 0u;
  return 1; /* synchronous copy means protocol scratch may be released */
}

static void delivered(void *user, uint32_t id, const void *bytes, size_t size) {
  node *n = (node *)user;
  assert(n->delivers < 64 && size <= RL_MAX_MESSAGE_BYTES);
  n->delivered_id[n->delivers] = id;
  n->delivered_size[n->delivers] = size;
  memcpy(n->delivered_data, bytes, size);
  ++n->delivers;
  if (n->reenter) n->reenter_status = rl_cancel(&n->session, n->ticket);
}
static void terminal(void *user, uint64_t tag, rl_terminal result) {
  node *n = (node *)user;
  ++n->terminals;
  n->last_tag = tag;
  n->last_terminal = result;
  if (n->reenter) n->reenter_status = rl_cancel(&n->session, n->ticket);
}

static rl_config defaults(uint32_t local, uint32_t peer) {
  rl_config cfg = {0};
  cfg.local_id = local;
  cfg.peer_id = peer;
  cfg.epoch = 0x87936123u;
  cfg.first_message_id = 1;
  cfg.rto_ms = 10;
  cfg.ack_delay_ms = 0;
  cfg.deadline_ms = 300;
  cfg.window = 3;
  cfg.max_retries = 5;
  fixture_set_directional_keys(&cfg);
  return cfg;
}
static int initialize(pair *p, rl_config a, rl_config b) {
  memset(p, 0, sizeof(*p));
  p->config_a = a;
  p->config_b = b;
  p->a.side = 0;
  p->b.side = 1;
  p->a.link = &p->link;
  p->b.link = &p->link;
  rl_callbacks ca = {emit, delivered, terminal, &p->a, fixture_hmac_sha256};
  rl_callbacks cb = {emit, delivered, terminal, &p->b, fixture_hmac_sha256};
  CHECK(rl_start(&p->a.session, &a, ca, 1, &p->a.ticket) == RL_OK);
  CHECK(rl_start(&p->b.session, &b, cb, 1, &p->b.ticket) == RL_OK);
  return 0;
}
static int setup(pair *p) {
  return initialize(p, defaults(17, 29), defaults(29, 17));
}
static int advance(node *n, uint64_t time, size_t budget) {
  size_t sent = 999;
  CHECK(rl_step(&n->session, n->ticket, time, budget, &sent) == RL_OK);
  CHECK(sent <= budget);
  return (int)sent;
}
static wire_item take(fake_link *link, size_t position) {
  assert(position < link->count);
  wire_item item = link->queued[position];
  memmove(link->queued + position, link->queued + position + 1,
          (link->count - position - 1) * sizeof(link->queued[0]));
  --link->count;
  return item;
}
static rl_result dispatch(pair *p, wire_item item, uint64_t time) {
  node *dst = item.destination ? &p->b : &p->a;
  return rl_input(&dst->session, dst->ticket, item.bytes, item.size, time);
}
static uint8_t kind(const wire_item *item) { return item->bytes[5]; }
static uint8_t index_of(const wire_item *item) { return item->bytes[6]; }

static int normal_delivery_and_duplicate(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint8_t message[150];
  for (size_t i = 0; i < sizeof(message); ++i) message[i] = (uint8_t)(i ^ 93u);
  CHECK(rl_send(&p.a.session, p.a.ticket, message, sizeof(message), 401, 0) == RL_OK);
  CHECK(p.a.terminals == 0);
  CHECK(advance(&p.a, 0, 4) == 3);
  CHECK(p.link.count == 3);
  wire_item saved = p.link.queued[1];
  while (p.link.count) CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_OK);
  CHECK(p.b.delivers == 1 && p.b.delivered_size[0] == sizeof(message));
  CHECK(memcmp(p.b.delivered_data, message, sizeof(message)) == 0);
  CHECK(p.a.terminals == 0); /* TX is not ACK */
  CHECK(dispatch(&p, saved, 1) == RL_OK);
  CHECK(p.b.delivers == 1 && p.b.session.stats.rx_duplicate == 1);
  CHECK(advance(&p.b, 1, 4) == 1);
  CHECK(p.link.count == 1 && kind(&p.link.queued[0]) == 2);
  CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_OK);
  CHECK(p.a.terminals == 1 && p.a.last_terminal == RL_TERMINAL_ACKED &&
        p.a.last_tag == 401);
  CHECK(dispatch(&p, saved, 2) == RL_OK);
  CHECK(advance(&p.b, 2, 2) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 2) == RL_OK);
  CHECK(p.a.terminals == 1);
  /* Reuse same fixed buffers for the next sequential message. */
  CHECK(rl_send(&p.a.session, p.a.ticket, message, 5, 402, 3) == RL_OK);
  CHECK(advance(&p.a, 3, 2) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 4) == RL_OK);
  CHECK(advance(&p.b, 4, 1) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 4) == RL_OK);
  CHECK(p.a.terminals == 2 && p.b.delivers == 2 && p.b.delivered_id[1] == 2);
  return 0;
}

static int reorder_selective_recovery(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint8_t data[150];
  memset(data, 0xa9, sizeof(data));
  CHECK(rl_send(&p.a.session, p.a.ticket, data, sizeof(data), 900, 0) == RL_OK);
  CHECK(advance(&p.a, 0, 3) == 3);
  CHECK(index_of(&p.link.queued[0]) == 0 && index_of(&p.link.queued[1]) == 1 &&
        index_of(&p.link.queued[2]) == 2);
  wire_item missing = take(&p.link, 0); /* fault: first fragment lost */
  CHECK(dispatch(&p, take(&p.link, 1), 1) == RL_OK); /* 2 before 1 */
  CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_OK);
  CHECK(p.b.delivers == 0);
  CHECK(advance(&p.b, 1, 1) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_OK);
  CHECK(p.a.session.tx.acked_mask == 6 && p.a.terminals == 0);
  CHECK(advance(&p.a, 9, 3) == 0);
  CHECK(advance(&p.a, 10, 3) == 1);
  CHECK(p.a.session.stats.tx_retransmitted == 1 && p.a.session.cwnd == 1);
  CHECK(index_of(&p.link.queued[0]) == 0);
  CHECK(dispatch(&p, take(&p.link, 0), 11) == RL_OK);
  CHECK(p.b.delivers == 1);
  CHECK(advance(&p.b, 11, 2) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 11) == RL_OK);
  CHECK(p.a.terminals == 1 && p.a.last_terminal == RL_TERMINAL_ACKED);
  CHECK(dispatch(&p, missing, 12) == RL_OK);
  CHECK(p.b.delivers == 1);
  return 0;
}

static int ack_loss_and_delayed_ack(void) {
  pair p;
  rl_config a = defaults(17, 29), b = defaults(29, 17);
  b.ack_delay_ms = 20;
  CHECK(initialize(&p, a, b) == 0);
  uint8_t data[] = { 7, 8, 9 };
  CHECK(rl_send(&p.a.session, p.a.ticket, data, sizeof(data), 100, 0) == RL_OK);
  CHECK(advance(&p.a, 0, 1) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 0) == RL_OK);
  CHECK(advance(&p.b, 0, 1) == 0);
  CHECK(advance(&p.a, 10, 1) == 1); /* delayed ACK caused retransmit */
  CHECK(dispatch(&p, take(&p.link, 0), 10) == RL_OK);
  CHECK(advance(&p.b, 19, 1) == 0); /* duplicate must not postpone ACK */
  CHECK(advance(&p.b, 20, 1) == 1);
  wire_item dropped_ack = take(&p.link, 0); /* fault: drop ACK */
  CHECK(kind(&dropped_ack) == 2);
  CHECK(advance(&p.a, 20, 1) == 1); /* ACK loss triggers one more send */
  CHECK(dispatch(&p, take(&p.link, 0), 20) == RL_OK);
  CHECK(advance(&p.b, 39, 1) == 0);
  CHECK(advance(&p.b, 40, 1) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 40) == RL_OK);
  CHECK(p.a.terminals == 1 && p.b.delivers == 1);
  CHECK(p.a.session.stats.tx_retransmitted == 2);
  return 0;
}

static int malicious_corrupted_and_wrong_epoch(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint8_t data[] = { 13, 14, 15 };
  CHECK(rl_send(&p.a.session, p.a.ticket, data, sizeof(data), 200, 0) == RL_OK);
  CHECK(advance(&p.a, 0, 1) == 1);
  wire_item valid = take(&p.link, 0);
  wire_item invalid = valid;
  invalid.size--;
  CHECK(dispatch(&p, invalid, 1) == RL_PROTOCOL);
  invalid = valid;
  invalid.bytes[36] ^= 0x20; /* corrupt payload, invalid CRC */
  CHECK(dispatch(&p, invalid, 1) == RL_PROTOCOL);
  invalid = valid;
  invalid.bytes[16] ^= 1; /* wrong epoch (also invalid CRC) */
  CHECK(dispatch(&p, invalid, 1) == RL_PROTOCOL);
  invalid = valid;
  invalid.bytes[12] ^= 1; /* forged/incorrect recipient */
  CHECK(dispatch(&p, invalid, 1) == RL_PROTOCOL);
  CHECK(p.b.delivers == 0 && p.b.session.stats.rx_invalid == 4);
  CHECK(dispatch(&p, valid, 1) == RL_OK);
  CHECK(advance(&p.b, 1, 1) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_OK);
  CHECK(p.a.terminals == 1 && p.b.delivers == 1);
  return 0;
}

static int capacity_backpressure_and_expiry(void) {
  pair p;
  CHECK(setup(&p) == 0);
  p.link.block_a = true;
  uint8_t bytes[80] = {1};
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, sizeof(bytes), 55, 0) == RL_OK);
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, 1, 56, 0) == RL_FULL);
  CHECK(advance(&p.a, 0, 5) == 0);
  CHECK(p.a.session.tx.sent_mask == 0 && p.a.terminals == 0);
  p.link.block_a = false;
  CHECK(advance(&p.a, 9, 1) == 1);
  CHECK(p.a.terminals == 0 && p.a.session.tx.sent_mask == 1);
  CHECK(advance(&p.a, 9, 1) == 1);
  CHECK(p.link.count == 2);
  /* No RX/ACK progress; fixed absolute deadline terminates admitted message. */
  CHECK(advance(&p.a, 300, 4) == 0);
  CHECK(p.a.last_terminal == RL_TERMINAL_TIMEOUT && p.a.terminals == 1 &&
        p.a.last_tag == 55);
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, 1, 57, 301) == RL_OK);
  CHECK(p.a.terminals == 1);
  CHECK(rl_cancel(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(p.a.last_terminal == RL_TERMINAL_CANCELLED && p.a.terminals == 2);
  CHECK(rl_cancel(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(p.a.terminals == 2);
  return 0;
}

static int emit_failure_and_bad_capacity(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint8_t bytes[RL_MAX_MESSAGE_BYTES + 1] = {0};
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, sizeof(bytes), 300, 0) == RL_INVALID);
  CHECK(p.a.terminals == 0);
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, 20, 301, 0) == RL_OK);
  p.link.fatal_a = true;
  size_t sent = 77;
  CHECK(rl_step(&p.a.session, p.a.ticket, 0, 5, &sent) == RL_EMIT_FAILED);
  CHECK(sent == 0 && p.a.terminals == 1 &&
        p.a.last_terminal == RL_TERMINAL_EMIT_FAILED && p.link.count == 0);
  CHECK(rl_cancel(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(p.a.terminals == 1);
  return 0;
}

static uint32_t calc_crc(uint8_t *packet, size_t size) {
  uint32_t crc = UINT32_MAX;
  for (size_t j = 0; j < size - RL_MAC_BYTES; ++j) {
    if (j >= 32 && j < 36) continue;
    crc ^= packet[j];
    for (unsigned i = 0; i < 8; ++i)
      crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
  }
  return ~crc;
}
static void rewrite_crc(wire_item *item) {
  uint32_t crc = calc_crc(item->bytes, item->size);
  item->bytes[32] = (uint8_t)(crc >> 24);
  item->bytes[33] = (uint8_t)(crc >> 16);
  item->bytes[34] = (uint8_t)(crc >> 8);
  item->bytes[35] = (uint8_t)crc;
}
static int crc_valid_ack_rejected_if_not_sent(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint8_t bytes[150] = {1};
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, sizeof(bytes), 401, 0) == RL_OK);
  CHECK(advance(&p.a, 0, 1) == 1); /* only fragment 0 was sent */
  CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_OK);
  CHECK(advance(&p.b, 1, 1) == 1);
  wire_item ack = take(&p.link, 0);
  CHECK(kind(&ack) == 2);
  ack.bytes[31] |= 0x07u; /* valid CRC but claims unsent fragments */
  rewrite_crc(&ack);
  /* An on-path adversary without a key cannot forge an ACK with fresh CRC. */
  CHECK(dispatch(&p, ack, 1) == RL_AUTH_FAILED);
  /* Even a malicious credential holder cannot acknowledge unsent fragments. */
  CHECK(fixture_hmac_sha256(NULL, p.b.session.config.tx_auth_key,
                            ack.bytes, ack.size - RL_MAC_BYTES,
                            ack.bytes + ack.size - RL_MAC_BYTES));
  CHECK(dispatch(&p, ack, 1) == RL_OK);
  CHECK(p.a.session.tx.acked_mask == 0 && p.a.terminals == 0);
  CHECK(rl_cancel(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(p.a.terminals == 1 && p.a.last_terminal == RL_TERMINAL_CANCELLED);
  return 0;
}

static int retry_exhaustion_and_cwnd(void) {
  pair p;
  rl_config a = defaults(17, 29), b = defaults(29, 17);
  a.max_retries = 1;
  a.deadline_ms = 200;
  CHECK(initialize(&p, a, b) == 0);
  uint8_t bytes[70] = {1};
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, sizeof(bytes), 44, 0) == RL_OK);
  CHECK(advance(&p.a, 0, 1) == 1);
  CHECK(advance(&p.a, 10, 1) == 1);
  CHECK(p.a.session.cwnd == 1 && p.a.session.stats.congestion_reduced == 1);
  CHECK(advance(&p.a, 20, 1) == 0);
  CHECK(p.a.last_terminal == RL_TERMINAL_TIMEOUT && p.a.terminals == 1);
  return 0;
}

static int stop_recycle_stale_and_reentrant(void) {
  pair p;
  CHECK(setup(&p) == 0);
  p.b.reenter = true;
  p.a.reenter = true;
  uint8_t bytes[2] = {1, 2};
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, 2, 66, 0) == RL_OK);
  CHECK(rl_seal(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, 1, 67, 0) == RL_SEALED);
  CHECK(rl_stop(&p.a.session, p.a.ticket) == RL_BUSY);
  CHECK(advance(&p.a, 0, 1) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_OK);
  CHECK(p.b.reenter_status == RL_BUSY);
  CHECK(rl_seal(&p.b.session, p.b.ticket) == RL_OK);
  CHECK(rl_stop(&p.b.session, p.b.ticket) == RL_BUSY); /* pending ACK */
  CHECK(advance(&p.b, 1, 1) == 1);
  CHECK(rl_stop(&p.b.session, p.b.ticket) == RL_OK);
  CHECK(dispatch(&p, take(&p.link, 0), 2) == RL_OK);
  CHECK(p.a.reenter_status == RL_BUSY && p.a.last_terminal == RL_TERMINAL_ACKED);
  CHECK(rl_stop(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(rl_cancel(&p.a.session, p.a.ticket) == RL_STALE);
  rl_ticket again = {0};
  rl_callbacks ca = {emit, delivered, terminal, &p.a, fixture_hmac_sha256};
  CHECK(rl_start(&p.a.session, &p.config_a, ca, 1, &again) == RL_STALE);
  p.config_a.epoch++;
  CHECK(rl_start(&p.a.session, &p.config_a, ca, 2, &again) == RL_OK);
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, 1, 99, 3) == RL_STALE);
  CHECK(rl_send(&p.a.session, again, bytes, 1, 99, 3) == RL_OK);
  CHECK(rl_cancel(&p.a.session, again) == RL_OK);
  CHECK(p.a.terminals == 2);
  CHECK(rl_seal(&p.a.session, again) == RL_OK);
  CHECK(rl_stop(&p.a.session, again) == RL_OK);
  return 0;
}

typedef struct wrong_owner {
  node *n;
  rl_result result;
} wrong_owner;
static void *different_thread(void *context) {
  wrong_owner *request = (wrong_owner *)context;
  request->result = rl_cancel(&request->n->session, request->n->ticket);
  return NULL;
}
static int wrong_thread_and_validation(void) {
  pair p;
  CHECK(setup(&p) == 0);
  wrong_owner req = {&p.a, RL_OK};
  pthread_t t;
  CHECK(pthread_create(&t, NULL, different_thread, &req) == 0);
  CHECK(pthread_join(t, NULL) == 0);
  CHECK(req.result == RL_WRONG_OWNER);
  uint8_t packet[3] = {0};
  CHECK(rl_input(&p.a.session, p.a.ticket, packet, 3, 0) == RL_PROTOCOL);
  CHECK(rl_send(&p.a.session, p.a.ticket, packet, 0, 1, 0) == RL_INVALID);
  CHECK(rl_send(&p.a.session, p.a.ticket, packet, 3, 1, 10) == RL_OK);
  size_t emitted = 7;
  CHECK(rl_step(&p.a.session, p.a.ticket, 9, 1, &emitted) == RL_INVALID);
  CHECK(emitted == 0);
  CHECK(rl_cancel(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(rl_seal(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(rl_stop(&p.a.session, p.a.ticket) == RL_OK);
  return 0;
}

static int seeded_loss_oracle(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint32_t random_state = 0x19630829u;
  uint8_t data[RL_MAX_MESSAGE_BYTES];
  for (unsigned msg = 0; msg < 40; ++msg) {
    size_t len = 1 + (msg * 83u) % sizeof(data);
    for (size_t i = 0; i < len; ++i) data[i] = (uint8_t)(msg ^ i);
    uint64_t start = (uint64_t)msg * 2000u;
    CHECK(rl_send(&p.a.session, p.a.ticket, data, len, msg + 1u, start) == RL_OK);
    bool completed = false;
    for (uint64_t now = start; now < start + 300u; ++now) {
      CHECK(advance(&p.a, now, 5) >= 0);
      CHECK(advance(&p.b, now, 5) >= 0);
      /* Pseudorandom, deterministic data/ACK packet loss + reordering.
       * Never drop more than a few consecutive emissions in this oracle. */
      unsigned delivered_packets = 0;
      while (p.link.count && delivered_packets < 8) {
        random_state = random_state * 1664525u + 1013904223u;
        size_t take_at = (random_state & 4u) ? p.link.count - 1u : 0u;
        wire_item item = take(&p.link, take_at);
        ++delivered_packets;
        if ((random_state % 7u) == 0u) continue; /* dropped */
        CHECK(dispatch(&p, item, now) == RL_OK);
      }
      if (p.a.terminals == msg + 1u) {
        CHECK(p.a.last_terminal == RL_TERMINAL_ACKED);
        CHECK(p.b.delivers == msg + 1u);
        CHECK(p.b.delivered_size[msg] == len);
        CHECK(memcmp(p.b.delivered_data, data, len) == 0);
        completed = true;
        break;
      }
    }
    CHECK(completed);
  }
  CHECK(p.a.session.stats.tx_retransmitted > 0);
  CHECK(p.b.session.stats.rx_duplicate > 0);
  CHECK(p.a.session.stats.terminal_acked == 40);
  CHECK(p.b.delivers == 40);
  return 0;
}


static bool failing_mac(void *user, const uint8_t key[RL_KEY_BYTES],
                        const void *bytes, size_t size, uint8_t out[RL_MAC_BYTES]) {
  (void)user; (void)key; (void)bytes; (void)size; (void)out;
  return false;
}

static bool reenter_mac(void *user, const uint8_t key[RL_KEY_BYTES],
                        const void *bytes, size_t size, uint8_t out[RL_MAC_BYTES]) {
  node *n = (node *)user;
  n->reenter_status = rl_cancel(&n->session, n->ticket);
  return fixture_hmac_sha256(user, key, bytes, size, out);
}

static int authenticated_callback_is_not_reentrant(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint8_t data[] = {1, 2};
  CHECK(rl_send(&p.a.session, p.a.ticket, data, sizeof(data), 88, 0) == RL_OK);
  CHECK(advance(&p.a, 0, 1) == 1);
  p.b.session.callbacks.mac = reenter_mac;
  CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_OK);
  CHECK(p.b.reenter_status == RL_BUSY && p.b.delivers == 1);
  CHECK(advance(&p.b, 1, 1) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_OK);
  CHECK(p.a.terminals == 1 && p.a.last_terminal == RL_TERMINAL_ACKED);
  return 0;
}

static int abort_after_crypto_failure_has_bounded_drain(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint8_t data[] = {4,5};
  CHECK(rl_send(&p.a.session, p.a.ticket, data, sizeof(data), 31, 0) == RL_OK);
  CHECK(advance(&p.a, 0, 1) == 1);
  CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_OK);
  CHECK(p.b.delivers == 1 && p.b.session.ack.pending);
  /* Provider failure must never downgrade ACK into unauthenticated output. */
  p.b.session.callbacks.mac = failing_mac;
  size_t emitted = 3;
  CHECK(rl_step(&p.b.session, p.b.ticket, 1, 1, &emitted) == RL_CRYPTO);
  CHECK(emitted == 0 && p.b.session.ack.pending && p.link.count == 0);
  CHECK(rl_abort(&p.b.session, p.b.ticket) == RL_INVALID); /* seal required */
  CHECK(rl_seal(&p.b.session, p.b.ticket) == RL_OK);
  CHECK(rl_drain(&p.b.session, p.b.ticket) == RL_BUSY);
  CHECK(rl_abort(&p.b.session, p.b.ticket) == RL_OK);
  CHECK(rl_stop(&p.b.session, p.b.ticket) == RL_OK);
  CHECK(p.b.session.stats.tx_acks == 0);
  /* Remote TX remains unsettled, never silently converted to a success. */
  CHECK(p.a.terminals == 0);
  CHECK(rl_seal(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(rl_abort(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(p.a.terminals == 1 && p.a.last_terminal == RL_TERMINAL_CANCELLED);
  CHECK(rl_abort(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(p.a.terminals == 1);
  CHECK(rl_stop(&p.a.session, p.a.ticket) == RL_OK);
  return 0;
}

static int hmac_known_answer_and_fail_closed_admission(void) {
  /* RFC 4231 test case 1: HMAC-SHA256 key 0x0b repeated 20 bytes, padded
   * with zero bytes here to the provider's fixed 32-byte key interface. */
  static const uint8_t expected[32] = {
    0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
    0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
    0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
    0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7
  };
  uint8_t key[RL_KEY_BYTES] = {0}, digest[RL_MAC_BYTES] = {0};
  for (unsigned i = 0; i < 20; ++i) key[i] = 0x0bu;
  CHECK(fixture_hmac_sha256(NULL, key, "Hi There", 8, digest));
  CHECK(memcmp(digest, expected, sizeof(expected)) == 0);

  rl_session s = {0};
  rl_ticket ticket = {0};
  node n = {0};
  rl_config cfg = defaults(17, 29);
  rl_callbacks cb = {emit, delivered, terminal, &n, fixture_hmac_sha256};
  n.session = s;
  cb.mac = NULL;
  CHECK(rl_start(&s, &cfg, cb, 1, &ticket) == RL_INVALID);
  cb.mac = fixture_hmac_sha256;
  memset(cfg.tx_auth_key, 0, sizeof(cfg.tx_auth_key));
  CHECK(rl_start(&s, &cfg, cb, 1, &ticket) == RL_INVALID);
  fixture_set_directional_keys(&cfg);
  memcpy(cfg.rx_auth_key, cfg.tx_auth_key, sizeof(cfg.rx_auth_key));
  CHECK(rl_start(&s, &cfg, cb, 1, &ticket) == RL_INVALID);
  return 0;
}

static int wrong_key_cannot_deliver_or_settle(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint8_t data[] = { 10, 11, 12 };
  CHECK(rl_send(&p.a.session, p.a.ticket, data, sizeof(data), 707, 0) == RL_OK);
  CHECK(advance(&p.a, 0, 1) == 1);
  wire_item original = take(&p.link, 0);
  p.b.session.config.rx_auth_key[0] ^= 0x40u;
  CHECK(dispatch(&p, original, 1) == RL_AUTH_FAILED);
  CHECK(p.b.delivers == 0 && p.b.session.stats.rx_auth_failed == 1);
  p.b.session.config.rx_auth_key[0] ^= 0x40u;
  CHECK(dispatch(&p, original, 1) == RL_OK);
  CHECK(advance(&p.b, 1, 1) == 1);
  wire_item genuine_ack = take(&p.link, 0);
  wire_item forged_ack = genuine_ack;
  forged_ack.bytes[RL_WIRE_HEADER_BYTES - 1] ^= 0x8u; /* CRC tampering */
  CHECK(dispatch(&p, forged_ack, 1) == RL_PROTOCOL);
  forged_ack = genuine_ack;
  forged_ack.bytes[RL_WIRE_HEADER_BYTES] ^= 0x40u; /* tag tampering */
  CHECK(dispatch(&p, forged_ack, 1) == RL_AUTH_FAILED);
  CHECK(p.a.terminals == 0);
  CHECK(dispatch(&p, genuine_ack, 1) == RL_OK);
  CHECK(p.a.terminals == 1 && p.a.last_terminal == RL_TERMINAL_ACKED);
  return 0;
}

static int crypto_provider_failure_never_emits_or_delivers(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint8_t bytes[] = {1, 2, 3};
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, sizeof(bytes), 901, 0) == RL_OK);
  p.a.session.callbacks.mac = failing_mac;
  size_t emitted = 8;
  CHECK(rl_step(&p.a.session, p.a.ticket, 0, 4, &emitted) == RL_CRYPTO);
  CHECK(emitted == 0 && p.link.count == 0 && p.a.terminals == 1 &&
        p.a.last_terminal == RL_TERMINAL_EMIT_FAILED);
  CHECK(p.a.session.stats.tx_crypto_failed == 1);
  CHECK(rl_cancel(&p.a.session, p.a.ticket) == RL_OK && p.a.terminals == 1);

  /* A real signed message must not reach receive callback when verifier fails. */
  p.a.session.callbacks.mac = fixture_hmac_sha256;
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, sizeof(bytes), 902, 1) == RL_OK);
  CHECK(advance(&p.a, 1, 1) == 1);
  p.b.session.callbacks.mac = failing_mac;
  CHECK(dispatch(&p, take(&p.link, 0), 1) == RL_CRYPTO);
  CHECK(p.b.delivers == 0 && p.b.session.stats.rx_auth_failed == 1);
  CHECK(rl_cancel(&p.a.session, p.a.ticket) == RL_OK);
  return 0;
}

static int stale_epoch_replay_and_key_erasure(void) {
  pair p;
  CHECK(setup(&p) == 0);
  uint8_t bytes[] = {5,6,7};
  CHECK(rl_send(&p.a.session, p.a.ticket, bytes, sizeof(bytes), 1, 0) == RL_OK);
  CHECK(advance(&p.a, 0, 1) == 1);
  wire_item old = take(&p.link, 0);
  CHECK(rl_cancel(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(rl_seal(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(rl_stop(&p.a.session, p.a.ticket) == RL_OK);
  CHECK(rl_seal(&p.b.session, p.b.ticket) == RL_OK);
  CHECK(rl_stop(&p.b.session, p.b.ticket) == RL_OK);
  for (size_t i = 0; i < RL_KEY_BYTES; ++i) {
    CHECK(p.a.session.config.tx_auth_key[i] == 0);
    CHECK(p.a.session.config.rx_auth_key[i] == 0);
    CHECK(p.b.session.config.tx_auth_key[i] == 0);
    CHECK(p.b.session.config.rx_auth_key[i] == 0);
  }
  CHECK(rl_input(&p.b.session, p.b.ticket, old.bytes, old.size, 1) == RL_STALE);
  rl_config b = p.config_b;
  b.epoch++;
  b.tx_auth_key[0] ^= 0x20u;
  b.rx_auth_key[0] ^= 0x10u;
  rl_ticket t = {0};
  rl_callbacks cb = {emit, delivered, terminal, &p.b, fixture_hmac_sha256};
  CHECK(rl_start(&p.b.session, &b, cb, 2, &t) == RL_OK);
  CHECK(rl_input(&p.b.session, t, old.bytes, old.size, 1) == RL_PROTOCOL);
  CHECK(p.b.delivers == 0 && p.b.session.stats.rx_invalid == 1);
  CHECK(rl_seal(&p.b.session, t) == RL_OK);
  CHECK(rl_stop(&p.b.session, t) == RL_OK);
  return 0;
}

int main(void) {
  if (abort_after_crypto_failure_has_bounded_drain()) return 1;
  if (authenticated_callback_is_not_reentrant()) return 1;
  if (hmac_known_answer_and_fail_closed_admission()) return 1;
  if (wrong_key_cannot_deliver_or_settle()) return 1;
  if (crypto_provider_failure_never_emits_or_delivers()) return 1;
  if (stale_epoch_replay_and_key_erasure()) return 1;
  if (normal_delivery_and_duplicate()) return 1;
  if (reorder_selective_recovery()) return 1;
  if (ack_loss_and_delayed_ack()) return 1;
  if (malicious_corrupted_and_wrong_epoch()) return 1;
  if (capacity_backpressure_and_expiry()) return 1;
  if (retry_exhaustion_and_cwnd()) return 1;
  if (emit_failure_and_bad_capacity()) return 1;
  if (crc_valid_ack_rejected_if_not_sent()) return 1;
  if (stop_recycle_stale_and_reentrant()) return 1;
  if (wrong_thread_and_validation()) return 1;
  if (seeded_loss_oracle()) return 1;
  puts("reliable-ethernet deterministic protocol oracle: PASS");
  return 0;
}
