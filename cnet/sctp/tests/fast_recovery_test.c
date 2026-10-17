#include "sctp_data_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Original RFC-first virtual-link tests. No direct writes to TX tickets,
 * loss counters, congestion state or recovery boundaries. The fixture
 * warms cwnd through real DATA/SACK traffic so a second reduction would
 * be observable rather than hidden by the four-MTU minimum. */
typedef struct fixture {
  cnet_sctp_data a, b;
  int admitted;
  uint32_t first;
  uint8_t packet[8][CNET_SCTP_DATA_MTU];
  size_t size[8];
} fixture;

static int owner(void *p) { return *(const int *)p; }
static uint32_t get32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}
static void put16(uint8_t *p, uint16_t n) {
  p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n;
}
static void put32(uint8_t *p, uint32_t n) {
  p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
  p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}
static cnet_sctp_data_config config(fixture *f, int initiator) {
  cnet_sctp_data_config c = {0};
  c.local_port = initiator ? 5000u : 5001u;
  c.remote_port = initiator ? 5001u : 5000u;
  c.local_tag = initiator ? 111u : 222u;
  c.peer_tag = initiator ? 222u : 111u;
  c.local_initial_tsn = initiator ? f->first - 32u : 900u;
  c.peer_initial_tsn = initiator ? 900u : f->first - 32u;
  c.outbound_streams = c.inbound_streams = 4u;
  c.peer_receive_window = CNET_SCTP_DATA_RECEIVE_CEILING;
  c.t3_initial_ms = 100u; c.t3_max_ms = 400u;
  c.max_retransmits = 3u;
  c.is_owner = owner; c.owner_context = &f->admitted;
  return c;
}
static void ack(fixture *f, uint64_t now, const void *p, size_t n) {
  uint8_t output[CNET_SCTP_DATA_MTU];
  size_t size = 99u;
  assert(cnet_sctp_data_input(&f->a, now, p, n, output, sizeof(output),
                             &size) == CNET_SCTP_DATA_OK);
  assert(size == 0u);
}
static void deliver(fixture *f, uint64_t now, const void *p, size_t n,
                    uint8_t *sack, size_t *size) {
  assert(cnet_sctp_data_input(&f->b, now, p, n, sack,
      CNET_SCTP_DATA_MTU, size) == CNET_SCTP_DATA_OK);
}
static void init(fixture *f, uint32_t first) {
  cnet_sctp_data_config a, b;
  uint8_t body[CNET_SCTP_DATA_PAYLOAD_MAX] = {0};
  uint8_t p[16][CNET_SCTP_DATA_MTU], sack[CNET_SCTP_DATA_MTU];
  cnet_sctp_data_message m = {0};
  size_t n[16], ns, nm;
  unsigned int sent = 0u, done = 0u;
  memset(f, 0, sizeof(*f)); f->admitted = 1; f->first = first;
  a = config(f, 1); b = config(f, 0);
  assert(cnet_sctp_data_init(&f->a, &a, 0u) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_init(&f->b, &b, 0u) == CNET_SCTP_DATA_OK);
  /* Stop-and-wait is application limited and must no longer warm cwnd.
   * Keep a bounded wire pipeline full, then drain every packet through the
   * receiver. A single virtual instant keeps the original test clock range. */
  while (done < 32u) {
    while (sent < 32u && sent - done < 16u) {
      const cnet_sctp_data_status status = cnet_sctp_data_send(&f->a, 1u,
          0u, 53u, 0, body, sizeof(body), p[sent % 16u],
          sizeof(p[0]), &n[sent % 16u]);
      if (status == CNET_SCTP_DATA_CAPACITY) break;
      assert(status == CNET_SCTP_DATA_OK);
      ++sent;
    }
    assert(done < sent);
    deliver(f, 1u, p[done % 16u], n[done % 16u], sack, &ns);
    ack(f, 1u, sack, ns);
    assert(cnet_sctp_data_take(&f->b, &m, body, sizeof(body), &nm) ==
           CNET_SCTP_DATA_OK);
    assert(nm == sizeof(body));
    ++done;
  }
  assert(f->a.next_tsn == first && f->a.in_flight == 0u);
  assert(f->a.cwnd > 8u * CNET_SCTP_DATA_MTU);
}
static void send(fixture *f, unsigned int i, uint64_t now) {
  const uint8_t body = (uint8_t)i;
  assert(i < 8u);
  assert(cnet_sctp_data_send(&f->a, now, 0u, 53u, 0, &body, 1u,
      f->packet[i], sizeof(f->packet[i]), &f->size[i]) == CNET_SCTP_DATA_OK);
}
static void gap(fixture *f, unsigned int i, uint64_t now) {
  uint8_t sack[CNET_SCTP_DATA_MTU]; size_t ns;
  deliver(f, now, f->packet[i], f->size[i], sack, &ns);
  ack(f, now + 1u, sack, ns);
}
static cnet_sctp_data_tx *ticket(fixture *f, unsigned int i) {
  unsigned int j;
  for (j = 0u; j < CNET_SCTP_DATA_TX_SLOTS; ++j)
    if (f->a.tx[j].used && f->a.tx[j].tsn == f->first + i)
      return &f->a.tx[j];
  assert(0); return NULL;
}
/* Flight accounting and retransmission intent must remain independent. */
static void assert_flight_invariant(const cnet_sctp_data *d) {
  size_t actual = 0u;
  unsigned int i;
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) {
    const cnet_sctp_data_tx *tx = &d->tx[i];
    if (!tx->used) {
      assert(!tx->flight_accounted && !tx->pending_retransmit);
      continue;
    }
    if (tx->flight_accounted) {
      assert(!tx->gap_acked);
      actual += tx->packet_size - 12u;
    }
    if (tx->pending_retransmit) {
      assert(!tx->gap_acked && !tx->fast_retransmitted);
      assert(tx->missing_reports >= 3u);
    }
  }
  /* Application payload accounting is distinct from padded DATA flight. */
  assert(actual <= CNET_SCTP_DATA_TX_SLOTS * CNET_SCTP_DATA_MTU);
}

