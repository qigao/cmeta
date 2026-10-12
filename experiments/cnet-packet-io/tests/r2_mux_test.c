#include "../r2_mux.h"
#include "../packet_io.h"
#include "../mac_lease.h"
#include "test_mac.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x); \
  return 1; \
} } while (0)
enum { LAB_FRAMES = 64 };

typedef struct lab lab;
typedef struct lab_peer lab_peer;

typedef struct tx_storage {
  lab *owner;
  unsigned peer_index;
  r2_mux_handle handle;
  uint8_t bytes[RL_MAX_WIRE_BYTES];
  bool busy;
} tx_storage;
typedef struct rx_storage {
  lab *owner;
  uint8_t bytes[RL_MAX_WIRE_BYTES];
  size_t size;
  bool busy;
} rx_storage;

struct lab_peer {
  lab *owner;
  unsigned index;
  rl_session local, remote;
  rl_mac_lease local_lease, remote_lease;
  rl_ticket local_ticket, remote_ticket;
  r2_mux_handle handle;
  unsigned local_delivered, remote_delivered;
  unsigned local_terminals, remote_terminals;
  rl_terminal local_terminal, remote_terminal;
  uint64_t local_tag;
  r2_mux_status callback_reentrancy;
  bool blocked;
};
struct lab {
  r2_mux mux;
  packet_io_owner io;
  packet_io_ticket io_ticket;
  packet_io_frame descriptors[R2_MUX_MAX_SESSIONS];
  unsigned peers_count;
  unsigned frame_capacity;
  lab_peer peers[R2_MUX_MAX_SESSIONS];
  tx_storage tx_frames[LAB_FRAMES];
  rx_storage rx_frames[LAB_FRAMES];
  packet_io_frame on_wire[LAB_FRAMES];
  size_t on_wire_count;
  unsigned rx_indices[LAB_FRAMES];
  size_t rx_count;
  unsigned transmit_trace[LAB_FRAMES];
  size_t trace_count;
  uint64_t clock_ms;
  unsigned frames_reclaimed, acks_reclaimed, input_bad_auth, input_unroutable;
};

static bool lease_mac(void *user, const uint8_t key[RL_KEY_BYTES],
                      const void *input, size_t size, uint8_t out[RL_MAC_BYTES]) {
  rl_mac_lease *lease = (rl_mac_lease *)user;
  if (!rl_mac_lease_enter(lease, lease_mac)) return false;
  bool ok = fixture_hmac_sha256(NULL, key, input, size, out);
  rl_mac_lease_leave(lease);
  return ok;
}

static void tx_released(void *context, uint64_t tag) {
  tx_storage *store = (tx_storage *)context;
  lab *l = store->owner;
  assert(store->busy && (size_t)tag == (size_t)(store - l->tx_frames));
  /* Always settle the physical frame even in NDEBUG/Release builds: never
   * bury ownership-changing calls inside an assert() expression. */
  r2_mux_status released = r2_mux_frame_unpin(&l->mux, store->handle);
  if (released != R2_MUX_OK) abort();
  store->busy = false;
  ++l->frames_reclaimed;
}
static void rx_released(void *context, uint64_t tag) {
  rx_storage *store = (rx_storage *)context;
  lab *l = store->owner;
  assert(store->busy && (size_t)tag == (size_t)(store - l->rx_frames));
  store->busy = false;
  ++l->acks_reclaimed;
}

