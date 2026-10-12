#include "packet_io.h"

#include <string.h>

static bool valid_frame(const packet_io_frame *frame) {
  return frame && frame->data && frame->size && frame->release;
}

static packet_io_result check_owner(packet_io_owner *owner, packet_io_ticket ticket) {
  if (!owner || owner->state == PACKET_IO_EMPTY) return PACKET_IO_INVALID;
  if (!pthread_equal(owner->thread, pthread_self())) return PACKET_IO_WRONG_OWNER;
  if (owner->generation != ticket.generation || owner->state == PACKET_IO_STOPPED)
    return PACKET_IO_STALE;
  if (owner->progressing) return PACKET_IO_BUSY;
  return PACKET_IO_OK;
}

packet_io_result packet_io_start(packet_io_owner *owner, packet_io_frame *slots,
                                 size_t capacity, packet_io_driver driver,
                                 void *driver_context, uint64_t generation,
                                 packet_io_ticket *out_ticket) {
  if (!owner || !slots || !out_ticket || capacity == 0 ||
      capacity > SIZE_MAX / sizeof(*slots) || generation == 0 ||
      !driver.tx_burst || !driver.rx_burst || !driver.quiescent)
    return PACKET_IO_INVALID;
  if (owner->state != PACKET_IO_EMPTY && owner->state != PACKET_IO_STOPPED)
    return PACKET_IO_BUSY;
  if (generation <= owner->generation) return PACKET_IO_STALE;
  memset(slots, 0, capacity * sizeof(*slots));
  owner->queue = slots;
  owner->capacity = capacity;
  owner->head = 0;
  owner->pending = 0;
  owner->driver = driver;
  owner->driver_context = driver_context;
  owner->thread = pthread_self();
  owner->generation = generation;
  owner->state = PACKET_IO_OPEN;
  owner->progressing = false;
  out_ticket->generation = generation;
  return PACKET_IO_OK;
}

packet_io_result packet_io_submit(packet_io_owner *owner, packet_io_ticket ticket,
                                  packet_io_frame *frame) {
  packet_io_result status = check_owner(owner, ticket);
  if (status != PACKET_IO_OK) return status;
  if (owner->state != PACKET_IO_OPEN) return PACKET_IO_SEALED;
  if (!valid_frame(frame)) return PACKET_IO_INVALID;
  if (owner->pending == owner->capacity) return PACKET_IO_FULL;
  size_t tail = (owner->head + owner->pending) % owner->capacity;
  owner->queue[tail] = *frame;
  memset(frame, 0, sizeof(*frame));
  ++owner->pending;
  return PACKET_IO_OK;
}

packet_io_result packet_io_progress(packet_io_owner *owner, packet_io_ticket ticket,
                                    size_t tx_budget, size_t rx_budget,
                                    packet_io_receive_fn on_receive, void *receive_context,
                                    size_t *out_tx, size_t *out_rx) {
  if (!out_tx || !out_rx) return PACKET_IO_INVALID;
  *out_tx = 0;
  *out_rx = 0;
  packet_io_result status = check_owner(owner, ticket);
  if (status != PACKET_IO_OK) return status;
  if (rx_budget && !on_receive) return PACKET_IO_INVALID;
  owner->progressing = true;
  if (tx_budget && owner->pending) {
    size_t offer = owner->capacity - owner->head;
    if (offer > owner->pending) offer = owner->pending;
    if (offer > tx_budget) offer = tx_budget;
    size_t accepted = owner->driver.tx_burst(owner->driver_context,
                                              &owner->queue[owner->head], offer);
    if (accepted > offer) {
      status = PACKET_IO_DRIVER_ERROR;
      goto done;
    }
    for (size_t i = 0; i < accepted; ++i) {
      memset(&owner->queue[owner->head], 0, sizeof(packet_io_frame));
      owner->head = (owner->head + 1u) % owner->capacity;
      --owner->pending;
    }
    *out_tx = accepted;
  }
  while (rx_budget) {
    packet_io_frame frames[8] = {{0}};
    size_t offer = rx_budget < 8u ? rx_budget : 8u;
    size_t received = owner->driver.rx_burst(owner->driver_context, frames, offer);
    if (received > offer) { /* driver contract violation: must not overrun */
      status = PACKET_IO_DRIVER_ERROR;
      goto done;
    }
    for (size_t i = 0; i < received; ++i) {
      if (!valid_frame(&frames[i])) {
        /* The driver's returned descriptors are malformed. Release any
         * valid ownership that it transferred, but never call an absent
         * release callback or deliver an invalid borrow to the consumer. */
        status = PACKET_IO_DRIVER_ERROR;
      } else if (status == PACKET_IO_OK) {
        on_receive(receive_context, &frames[i]);
        ++*out_rx;
      }
      if (frames[i].release) frames[i].release(frames[i].release_context, frames[i].tag);
    }
    if (status != PACKET_IO_OK || received != offer) break;
    rx_budget -= received;
  }
done:
  owner->progressing = false;
  return status;
}

packet_io_result packet_io_seal(packet_io_owner *owner, packet_io_ticket ticket) {
  packet_io_result status = check_owner(owner, ticket);
  if (status != PACKET_IO_OK) return status;
  owner->state = PACKET_IO_DRAINING;
  return PACKET_IO_OK;
}

packet_io_result packet_io_abort_staged(packet_io_owner *owner, packet_io_ticket ticket) {
  packet_io_result status = check_owner(owner, ticket);
  if (status != PACKET_IO_OK) return status;
  if (owner->state != PACKET_IO_DRAINING) return PACKET_IO_INVALID;
  owner->progressing = true;
  while (owner->pending) {
    packet_io_frame frame = owner->queue[owner->head];
    memset(&owner->queue[owner->head], 0, sizeof(packet_io_frame));
    owner->head = (owner->head + 1u) % owner->capacity;
    --owner->pending;
    frame.release(frame.release_context, frame.tag);
  }
  owner->progressing = false;
  return PACKET_IO_OK;
}

packet_io_result packet_io_drain(packet_io_owner *owner, packet_io_ticket ticket) {
  packet_io_result status = check_owner(owner, ticket);
  if (status != PACKET_IO_OK) return status;
  if (owner->state != PACKET_IO_DRAINING) return PACKET_IO_INVALID;
  if (owner->pending || !owner->driver.quiescent(owner->driver_context))
    return PACKET_IO_BUSY;
  return PACKET_IO_OK;
}

packet_io_result packet_io_stop(packet_io_owner *owner, packet_io_ticket ticket) {
  packet_io_result status = packet_io_drain(owner, ticket);
  if (status != PACKET_IO_OK) return status;
  owner->state = PACKET_IO_STOPPED;
  return PACKET_IO_OK;
}