static void finished(fixture *f) {
  assert_flight_invariant(&f->a);
  assert(cnet_sctp_data_stop(&f->a) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_stop(&f->b) == CNET_SCTP_DATA_OK);
}
static size_t peer_sack(fixture *f, uint32_t cumulative,
                        uint16_t first, uint16_t last, uint8_t *packet) {
  uint8_t value[16] = {0}; size_t n;
  cnet_sctp_wire_chunk c = {0};
  put32(value, cumulative);
  put32(value + 4u, CNET_SCTP_DATA_RECEIVE_CEILING);
  put16(value + 8u, first ? 1u : 0u);
  put16(value + 12u, first); put16(value + 14u, last);
  c.type = 3u; c.value = value; c.value_size = first ? 16u : 12u;
  assert(cnet_sctp_wire_encode(f->b.config.local_port,
      f->b.config.remote_port, f->a.config.local_tag, &c, 1u,
      packet, CNET_SCTP_DATA_MTU, &n) == CNET_SCTP_WIRE_OK);
  return n;
}

/* 7.2.4(4): the first outstanding retransmission restarts T3, a later one
 * does not. All failures must leave even the original deadline unchanged. */
static void timer_restart(void) {
  fixture f; cnet_sctp_data before;
  uint8_t output[CNET_SCTP_DATA_MTU]; size_t n;
  uint64_t deadline; unsigned int i;
  init(&f, UINT32_C(0xfffffffe));
  for (i = 0u; i < 5u; ++i) send(&f, i, 64u + i);
  for (i = 2u; i < 5u; ++i) gap(&f, i, 76u + 2u * i);
  memcpy(&before, &f.a, sizeof(before));
  memset(output, 0xa5, sizeof(output));
  assert(cnet_sctp_data_advance(&f.a, 90u, output, 1u, &n) ==
         CNET_SCTP_DATA_CAPACITY);
  assert(n == 0u && memcmp(&before, &f.a, sizeof(before)) == 0);
  assert(output[0] == 0xa5u);
  f.admitted = 0;
  assert(cnet_sctp_data_advance(&f.a, 90u, output, sizeof(output), &n) ==
         CNET_SCTP_DATA_OWNER);
  assert(memcmp(&before, &f.a, sizeof(before)) == 0); f.admitted = 1;
  assert(cnet_sctp_data_advance(&f.a, 63u, output, sizeof(output), &n) ==
         CNET_SCTP_DATA_CLOCK);
  assert(memcmp(&before, &f.a, sizeof(before)) == 0);
  assert(cnet_sctp_data_advance(&f.a, 90u, output, sizeof(output), &n) ==
         CNET_SCTP_DATA_OK);
  assert(n == f.size[0] && memcmp(output, f.packet[0], n) == 0);
  assert(f.a.t3_deadline_ms == 90u + f.a.rto_ms);
  deadline = f.a.t3_deadline_ms;
  assert(cnet_sctp_data_advance(&f.a, 91u, output, sizeof(output), &n) ==
         CNET_SCTP_DATA_OK);
  assert(n == f.size[1] && memcmp(output, f.packet[1], n) == 0);
  assert(f.a.t3_deadline_ms == deadline);
  assert(cnet_sctp_data_advance(&f.a, before.t3_deadline_ms,
      output, sizeof(output), &n) == CNET_SCTP_DATA_OK);
  assert(n == 0u); /* the original, now superseded deadline is not due */
  finished(&f);
}