static int local_emit(void *user, const void *bytes, size_t size) {
  lab_peer *p = (lab_peer *)user;
  lab *l = p->owner;
  if (p->blocked) return 0; /* a deliberately slow peer must not starve others */
  tx_storage *store = NULL;
  for (unsigned i = 0; i < LAB_FRAMES; ++i)
    if (!l->tx_frames[i].busy) { store = &l->tx_frames[i]; break; }
  if (!store || l->trace_count == LAB_FRAMES || size > RL_MAX_WIRE_BYTES) return 0;
  r2_mux_status pin = r2_mux_frame_pin(&l->mux, p->handle);
  if (pin == R2_MUX_FULL) return 0;
  if (pin != R2_MUX_OK) return -1;
  store->busy = true;
  store->owner = l;
  store->peer_index = p->index;
  store->handle = p->handle;
  memcpy(store->bytes, bytes, size);
  packet_io_frame frame = {
    store->bytes, size, (uint64_t)(store - l->tx_frames),
    tx_released, store
  };
  packet_io_result accepted = packet_io_submit(&l->io, l->io_ticket, &frame);
  if (accepted != PACKET_IO_OK) {
    r2_mux_status rollback = r2_mux_frame_unpin(&l->mux, p->handle);
    if (rollback != R2_MUX_OK) abort();
    store->busy = false;
    return accepted == PACKET_IO_FULL ? 0 : -1;
  }
  l->transmit_trace[l->trace_count++] = p->index;
  return 1; /* copied bytes, independently retained frame lease */
}

static int remote_emit(void *user, const void *bytes, size_t size) {
  lab_peer *p = (lab_peer *)user;
  lab *l = p->owner;
  rx_storage *store = NULL;
  for (unsigned i = 0; i < LAB_FRAMES; ++i)
    if (!l->rx_frames[i].busy) { store = &l->rx_frames[i]; break; }
  if (!store || l->rx_count == LAB_FRAMES || size > RL_MAX_WIRE_BYTES) return 0;
  store->owner = l;
  store->busy = true;
  store->size = size;
  memcpy(store->bytes, bytes, size);
  l->rx_indices[l->rx_count++] = (unsigned)(store - l->rx_frames);
  return 1;
}

static void local_deliver(void *user, uint32_t message_id,
                          const void *bytes, size_t size) {
  lab_peer *p = (lab_peer *)user;
  (void)message_id; (void)bytes; (void)size;
  ++p->local_delivered;
  /* No application callback can recursively reschedule a live Owner. */
  size_t out = 777;
  p->callback_reentrancy = r2_mux_step(&p->owner->mux, p->owner->clock_ms, 1, &out);
  assert(p->callback_reentrancy == R2_MUX_BUSY && out == 0);
}
static void remote_deliver(void *user, uint32_t message_id,
                           const void *bytes, size_t size) {
  lab_peer *p = (lab_peer *)user;
  (void)message_id; (void)bytes; (void)size;
  ++p->remote_delivered;
}
static void local_terminal(void *user, uint64_t tag, rl_terminal result) {
  lab_peer *p = (lab_peer *)user;
  ++p->local_terminals;
  p->local_terminal = result;
  p->local_tag = tag;
  /* A terminal may also originate from an explicit owner-side cancel,
   * outside any mux callback. Assert fencing only for a nested invocation. */
  if (p->owner->mux.in_call) {
    size_t out = 777;
    p->callback_reentrancy = r2_mux_step(&p->owner->mux,
                                         p->owner->clock_ms, 1, &out);
    assert(p->callback_reentrancy == R2_MUX_BUSY && out == 0);
  }
}
static void remote_terminal(void *user, uint64_t tag, rl_terminal result) {
  lab_peer *p = (lab_peer *)user;
  (void)tag; (void)result;
  ++p->remote_terminals;
}

