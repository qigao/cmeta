#include "../dma_completion_gate.h"
#include "../packet_io.h"
#include "../r2_mux.h"
#include "test_mac.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
  return 1; \
} } while (0)

typedef struct test_host test_host;
typedef struct held_frame {
  test_host *host;
  uint8_t bytes[RL_MAX_WIRE_BYTES];
  size_t size;
  r2_dma_ticket completion;
  bool occupied;
} held_frame;

struct test_host {
  pthread_t owner;
  r2_mux mux;
  r2_mux_handle handle;
  rl_session protocol;
  rl_ticket protocol_ticket;
  r2_dma_gate gate;
  packet_io_owner io;
  packet_io_ticket io_ticket;
  packet_io_frame staging[2], accepted[2];
  size_t accepted_count;
  held_frame payload[2];
  unsigned live_frames, frames_finalized, direct_finalized;
  unsigned logical_terminals;
  rl_terminal last_terminal;
};

static void final_frame(void *context, uint64_t tag) {
  held_frame *held = (held_frame *)context;
  test_host *h = held->host;
  (void)tag;
  /* Completion delivery is Owner-local; no off-thread pointer is freed. */
  if (!pthread_equal(h->owner, pthread_self()) || !h->gate.polling ||
      !held->occupied || !h->live_frames) {
    fprintf(stderr, "invalid physical finalizer thread or state\n");
    return;
  }
  held->occupied = false;
  --h->live_frames;
  ++h->frames_finalized;
}

static void final_direct(void *context, uint64_t tag) {
  test_host *h = (test_host *)context;
  if (!pthread_equal(h->owner, pthread_self()) || !h->gate.polling ||
      !tag) {
    fprintf(stderr, "direct finalizer violated Owner barrier\n");
    return;
  }
  ++h->direct_finalized;
}

