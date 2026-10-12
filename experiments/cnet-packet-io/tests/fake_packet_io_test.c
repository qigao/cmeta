#include "../packet_io.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { \
  fprintf(stderr, "check failed: %s at %s:%d\n", #expression, __FILE__, __LINE__); \
  return 1; \
} } while (0)

typedef struct release_probe {
  unsigned released[64];
  unsigned delivered;
  packet_io_owner *owner;
  packet_io_ticket ticket;
  packet_io_result callback_seal;
} release_probe;

typedef struct fake_driver {
  packet_io_frame tx_held[8];
  size_t tx_count;
  size_t tx_limit;
  packet_io_frame rx_ready[8];
  size_t rx_count;
  bool invalid_tx_count;
} fake_driver;

static void release_frame(void *context, uint64_t tag) {
  release_probe *probe = (release_probe *)context;
  assert(tag < 64);
  ++probe->released[tag];
}

static packet_io_frame frame(release_probe *probe, uint64_t tag) {
  static const char bytes[] = "packet";
  packet_io_frame result = {bytes, sizeof(bytes), tag, release_frame, probe};
  return result;
}

static size_t fake_tx(void *context, const packet_io_frame *frames, size_t count) {
  fake_driver *driver = (fake_driver *)context;
  if (driver->invalid_tx_count) return count + 1;
  size_t free_slots = 8u - driver->tx_count;
  if (free_slots > driver->tx_limit) free_slots = driver->tx_limit;
  if (count > free_slots) count = free_slots;
  for (size_t i = 0; i < count; ++i)
    driver->tx_held[driver->tx_count++] = frames[i];
  return count;
}

static size_t fake_rx(void *context, packet_io_frame *frames, size_t count) {
  fake_driver *driver = (fake_driver *)context;
  if (count > driver->rx_count) count = driver->rx_count;
  for (size_t i = 0; i < count; ++i) frames[i] = driver->rx_ready[i];
  memmove(driver->rx_ready, driver->rx_ready + count,
          (driver->rx_count - count) * sizeof(*frames));
  driver->rx_count -= count;
  return count;
}

static bool fake_quiescent(void *context) {
  fake_driver *driver = (fake_driver *)context;
  return driver->tx_count == 0 && driver->rx_count == 0;
}

static void complete_tx(fake_driver *driver) {
  for (size_t i = 0; i < driver->tx_count; ++i) {
    packet_io_frame *f = &driver->tx_held[i];
    f->release(f->release_context, f->tag);
  }
  driver->tx_count = 0;
}

static packet_io_driver fake_ops(void) {
  packet_io_driver ops = {fake_tx, fake_rx, fake_quiescent};
  return ops;
}

static int test_partial_capacity_and_drain(void) {
  packet_io_owner owner = {0};
  packet_io_frame slots[3];
  packet_io_ticket ticket = {0};
  fake_driver driver = {.tx_limit = 1};
  release_probe probe = {0};
  CHECK(packet_io_start(&owner, slots, 3, fake_ops(), &driver, 11, &ticket) == PACKET_IO_OK);
  for (uint64_t i = 0; i < 3; ++i) {
    packet_io_frame f = frame(&probe, i);
    CHECK(packet_io_submit(&owner, ticket, &f) == PACKET_IO_OK);
    CHECK(f.data == NULL); /* successful move */
  }
  packet_io_frame rejected = frame(&probe, 3);
  CHECK(packet_io_submit(&owner, ticket, &rejected) == PACKET_IO_FULL);
  CHECK(rejected.data != NULL && probe.released[3] == 0); /* rejection owns nothing */
  size_t tx = 0, rx = 0;
  CHECK(packet_io_progress(&owner, ticket, 3, 0, NULL, NULL, &tx, &rx) == PACKET_IO_OK);
  CHECK(tx == 1 && owner.pending == 2 && driver.tx_count == 1);
  CHECK(packet_io_progress(&owner, ticket, 3, 0, NULL, NULL, &tx, &rx) == PACKET_IO_OK);
  CHECK(tx == 1 && owner.pending == 1 && driver.tx_count == 2);
  CHECK(packet_io_seal(&owner, ticket) == PACKET_IO_OK);
  CHECK(packet_io_stop(&owner, ticket) == PACKET_IO_BUSY);
  CHECK(packet_io_progress(&owner, ticket, 2, 0, NULL, NULL, &tx, &rx) == PACKET_IO_OK);
  CHECK(tx == 1 && owner.pending == 0);
  CHECK(packet_io_drain(&owner, ticket) == PACKET_IO_BUSY); /* driver holds DMA */
  CHECK(probe.released[0] == 0);
  complete_tx(&driver);
  CHECK(packet_io_stop(&owner, ticket) == PACKET_IO_OK);
  for (uint64_t i = 0; i < 3; ++i) CHECK(probe.released[i] == 1);
  CHECK(probe.released[3] == 0);
  rejected.release(rejected.release_context, rejected.tag); /* caller-owned */
  return 0;
}