static size_t fake_tx(void *user, const packet_io_frame *frames, size_t count) {
  lab *l = (lab *)user;
  size_t available = LAB_FRAMES - l->on_wire_count;
  if (count > available) count = available;
  for (size_t i = 0; i < count; ++i)
    l->on_wire[l->on_wire_count++] = frames[i];
  return count; /* physical completion is NOT peer ACK */
}
static size_t fake_rx(void *user, packet_io_frame *frames, size_t capacity) {
  lab *l = (lab *)user;
  if (capacity > l->rx_count) capacity = l->rx_count;
  for (size_t i = 0; i < capacity; ++i) {
    rx_storage *store = &l->rx_frames[l->rx_indices[i]];
    assert(store->busy);
    frames[i].data = store->bytes;
    frames[i].size = store->size;
    frames[i].tag = (uint64_t)(store - l->rx_frames);
    frames[i].release = rx_released;
    frames[i].release_context = store;
  }
  memmove(l->rx_indices, l->rx_indices + capacity,
          (l->rx_count - capacity) * sizeof(l->rx_indices[0]));
  l->rx_count -= capacity;
  return capacity;
}
static bool fake_quiescent(void *user) {
  lab *l = (lab *)user;
  if (l->on_wire_count || l->rx_count) return false;
  for (unsigned i = 0; i < LAB_FRAMES; ++i)
    if (l->tx_frames[i].busy || l->rx_frames[i].busy) return false;
  return true;
}
static void on_packet_rx(void *user, const packet_io_frame *frame) {
  lab *l = (lab *)user;
  rl_result child = RL_INVALID;
  r2_mux_status routed = r2_mux_input(&l->mux, frame->data, frame->size,
                                      l->clock_ms, &child);
  if (routed == R2_MUX_UNROUTABLE) ++l->input_unroutable;
  else {
    assert(routed == R2_MUX_OK);
    if (child == RL_AUTH_FAILED || child == RL_PROTOCOL) ++l->input_bad_auth;
    else assert(child == RL_OK);
  }
}
static rl_config config_for(unsigned index, bool remote, uint32_t epoch) {
  rl_config c = {0};
  c.local_id = remote ? 29u + index : 17u;
  c.peer_id = remote ? 17u : 29u + index;
  c.epoch = epoch;
  c.first_message_id = 1;
  c.rto_ms = 10;
  c.deadline_ms = 200;
  c.ack_delay_ms = 0;
  c.window = 3;
  c.max_retries = 5;
  for (unsigned j = 0; j < RL_KEY_BYTES; ++j) {
    uint8_t a = (uint8_t)(0x21u + j + index * 3u);
    uint8_t b = (uint8_t)(0x91u + j + index * 3u);
    c.tx_auth_key[j] = remote ? b : a;
    c.rx_auth_key[j] = remote ? a : b;
  }
  return c;
}
static int lab_init(lab *l, unsigned count, unsigned capacity) {
  CHECK(count >= 1 && count <= 8 && capacity >= 1 && capacity <= 8);
  memset(l, 0, sizeof(*l));
  l->peers_count = count;
  l->frame_capacity = capacity;
  CHECK(r2_mux_start(&l->mux, 1, (uint8_t)count, 4) == R2_MUX_OK);
  packet_io_driver io = {fake_tx, fake_rx, fake_quiescent};
  CHECK(packet_io_start(&l->io, l->descriptors, capacity, io, l,
                        1, &l->io_ticket) == PACKET_IO_OK);
  for (unsigned i = 0; i < count; ++i) {
    lab_peer *p = &l->peers[i];
    p->owner = l;
    p->index = i;
    rl_config local = config_for(i, false, 0xabc00000u + i);
    rl_config remote = config_for(i, true, 0xabc00000u + i);
    rl_callbacks lc = {local_emit, local_deliver, local_terminal,
                       p, lease_mac, &p->local_lease};
    rl_callbacks rc = {remote_emit, remote_deliver, remote_terminal,
                       p, lease_mac, &p->remote_lease};
    CHECK(rl_start(&p->local, &local, lc, 1, &p->local_ticket) == RL_OK);
    CHECK(rl_start(&p->remote, &remote, rc, 1, &p->remote_ticket) == RL_OK);
    CHECK(rl_mac_lease_bind(&p->local_lease, &p->local, p->local_ticket) == RL_MAC_OK);
    CHECK(rl_mac_lease_bind(&p->remote_lease, &p->remote, p->remote_ticket) == RL_MAC_OK);
    CHECK(r2_mux_attach(&l->mux, &p->local, p->local_ticket, &p->handle) == R2_MUX_OK);
    CHECK(p->handle.slot == i);
  }
  return 0;
}
static int hub_send(lab *l) {
  for (unsigned i = 0; i < l->peers_count; ++i) {
    uint8_t bytes[3] = {(uint8_t)i, 0x82, 0x47};
    lab_peer *p = &l->peers[i];
    CHECK(rl_send(&p->local, p->local_ticket, bytes, sizeof(bytes),
                  (uint64_t)(100u + i), 0) == RL_OK);
  }
  return 0;
}
static int progress_io(lab *l, uint64_t now, size_t tx_limit,
                       size_t rx_limit, size_t *sent, size_t *received) {
  l->clock_ms = now;
  CHECK(packet_io_progress(&l->io, l->io_ticket, tx_limit, rx_limit,
                           on_packet_rx, l, sent, received) == PACKET_IO_OK);
  return 0;
}
static int deliver_wire(lab *l, uint64_t now) {
  l->clock_ms = now;
  while (l->on_wire_count) {
    packet_io_frame f = l->on_wire[0];
    memmove(l->on_wire, l->on_wire + 1,
            (--l->on_wire_count) * sizeof(l->on_wire[0]));
    tx_storage *store = (tx_storage *)f.release_context;
    lab_peer *p = &l->peers[store->peer_index];
    CHECK(rl_input(&p->remote, p->remote_ticket, f.data, f.size, now) == RL_OK);
    f.release(f.release_context, f.tag);
  }
  return 0;
}
static int emit_remote(lab *l, uint64_t now) {
  l->clock_ms = now;
  for (unsigned i = 0; i < l->peers_count; ++i) {
    lab_peer *p = &l->peers[i];
    size_t n = 0;
    CHECK(rl_step(&p->remote, p->remote_ticket, now, 1, &n) == RL_OK);
  }
  return 0;
}
static int finish(lab *l) {
  CHECK(l->on_wire_count == 0 && l->rx_count == 0);
  for (unsigned i = 0; i < l->peers_count; ++i) {
    lab_peer *p = &l->peers[i];
    CHECK(rl_seal(&p->local, p->local_ticket) == RL_OK);
    CHECK(rl_mac_lease_seal(&p->local_lease, p->local_ticket) == RL_MAC_OK);
    CHECK(rl_abort(&p->local, p->local_ticket) == RL_OK);
    CHECK(rl_stop(&p->local, p->local_ticket) == RL_OK);
    CHECK(rl_mac_lease_release(&p->local_lease, p->local_ticket) == RL_MAC_OK);
    CHECK(r2_mux_detach(&l->mux, p->handle) == R2_MUX_OK);
    CHECK(rl_seal(&p->remote, p->remote_ticket) == RL_OK);
    CHECK(rl_mac_lease_seal(&p->remote_lease, p->remote_ticket) == RL_MAC_OK);
    CHECK(rl_abort(&p->remote, p->remote_ticket) == RL_OK);
    CHECK(rl_stop(&p->remote, p->remote_ticket) == RL_OK);
    CHECK(rl_mac_lease_release(&p->remote_lease, p->remote_ticket) == RL_MAC_OK);
  }
  CHECK(r2_mux_seal(&l->mux) == R2_MUX_OK);
  CHECK(r2_mux_stop(&l->mux) == R2_MUX_OK);
  CHECK(packet_io_seal(&l->io, l->io_ticket) == PACKET_IO_OK);
  CHECK(packet_io_stop(&l->io, l->io_ticket) == PACKET_IO_OK);
  CHECK(l->frames_reclaimed == l->trace_count);
  return 0;
}

