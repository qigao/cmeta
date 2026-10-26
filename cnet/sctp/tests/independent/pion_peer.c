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
/* The one named I-DATA is dropped only AFTER the real C11 sender has
 * accepted the TSN. No DCEP, SACK, FORWARD or successor is filtered.
 * This is a test-only UDP adapter, NOT an application cancellation API. */
static uint32_t dropped_tsn = 0u;
static int dropped_tsn_live = 0;
static uint32_t chunk_tsn(const uint8_t *wire) {
  return ((uint32_t)wire[16u] << 24u) |
      ((uint32_t)wire[17u] << 16u) |
      ((uint32_t)wire[18u] << 8u) | wire[19u];
}
static int emit_app(cnet_sctp_session *session, uint64_t now, int fd,
    const struct sockaddr_in *destination, const char *payload, int drop) {
  uint8_t output[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  size_t size = 0u;
  cnet_sctp_session_status status = cnet_sctp_session_send(session,
      now, 0u, 1, payload, strlen(payload), output, sizeof(output), &size);
  if (status != CNET_SCTP_SESSION_OK || size <= 12u ||
      output[12u] != 64u) {
    fprintf(stderr, "C11 oracle user egress failed %d/%zu\n",
        (int)status, size);
    return -1;
  }
  if (drop) {
    dropped_tsn = chunk_tsn(output);
    dropped_tsn_live = 1;
    puts("C11_DROPPED_ONE_I_DATA");
    fflush(stdout);
    return 0;
  }
  return publish(fd, destination, output, size);
}

int main(int argc, char **argv) {
  oracle_context context = {
    UINT32_C(0x11223344), UINT32_C(0x10203040), UINT32_C(0x13579bdf)
  };
  cnet_sctp_session_create_config cfg = {0};
  cnet_sctp_session *session = NULL;
  struct sockaddr_in local = {0}, destination = {0};
  uint64_t original_generation = 0u;
  unsigned int stage = 0u;
  int original_abandoned = 0;
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
          if (event.kind == CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN) {
            if (stage != 0u || event.stream_id != 0u ||
                event.generation == 0u ||
                emit_app(session, now, fd, &destination,
                    "c11-nominal", 0) != 0) {
              failure = 18; break;
            }
            original_generation = event.generation;
            stage = 1u;
            puts("C11_NOMINAL_SENT");
            fflush(stdout);
          }
          if (event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED) {
            if (stage < 2u || event.stream_id != 0u ||
                event.generation != original_generation ||
                event.abandon_reason != CNET_SCTP_SESSION_ABANDON_TIMED ||
                event.message_size != strlen("c11-drop")) {
              failure = 19; break;
            }
            original_abandoned = 1;
            puts("C11_LOCAL_ABANDONMENT");
            fflush(stdout);
          }
        }
        if (failure != 0) break;
      }
      if (stage == 1u) {
        cnet_sctp_session_tx_usage usage = {0};
        status = cnet_sctp_session_get_tx_usage(session, 0u,
            original_generation, &usage);
        if (status != CNET_SCTP_SESSION_OK) {
          failure = 20; break;
        }
        if (usage.retained_bytes == 0u && !usage.has_pending_message) {
          if (emit_app(session, now, fd, &destination,
                  "c11-drop", 1) != 0) {
            failure = 21; break;
          }
          stage = 2u;
        }
      }
      if (stage == 3u && original_abandoned) {
        cnet_sctp_session_forward_progress progress = {0};
        status = cnet_sctp_session_take_forward_progress(session, &progress);
        if (status == CNET_SCTP_SESSION_OK) {
          if (progress.generation != original_generation ||
              progress.stream_id != 0u ||
              progress.abandon_reason != CNET_SCTP_SESSION_ABANDON_TIMED ||
              progress.message_size != strlen("c11-drop") ||
              emit_app(session, now, fd, &destination,
                  "c11-after-forward", 0) != 0) {
            failure = 22; break;
          }
          stage = 4u;
          puts("C11_PEER_CONFIRMED_I_FORWARD");
          fflush(stdout);
        } else if (status != CNET_SCTP_SESSION_EMPTY) {
          failure = 23; break;
        }
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
        if (written != 0u && reply[12u] == 64u &&
            dropped_tsn_live) {
          /* T3 retransmissions of the IDENTICAL dropped TSN must not
           * accidentally deliver its payload before TIMED expiry. Do not
           * drop an unrelated TSN or a DCEP control packet. */
          if (stage != 2u || chunk_tsn(reply) != dropped_tsn) {
            failure = 26; break;
          }
          puts("C11_FILTERED_SAME_TSN_RETRANSMISSION");
          fflush(stdout);
        } else {
          if (written != 0u && reply[12u] == 194u) {
            if (stage != 2u || !dropped_tsn_live) {
              failure = 24; break;
            }
            dropped_tsn_live = 0;
            stage = 3u;
            puts("C11_REAL_I_FORWARD_TSN_194");
            fflush(stdout);
          } else if (written != 0u && reply[12u] == 192u) {
            fprintf(stderr,
                "C11 emitted legacy FORWARD instead of I-FORWARD\n");
            failure = 25; break;
          }
          if (publish(fd, &destination, reply, written) != 0) {
            failure = 16; break;
          }
        }
        /* Owner advance() can publish the local PR abandonment without any
         * incoming UDP packet. Drain its copied original-generation result
         * now, before trying to collect real Pion acknowledgement of the
         * I-FORWARD control. Never rely on a future ingress to poll events. */
        if (stage == 3u) {
          unsigned int i;
          for (i = 0u; i < 16u; ++i) {
            cnet_sctp_session_event event = {0};
            size_t reply_size = 0u;
            status = cnet_sctp_session_poll(session, now, &event,
                body, sizeof(body), reply, sizeof(reply), &reply_size);
            if (status == CNET_SCTP_SESSION_EMPTY) break;
            if (status != CNET_SCTP_SESSION_OK ||
                event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE_ABANDONED ||
                event.stream_id != 0u ||
                event.generation != original_generation ||
                event.abandon_reason != CNET_SCTP_SESSION_ABANDON_TIMED ||
                event.message_size != strlen("c11-drop")) {
              fprintf(stderr, "C11 oracle local abandonment wrong: %d/%d\n",
                  (int)status, (int)event.kind);
              failure = 27; break;
            }
            original_abandoned = 1;
            puts("C11_LOCAL_ABANDONMENT");
            fflush(stdout);
            if (publish(fd, &destination, reply, reply_size) != 0) {
              failure = 28; break;
            }
          }
          if (failure != 0) break;
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