static int emit(void *context, const void *bytes, size_t size) {
  test_host *h = (test_host *)context;
  held_frame *held = NULL;
  for (unsigned i = 0; i < 2; ++i)
    if (!h->payload[i].occupied) { held = &h->payload[i]; break; }
  if (!held || size > RL_MAX_WIRE_BYTES) return 0;
  r2_dma_ticket completion = {0};
  r2_dma_status armed = r2_dma_gate_arm(&h->gate, h->handle, final_frame,
                                        held, (uint64_t)(held - h->payload + 1),
                                        &completion);
  if (armed == R2_DMA_FULL) return 0;
  if (armed != R2_DMA_OK) return -1;
  held->host = h;
  held->occupied = true;
  held->size = size;
  held->completion = completion;
  memcpy(held->bytes, bytes, size);
  packet_io_frame frame = {
    held->bytes, size, completion.serial, final_frame, held
  };
  packet_io_result result = packet_io_submit(&h->io, h->io_ticket, &frame);
  if (result != PACKET_IO_OK) {
    if (r2_dma_gate_rollback(&h->gate, completion) != R2_DMA_OK)
      return -1;
    held->occupied = false;
    return result == PACKET_IO_FULL ? 0 : -1;
  }
  ++h->live_frames; /* frame now owns one pre-reserved completion record */
  return 1;
}
static void on_deliver(void *context, uint32_t message_id,
                       const void *bytes, size_t size) {
  (void)context; (void)message_id; (void)bytes; (void)size;
}
static void on_terminal(void *context, uint64_t tag, rl_terminal result) {
  test_host *h = (test_host *)context;
  (void)tag;
  ++h->logical_terminals;
  h->last_terminal = result;
}
static size_t fake_tx(void *context, const packet_io_frame *frames, size_t count) {
  test_host *h = (test_host *)context;
  size_t space = 2u - h->accepted_count;
  if (count > space) count = space;
  for (size_t i = 0; i < count; ++i)
    h->accepted[h->accepted_count++] = frames[i];
  /* Fake PMD owns frames until an external producer announces safe release.
   * This acceptance does NOT invoke finalizers or publish a reliable ACK. */
  return count;
}
static size_t fake_rx(void *context, packet_io_frame *frames, size_t count) {
  (void)context; (void)frames; (void)count;
  return 0;
}
static bool fake_quiescent(void *context) {
  const test_host *h = (const test_host *)context;
  return h->live_frames == 0;
}
static int setup(test_host *h, size_t capacity) {
  memset(h, 0, sizeof(*h));
  h->owner = pthread_self();
  CHECK(r2_mux_start(&h->mux, 1, 1, 4) == R2_MUX_OK);
  rl_config cfg = {0};
  cfg.local_id = 17; cfg.peer_id = 29;
  cfg.epoch = 777; cfg.first_message_id = 1;
  cfg.rto_ms = 10; cfg.deadline_ms = 200;
  cfg.window = 3; cfg.max_retries = 3;
  fixture_set_directional_keys(&cfg);
  rl_callbacks cb = {emit, on_deliver, on_terminal, h,
                     fixture_hmac_sha256, h};
  CHECK(rl_start(&h->protocol, &cfg, cb, 1, &h->protocol_ticket) == RL_OK);
  CHECK(r2_mux_attach(&h->mux, &h->protocol, h->protocol_ticket,
                      &h->handle) == R2_MUX_OK);
  CHECK(r2_dma_gate_start(&h->gate, &h->mux, 1, capacity) == R2_DMA_OK);
  packet_io_driver fake = {fake_tx, fake_rx, fake_quiescent};
  CHECK(packet_io_start(&h->io, h->staging, 2, fake, h, 1,
                        &h->io_ticket) == PACKET_IO_OK);
  return 0;
}
static int stop_protocol(test_host *h) {
  CHECK(rl_seal(&h->protocol, h->protocol_ticket) == RL_OK);
  CHECK(rl_abort(&h->protocol, h->protocol_ticket) == RL_OK);
  CHECK(rl_stop(&h->protocol, h->protocol_ticket) == RL_OK);
  return 0;
}
static int destroy_host(test_host *h) {
  CHECK(r2_mux_detach(&h->mux, h->handle) == R2_MUX_OK);
  CHECK(r2_mux_seal(&h->mux) == R2_MUX_OK);
  CHECK(r2_mux_stop(&h->mux) == R2_MUX_OK);
  CHECK(r2_dma_gate_destroy(&h->gate) == R2_DMA_OK);
  return 0;
}

typedef struct producer {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  bool go;
  r2_dma_gate *gate;
  r2_dma_ticket token;
  r2_mux *mux;
  r2_mux_handle handle;
  r2_dma_status first, duplicate;
  r2_mux_status illegal_owner_release;
} producer;

static void *producer_worker(void *context) {
  producer *p = (producer *)context;
  pthread_mutex_lock(&p->mutex);
  while (!p->go) pthread_cond_wait(&p->condition, &p->mutex);
  pthread_mutex_unlock(&p->mutex);
  /* The worker may NOT settle Owner-owned mux references directly. */
  p->illegal_owner_release = r2_mux_frame_unpin(p->mux, p->handle);
  p->first = r2_dma_gate_post(p->gate, p->token);
  p->duplicate = r2_dma_gate_post(p->gate, p->token);
  return NULL;
}