static int equal_fairness(unsigned n) {
  lab l;
  CHECK(lab_init(&l, n, 8) == 0);
  CHECK(hub_send(&l) == 0);
  size_t out = 0, tx = 0, rx = 0;
  CHECK(r2_mux_step(&l.mux, 0, R2_MUX_MAX_BUDGET + 1u, &out) ==
        R2_MUX_INVALID && out == 0);
  CHECK(r2_mux_step(&l.mux, 0, n, &out) == R2_MUX_OK && out == n);
  CHECK(l.trace_count == n);
  for (unsigned i = 0; i < n; ++i) {
    CHECK(l.transmit_trace[i] == i);
    CHECK(l.peers[i].local_terminals == 0);
    CHECK(l.mux.slots[i].physical_refs == 1);
  }
  CHECK(progress_io(&l, 1, n, 0, &tx, &rx) == 0 && tx == n && rx == 0);
  CHECK(l.on_wire_count == n);
  CHECK(deliver_wire(&l, 1) == 0);
  CHECK(l.frames_reclaimed == n);
  CHECK(emit_remote(&l, 1) == 0 && l.rx_count == n);
  CHECK(progress_io(&l, 2, 0, n, &tx, &rx) == 0 && rx == n);
  for (unsigned i = 0; i < n; ++i) {
    lab_peer *p = &l.peers[i];
    CHECK(p->remote_delivered == 1);
    CHECK(p->local_terminals == 1 && p->local_terminal == RL_TERMINAL_ACKED);
    CHECK(p->local_tag == (uint64_t)(100u + i));
    CHECK(l.mux.slots[i].physical_refs == 0);
  }
  CHECK(finish(&l) == 0);
  return 0;
}
static int capacity_and_slow_peer_fairness(void) {
  lab l;
  CHECK(lab_init(&l, 4, 2) == 0);
  CHECK(hub_send(&l) == 0);
  size_t out = 0, tx = 0, rx = 0;
  CHECK(r2_mux_step(&l.mux, 0, 4, &out) == R2_MUX_OK && out == 2);
  CHECK(l.trace_count == 2 && l.transmit_trace[0] == 0 &&
        l.transmit_trace[1] == 1);
  CHECK(progress_io(&l, 1, 2, 0, &tx, &rx) == 0 && tx == 2);
  CHECK(r2_mux_step(&l.mux, 1, 4, &out) == R2_MUX_OK && out == 2);
  CHECK(l.transmit_trace[2] == 2 && l.transmit_trace[3] == 3);
  CHECK(progress_io(&l, 2, 2, 0, &tx, &rx) == 0 && tx == 2);
  CHECK(deliver_wire(&l, 2) == 0);
  CHECK(emit_remote(&l, 2) == 0);
  CHECK(progress_io(&l, 3, 0, 4, &tx, &rx) == 0 && rx == 4);
  for(unsigned i = 0; i < 4; ++i)
    CHECK(l.peers[i].local_terminals == 1);
  CHECK(finish(&l) == 0);

  /* A logically fresh research host has fresh zeroed backing storage. */
  lab slow;
  CHECK(lab_init(&slow, 4, 8) == 0);
  CHECK(hub_send(&slow) == 0);
  slow.peers[0].blocked = true;
  CHECK(r2_mux_step(&slow.mux, 0, 4, &out) == R2_MUX_OK && out == 3);
  CHECK(slow.trace_count == 3 && slow.transmit_trace[0] == 1 &&
        slow.transmit_trace[1] == 2 && slow.transmit_trace[2] == 3);
  slow.peers[0].blocked = false;
  CHECK(r2_mux_step(&slow.mux, 1, 4, &out) == R2_MUX_OK && out == 1);
  CHECK(slow.trace_count == 4 && slow.transmit_trace[3] == 0);
  CHECK(progress_io(&slow, 2, 8, 0, &tx, &rx) == 0 && tx == 4);
  CHECK(deliver_wire(&slow, 2) == 0);
  CHECK(emit_remote(&slow, 2) == 0);
  CHECK(progress_io(&slow, 3, 0, 4, &tx, &rx) == 0 && rx == 4);
  CHECK(finish(&slow) == 0);
  return 0;
}