/* 7.2.4 HTNA exception: a cumulative advance during recovery provides a
 * miss indication even when the higher Gap ACKs were already counted.
 * A newly sent tail outside those blocks is not "reported missing". */
static void partial_one(uint32_t first, int enter_recovery) {
  fixture f; cnet_sctp_data before;
  uint8_t fast[CNET_SCTP_DATA_MTU], sack[CNET_SCTP_DATA_MTU];
  uint8_t output[CNET_SCTP_DATA_MTU], stale[CNET_SCTP_DATA_MTU], body[1];
  cnet_sctp_data_message m = {0};
  size_t nfast = 0u, ns, nstale, n, nm;
  unsigned int i; uint32_t window;
  uint64_t srtt, variance, rto, deadline;
  init(&f, first);
  for (i = 0u; i < 7u; ++i) send(&f, i, 64u + i);
  for (i = 1u; i < 4u; ++i) gap(&f, i, 78u + 2u * i);
  if (enter_recovery) {
    const uint32_t old_window = f.a.cwnd;
    assert(cnet_sctp_data_advance(&f.a, 90u, fast, sizeof(fast), &nfast) ==
           CNET_SCTP_DATA_OK);
    assert(nfast == f.size[0] && memcmp(fast, f.packet[0], nfast) == 0);
    assert(f.a.cwnd == old_window / 2u && f.a.recovery_active);
    assert(f.a.recovery_exit_tsn == first + 6u);
  }
  send(&f, 7u, 91u); /* must not move the original recovery exit point */
  gap(&f, 5u, 92u); gap(&f, 6u, 94u);
  assert(ticket(&f, 4u)->missing_reports == 2u);
  assert(ticket(&f, 7u)->missing_reports == 0u);
  nstale = peer_sack(&f, first - 1u, 6u, 7u, stale);
  deliver(&f, 96u, enter_recovery ? fast : f.packet[0],
          enter_recovery ? nfast : f.size[0], sack, &ns);
  assert(get32(sack + 16u) == first + 3u);
  srtt = f.a.srtt_x8_ms; variance = f.a.rttvar_x4_ms; rto = f.a.rto_ms;
  window = f.a.cwnd;
  ack(&f, 97u, sack, ns);
  assert(ticket(&f, 4u)->missing_reports == (enter_recovery ? 3u : 2u));
  assert(ticket(&f, 7u)->missing_reports == 0u);
  deadline = f.a.t3_deadline_ms;
  ack(&f, 98u, sack, ns); /* exact duplicate adds no evidence */
  assert(ticket(&f, 4u)->missing_reports == (enter_recovery ? 3u : 2u));
  assert(f.a.t3_deadline_ms == deadline);
  memcpy(&before, &f.a, sizeof(before));
  ack(&f, 99u, stale, nstale); /* old cumulative SACK cannot undo state */
  assert(memcmp(&before, &f.a, sizeof(before)) == 0);
  if (!enter_recovery) { finished(&f); return; }
  assert(f.a.cwnd == window && f.a.recovery_active);
  assert(cnet_sctp_data_advance(&f.a, 100u, output, 0u, &n) ==
         CNET_SCTP_DATA_CAPACITY);
  assert(memcmp(&before, &f.a, sizeof(before)) == 0);
  assert(cnet_sctp_data_advance(&f.a, 100u, output, sizeof(output), &n) ==
         CNET_SCTP_DATA_OK);
  assert(n == f.size[4] && memcmp(output, f.packet[4], n) == 0);
  assert(f.a.cwnd == window && f.a.recovery_exit_tsn == first + 6u);
  deliver(&f, 101u, output, n, sack, &ns); ack(&f, 102u, sack, ns);
  assert(!f.a.recovery_active && f.a.last_ack == first + 6u);
  assert(f.a.in_flight == 1u && ticket(&f, 7u)->missing_reports == 0u);
  assert(f.a.cwnd == window); /* exit ACK must not also grow cwnd */
  assert(f.a.srtt_x8_ms == srtt && f.a.rttvar_x4_ms == variance &&
         f.a.rto_ms == rto); /* fast retransmitted + prior Gap ACK: no RTT */
  for (i = 0u; i < 7u; ++i) {
    assert(cnet_sctp_data_take(&f.b, &m, body, sizeof(body), &nm) ==
           CNET_SCTP_DATA_OK);
    assert(nm == 1u && body[0] == i && m.tsn == first + i);
    assert(m.stream_sequence == 32u + i && m.ppid == 53u && !m.unordered);
  }
  assert(cnet_sctp_data_take(&f.b, &m, body, sizeof(body), &nm) ==
         CNET_SCTP_DATA_EMPTY);
  deliver(&f, 103u, f.packet[4], f.size[4], sack, &ns);
  ack(&f, 104u, sack, ns);
  assert(cnet_sctp_data_take(&f.b, &m, body, sizeof(body), &nm) ==
         CNET_SCTP_DATA_EMPTY); /* late original is not delivered twice */
  deliver(&f, 105u, f.packet[7], f.size[7], sack, &ns);
  ack(&f, 106u, sack, ns);
  assert(f.a.in_flight == 0u && f.a.t3_deadline_ms == 0u);
  assert(cnet_sctp_data_take(&f.b, &m, body, sizeof(body), &nm) ==
         CNET_SCTP_DATA_OK);
  assert(nm == 1u && body[0] == 7u);
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i) assert(!f.a.tx[i].used);
  finished(&f);
}
static void partial_recovery(void) {
  partial_one(100u, 1); partial_one(UINT32_C(0xfffffffc), 1);
  partial_one(0u, 1);
}
static void ordinary_htna(void) { partial_one(100u, 0); }

