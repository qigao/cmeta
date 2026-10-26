/* Test-only POSIX UDP datagram adapter for a truly independent Pion SCTP
 * association. It uses ONLY the installed opaque C11 Session public header.
 * UDP replaces a DTLS application's datagram transport for this isolated
 * negotiation/wire conformance fixture. It is NOT a production transport,
 * authentication provider, installed target, or M4 DTLS qualification. */
#define _POSIX_C_SOURCE 200809L
#include <cnet/sctp_session.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct oracle_context {
  uint32_t tag;
  uint32_t tsn;
  uint32_t key;
} oracle_context;

static volatile sig_atomic_t shutting_down = 0;
static void stop_signal(int signo) { (void)signo; shutting_down = 1; }
static int owner(void *context) { return context != NULL; }
static int entropy(void *context, uint8_t *p, size_t n) {
  const oracle_context *c = (const oracle_context *)context;
  if (!c || !p || n != 8u) return -1;
  p[0] = (uint8_t)(c->tag >> 24); p[1] = (uint8_t)(c->tag >> 16);
  p[2] = (uint8_t)(c->tag >> 8); p[3] = (uint8_t)c->tag;
  p[4] = (uint8_t)(c->tsn >> 24); p[5] = (uint8_t)(c->tsn >> 16);
  p[6] = (uint8_t)(c->tsn >> 8); p[7] = (uint8_t)c->tsn;
  return 0;
}
static int test_mac(void *context, const uint8_t *message, size_t n,
    uint8_t output[CNET_SCTP_SESSION_MAC_BYTES]) {
  const oracle_context *c = (const oracle_context *)context;
  uint32_t state;
  size_t i;
  if (!c || (!message && n != 0u) || !output) return -1;
  state = c->key;
  for (i = 0u; i < n; ++i)
    state = state * UINT32_C(65599) + message[i];
  for (i = 0u; i < CNET_SCTP_SESSION_MAC_BYTES; ++i) {
    state = state * UINT32_C(1664525) + UINT32_C(1013904223);
    output[i] = (uint8_t)(state >> 24);
  }
  return 0;
}
static int test_verify(void *context, const uint8_t *message, size_t n,
    const uint8_t expected[CNET_SCTP_SESSION_MAC_BYTES]) {
  uint8_t computed[CNET_SCTP_SESSION_MAC_BYTES];
  uint8_t bad = 0u;
  size_t i;
  if (!expected || test_mac(context, message, n, computed) != 0) return -1;
  for (i = 0u; i < sizeof(computed); ++i)
    bad |= (uint8_t)(expected[i] ^ computed[i]);
  return bad == 0u ? 0 : -1;
}
static uint64_t monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return UINT64_MAX;
  return (uint64_t)ts.tv_sec * UINT64_C(1000) +
      (uint64_t)(ts.tv_nsec / 1000000L);
}
static int publish(int fd, const struct sockaddr_in *destination,
    const void *bytes, size_t length) {
  if (length == 0u) return 0;
  if (!bytes || length > CNET_SCTP_SESSION_MAX_PACKET_BYTES) return -1;
  if (sendto(fd, bytes, length, 0, (const struct sockaddr *)destination,
          sizeof(*destination)) != (ssize_t)length)
    return -1;
  printf("TX_TYPE=%u\n", (unsigned)((const uint8_t *)bytes)[12]);
  fflush(stdout);
  return 0;
}
static void trace_event(const cnet_sctp_session_event *event,
    const uint8_t *body) {
  if (event->kind == CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN) {
    printf("CHANNEL_OPEN=%u\n", (unsigned)event->stream_id);
  } else if (event->kind == CNET_SCTP_SESSION_EVENT_MESSAGE) {
    size_t i;
    printf("MESSAGE=%u:", (unsigned)event->stream_id);
    for (i = 0u; i < event->message_size; ++i)
      printf("%02x", (unsigned)body[i]);
    putchar('\n');
  } else if (event->kind != CNET_SCTP_SESSION_EVENT_NONE) {
    printf("ULP_EVENT=%d\n", (int)event->kind);
  }
  fflush(stdout);
}
int main(int argc, char **argv) {
  oracle_context context = {
    UINT32_C(0x11223344), UINT32_C(0x10203040), UINT32_C(0x13579bdf)
  };
  cnet_sctp_session_create_config cfg = {0};
  cnet_sctp_session *session = NULL;
  struct sockaddr_in local = {0}, destination = {0};
  uint8_t packet[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t reply[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t body[CNET_SCTP_SESSION_MAX_MESSAGE_BYTES];
  uint64_t start, now;
  int fd = -1, failure = 0;
  unsigned int local_port, peer_port;
  if (argc != 3) {
    fprintf(stderr, "usage: pion_oracle_peer UDP_LISTEN_PORT UDP_PEER_PORT\n");
    return 2;
  }
  local_port = (unsigned int)strtoul(argv[1], NULL, 10);
  peer_port = (unsigned int)strtoul(argv[2], NULL, 10);
  if (local_port == 0u || peer_port == 0u ||
      local_port > UINT16_MAX || peer_port > UINT16_MAX ||
      local_port == peer_port) return 2;
  cfg.abi_version = CNET_SCTP_SESSION_ABI_VERSION;
  cfg.struct_size = sizeof(cfg);
  cfg.local_port = 5000u;
  cfg.remote_port = 5000u; /* Pion uses equal SCTP ports by default. */
  cfg.receive_window = 32768u;
  cfg.inbound_streams = cfg.outbound_streams = 4u;
  cfg.t1_ms = 100u;
  cfg.t1_max_ms = 400u;
  cfg.cookie_lifetime_ms = 5000u;
  cfg.peer_binding = UINT64_C(0x0123456789abcdef);
  cfg.max_retransmits = 5u;
  cfg.is_dtls_client = 0u; /* Pion DCEP client uses even SID=0. */
  cfg.is_owner = owner;
  cfg.entropy = entropy;
  cfg.mac = test_mac;
  cfg.verify_mac = test_verify;
  cfg.provider_context = &context;
  if (cnet_sctp_session_create(&cfg, 0u, &session) !=
      CNET_SCTP_SESSION_OK) {
    fprintf(stderr, "C11 oracle session create failed\n");
    return 3;
  }
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  local.sin_port = htons((uint16_t)local_port);
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  destination.sin_port = htons((uint16_t)peer_port);
  if (fd < 0 ||
      bind(fd, (const struct sockaddr *)&local, sizeof(local)) != 0) {
    fprintf(stderr, "C11 oracle UDP bind failed: %s\n", strerror(errno));
    failure = 4; goto done;
  }
  if (signal(SIGTERM, stop_signal) == SIG_ERR) { failure = 5; goto done; }
  start = monotonic_ms();
  if (start == UINT64_MAX) { failure = 6; goto done; }
  puts("READY");
  fflush(stdout);
  while (!shutting_down) {
    fd_set readfds;
    struct timeval wait = {0, 25000};
    int n;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    n = select(fd + 1, &readfds, NULL, NULL, &wait);
    if (n < 0 && errno != EINTR) {
      fprintf(stderr, "C11 oracle select failed: %s\n", strerror(errno));
      failure = 7; break;
    }
    now = monotonic_ms() - start;
    if (now > UINT64_C(15000)) {
      fprintf(stderr, "C11 oracle exceeded 15s bound\n");
      failure = 8; break;
    }
    if (n > 0 && FD_ISSET(fd, &readfds)) {
      const ssize_t received = recvfrom(fd, packet, sizeof(packet),
          0, NULL, NULL);
      size_t written = 0u;
      cnet_sctp_session_status status;
      if (received < 16) { failure = 9; break; }
      printf("RX_TYPE=%u\n", (unsigned)packet[12]);
      fflush(stdout);
      status = cnet_sctp_session_input(session, now, packet,
          (size_t)received, reply, sizeof(reply), &written);
      if (status != CNET_SCTP_SESSION_OK) {
        fprintf(stderr, "C11 oracle input refused: status=%d chunk=%u\n",
            (int)status, (unsigned)packet[12]);
        failure = 10; break;
      }
      if (publish(fd, &destination, reply, written) != 0) {
        failure = 11; break;
      }
      {
        unsigned int i;
        for (i = 0u; i < 16u; ++i) {
          cnet_sctp_session_event event = {0};
          written = 0u;
          status = cnet_sctp_session_poll(session, now, &event,
              body, sizeof(body), reply, sizeof(reply), &written);
          if (status == CNET_SCTP_SESSION_EMPTY ||
              status == CNET_SCTP_SESSION_CLOSED) break;
          if (status != CNET_SCTP_SESSION_OK) {
            fprintf(stderr, "C11 oracle poll refused: %d\n", (int)status);
            failure = 12; break;
          }
          trace_event(&event, body);
          if (publish(fd, &destination, reply, written) != 0) {
            failure = 13; break;
          }
        }
        if (failure != 0) break;
      }
    }
    {
      int has_deadline = 0;
      uint64_t due = 0u;
      cnet_sctp_session_status status =
          cnet_sctp_session_next_deadline(session, &due, &has_deadline);
      if (status != CNET_SCTP_SESSION_OK &&
          status != CNET_SCTP_SESSION_CLOSED) {
        fprintf(stderr, "C11 oracle deadline refused: %d\n", (int)status);
        failure = 14; break;
      }
      if (status == CNET_SCTP_SESSION_OK && has_deadline && due <= now) {
        size_t written = 0u;
        status = cnet_sctp_session_advance(session, now,
            reply, sizeof(reply), &written);
        if (status != CNET_SCTP_SESSION_OK) {
          fprintf(stderr, "C11 oracle advance refused: %d\n", (int)status);
          failure = 15; break;
        }
        if (publish(fd, &destination, reply, written) != 0) {
          failure = 16; break;
        }
      }
    }
  }
done:
  if (session) {
    if (cnet_sctp_session_stop(session) != CNET_SCTP_SESSION_OK ||
        !cnet_sctp_session_drained(session) ||
        cnet_sctp_session_destroy(session) != CNET_SCTP_SESSION_OK)
      failure = 17;
  }
  if (fd >= 0) close(fd);
  if (!failure) { puts("C11_ORACLE_EXIT_OK"); fflush(stdout); }
  return failure;
}