static int spoofed_ack_isolates_only_one_peer(void) {
  lab l;
  CHECK(lab_init(&l, 4, 8) == 0);
  CHECK(hub_send(&l) == 0);
  size_t out=0, tx=0, rx=0;
  CHECK(r2_mux_step(&l.mux, 0, 4, &out) == R2_MUX_OK && out == 4);
  CHECK(progress_io(&l, 1, 4, 0, &tx, &rx) == 0);
  CHECK(deliver_wire(&l, 1) == 0);
  CHECK(emit_remote(&l, 1) == 0 && l.rx_count == 4);
  /* One independently authenticated peer's ACK is corrupt: the others still
   * settle, with no false terminal for its neighbor. */
  l.rx_frames[l.rx_indices[1]].bytes[
      l.rx_frames[l.rx_indices[1]].size - 1] ^= 0x01u;
  CHECK(progress_io(&l, 2, 0, 4, &tx, &rx) == 0 && rx == 4);
  CHECK(l.input_bad_auth == 1);
  for(unsigned i = 0; i < 4; ++i)
    CHECK(l.peers[i].local_terminals == (i == 1 ? 0u : 1u));
  CHECK(r2_mux_step(&l.mux, 10, 4, &out) == R2_MUX_OK && out == 1);
  CHECK(l.transmit_trace[l.trace_count-1] == 1);
  CHECK(progress_io(&l, 11, 1, 0, &tx, &rx) == 0 && tx == 1);
  CHECK(deliver_wire(&l, 11) == 0);
  CHECK(emit_remote(&l, 11) == 0);
  CHECK(progress_io(&l, 12, 0, 1, &tx, &rx) == 0 && rx == 1);
  CHECK(l.peers[1].local_terminals == 1 &&
        l.peers[1].local_terminal == RL_TERMINAL_ACKED);
  CHECK(finish(&l) == 0);
  return 0;
}