static int test_abort_and_generation(void) {
  packet_io_owner owner = {0};
  packet_io_frame slots[2];
  packet_io_ticket first = {0}, second = {0};
  fake_driver driver = {.tx_limit = 1};
  release_probe probe = {0};
  CHECK(packet_io_start(&owner, slots, 2, fake_ops(), &driver, 7, &first) == PACKET_IO_OK);
  packet_io_frame a = frame(&probe, 5), b = frame(&probe, 6);
  CHECK(packet_io_submit(&owner, first, &a) == PACKET_IO_OK);
  CHECK(packet_io_submit(&owner, first, &b) == PACKET_IO_OK);
  CHECK(packet_io_abort_staged(&owner, first) == PACKET_IO_INVALID); /* seal required */
  CHECK(packet_io_seal(&owner, first) == PACKET_IO_OK);
  CHECK(packet_io_submit(&owner, first, &a) == PACKET_IO_SEALED);
  CHECK(packet_io_abort_staged(&owner, first) == PACKET_IO_OK);
  CHECK(probe.released[5] == 1 && probe.released[6] == 1);
  CHECK(packet_io_stop(&owner, first) == PACKET_IO_OK);
  CHECK(packet_io_start(&owner, slots, 2, fake_ops(), &driver, 7, &second) == PACKET_IO_STALE);
  CHECK(packet_io_start(&owner, slots, 2, fake_ops(), &driver, 8, &second) == PACKET_IO_OK);
  packet_io_frame c = frame(&probe, 7);
  CHECK(packet_io_submit(&owner, first, &c) == PACKET_IO_STALE && c.data);
  CHECK(packet_io_submit(&owner, second, &c) == PACKET_IO_OK);
  CHECK(packet_io_seal(&owner, second) == PACKET_IO_OK);
  CHECK(packet_io_abort_staged(&owner, second) == PACKET_IO_OK);
  CHECK(packet_io_stop(&owner, second) == PACKET_IO_OK);
  CHECK(probe.released[7] == 1);
  return 0;
}

static void observe_rx(void *context, const packet_io_frame *f) {
  release_probe *probe = (release_probe *)context;
  assert(f->size > 0 && f->data != NULL);
  ++probe->delivered;
  probe->callback_seal = packet_io_seal(probe->owner, probe->ticket);
}

static int test_receive_borrow_and_reentrancy(void) {
  packet_io_owner owner = {0};
  packet_io_frame slots[2];
  packet_io_ticket ticket = {0};
  fake_driver driver = {.tx_limit = 2};
  release_probe probe = {0};
  CHECK(packet_io_start(&owner, slots, 2, fake_ops(), &driver, 12, &ticket) == PACKET_IO_OK);
  probe.owner = &owner;
  probe.ticket = ticket;
  driver.rx_ready[0] = frame(&probe, 10);
  driver.rx_ready[1] = frame(&probe, 11);
  driver.rx_count = 2;
  size_t tx = 0, rx = 0;
  CHECK(packet_io_progress(&owner, ticket, 0, 1, NULL, NULL, &tx, &rx) == PACKET_IO_INVALID);
  CHECK(driver.rx_count == 2);
  CHECK(packet_io_progress(&owner, ticket, 0, 2, observe_rx, &probe, &tx, &rx) == PACKET_IO_OK);
  CHECK(tx == 0 && rx == 2 && probe.delivered == 2);
  CHECK(probe.callback_seal == PACKET_IO_BUSY); /* cannot reenter active owner */
  CHECK(probe.released[10] == 1 && probe.released[11] == 1);
  CHECK(packet_io_seal(&owner, ticket) == PACKET_IO_OK);
  CHECK(packet_io_stop(&owner, ticket) == PACKET_IO_OK);
  return 0;
}

static int test_driver_violation_leaves_staging(void) {
  packet_io_owner owner = {0};
  packet_io_frame slots[1];
  packet_io_ticket ticket = {0};
  fake_driver driver = {.invalid_tx_count = true};
  release_probe probe = {0};
  CHECK(packet_io_start(&owner, slots, 1, fake_ops(), &driver, 1, &ticket) == PACKET_IO_OK);
  packet_io_frame f = frame(&probe, 20);
  CHECK(packet_io_submit(&owner, ticket, &f) == PACKET_IO_OK);
  size_t tx = 88, rx = 88;
  CHECK(packet_io_progress(&owner, ticket, 1, 0, NULL, NULL, &tx, &rx) == PACKET_IO_DRIVER_ERROR);
  CHECK(tx == 0 && rx == 0 && owner.pending == 1);
  CHECK(packet_io_seal(&owner, ticket) == PACKET_IO_OK);
  CHECK(packet_io_abort_staged(&owner, ticket) == PACKET_IO_OK);
  CHECK(probe.released[20] == 1);
  CHECK(packet_io_stop(&owner, ticket) == PACKET_IO_OK);
  return 0;
}