/* Consecutive misses reset on a Gap ACK. A later omission is one new
 * reneging indication (6.2.1 D(iii)), not the old two plus an instant FR. */
static void reneging_streak(void) {
  fixture f; uint8_t sack[CNET_SCTP_DATA_MTU], withdrawn[CNET_SCTP_DATA_MTU];
  size_t ns, nw; unsigned int i;
  init(&f, 100u);
  for (i = 0u; i < 4u; ++i) send(&f, i, 64u + i);
  gap(&f, 2u, 80u); gap(&f, 3u, 82u);
  assert(ticket(&f, 1u)->missing_reports == 2u);
  deliver(&f, 84u, f.packet[1], f.size[1], sack, &ns); ack(&f, 85u, sack, ns);
  assert(ticket(&f, 1u)->gap_acked && ticket(&f, 1u)->missing_reports == 0u);
  nw = peer_sack(&f, f.first - 1u, 3u, 4u, withdrawn);
  ack(&f, 86u, withdrawn, nw);
  assert(!ticket(&f, 1u)->gap_acked && ticket(&f, 1u)->missing_reports == 1u);
  assert(ticket(&f, 1u)->ack_counted && f.a.in_flight == 2u);
  ack(&f, 87u, withdrawn, nw);
  assert(ticket(&f, 1u)->missing_reports == 1u);
  ack(&f, 88u, sack, ns);
  assert(ticket(&f, 1u)->missing_reports == 0u);
  ack(&f, 89u, withdrawn, nw);
  assert(ticket(&f, 1u)->missing_reports == 1u);
  finished(&f);
}

/* CRC-valid protocol errors and timer overflow are rejected before
 * ACK settlement, miss counters, RTT or recovery state can change. */
static void sack_rejection_atomicity(void) {
  fixture f; cnet_sctp_data before;
  uint8_t sack[CNET_SCTP_DATA_MTU], output[CNET_SCTP_DATA_MTU];
  size_t ns, n; unsigned int i;
  init(&f, 100u);
  for (i = 0u; i < 5u; ++i) send(&f, i, 64u + i);
  gap(&f, 2u, 80u); gap(&f, 3u, 82u);
  memcpy(&before, &f.a, sizeof(before));
  ns = peer_sack(&f, f.first - 1u, 1u, 9u, sack); /* ACKs unsent TSNs */
  assert(cnet_sctp_data_input(&f.a, 90u, sack, ns, output, sizeof(output),
                             &n) == CNET_SCTP_DATA_PROTOCOL);
  assert(n == 0u && memcmp(&before, &f.a, sizeof(before)) == 0);
  ns = peer_sack(&f, f.a.last_ack + UINT32_C(0x80000000), 0u, 0u, sack);
  assert(cnet_sctp_data_input(&f.a, 90u, sack, ns, output, sizeof(output),
                             &n) == CNET_SCTP_DATA_PROTOCOL);
  assert(memcmp(&before, &f.a, sizeof(before)) == 0);
  ns = peer_sack(&f, f.first, 0u, 0u, sack);
  assert(cnet_sctp_data_input(&f.a, UINT64_MAX - 200u, sack, ns,
      output, sizeof(output), &n) == CNET_SCTP_DATA_CLOCK);
  assert(n == 0u && memcmp(&before, &f.a, sizeof(before)) == 0);
  finished(&f);
}

