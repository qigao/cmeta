/*
 * Explicit, opt-in TWO-PHYSICAL-PCI-ETHERNET-PORT qualification.
 * Requires two dedicated VFIO-bound physical PFs with a cabled link.
 *
 * This does NOT establish peer reliability, real DSO unload correctness,
 * off-owner driver callback semantics or comparative performance.
 *
 * Critical ownership invariant: rte_eth_tx_burst() transfers the driver's
 * mbuf reference on acceptance, NOT remote ACK. We keep an observer ref,
 * request driver-supported rte_eth_tx_done_cleanup(), and only release the
 * observer after the driver has dropped its reference. ENOTSUP is a FAIL.
 */
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HARDWARE_PACKET_COUNT 16u
#define HARDWARE_PACKET_BYTES 128u
#define HARDWARE_ETHER_TYPE 0x88b5u

static const uint8_t stamp[4] = {'R', '2', 'H', 'W'};

static int find_physical_port(const char *bdf, uint16_t *out) {
  uint16_t port;
  RTE_ETH_FOREACH_DEV(port) {
    char name[RTE_ETH_NAME_MAX_LEN] = {0};
    if (rte_eth_dev_get_name_by_port(port, name) != 0) return -1;
    if (strcmp(name, bdf) != 0) continue;
    struct rte_eth_dev_info info = {0};
    if (rte_eth_dev_info_get(port, &info) != 0 ||
        !info.device || !info.driver_name) return -1;
    printf("HARDWARE_PORT: id=%u pci=%s driver=%s socket=%d\n",
           (unsigned)port, name, info.driver_name, rte_eth_dev_socket_id(port));
    *out = port;
    return 0;
  }
  fprintf(stderr, "HARDWARE_FAIL: DPDK has no exact PCI port %s\n", bdf);
  return -1;
}

static int configure_port(uint16_t port, struct rte_mempool *rx_pool) {
  struct rte_eth_conf config = {0};
  uint16_t rx_desc = 256, tx_desc = 256;
  int socket = rte_eth_dev_socket_id(port);
  if (socket < 0) socket = SOCKET_ID_ANY;
  if (rte_eth_dev_configure(port, 1, 1, &config) != 0) return -1;
  if (rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_desc, &tx_desc) != 0) return -1;
  if (rte_eth_rx_queue_setup(port, 0, rx_desc, (unsigned)socket, NULL, rx_pool) != 0)
    return -1;
  if (rte_eth_tx_queue_setup(port, 0, tx_desc, (unsigned)socket, NULL) != 0)
    return -1;
  if (rte_eth_dev_start(port) != 0) return -1;
  return 0;
}

static int require_link(uint16_t port) {
  for (unsigned attempt = 0; attempt < 5000; ++attempt) {
    struct rte_eth_link link = {0};
    if (rte_eth_link_get_nowait(port, &link) != 0) return -1;
    if (link.link_status == RTE_ETH_LINK_UP && link.link_speed > 0) {
      printf("HARDWARE_LINK: port=%u speed_mbps=%u duplex=%u\n",
             (unsigned)port, link.link_speed, link.link_duplex);
      return 0;
    }
    rte_delay_us_sleep(1000);
  }
  fprintf(stderr, "HARDWARE_FAIL: link DOWN on port %u\n", (unsigned)port);
  return -1;
}

static void payload(uint8_t *dest, const struct rte_ether_addr *dst,
                    const struct rte_ether_addr *src,
                    uint32_t nonce, unsigned sequence) {
  memset(dest, 0, HARDWARE_PACKET_BYTES);
  memcpy(dest, dst->addr_bytes, RTE_ETHER_ADDR_LEN);
  memcpy(dest + RTE_ETHER_ADDR_LEN, src->addr_bytes, RTE_ETHER_ADDR_LEN);
  dest[12] = (uint8_t)(HARDWARE_ETHER_TYPE >> 8);
  dest[13] = (uint8_t)HARDWARE_ETHER_TYPE;
  memcpy(dest + 14, stamp, sizeof(stamp));
  dest[18] = (uint8_t)(nonce >> 24);
  dest[19] = (uint8_t)(nonce >> 16);
  dest[20] = (uint8_t)(nonce >> 8);
  dest[21] = (uint8_t)nonce;
  dest[22] = (uint8_t)(sequence >> 8);
  dest[23] = (uint8_t)sequence;
  for (unsigned i = 24; i < HARDWARE_PACKET_BYTES; ++i)
    dest[i] = (uint8_t)(sequence ^ i ^ 0x5au);
}

