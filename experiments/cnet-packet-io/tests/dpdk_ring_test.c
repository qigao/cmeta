/* Optional, real rte_ethdev net_ring integration; no physical NIC and no
 * claims about DMA completion, wire latency, or reliable peer delivery. */
#include "../packet_io.h"

#include <rte_eal.h>
#include <rte_eth_ring.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* This test only targets one explicitly shared loopback software ring. */
typedef struct dpdk_ring_driver {
  uint16_t port;
  struct rte_mempool *pool;
  size_t inflight;
  unsigned copied_transmits;
  unsigned observed_receives;
} dpdk_ring_driver;

typedef struct ring_probe {
  unsigned released;
  unsigned received;
} ring_probe;

static void tx_payload_released(void *context, uint64_t tag) {
  ring_probe *probe = (ring_probe *)context;
  if (tag == 42u) ++probe->released;
}

static void rx_mbuf_released(void *context, uint64_t tag) {
  dpdk_ring_driver *driver = (dpdk_ring_driver *)context;
  struct rte_mbuf *mbuf = (struct rte_mbuf *)(uintptr_t)tag;
  /* Actual test-owned ring semantics: RX return transfers this same mbuf. */
  if (driver->inflight) --driver->inflight;
  rte_pktmbuf_free(mbuf);
}

static size_t ring_tx(void *context, const packet_io_frame *frames, size_t count) {
  dpdk_ring_driver *driver = (dpdk_ring_driver *)context;
  struct rte_mbuf *mbufs[8];
  size_t staged = 0;
  if (count > 8u) count = 8u;
  for (; staged < count; ++staged) {
    if (frames[staged].size > UINT16_MAX) break;
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(driver->pool);
    if (!mbuf) break;
    void *bytes = rte_pktmbuf_append(mbuf, (uint16_t)frames[staged].size);
    if (!bytes) { rte_pktmbuf_free(mbuf); break; }
    memcpy(bytes, frames[staged].data, frames[staged].size);
    mbufs[staged] = mbuf;
  }
  uint16_t accepted = staged ? rte_eth_tx_burst(driver->port, 0, mbufs, (uint16_t)staged) : 0;
  for (size_t i = 0; i < accepted; ++i) {
    ++driver->inflight;
    ++driver->copied_transmits;
    /* Safe to release caller's original bytes because they were COPIED into
     * independent rte_mbuf storage before NIC/PMD admission. This callback
     * is not the reliable-message ACK. */
    frames[i].release(frames[i].release_context, frames[i].tag);
  }
  for (size_t i = accepted; i < staged; ++i) rte_pktmbuf_free(mbufs[i]);
  return accepted;
}

static size_t ring_rx(void *context, packet_io_frame *frames, size_t capacity) {
  dpdk_ring_driver *driver = (dpdk_ring_driver *)context;
  struct rte_mbuf *mbufs[8];
  if (capacity > 8u) capacity = 8u;
  uint16_t received = rte_eth_rx_burst(driver->port, 0, mbufs, (uint16_t)capacity);
  for (uint16_t i = 0; i < received; ++i) {
    frames[i].data = rte_pktmbuf_mtod(mbufs[i], const void *);
    frames[i].size = rte_pktmbuf_pkt_len(mbufs[i]);
    frames[i].tag = (uint64_t)(uintptr_t)mbufs[i];
    frames[i].release = rx_mbuf_released;
    frames[i].release_context = driver;
    ++driver->observed_receives;
  }
  return received;
}

static bool ring_quiescent(void *context) {
  const dpdk_ring_driver *driver = (const dpdk_ring_driver *)context;
  /* net_ring loopback only: each accepted frame is received and freed in RX.
   * This is NOT a valid quiescence oracle for a real NIC PMD. */
  return driver->inflight == 0;
}

static void receive_frame(void *context, const packet_io_frame *frame) {
  ring_probe *probe = (ring_probe *)context;
  const uint8_t *bytes = (const uint8_t *)frame->data;
  if (frame->size == 64u && bytes[12] == 0x88u && bytes[13] == 0xb5u)
    ++probe->received;
}