static int cancel_before_offowner_completion(void) {
  test_host h;
  CHECK(setup(&h, 2) == 0);
  uint8_t payload[32] = {1,2,3};
  CHECK(rl_send(&h.protocol, h.protocol_ticket, payload, sizeof(payload),
                101u, 0u) == RL_OK);
  size_t emitted = 0, tx = 0, rx = 0;
  CHECK(r2_mux_step(&h.mux, 0, 1, &emitted) == R2_MUX_OK && emitted == 1);
  CHECK(h.live_frames == 1 && h.gate.active == 1);
  CHECK(h.logical_terminals == 0);
  CHECK(packet_io_progress(&h.io, h.io_ticket, 1, 0, NULL, NULL,
                           &tx, &rx) == PACKET_IO_OK && tx == 1);
  CHECK(h.accepted_count == 1 && h.frames_finalized == 0);
  CHECK(rl_cancel(&h.protocol, h.protocol_ticket) == RL_OK);
  CHECK(h.logical_terminals == 1 && h.last_terminal == RL_TERMINAL_CANCELLED);
  CHECK(stop_protocol(&h) == 0);
  CHECK(r2_dma_gate_seal(&h.gate) == R2_DMA_OK);
  CHECK(packet_io_seal(&h.io, h.io_ticket) == PACKET_IO_OK);
  CHECK(r2_dma_gate_drain(&h.gate) == R2_DMA_BUSY);
  CHECK(r2_dma_gate_stop(&h.gate) == R2_DMA_BUSY);
  CHECK(packet_io_stop(&h.io, h.io_ticket) == PACKET_IO_BUSY);
  CHECK(r2_mux_detach(&h.mux, h.handle) == R2_MUX_BUSY);

  producer p = {0};
  p.gate = &h.gate;
  p.mux = &h.mux;
  p.handle = h.handle;
  p.token = h.payload[0].completion;
  CHECK(pthread_mutex_init(&p.mutex, NULL) == 0);
  CHECK(pthread_cond_init(&p.condition, NULL) == 0);
  pthread_t worker;
  CHECK(pthread_create(&worker, NULL, producer_worker, &p) == 0);
  pthread_mutex_lock(&p.mutex);
  p.go = true;
  pthread_cond_broadcast(&p.condition);
  pthread_mutex_unlock(&p.mutex);
  CHECK(pthread_join(worker, NULL) == 0);
  CHECK(p.illegal_owner_release == R2_MUX_WRONG_OWNER);
  CHECK(p.first == R2_DMA_OK && p.duplicate == R2_DMA_DUPLICATE);
  CHECK(pthread_cond_destroy(&p.condition) == 0);
  CHECK(pthread_mutex_destroy(&p.mutex) == 0);
  CHECK(r2_dma_gate_fence_joined_producers(&h.gate) == R2_DMA_OK);
  CHECK(r2_dma_gate_post(&h.gate, p.token) == R2_DMA_SEALED);
  CHECK(r2_dma_gate_poll(&h.gate, 0, &emitted) == R2_DMA_OK && emitted == 0);
  CHECK(r2_dma_gate_drain(&h.gate) == R2_DMA_BUSY);
  CHECK(r2_dma_gate_poll(&h.gate, 1, &emitted) == R2_DMA_OK && emitted == 1);
  CHECK(h.frames_finalized == 1 && h.live_frames == 0);
  CHECK(h.logical_terminals == 1); /* physical completion is NOT peer ACK */
  CHECK(h.mux.slots[0].physical_refs == 0);
  CHECK(r2_dma_gate_drain(&h.gate) == R2_DMA_OK);
  CHECK(packet_io_stop(&h.io, h.io_ticket) == PACKET_IO_OK);
  CHECK(r2_dma_gate_stop(&h.gate) == R2_DMA_OK);
  CHECK(destroy_host(&h) == 0);
  return 0;
}

typedef struct simple_worker {
  r2_dma_gate *gate;
  r2_dma_ticket token;
  r2_dma_status status;
} simple_worker;
static void *post_once(void *value) {
  simple_worker *w = (simple_worker *)value;
  w->status = r2_dma_gate_post(w->gate, w->token);
  return NULL;
}