typedef struct worker_request {
  packet_io_owner *owner;
  packet_io_ticket ticket;
  packet_io_result result;
  packet_io_frame *frame;
} worker_request;

static void *wrong_owner_worker(void *value) {
  worker_request *request = (worker_request *)value;
  request->result = packet_io_submit(request->owner, request->ticket, request->frame);
  return NULL;
}

static int test_thread_affinity(void) {
  packet_io_owner owner = {0};
  packet_io_frame slots[1];
  packet_io_ticket ticket = {0};
  fake_driver driver = {.tx_limit = 1};
  release_probe probe = {0};
  CHECK(packet_io_start(&owner, slots, 1, fake_ops(), &driver, 1, &ticket) == PACKET_IO_OK);
  packet_io_frame f = frame(&probe, 21);
  worker_request req = {&owner, ticket, PACKET_IO_OK, &f};
  pthread_t worker;
  CHECK(pthread_create(&worker, NULL, wrong_owner_worker, &req) == 0);
  CHECK(pthread_join(worker, NULL) == 0);
  CHECK(req.result == PACKET_IO_WRONG_OWNER && f.data);
  CHECK(owner.pending == 0 && probe.released[21] == 0);
  CHECK(packet_io_seal(&owner, ticket) == PACKET_IO_OK);
  CHECK(packet_io_stop(&owner, ticket) == PACKET_IO_OK);
  f.release(f.release_context, f.tag);
  return 0;
}

static int test_fifo_wrap_and_zero_budget(void) {
  packet_io_owner owner = {0};
  packet_io_frame slots[2];
  packet_io_ticket ticket = {0};
  fake_driver driver = {.tx_limit = 1};
  release_probe probe = {0};
  CHECK(packet_io_start(&owner, slots, 2, fake_ops(), &driver, 27, &ticket) == PACKET_IO_OK);
  packet_io_frame a = frame(&probe, 30), b = frame(&probe, 31), c = frame(&probe, 32);
  CHECK(packet_io_submit(&owner, ticket, &a) == PACKET_IO_OK);
  CHECK(packet_io_submit(&owner, ticket, &b) == PACKET_IO_OK);
  size_t tx = 99, rx = 99;
  CHECK(packet_io_progress(&owner, ticket, 0, 0, NULL, NULL, &tx, &rx) == PACKET_IO_OK);
  CHECK(tx == 0 && rx == 0 && owner.pending == 2 && driver.tx_count == 0);
  CHECK(packet_io_progress(&owner, ticket, 1, 0, NULL, NULL, &tx, &rx) == PACKET_IO_OK);
  CHECK(tx == 1 && owner.pending == 1);
  CHECK(packet_io_submit(&owner, ticket, &c) == PACKET_IO_OK); /* wrap to slot 0 */
  CHECK(packet_io_progress(&owner, ticket, 2, 0, NULL, NULL, &tx, &rx) == PACKET_IO_OK);
  CHECK(tx == 1 && owner.pending == 1);
  CHECK(packet_io_progress(&owner, ticket, 2, 0, NULL, NULL, &tx, &rx) == PACKET_IO_OK);
  CHECK(tx == 1 && owner.pending == 0);
  CHECK(driver.tx_count == 3 && driver.tx_held[0].tag == 30 &&
        driver.tx_held[1].tag == 31 && driver.tx_held[2].tag == 32);
  CHECK(packet_io_seal(&owner, ticket) == PACKET_IO_OK);
  CHECK(packet_io_drain(&owner, ticket) == PACKET_IO_BUSY);
  complete_tx(&driver);
  CHECK(packet_io_stop(&owner, ticket) == PACKET_IO_OK);
  CHECK(probe.released[30] == 1 && probe.released[31] == 1 &&
        probe.released[32] == 1);
  return 0;
}

int main(void) {
  if (test_partial_capacity_and_drain()) return 1;
  if (test_abort_and_generation()) return 1;
  if (test_receive_borrow_and_reentrancy()) return 1;
  if (test_driver_violation_leaves_staging()) return 1;
  if (test_thread_affinity()) return 1;
  if (test_fifo_wrap_and_zero_budget()) return 1;
  puts("packet_io fake Owner conformance: PASS");
  return 0;
}