int main(void) {
  char *eal_argv[] = {(char *)"packet_io_ring", (char *)"-l", (char *)"0",
                      (char *)"--no-huge", (char *)"--no-pci", (char *)"--no-telemetry",
                      (char *)"--file-prefix=cnet_packet_ring"};
  int eal_argc = (int)(sizeof(eal_argv) / sizeof(*eal_argv));
  if (rte_eal_init(eal_argc, eal_argv) < 0) {
    fprintf(stderr, "DPDK EAL unavailable: skip, not a successful PMD test\n");
    return 77;
  }
  int result = 1;
  struct rte_ring *ring = rte_ring_create("cnet_packet_ring", 256u, 0,
                                          RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!ring) goto cleanup_eal;
  struct rte_ring *queues[1] = {ring};
  int port = rte_eth_from_rings("net_ring0", queues, 1, queues, 1, 0);
  if (port < 0) goto cleanup_ring;
  struct rte_mempool *pool = rte_pktmbuf_pool_create("cnet_packet_pool", 256, 32, 0,
                                                      RTE_MBUF_DEFAULT_BUF_SIZE, 0);
  if (!pool) goto cleanup_port;
  struct rte_eth_conf config = {0};
  if (rte_eth_dev_configure((uint16_t)port, 1, 1, &config) != 0) goto cleanup_port;
  if (rte_eth_rx_queue_setup((uint16_t)port, 0, 128, 0, NULL, pool) != 0)
    goto cleanup_port;
  if (rte_eth_tx_queue_setup((uint16_t)port, 0, 128, 0, NULL) != 0)
    goto cleanup_port;
  if (rte_eth_dev_start((uint16_t)port) != 0) goto cleanup_port;
  dpdk_ring_driver driver = {.port = (uint16_t)port, .pool = pool};
  packet_io_driver driver_ops = {ring_tx, ring_rx, ring_quiescent};
  packet_io_owner owner = {0};
  packet_io_frame slots[4];
  packet_io_ticket ticket = {0};
  ring_probe probe = {0};
  if (packet_io_start(&owner, slots, 4, driver_ops, &driver, 1, &ticket) != PACKET_IO_OK)
    goto cleanup_started;
  uint8_t packet[64] = {0};
  packet[12] = 0x88; packet[13] = 0xb5; /* local experimental EtherType */
  packet_io_frame payload = {packet, sizeof(packet), 42, tx_payload_released, &probe};
  if (packet_io_submit(&owner, ticket, &payload) != PACKET_IO_OK) goto cleanup_started;
  size_t tx = 0, rx = 0;
  if (packet_io_progress(&owner, ticket, 1, 0, NULL, NULL, &tx, &rx) != PACKET_IO_OK ||
      tx != 1 || probe.released != 1 || driver.inflight != 1) goto cleanup_started;
  if (packet_io_seal(&owner, ticket) != PACKET_IO_OK ||
      packet_io_drain(&owner, ticket) != PACKET_IO_BUSY) goto cleanup_started;
  for (unsigned i = 0; i < 256 && !probe.received; ++i) {
    if (packet_io_progress(&owner, ticket, 0, 1, receive_frame, &probe, &tx, &rx) != PACKET_IO_OK)
      goto cleanup_started;
  }
  if (probe.received != 1 || driver.inflight != 0 ||
      packet_io_stop(&owner, ticket) != PACKET_IO_OK) goto cleanup_started;
  printf("DPDK net_ring ethdev: TX accepted=1, RX=1, reclaimed=1, peer ACK NOT tested\n");
  result = 0;
cleanup_started:
  rte_eth_dev_stop((uint16_t)port);
cleanup_port:
  rte_eth_dev_close((uint16_t)port);
cleanup_ring:
  rte_ring_free(ring);
cleanup_eal:
  rte_eal_cleanup();
  if (result) fprintf(stderr, "DPDK net_ring integration FAILED\n");
  return result;
}