static int bounded_multi_producer_and_generation(void) {
  test_host h;
  CHECK(setup(&h, 2) == 0);
  r2_dma_ticket a = {0}, b = {0}, c = {0}, rejected = {0};
  CHECK(r2_dma_gate_arm(&h.gate, h.handle, final_direct, &h, 1, &a) == R2_DMA_OK);
  CHECK(r2_dma_gate_arm(&h.gate, h.handle, final_direct, &h, 2, &b) == R2_DMA_OK);
  CHECK(r2_dma_gate_arm(&h.gate, h.handle, final_direct, &h, 3, &rejected) ==
        R2_DMA_FULL);
  CHECK(h.mux.slots[0].physical_refs == 2);
  CHECK(r2_dma_gate_rollback(&h.gate, a) == R2_DMA_OK);
  CHECK(r2_dma_gate_rollback(&h.gate, a) == R2_DMA_STALE);
  CHECK(h.direct_finalized == 0);
  CHECK(r2_dma_gate_arm(&h.gate, h.handle, final_direct, &h, 3, &c) ==
        R2_DMA_OK);
  CHECK(c.serial != a.serial && c.slot == a.slot);
  CHECK(r2_dma_gate_post(&h.gate, a) == R2_DMA_STALE);
  CHECK(r2_dma_gate_seal(&h.gate) == R2_DMA_OK);
  simple_worker req[3] = {
    {&h.gate, b, R2_DMA_INVALID},
    {&h.gate, c, R2_DMA_INVALID},
    {&h.gate, b, R2_DMA_INVALID}
  };
  pthread_t threads[3];
  for (unsigned i = 0; i < 3; ++i)
    CHECK(pthread_create(&threads[i], NULL, post_once, &req[i]) == 0);
  for (unsigned i = 0; i < 3; ++i)
    CHECK(pthread_join(threads[i], NULL) == 0);
  unsigned good = 0, dup = 0;
  for (unsigned i = 0; i < 3; ++i) {
    good += req[i].status == R2_DMA_OK;
    dup += req[i].status == R2_DMA_DUPLICATE;
  }
  CHECK(good == 2 && dup == 1);
  CHECK(r2_dma_gate_fence_joined_producers(&h.gate) == R2_DMA_OK);
  CHECK(r2_dma_gate_drain(&h.gate) == R2_DMA_BUSY);
  size_t finalized = 0;
  CHECK(r2_dma_gate_poll(&h.gate, 1, &finalized) == R2_DMA_OK && finalized == 1);
  CHECK(h.direct_finalized == 1 && h.gate.active == 1);
  CHECK(r2_dma_gate_poll(&h.gate, 1, &finalized) == R2_DMA_OK && finalized == 1);
  CHECK(h.direct_finalized == 2 && h.gate.active == 0);
  CHECK(h.mux.slots[0].physical_refs == 0);
  CHECK(r2_dma_gate_drain(&h.gate) == R2_DMA_OK);
  CHECK(r2_dma_gate_stop(&h.gate) == R2_DMA_OK);

  CHECK(r2_dma_gate_start(&h.gate, &h.mux, 1, 2) == R2_DMA_STALE);
  CHECK(r2_dma_gate_start(&h.gate, &h.mux, 2, 2) == R2_DMA_OK);
  CHECK(r2_dma_gate_post(&h.gate, b) == R2_DMA_STALE);
  r2_dma_ticket d = {0};
  CHECK(r2_dma_gate_arm(&h.gate, h.handle, final_direct, &h, 4, &d) == R2_DMA_OK);
  CHECK(d.generation == 2 && d.serial > c.serial);
  CHECK(r2_dma_gate_rollback(&h.gate, d) == R2_DMA_OK);
  CHECK(r2_dma_gate_seal(&h.gate) == R2_DMA_OK);
  CHECK(r2_dma_gate_fence_joined_producers(&h.gate) == R2_DMA_OK);
  CHECK(r2_dma_gate_stop(&h.gate) == R2_DMA_OK);
  CHECK(stop_protocol(&h) == 0);
  CHECK(packet_io_seal(&h.io, h.io_ticket) == PACKET_IO_OK);
  CHECK(packet_io_stop(&h.io, h.io_ticket) == PACKET_IO_OK);
  CHECK(destroy_host(&h) == 0);
  return 0;
}

int main(void) {
  if (cancel_before_offowner_completion()) return 1;
  if (bounded_multi_producer_and_generation()) return 1;
  puts("DMA-like off-owner completion: bounded mailbox, token fencing and Owner finalizers PASS");
  return 0;
}
