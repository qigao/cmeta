#include "../packet_io.h"
#include "../reliable.h"
#include "test_mac.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "%s:%d assertion: %s\n", __FILE__, __LINE__, #expr); \
  return 1; \
} } while (0)

typedef struct channel channel;

typedef struct wire_buffer {
  channel *link;
  uint8_t owner_id;
  uint8_t wire[RL_MAX_WIRE_BYTES];
  bool busy;
} wire_buffer;

typedef struct port {
  channel *link;
  uint8_t id;
  packet_io_owner io;
  packet_io_ticket io_ticket;
  packet_io_frame descriptors[3];
  packet_io_frame inbound[32];
  size_t inbound_count;
  size_t outstanding;
  rl_session protocol;
  rl_ticket protocol_ticket;
  unsigned delivers;
  unsigned terminals;
  rl_terminal terminal_state;
  uint64_t terminal_tag;
  size_t delivered_bytes;
  uint8_t delivered[RL_MAX_MESSAGE_BYTES];
} port;

struct channel {
  wire_buffer buffers[32];
  port peers[2];
  unsigned released;
  uint64_t now;
};

static void release_wire(void *context, uint64_t tag) {
  wire_buffer *slot = (wire_buffer *)context;
  assert(tag == (uint64_t)(slot - slot->link->buffers));
  assert(slot->busy);
  port *source = &slot->link->peers[slot->owner_id];
  assert(source->outstanding);
  --source->outstanding;
  slot->busy = false;
  ++slot->link->released;
}

static int emit(void *context, const void *bytes, size_t size) {
  port *p = (port *)context;
  wire_buffer *slot = NULL;
  for (size_t i = 0; i < 32; ++i) {
    if (!p->link->buffers[i].busy) { slot = &p->link->buffers[i]; break; }
  }
  if (!slot) return 0;
  assert(size <= RL_MAX_WIRE_BYTES);
  slot->busy = true;
  slot->link = p->link;
  slot->owner_id = p->id;
  memcpy(slot->wire, bytes, size);
  packet_io_frame frame = {slot->wire, size,
      (uint64_t)(slot - p->link->buffers), release_wire, slot};
  packet_io_result admitted = packet_io_submit(&p->io, p->io_ticket, &frame);
  if (admitted != PACKET_IO_OK) { slot->busy = false; return 0; }
  ++p->outstanding;
  return 1; /* explicit copy has completed; no borrowed proto bytes escape */
}
static void on_deliver(void *context, uint32_t message_id,
                       const void *data, size_t size) {
  port *p = (port *)context;
  assert(message_id == 1u && size <= sizeof(p->delivered));
  memcpy(p->delivered, data, size);
  p->delivered_bytes = size;
  ++p->delivers;
}
static void on_terminal(void *context, uint64_t tag, rl_terminal outcome) {
  port *p = (port *)context;
  ++p->terminals;
  p->terminal_state = outcome;
  p->terminal_tag = tag;
}