/* All NIC queue calls (TX/RX/cleanup) occur on this one EAL Owner. */
static int exercise_direction(uint16_t tx_port, uint16_t rx_port,
                              struct rte_mempool *tx_pool,
                              uint32_t nonce) {
  struct rte_ether_addr tx_mac = {{0}}, rx_mac = {{0}};
  struct rte_mbuf *observed[HARDWARE_PACKET_COUNT] = {0};
  uint32_t tx_before = rte_mempool_avail_count(tx_pool);
  unsigned received = 0;
  uint32_t received_mask = 0;
  unsigned ignored = 0;
  int verdict = -1;

  if (rte_eth_macaddr_get(tx_port, &tx_mac) != 0 ||
      rte_eth_macaddr_get(rx_port, &rx_mac) != 0)
    goto done;

  /* A cleanup-capable PMD is mandatory for the observable refcount gate.
   * -ENOTSUP/-ENODEV/etc are hard failures, NEVER a software fallback. */
  int capability = rte_eth_tx_done_cleanup(tx_port, 0, 0);
  if (capability < 0) {
    fprintf(stderr, "HARDWARE_FAIL: tx_done_cleanup unsupported on %u: %d\n",
            (unsigned)tx_port, capability);
    goto done;
  }

  for (unsigned seq = 0; seq < HARDWARE_PACKET_COUNT; ++seq) {
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(tx_pool);
    if (!mbuf) {
      fprintf(stderr, "HARDWARE_FAIL: TX pool exhausted\n");
      goto done;
    }
    uint8_t *bytes = (uint8_t *)rte_pktmbuf_append(mbuf, HARDWARE_PACKET_BYTES);
    if (!bytes) { rte_pktmbuf_free(mbuf); goto done; }
    payload(bytes, &rx_mac, &tx_mac, nonce, seq);
    /* Keep one caller observer ref until driver's TX ref is reclaimed. */
    rte_mbuf_refcnt_update(mbuf, 1);
    if (rte_eth_tx_burst(tx_port, 0, &mbuf, 1) != 1) {
      /* Not accepted: the caller owns BOTH references. */
      rte_pktmbuf_free(mbuf);
      rte_pktmbuf_free(mbuf);
      fprintf(stderr, "HARDWARE_FAIL: physical TX queue short admission at %u\n", seq);
      goto done;
    }
    observed[seq] = mbuf;
  }

  const uint64_t hz = rte_get_timer_hz();
  uint64_t deadline = rte_get_timer_cycles() + 8u * hz;
  while (received < HARDWARE_PACKET_COUNT && rte_get_timer_cycles() < deadline) {
    struct rte_mbuf *rx[32];
    uint16_t count = rte_eth_rx_burst(rx_port, 0, rx, 32);
    for (uint16_t i = 0; i < count; ++i) {
      struct rte_mbuf *mbuf = rx[i];
      if (rte_pktmbuf_pkt_len(mbuf) == HARDWARE_PACKET_BYTES &&
          rte_pktmbuf_is_contiguous(mbuf)) {
        uint8_t *p = rte_pktmbuf_mtod(mbuf, uint8_t *);
        uint32_t found_nonce = ((uint32_t)p[18] << 24) |
                               ((uint32_t)p[19] << 16) |
                               ((uint32_t)p[20] << 8) | p[21];
        unsigned seq = ((unsigned)p[22] << 8) | p[23];
        if (p[12] == (HARDWARE_ETHER_TYPE >> 8) &&
            p[13] == (HARDWARE_ETHER_TYPE & 255u) &&
            memcmp(p + 14, stamp, sizeof(stamp)) == 0 &&
            found_nonce == nonce && seq < HARDWARE_PACKET_COUNT &&
            memcmp(p, rx_mac.addr_bytes, RTE_ETHER_ADDR_LEN) == 0 &&
            memcmp(p + 6, tx_mac.addr_bytes, RTE_ETHER_ADDR_LEN) == 0) {
          uint8_t reference[HARDWARE_PACKET_BYTES];
          payload(reference, &rx_mac, &tx_mac, nonce, seq);
          if (memcmp(reference, p, HARDWARE_PACKET_BYTES) != 0 ||
              (received_mask & (1u << seq))) {
            fprintf(stderr, "HARDWARE_FAIL: corrupt or duplicate physical frame\n");
            rte_pktmbuf_free(mbuf);
            goto done;
          }
          received_mask |= 1u << seq;
          ++received;
        } else ++ignored;
      } else ++ignored;
      rte_pktmbuf_free(mbuf);
    }
    if (received < HARDWARE_PACKET_COUNT) rte_delay_us_sleep(1000);
  }
  if (received != HARDWARE_PACKET_COUNT) {
    fprintf(stderr, "HARDWARE_FAIL: only %u/%u physical frames received\n",
            received, HARDWARE_PACKET_COUNT);
    goto done;
  }

  /* Cleanup only on the same EAL Owner. Driver must drop its TX reference;
   * a separate observer ref prevents mbuf recycling until Owner releases it.
   * This proves a DPDK-driver ref was reclaimed, not IOMMU electrical safety. */
  deadline = rte_get_timer_cycles() + 8u * hz;
  for (;;) {
    int cleaned = rte_eth_tx_done_cleanup(tx_port, 0, 0);
    if (cleaned < 0) {
      fprintf(stderr, "HARDWARE_FAIL: PMD cleanup returned %d\n", cleaned);
      goto done;
    }
    unsigned outstanding = 0;
    for (unsigned i = 0; i < HARDWARE_PACKET_COUNT; ++i)
      if (observed[i] && rte_mbuf_refcnt_read(observed[i]) != 1) ++outstanding;
    if (!outstanding) break;
    if (rte_get_timer_cycles() >= deadline) {
      fprintf(stderr, "HARDWARE_FAIL: %u TX mbufs still driver-owned after deadline\n",
              outstanding);
      goto done;
    }
    rte_delay_us_sleep(1000);
  }

  for (unsigned i = 0; i < HARDWARE_PACKET_COUNT; ++i) {
    rte_pktmbuf_free(observed[i]);
    observed[i] = NULL;
  }
  if (rte_mempool_avail_count(tx_pool) != tx_before) {
    fprintf(stderr, "HARDWARE_FAIL: TX mempool refs did not return to baseline\n");
    goto done;
  }

  printf("HARDWARE_DIRECTION: tx=%u rx=%u admitted=%u wire_rx=%u "
         "pmddriver_ref_reclaimed=%u observer_refs_released=%u background_ignored=%u\n",
         (unsigned)tx_port, (unsigned)rx_port,
         HARDWARE_PACKET_COUNT, received, HARDWARE_PACKET_COUNT,
         HARDWARE_PACKET_COUNT, ignored);
  verdict = 0;
done:
  /* If the PMD has not proved release, retain the observer references until
   * device shutdown; never call a completion callback on unknown DMA state. */
  return verdict;
}