static int inbound_incast_and_owner_callback_fencing(void) {
  lab l;
  CHECK(lab_init(&l, 8, 8) == 0);
  for (unsigned i = 0; i < 8; ++i) {
    lab_peer *p = &l.peers[i];
    uint8_t data[] = {(uint8_t)i, 0x55};
    CHECK(rl_send(&p->remote, p->remote_ticket, data, sizeof(data),
                  (uint64_t)i, 0) == RL_OK);
  }
  CHECK(emit_remote(&l, 0) == 0 && l.rx_count == 8);
  size_t out=0, tx=0, rx=0;
  CHECK(progress_io(&l, 1, 0, 8, &tx, &rx) == 0 && rx == 8);
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(l.peers[i].local_delivered == 1 &&
          l.peers[i].callback_reentrancy == R2_MUX_BUSY);
    CHECK(l.peers[i].remote_terminals == 0);
  }
  CHECK(r2_mux_step(&l.mux, 1, 8, &out) == R2_MUX_OK && out == 8);
  CHECK(progress_io(&l, 2, 8, 0, &tx, &rx) == 0 && tx == 8);
  CHECK(deliver_wire(&l, 2) == 0);
  for (unsigned i = 0; i < 8; ++i)
    CHECK(l.peers[i].remote_terminals == 1);
  CHECK(finish(&l) == 0);
  return 0;
}

typedef struct worker_args {
  r2_mux *mux;
  r2_mux_status result;
} worker_args;
static void *cross_thread(void *value) {
  worker_args *args = (worker_args *)value;
  size_t emitted = 777;
  args->result = r2_mux_step(args->mux, 0, 1, &emitted);
  assert(emitted == 0);
  return NULL;
}