static void t3_after_fast(void) {
  fixture f; cnet_sctp_data before;
  uint8_t output[CNET_SCTP_DATA_MTU]; size_t n;
  uint64_t due, rto; unsigned int i;
  init(&f, 100u);
  for (i = 0u; i < 5u; ++i) send(&f, i, 64u + i);
  for (i = 2u; i < 5u; ++i) gap(&f, i, 76u + 2u * i);
  assert(cnet_sctp_data_advance(&f.a, 90u, output, sizeof(output), &n) ==
         CNET_SCTP_DATA_OK); /* deliberately lose this retransmission */
  due = f.a.t3_deadline_ms; rto = f.a.rto_ms;
  memcpy(&before, &f.a, sizeof(before));
  assert(cnet_sctp_data_advance(&f.a, due, output, 0u, &n) ==
         CNET_SCTP_DATA_CAPACITY);
  assert(n == 0u && memcmp(&before, &f.a, sizeof(before)) == 0);
  assert(cnet_sctp_data_advance(&f.a, due, output, sizeof(output), &n) ==
         CNET_SCTP_DATA_OK);
  assert(n == f.size[0] && memcmp(output, f.packet[0], n) == 0);
  assert(ticket(&f, 0u)->retransmits == 2u);
  assert(!f.a.recovery_active && f.a.cwnd == CNET_SCTP_DATA_MTU);
  assert(f.a.rto_ms == 2u * rto);
  for (i = 0u; i < CNET_SCTP_DATA_TX_SLOTS; ++i)
    assert(f.a.tx[i].missing_reports == 0u);
  assert(cnet_sctp_data_advance(&f.a, due + 1u, output, sizeof(output), &n) ==
         CNET_SCTP_DATA_OK);
  assert(n == 0u); /* stale evidence for the second hole was cleared */
  assert(cnet_sctp_data_advance(&f.a, f.a.t3_deadline_ms, output,
      sizeof(output), &n) == CNET_SCTP_DATA_OK);
  assert(ticket(&f, 0u)->retransmits == 3u);
  assert(cnet_sctp_data_advance(&f.a, f.a.t3_deadline_ms, output,
      sizeof(output), &n) == CNET_SCTP_DATA_EXPIRED);
  assert(n == 0u && f.a.closed);
  assert(cnet_sctp_data_stop(&f.b) == CNET_SCTP_DATA_OK);
}


/* Three independent missing TSNs become eligible after three higher DATA
 * arrivals. Each advance emits one retained ticket in TSN order, without
 * mistaking pending work for a new DATA flight or cutting cwnd three times. */
static void multi_tsn_fast_recovery(void) {
  fixture f;
  uint8_t output[CNET_SCTP_DATA_MTU];
  size_t n;
  uint32_t reduced = 0u;
  unsigned int i;
  init(&f, 100u);
  for (i = 0u; i < 6u; ++i) send(&f, i, 64u + i);
  for (i = 3u; i < 6u; ++i) gap(&f, i, 80u + i * 2u);
  for (i = 0u; i < 3u; ++i) {
    assert(ticket(&f, i)->pending_retransmit);
    assert(ticket(&f, i)->flight_accounted);
  }
  assert_flight_invariant(&f.a);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_data_advance(&f.a, 95u + i, output,
        sizeof(output), &n) == CNET_SCTP_DATA_OK);
    assert(n == f.size[i] && memcmp(output, f.packet[i], n) == 0);
    assert(!ticket(&f, i)->pending_retransmit);
    assert(ticket(&f, i)->retransmits == 1u);
    if (i == 0u) reduced = f.a.cwnd;
    else assert(f.a.cwnd == reduced);
    assert_flight_invariant(&f.a);
  }
  assert(cnet_sctp_data_advance(&f.a, 99u, output,
      sizeof(output), &n) == CNET_SCTP_DATA_OK);
  assert(n == 0u);
  finished(&f);
}


/* A SACK can revoke a prior Gap ACK. The retained ticket must reenter
 * flight accounting without pretending that its packet was retransmitted. */
static void reneging_flight_accounting(void) {
  fixture f;
  uint8_t sack[CNET_SCTP_DATA_MTU], withdrawn[CNET_SCTP_DATA_MTU];
  size_t ns, nw;
  init(&f, 100u);
  send(&f, 0u, 64u);
  send(&f, 1u, 65u);
  send(&f, 2u, 66u);
  gap(&f, 1u, 80u);
  assert(ticket(&f, 1u)->gap_acked);
  assert(!ticket(&f, 1u)->flight_accounted);
  assert_flight_invariant(&f.a);
  nw = peer_sack(&f, f.first - 1u, 3u, 3u, withdrawn);
  ack(&f, 85u, withdrawn, nw);
  assert(ticket(&f, 1u)->flight_accounted);
  assert(!ticket(&f, 1u)->gap_acked);
  assert(ticket(&f, 1u)->retransmits == 0u);
  assert_flight_invariant(&f.a);
  ns = peer_sack(&f, f.first + 2u, 0u, 0u, sack);
  ack(&f, 86u, sack, ns);
  assert(f.a.in_flight == 0u);
  assert_flight_invariant(&f.a);
  finished(&f);
}


/* T3 output is a real retransmission, but a later cumulative SACK must
 * release its wait barrier and must not let the superseded deadline resend
 * the acknowledged TSN. Keep another outstanding TSN to exercise R3. */