int main(int argc, char **argv) {
  if (argc != 5 ||
      strcmp(argv[4], "I_ACKNOWLEDGE_DEDICATED_OFFLINE_NICS") != 0 ||
      strcmp(argv[1], argv[2]) == 0) {
    fprintf(stderr, "HARDWARE_FAIL: requires tx-BDF rx-BDF lcore explicit-ack\n");
    return 2;
  }
  char prefix[80];
  snprintf(prefix, sizeof(prefix), "--file-prefix=r2_hw_%ld", (long)getpid());
  char *eal_argv[] = {
    (char *)"packet_io_physical", (char *)"-l", argv[3],
    (char *)"-a", argv[1], (char *)"-a", argv[2],
    prefix, (char *)"--no-telemetry"
  };
  int eal_argc = (int)(sizeof(eal_argv) / sizeof(eal_argv[0]));
  if (rte_eal_init(eal_argc, eal_argv) < 0) {
    fprintf(stderr, "HARDWARE_FAIL: EAL could not initialize both VFIO NICs\n");
    return 3;
  }
  int result = 1;
  uint16_t tx_port = 0, rx_port = 0;
  bool tx_started = false, rx_started = false, ports_resolved = false;
  struct rte_mempool *tx_pool = NULL, *rx_pool = NULL;
  if (rte_eth_dev_count_avail() != 2 ||
      find_physical_port(argv[1], &tx_port) != 0 ||
      find_physical_port(argv[2], &rx_port) != 0 ||
      tx_port == rx_port)
    goto cleanup;
  ports_resolved = true;
  int socket = rte_socket_id();
  rx_pool = rte_pktmbuf_pool_create("r2_hw_rx", 2048, 32, 0,
                                   RTE_MBUF_DEFAULT_BUF_SIZE, socket);
  tx_pool = rte_pktmbuf_pool_create("r2_hw_tx", 1024, 32, 0,
                                   RTE_MBUF_DEFAULT_BUF_SIZE, socket);
  if (!rx_pool || !tx_pool) goto cleanup;
  if (configure_port(tx_port, rx_pool) != 0) goto cleanup;
  tx_started = true;
  if (configure_port(rx_port, rx_pool) != 0) goto cleanup;
  rx_started = true;
  if (require_link(tx_port) != 0 || require_link(rx_port) != 0) goto cleanup;
  if (exercise_direction(tx_port, rx_port, tx_pool, UINT32_C(0xbac01234)) != 0)
    goto cleanup;
  if (exercise_direction(rx_port, tx_port, tx_pool, UINT32_C(0xbac05678)) != 0)
    goto cleanup;
  result = 0;
cleanup:
  if (rx_started && rte_eth_dev_stop(rx_port) != 0) result = 1;
  if (tx_started && rte_eth_dev_stop(tx_port) != 0) result = 1;
  /* Even an unsuccessful test must not claim cleanup: device stop/close
   * is a best-effort emergency barrier, not a successful TX completion. */
  if (ports_resolved) {
    if (rte_eth_dev_close(rx_port) != 0) result = 1;
    if (rte_eth_dev_close(tx_port) != 0) result = 1;
  }
  if (result == 0 && tx_pool && rx_pool) {
    /* Only after SUCCESSFUL port close may RX descriptor pools be assessed.
     * Actual pool accounting must have no hidden retained DMA references. */
    uint32_t rx_free = rte_mempool_avail_count(rx_pool);
    uint32_t tx_free = rte_mempool_avail_count(tx_pool);
    if (rx_free != rx_pool->size || tx_free != tx_pool->size) {
      fprintf(stderr,
              "HARDWARE_FAIL: final pool baseline mismatch RX %u/%u TX %u/%u\n",
              rx_free, rx_pool->size, tx_free, tx_pool->size);
      result = 1;
    } else {
      rte_mempool_free(rx_pool);
      rte_mempool_free(tx_pool);
    }
  }
  /* A failed test retains unresolved observer references; do not guess that
   * physical DMA is safe merely because the process is exiting. */
  if (rte_eal_cleanup() < 0) result = 1;
  if (result == 0)
    puts("HARDWARE_PMD_SMOKE_PASS: physical two-port bidirectional RX and "
         "supported tx_done_cleanup refcount observability; NOT an R2 benchmark");
  else fprintf(stderr, "HARDWARE_PMD_SMOKE_FAIL: NOT QUALIFIED\n");
  return result;
}
