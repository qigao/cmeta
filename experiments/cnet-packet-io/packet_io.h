#ifndef CNET_RESEARCH_PACKET_IO_H
#define CNET_RESEARCH_PACKET_IO_H

/* Experimental Linux-only SPI. Not part of the installed Salts/CNet ABI. */
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum packet_io_result {
  PACKET_IO_OK = 0,
  PACKET_IO_INVALID = 1,
  PACKET_IO_FULL = 2,
  PACKET_IO_WRONG_OWNER = 3,
  PACKET_IO_STALE = 4,
  PACKET_IO_SEALED = 5,
  PACKET_IO_BUSY = 6,
  PACKET_IO_DRIVER_ERROR = 7
} packet_io_result;

typedef void (*packet_io_release_fn)(void *context, uint64_t tag);

typedef struct packet_io_frame {
  const void *data;
  size_t size;
  uint64_t tag;
  packet_io_release_fn release;
  void *release_context;
} packet_io_frame;

/* tx_burst takes ownership of exactly the accepted prefix; the unaccepted
 * suffix stays caller-owned. This is queue admission, NOT a wire/peer ACK.
 * rx_burst moves ownership of each returned frame to its caller.
 * quiescent must remain false while driver/PMD may use any accepted frame. */
typedef struct packet_io_driver {
  size_t (*tx_burst)(void *context, const packet_io_frame *frames, size_t count);
  size_t (*rx_burst)(void *context, packet_io_frame *frames, size_t capacity);
  bool (*quiescent)(void *context);
} packet_io_driver;

typedef struct packet_io_ticket {
  uint64_t generation;
} packet_io_ticket;

typedef enum packet_io_state {
  PACKET_IO_EMPTY = 0,
  PACKET_IO_OPEN,
  PACKET_IO_DRAINING,
  PACKET_IO_STOPPED
} packet_io_state;

typedef struct packet_io_owner {
  packet_io_frame *queue; /* caller-supplied bounded array */
  size_t capacity;
  size_t head;
  size_t pending;
  packet_io_driver driver;
  void *driver_context;
  pthread_t thread;
  uint64_t generation;
  packet_io_state state;
  bool progressing;
} packet_io_owner;

/* No allocations in start, submit, progress, drain or stop. The zeroed owner
 * and queue storage remain address-stable until stop succeeds. Re-starting a
 * stopped owner requires a strictly greater generation. */
packet_io_result packet_io_start(packet_io_owner *owner, packet_io_frame *slots,
                                 size_t capacity, packet_io_driver driver,
                                 void *driver_context, uint64_t generation,
                                 packet_io_ticket *out_ticket);

/* Successfully admitted input frame is move-owned; its source is cleared.
 * Rejection leaves the source untouched. Callback release occurs once, after
 * the owner aborts its staging or the driver safely reclaims an accepted TX. */
packet_io_result packet_io_submit(packet_io_owner *owner, packet_io_ticket ticket,
                                  packet_io_frame *frame);

typedef void (*packet_io_receive_fn)(void *context, const packet_io_frame *frame);

/* Caller-driven, nonblocking and owner-thread-only. RX views live only inside
 * on_receive; this layer releases the inbound driver frame after callback.
 * RX cannot be requested without a callback; the receive path is not a retained
 * loan or a zero-copy GPU contract. Reentrant owner calls are rejected. */
packet_io_result packet_io_progress(packet_io_owner *owner, packet_io_ticket ticket,
                                    size_t tx_budget, size_t rx_budget,
                                    packet_io_receive_fn on_receive, void *receive_context,
                                    size_t *out_tx, size_t *out_rx);

packet_io_result packet_io_seal(packet_io_owner *owner, packet_io_ticket ticket);
/* Discard staged, NOT-yet-driver-accepted TX frames. Safe only after seal. */
packet_io_result packet_io_abort_staged(packet_io_owner *owner, packet_io_ticket ticket);
/* BUSY until staging is empty AND driver reports all outstanding DMA safe. */
packet_io_result packet_io_drain(packet_io_owner *owner, packet_io_ticket ticket);
packet_io_result packet_io_stop(packet_io_owner *owner, packet_io_ticket ticket);

#endif