static void t3_sack_interleave(void) {
  fixture f;
  uint8_t output[CNET_SCTP_DATA_MTU], sack[CNET_SCTP_DATA_MTU];
  size_t n, ns;
  uint64_t due, renewed;
  init(&f, 100u);
  send(&f, 0u, 64u);
  send(&f, 1u, 65u);
  due = f.a.t3_deadline_ms;
  assert(cnet_sctp_data_advance(&f.a, due, output,
      sizeof(output), &n) == CNET_SCTP_DATA_OK);
  assert(n == f.size[0] && memcmp(output, f.packet[0], n) == 0);
  assert(f.a.t3_wait_ack && f.a.t3_wait_tsn == f.first);
  assert(ticket(&f, 0u)->flight_accounted);
  assert_flight_invariant(&f.a);
  ns = peer_sack(&f, f.first, 0u, 0u, sack);
  ack(&f, due + 1u, sack, ns);
  assert(!f.a.t3_wait_ack);
  assert(f.a.last_ack == f.first);
  assert(f.a.in_flight == 1u);
  renewed = f.a.t3_deadline_ms;
  assert(renewed > due + 1u);
  assert_flight_invariant(&f.a);
  assert(cnet_sctp_data_advance(&f.a, due + 2u, output,
      sizeof(output), &n) == CNET_SCTP_DATA_OK);
  assert(n == 0u);
  assert(cnet_sctp_data_advance(&f.a, renewed, output,
      sizeof(output), &n) == CNET_SCTP_DATA_OK);
  assert(n == f.size[1] && memcmp(output, f.packet[1], n) == 0);
  assert_flight_invariant(&f.a);
  finished(&f);
}



/* A capacity failure at an expired T3 is transactional. A cumulative
 * SACK arriving before the next Owner advance retires the old candidate;
 * the next timeout must select the remaining outstanding TSN instead. */
static void t3_capacity_then_sack(void) {
  fixture f;
  cnet_sctp_data before;
  uint8_t output[CNET_SCTP_DATA_MTU], sack[CNET_SCTP_DATA_MTU];
  size_t n = 0u, ns;
  uint64_t due, next_due;
  init(&f, 100u);
  send(&f, 0u, 64u);
  send(&f, 1u, 65u);
  due = f.a.t3_deadline_ms;
  memcpy(&before, &f.a, sizeof(before));
  assert(cnet_sctp_data_advance(&f.a, due, output, 0u, &n) ==
      CNET_SCTP_DATA_CAPACITY);
  assert(n == 0u && memcmp(&before, &f.a, sizeof(before)) == 0);
  ns = peer_sack(&f, f.first, 0u, 0u, sack);
  ack(&f, due + 1u, sack, ns);
  assert(f.a.last_ack == f.first && !f.a.t3_wait_ack);
  assert_flight_invariant(&f.a);
  next_due = f.a.t3_deadline_ms;
  assert(next_due > due);
  assert(cnet_sctp_data_advance(&f.a, next_due, output,
      sizeof(output), &n) == CNET_SCTP_DATA_OK);
  assert(n == f.size[1] && memcmp(output, f.packet[1], n) == 0);
  assert(f.a.t3_wait_tsn == f.first + 1u);
  assert_flight_invariant(&f.a);
  finished(&f);
}


/* The T3 deadline outranks pending fast retransmissions. After timeout,
 * stale missing reports must not trigger another fast send in that epoch.
 * This checks the actual scheduler without manually changing TX state. */
static void t3_preempts_pending_fast(void) {
  fixture f;
  uint8_t out[CNET_SCTP_DATA_MTU];
  size_t n;
  uint64_t due;
  unsigned int i;
  init(&f, 100u);
  for (i = 0u; i < 6u; ++i) send(&f, i, 64u + i);
  for (i = 3u; i < 6u; ++i) gap(&f, i, 80u + i * 2u);
  assert(ticket(&f, 0u)->pending_retransmit);
  assert(ticket(&f, 1u)->pending_retransmit);
  due = f.a.t3_deadline_ms;
  assert(cnet_sctp_data_advance(&f.a, due, out, sizeof(out), &n)
      == CNET_SCTP_DATA_OK);
  assert(n == f.size[0] && memcmp(out, f.packet[0], n) == 0);
  assert(f.a.t3_wait_ack && f.a.t3_wait_tsn == f.first);
  assert(f.a.cwnd == CNET_SCTP_DATA_MTU);
  for (i = 0u; i < 3u; ++i) {
    assert(!ticket(&f, i)->pending_retransmit);
    assert(ticket(&f, i)->missing_reports == 0u);
  }
  assert(cnet_sctp_data_advance(&f.a, due + 1u, out,
      sizeof(out), &n) == CNET_SCTP_DATA_OK);
  assert(n == 0u);
  assert_flight_invariant(&f.a);
  finished(&f);
}


/* After T3, a cumulative ACK clears the wait barrier. Fresh higher-TSN
 * evidence can identify another hole, but stale pre-timeout reports cannot. */