static int cancel_with_physical_frame_and_slot_recycle(void) {
  lab l;
  CHECK(lab_init(&l, 1, 1) == 0);
  worker_args arg = {&l.mux, R2_MUX_OK};
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, cross_thread, &arg) == 0);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(arg.result == R2_MUX_WRONG_OWNER);
  CHECK(hub_send(&l) == 0);
  lab_peer *p=&l.peers[0];
  r2_mux_handle old=p->handle;
  size_t out=0, tx=0, rx=0;
  CHECK(r2_mux_step(&l.mux, 0, 1, &out) == R2_MUX_OK && out == 1);
  CHECK(progress_io(&l, 1, 1, 0, &tx, &rx) == 0 && tx == 1);
  CHECK(l.mux.slots[0].physical_refs == 1);
  CHECK(rl_cancel(&p->local, p->local_ticket) == RL_OK);
  CHECK(p->local_terminals == 1 && p->local_terminal == RL_TERMINAL_CANCELLED);
  CHECK(rl_seal(&p->local, p->local_ticket) == RL_OK);
  CHECK(rl_mac_lease_seal(&p->local_lease, p->local_ticket) == RL_MAC_OK);
  CHECK(rl_stop(&p->local, p->local_ticket) == RL_OK);
  CHECK(r2_mux_detach(&l.mux, old) == R2_MUX_BUSY);
  CHECK(deliver_wire(&l, 2) == 0);
  CHECK(l.mux.slots[0].physical_refs == 0);
  CHECK(rl_mac_lease_release(&p->local_lease, p->local_ticket) == RL_MAC_OK);
  CHECK(r2_mux_detach(&l.mux, old) == R2_MUX_OK);
  CHECK(r2_mux_frame_unpin(&l.mux, old) == R2_MUX_STALE);
  CHECK(emit_remote(&l, 2) == 0 && l.rx_count == 1); /* delayed old ACK */
  rl_config next = config_for(0, false, 0xabc00001u);
  rl_callbacks lc = {local_emit, local_deliver, local_terminal,
                     p, lease_mac, &p->local_lease};
  CHECK(rl_start(&p->local, &next, lc, 2, &p->local_ticket) == RL_OK);
  CHECK(rl_mac_lease_bind(&p->local_lease, &p->local, p->local_ticket) == RL_MAC_OK);
  CHECK(r2_mux_attach(&l.mux, &p->local, p->local_ticket,
                      &p->handle) == R2_MUX_OK);
  CHECK(p->handle.generation != old.generation);
  CHECK(r2_mux_frame_pin(&l.mux, old) == R2_MUX_STALE);
  CHECK(progress_io(&l, 3, 0, 1, &tx, &rx) == 0 && rx == 1);
  CHECK(l.input_unroutable == 1 && p->local_terminals == 1);
  CHECK(rl_seal(&p->local, p->local_ticket) == RL_OK);
  CHECK(rl_mac_lease_seal(&p->local_lease, p->local_ticket) == RL_MAC_OK);
  CHECK(rl_stop(&p->local, p->local_ticket) == RL_OK);
  CHECK(rl_mac_lease_release(&p->local_lease, p->local_ticket) == RL_MAC_OK);
  CHECK(r2_mux_detach(&l.mux, p->handle) == R2_MUX_OK);
  CHECK(rl_seal(&p->remote, p->remote_ticket) == RL_OK);
  CHECK(rl_mac_lease_seal(&p->remote_lease, p->remote_ticket) == RL_MAC_OK);
  CHECK(rl_abort(&p->remote, p->remote_ticket) == RL_OK);
  CHECK(rl_stop(&p->remote, p->remote_ticket) == RL_OK);
  CHECK(rl_mac_lease_release(&p->remote_lease, p->remote_ticket) == RL_MAC_OK);
  CHECK(r2_mux_seal(&l.mux) == R2_MUX_OK);
  CHECK(r2_mux_stop(&l.mux) == R2_MUX_OK);
  CHECK(packet_io_seal(&l.io, l.io_ticket) == PACKET_IO_OK);
  CHECK(packet_io_stop(&l.io, l.io_ticket) == PACKET_IO_OK);
  CHECK(p->local_terminals == 1);
  return 0;
}

int main(void) {
  const unsigned counts[] = {1,2,4,8};
  for(unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);++i)
    if(equal_fairness(counts[i])) return 1;
  if(capacity_and_slow_peer_fairness()) return 1;
  if(spoofed_ack_isolates_only_one_peer()) return 1;
  if(inbound_incast_and_owner_callback_fencing()) return 1;
  if(cancel_with_physical_frame_and_slot_recycle()) return 1;
  puts("R2 mux: fixed Owner, 1/2/4/8 fair sessions, incast, backpressure, ACK and drain PASS");
  return 0;
}