static size_t tx_burst(void *context, const packet_io_frame *frames, size_t count) {
  port *from = (port *)context;
  port *to = &from->link->peers[1u - from->id];
  size_t available = 32 - to->inbound_count;
  if (count > available) count = available;
  for (size_t i = 0; i < count; ++i)
    to->inbound[to->inbound_count++] = frames[i];
  return count; /* physical frame ownership moves to peer fake-driver */
}
static size_t rx_burst(void *context, packet_io_frame *frames, size_t cap) {
  port *p = (port *)context;
  if (cap > p->inbound_count) cap = p->inbound_count;
  for (size_t i = 0; i < cap; ++i) frames[i] = p->inbound[i];
  memmove(p->inbound, p->inbound + cap,
          (p->inbound_count - cap) * sizeof(*p->inbound));
  p->inbound_count -= cap;
  return cap;
}
static bool quiescent(void *context) {
  const port *p = (const port *)context;
  return p->outstanding == 0;
}
static void received_frame(void *context, const packet_io_frame *frame) {
  port *p = (port *)context;
  rl_result status = rl_input(&p->protocol, p->protocol_ticket,
                              frame->data, frame->size, p->link->now);
  assert(status == RL_OK);
}
static int init(channel *c) {
  memset(c, 0, sizeof(*c));
  for (uint8_t i = 0; i < 2; ++i) {
    port *p = &c->peers[i];
    p->id = i;
    p->link = c;
    packet_io_driver fake = {tx_burst, rx_burst, quiescent};
    CHECK(packet_io_start(&p->io, p->descriptors, 3, fake, p,
                          1, &p->io_ticket) == PACKET_IO_OK);
    rl_config config = {0};
    config.local_id = i ? 29 : 17;
    config.peer_id = i ? 17 : 29;
    config.epoch = 0xe2ca1234u;
    config.first_message_id = 1;
    config.rto_ms = 10;
    config.deadline_ms = 200;
    config.ack_delay_ms = 0;
    config.window = 3;
    config.max_retries = 3;
    fixture_set_directional_keys(&config);
    rl_callbacks cb = {emit, on_deliver, on_terminal, p, fixture_hmac_sha256, p};
    CHECK(rl_start(&p->protocol, &config, cb, 1,
                   &p->protocol_ticket) == RL_OK);
  }
  return 0;
}
static int progress(port *p, uint64_t now, size_t tx, size_t rx) {
  size_t transmitted = 0, received = 0;
  p->link->now = now;
  CHECK(packet_io_progress(&p->io, p->io_ticket, tx, rx,
                           received_frame, p, &transmitted, &received) == PACKET_IO_OK);
  CHECK(transmitted <= tx && received <= rx);
  return (int)(transmitted + received);
}
static int step(port *p, uint64_t now, size_t budget) {
  size_t emitted = 0;
  CHECK(rl_step(&p->protocol, p->protocol_ticket, now, budget, &emitted) == RL_OK);
  return (int)emitted;
}
static int two_layer_ack_boundaries(void) {
  channel c;
  CHECK(init(&c) == 0);
  port *a = &c.peers[0], *b = &c.peers[1];
  uint8_t bytes[150];
  for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(i + 41u);
  CHECK(rl_send(&a->protocol, a->protocol_ticket, bytes, sizeof(bytes), 77, 0) == RL_OK);
  CHECK(step(a, 0, 8) == 3);
  CHECK(a->io.pending == 3 && a->terminals == 0);
  CHECK(progress(a, 1, 3, 0) == 3);
  CHECK(a->io.pending == 0 && a->outstanding == 3);
  CHECK(packet_io_seal(&a->io, a->io_ticket) == PACKET_IO_OK);
  CHECK(packet_io_drain(&a->io, a->io_ticket) == PACKET_IO_BUSY);
  CHECK(progress(b, 2, 0, 3) == 3);
  CHECK(b->delivers == 1 && b->delivered_bytes == sizeof(bytes) &&
        memcmp(b->delivered, bytes, sizeof(bytes)) == 0);
  CHECK(a->outstanding == 0 && a->terminals == 0);
  CHECK(step(b, 2, 1) == 1);
  CHECK(progress(b, 3, 1, 0) == 1);
  CHECK(b->outstanding == 1 && a->terminals == 0);
  CHECK(progress(a, 4, 0, 1) == 1); /* ACK consumed by protocol */
  CHECK(a->terminals == 1 && a->terminal_state == RL_TERMINAL_ACKED &&
        a->terminal_tag == 77);
  CHECK(b->outstanding == 0);
  CHECK(packet_io_stop(&a->io, a->io_ticket) == PACKET_IO_OK);
  CHECK(rl_seal(&a->protocol, a->protocol_ticket) == RL_OK);
  CHECK(rl_stop(&a->protocol, a->protocol_ticket) == RL_OK);
  CHECK(rl_seal(&b->protocol, b->protocol_ticket) == RL_OK);
  CHECK(rl_stop(&b->protocol, b->protocol_ticket) == RL_OK);
  CHECK(packet_io_seal(&b->io, b->io_ticket) == PACKET_IO_OK);
  CHECK(packet_io_stop(&b->io, b->io_ticket) == PACKET_IO_OK);
  CHECK(c.released == 4);
  return 0;
}

static int cancelled_tx_does_not_reclaim_physical_frame(void) {
  channel c;
  CHECK(init(&c) == 0);
  port *a = &c.peers[0], *b = &c.peers[1];
  uint8_t bytes[4] = {1, 2, 3, 4};
  CHECK(rl_send(&a->protocol, a->protocol_ticket, bytes, sizeof(bytes), 99, 0) == RL_OK);
  CHECK(step(a, 0, 1) == 1);
  CHECK(progress(a, 1, 1, 0) == 1);
  CHECK(a->outstanding == 1);
  CHECK(rl_cancel(&a->protocol, a->protocol_ticket) == RL_OK);
  CHECK(a->terminals == 1 && a->terminal_state == RL_TERMINAL_CANCELLED);
  CHECK(packet_io_seal(&a->io, a->io_ticket) == PACKET_IO_OK);
  CHECK(packet_io_drain(&a->io, a->io_ticket) == PACKET_IO_BUSY);
  CHECK(progress(b, 2, 0, 1) == 1);
  CHECK(b->delivers == 1 && a->outstanding == 0 && c.released == 1);
  CHECK(step(b, 2, 1) == 1);
  CHECK(progress(b, 3, 1, 0) == 1);
  CHECK(progress(a, 4, 0, 1) == 1);
  CHECK(a->terminals == 1); /* a late ACK cannot re-settle cancelled TX */
  CHECK(packet_io_stop(&a->io, a->io_ticket) == PACKET_IO_OK);
  CHECK(rl_seal(&a->protocol, a->protocol_ticket) == RL_OK);
  CHECK(rl_stop(&a->protocol, a->protocol_ticket) == RL_OK);
  CHECK(rl_seal(&b->protocol, b->protocol_ticket) == RL_OK);
  CHECK(rl_stop(&b->protocol, b->protocol_ticket) == RL_OK);
  CHECK(packet_io_seal(&b->io, b->io_ticket) == PACKET_IO_OK);
  CHECK(packet_io_stop(&b->io, b->io_ticket) == PACKET_IO_OK);
  CHECK(c.released == 2);
  return 0;
}

int main(void) {
  if (two_layer_ack_boundaries()) return 1;
  if (cancelled_tx_does_not_reclaim_physical_frame()) return 1;
  puts("reliable + Packet IO ownership integration: PASS");
  return 0;
}