static void t3_ack_then_fresh_loss(void) {
  fixture f;
  uint8_t out[CNET_SCTP_DATA_MTU], sack[CNET_SCTP_DATA_MTU];
  size_t n, ns;
  uint64_t due;
  unsigned int i;
  init(&f, 100u);
  for (i = 0u; i < 6u; ++i) send(&f, i, 64u + i);
  due = f.a.t3_deadline_ms;
  assert(cnet_sctp_data_advance(&f.a, due, out, sizeof(out), &n)
      == CNET_SCTP_DATA_OK);
  assert(n == f.size[0]);
  assert(f.a.t3_wait_ack);
  ns = peer_sack(&f, f.first, 0u, 0u, sack);
  ack(&f, due + 1u, sack, ns);
  assert(!f.a.t3_wait_ack);
  assert(f.a.last_ack == f.first);
  for (i = 1u; i < 6u; ++i)
    assert(ticket(&f, i)->missing_reports == 0u);
  /* New higher TSNs provide fresh evidence about TSN first+1. */
  for (i = 3u; i < 6u; ++i) {
    ns = peer_sack(&f, f.first, (uint16_t)i,
                   (uint16_t)i, sack);
    ack(&f, due + 2u + i, sack, ns);
  }
  assert(ticket(&f, 1u)->pending_retransmit);
  assert_flight_invariant(&f.a);
  finished(&f);
}


/* An identical SACK must not create three independent missing reports.
 * A new higher-TSN acknowledgement is needed for each new indication. */
static void duplicate_sack_no_false_fast(void) {
  fixture f;
  uint8_t sack[CNET_SCTP_DATA_MTU], out[CNET_SCTP_DATA_MTU];
  size_t ns, n;
  uint8_t misses;
  uint64_t deadline;
  unsigned int i;
  init(&f, 100u);
  for (i = 0u; i < 5u; ++i) send(&f, i, 64u + i);
  ns = peer_sack(&f, f.first - 1u, 4u, 4u, sack);
  ack(&f, 80u, sack, ns);
  misses = ticket(&f, 0u)->missing_reports;
  deadline = f.a.t3_deadline_ms;
  assert(misses == 1u);
  for (i = 0u; i < 5u; ++i) {
    ack(&f, 81u + i, sack, ns);
    assert(ticket(&f, 0u)->missing_reports == misses);
    assert(!ticket(&f, 0u)->pending_retransmit);
    assert(f.a.t3_deadline_ms == deadline);
  }
  assert(cnet_sctp_data_advance(&f.a, 90u, out,
      sizeof(out), &n) == CNET_SCTP_DATA_OK);
  assert(n == 0u);
  assert_flight_invariant(&f.a);
  finished(&f);
}


/* A reneged Gap ACK restores flight exactly once. Repeating the withdrawn
 * SACK cannot add another miss; re-ACKing the TSN cancels pending intent. */
static void reneging_reack_pending(void) {
  fixture f;
  uint8_t sack[CNET_SCTP_DATA_MTU], withdrawn[CNET_SCTP_DATA_MTU];
  size_t ns, nw, before;
  init(&f, 100u);
  send(&f, 0u, 64u);
  send(&f, 1u, 65u);
  send(&f, 2u, 66u);
  gap(&f, 1u, 80u);
  assert(ticket(&f, 1u)->gap_acked);
  assert(!ticket(&f, 1u)->flight_accounted);
  nw = peer_sack(&f, f.first - 1u, 3u, 3u, withdrawn);
  ack(&f, 85u, withdrawn, nw);
  assert(ticket(&f, 1u)->flight_accounted);
  assert(ticket(&f, 1u)->missing_reports == 1u);
  before = f.a.in_flight;
  ack(&f, 86u, withdrawn, nw);
  assert(f.a.in_flight == before);
  assert(ticket(&f, 1u)->missing_reports == 1u);
  ns = peer_sack(&f, f.first - 1u, 2u, 3u, sack);
  ack(&f, 87u, sack, ns);
  assert(ticket(&f, 1u)->gap_acked);
  assert(!ticket(&f, 1u)->flight_accounted);
  assert(!ticket(&f, 1u)->pending_retransmit);
  assert(ticket(&f, 1u)->missing_reports == 0u);
  assert_flight_invariant(&f.a);
  finished(&f);
}


/* A single loss epoch reduces cwnd once even when several TSNs become
 * eligible. The outstanding DATA flight is not retroactively discarded
 * merely because cwnd fell below it. */
static void multi_loss_single_cwnd_reduction(void) {
  fixture f;
  uint8_t out[CNET_SCTP_DATA_MTU];
  size_t n, flight_before;
  uint32_t cwnd_before, reduced;
  unsigned int i;
  init(&f, 100u);
  for (i = 0u; i < 6u; ++i) send(&f, i, 64u + i);
  for (i = 3u; i < 6u; ++i) gap(&f, i, 80u + i * 2u);
  cwnd_before = f.a.cwnd;
  flight_before = f.a.in_flight;
  assert(cnet_sctp_data_advance(&f.a, 95u, out,
      sizeof(out), &n) == CNET_SCTP_DATA_OK);
  assert(n == f.size[0]);
  reduced = f.a.cwnd;
  assert(reduced <= cwnd_before);
  assert(f.a.in_flight == flight_before);
  assert_flight_invariant(&f.a);
  for (i = 1u; i < 3u; ++i) {
    assert(cnet_sctp_data_advance(&f.a, 95u + i, out,
        sizeof(out), &n) == CNET_SCTP_DATA_OK);
    assert(n == f.size[i]);
    assert(f.a.cwnd == reduced);
    assert(f.a.in_flight == flight_before);
    assert_flight_invariant(&f.a);
  }
  finished(&f);
}



/* RFC 9260 6.1(C): loss repair must outrank both direct and queued DATA.
 * Refusal must retain all caller input state, and deadline() must wake the
 * Owner instead of leaving the pending loss unseen until T3. */
static void pending_priority_for_direct_and_pump(void) {
  fixture f;
  const uint8_t body[2] = {0x41u, 0x42u};
  uint8_t out[CNET_SCTP_DATA_MTU];
  uint32_t next;
  uint64_t due = 0u;
  int has_due = 0;
  size_t n = 123u;
  unsigned int i;
  init(&f, 100u);
  for (i = 0u; i < 6u; ++i) send(&f, i, 64u + i);
  for (i = 3u; i < 6u; ++i) gap(&f, i, 80u + i * 2u);
  for (i = 0u; i < 3u; ++i) assert(ticket(&f, i)->pending_retransmit);
  assert(cnet_sctp_data_deadline(&f.a, &due, &has_due) == CNET_SCTP_DATA_OK);
  assert(has_due && due == f.a.last_time_ms);
  next = f.a.next_tsn;
  assert(cnet_sctp_data_send(&f.a, 94u, 0u, 53u, 0, body,
      sizeof(body), out, sizeof(out), &n) ==
      CNET_SCTP_DATA_RETRANSMIT_PENDING);
  assert(n == 0u && f.a.next_tsn == next);
  assert(cnet_sctp_data_queue_message(&f.a, 94u, 0u, 53u, 0,
      body, sizeof(body)) == CNET_SCTP_DATA_OK);
  assert(cnet_sctp_data_pump(&f.a, 94u, out, sizeof(out), &n) ==
      CNET_SCTP_DATA_RETRANSMIT_PENDING);
  assert(n == 0u && f.a.pending.active &&
      f.a.pending.offset == 0u && f.a.next_tsn == next);
  for (i = 0u; i < 3u; ++i) {
    assert(cnet_sctp_data_advance(&f.a, 95u + i, out,
        sizeof(out), &n) == CNET_SCTP_DATA_OK);
    assert(n == f.size[i] && memcmp(out, f.packet[i], n) == 0);
  }
  assert(cnet_sctp_data_pump(&f.a, 99u, out, sizeof(out), &n) ==
      CNET_SCTP_DATA_OK);
  assert(n != 0u && !f.a.pending.active && f.a.next_tsn == next + 1u);
  assert_flight_invariant(&f.a);
  finished(&f);
}

int main(int argc, char **argv) {
  static const struct { const char *name; void (*run)(void); } cases[] = {
    {"multi_tsn_fast_recovery", multi_tsn_fast_recovery},
    {"pending_priority_for_direct_and_pump", pending_priority_for_direct_and_pump},
    {"t3_sack_interleave", t3_sack_interleave},
    {"t3_capacity_then_sack", t3_capacity_then_sack},
    {"t3_preempts_pending_fast", t3_preempts_pending_fast},
    {"t3_ack_then_fresh_loss", t3_ack_then_fresh_loss},
    {"duplicate_sack_no_false_fast", duplicate_sack_no_false_fast},
    {"reneging_reack_pending", reneging_reack_pending},
    {"multi_loss_single_cwnd_reduction", multi_loss_single_cwnd_reduction},
    {"reneging_flight_accounting", reneging_flight_accounting},
    {"timer_restart", timer_restart}, {"partial_recovery", partial_recovery},
    {"ordinary_htna", ordinary_htna}, {"reneging_streak", reneging_streak},
    {"t3_after_fast", t3_after_fast},
    {"sack_rejection_atomicity", sack_rejection_atomicity}
  };
  size_t i; unsigned int ran = 0u;
  for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
    cases[i].run(); ++ran; printf("PASS %s\n", cases[i].name);
  }
  assert(ran != 0u);
  return 0;
}
