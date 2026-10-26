/* Test-only POSIX UDP datagram adapter for a truly independent Pion SCTP
 * association. It uses ONLY the installed opaque C11 Session public header.
 * UDP replaces a DTLS application's datagram transport for this isolated
 * negotiation/wire conformance fixture. It is NOT a production transport,
 * authentication provider, installed target, or M4 DTLS qualification. */
#define _POSIX_C_SOURCE 200809L
#include <cnet/sctp_session.h>
#include "pion_dtls_transport.h"

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
  uint64_t admitted_generation;
} oracle_context;

static volatile sig_atomic_t shutting_down = 0;
static pion_test_dtls secure_transport = {0};
static int use_real_dtls = 0;
static void stop_signal(int signo) { (void)signo; shutting_down = 1; }
static int owner(void *context) {
  const oracle_context *c = (const oracle_context *)context;
  if (!c) return 0;
  /* The same C11 Owner stays admitted only for the exact authenticated
   * DTLS generation that created this Session. This is a single-threaded
   * test gate, NOT production lease/concurrent cancel/drain. */
  return !use_real_dtls ||
      (secure_transport.accepted && secure_transport.generation != 0u &&
       c->admitted_generation == secure_transport.generation);
}
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
  if (use_real_dtls) {
    if (pion_test_dtls_send(&secure_transport, fd, destination,
            (const uint8_t *)bytes, length) != 0)
      return -1;
  } else if (sendto(fd, bytes, length, 0,
          (const struct sockaddr *)destination,
          sizeof(*destination)) != (ssize_t)length) {
    return -1;
  }
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
/* An additional, independently qualified scenario drops the FIRST genuine
 * I-FORWARD wire control, then requires a byte-for-byte identical Owner
 * retransmission before Pion may retire the abandoned unordered MID. */
static int drop_first_iforward = 0;
static int first_forward_dropped = 0;
static int forward_retransmitted = 0;
static uint8_t saved_forward[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
static size_t saved_forward_size = 0u;
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

/* Real OpenSSL/Pion DTLS has authenticated the original peer, then the
 * external test Owner Generation was revoked. Every public Session surface
 * below must now refuse with OWNER before touching copied user buffers,
 * allocating TSNs, reading ULP events, consuming terminal receipts, or
 * releasing the allocated opaque handle. This remains a single-threaded,
 * test-only lease check, NOT production concurrent DSO/DTLS teardown. */
static int qualify_revoked_owner_facade(cnet_sctp_session *session,
    uint64_t now, uint64_t original_generation) {
  cnet_sctp_session_event event = {0};
  cnet_sctp_session_tx_usage usage = {0};
  cnet_sctp_session_reliable_ack ack = {0};
  cnet_sctp_session_forward_progress progress = {0};
  cnet_sctp_session_terminal_failure failure = {0};
  cnet_sctp_session_peer_abort abort_info = {0};
  cnet_sctp_session_unsent_cancelled cancelled = {0};
  uint8_t wire[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t message[32], reason[32];
  uint64_t ticket = UINT64_C(9876), deadline = 0u;
  size_t written = sizeof(wire);
  int has_deadline = 0;

  if (!session || !original_generation) return -1;
  if (cnet_sctp_session_get_tx_usage(session, 0u, original_generation,
          &usage) != CNET_SCTP_SESSION_OWNER ||
      usage.generation != 0u || usage.retained_bytes != 0u ||
      usage.queued_bytes != 0u)
    return -1;
  if (cnet_sctp_session_poll(session, now, &event, message,
          sizeof(message), wire, sizeof(wire), &written) !=
          CNET_SCTP_SESSION_OWNER ||
      event.kind != CNET_SCTP_SESSION_EVENT_NONE || written != 0u)
    return -1;
  written = sizeof(wire);
  if (cnet_sctp_session_advance(session, now, wire,
          sizeof(wire), &written) != CNET_SCTP_SESSION_OWNER ||
      written != 0u)
    return -1;
  if (cnet_sctp_session_next_deadline(session, &deadline,
          &has_deadline) != CNET_SCTP_SESSION_OWNER)
    return -1;
  /* Revoking the authenticated DTLS Owner must block EVERY public wire
   * mutation, not only ingress and receipt reads. Preserve the opaque
   * Session for an explicit successful Owner hand-back at teardown. */
  if (cnet_sctp_session_start(session, now, wire,
          sizeof(wire), &written) != CNET_SCTP_SESSION_OWNER)
    return -1;
  written = sizeof(wire);
  if (cnet_sctp_session_open_channel(session, now, 2u,
          CNET_SCTP_SESSION_CHANNEL_RELIABLE_ORDERED,
          0u, 0u, "stale", 5u, NULL, 0u,
          wire, sizeof(wire), &written) != CNET_SCTP_SESSION_OWNER ||
      written != 0u)
    return -1;
  written = sizeof(wire);
  if (cnet_sctp_session_send(session, now, 0u, 1,
          "stale", 5u, wire, sizeof(wire),
          &written) != CNET_SCTP_SESSION_OWNER || written != 0u)
    return -1;
  if (cnet_sctp_session_queue_message(session, now, 0u, 1,
          "stale", 5u) != CNET_SCTP_SESSION_OWNER)
    return -1;
  ticket = UINT64_C(9876);
  if (cnet_sctp_session_queue_reliable_tracked(session, now, 0u,
          original_generation, 1, "stale", 5u,
          &ticket) != CNET_SCTP_SESSION_OWNER || ticket != 0u)
    return -1;
  written = sizeof(wire);
  if (cnet_sctp_session_pump_message(session, now, wire,
          sizeof(wire), &written) != CNET_SCTP_SESSION_OWNER ||
      written != 0u)
    return -1;
  {
    uint64_t close_generation = 0u;
    written = sizeof(wire);
    if (cnet_sctp_session_close_channel(session, now, 0u,
            wire, sizeof(wire), &written,
            &close_generation) != CNET_SCTP_SESSION_OWNER ||
        written != 0u || close_generation != 0u)
      return -1;
  }
  written = sizeof(wire);
  if (cnet_sctp_session_send_reliable_tracked(session, now,
          0u, original_generation, 1, "stale", 5u,
          wire, sizeof(wire), &written, &ticket) !=
          CNET_SCTP_SESSION_OWNER || written != 0u || ticket != 0u)
    return -1;
  if (cnet_sctp_session_cancel_unsent_message(session, now,
          0u, original_generation, &cancelled) !=
          CNET_SCTP_SESSION_OWNER || cancelled.generation != 0u ||
      cancelled.ticket != 0u)
    return -1;
  if (cnet_sctp_session_take_reliable_ack(session, &ack) !=
          CNET_SCTP_SESSION_OWNER || ack.ticket != 0u)
    return -1;
  if (cnet_sctp_session_take_forward_progress(session, &progress) !=
          CNET_SCTP_SESSION_OWNER || progress.generation != 0u)
    return -1;
  if (cnet_sctp_session_take_terminal_failure(session, &failure) !=
          CNET_SCTP_SESSION_OWNER || failure.generation != 0u)
    return -1;
  written = sizeof(reason);
  if (cnet_sctp_session_take_peer_abort(session, &abort_info,
          reason, sizeof(reason), &written) != CNET_SCTP_SESSION_OWNER ||
      written != 0u || abort_info.has_user_reason != 0u)
    return -1;
  if (cnet_sctp_session_stop(session) != CNET_SCTP_SESSION_OWNER)
    return -1;
  /* Destroy denial MUST retain the same live handle for explicit Owner
   * hand-back at the fixture's normal cleanup. Call this LAST: an unsafe
   * success would have freed it and is a hard test failure. */
  if (cnet_sctp_session_destroy(session) != CNET_SCTP_SESSION_OWNER)
    return -1;
  return 0;
}

int main(int argc, char **argv) {
  oracle_context context = {
    UINT32_C(0x11223344), UINT32_C(0x10203040),
    UINT32_C(0x13579bdf), 0u
  };
  cnet_sctp_session_create_config cfg = {0};
  cnet_sctp_session *session = NULL;
  struct sockaddr_in local = {0}, destination = {0};
  uint64_t original_generation = 0u;
  unsigned int stage = 0u;
  int original_abandoned = 0;
  int revoke_after_message = 0;
  int revoked = 0;
  int revoked_input_refused = 0;
  uint8_t packet[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t encrypted[4096];
  uint8_t reply[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t body[CNET_SCTP_SESSION_MAX_MESSAGE_BYTES];
  uint64_t start, now;
  int fd = -1, failure = 0;
  unsigned int local_port, peer_port;
  if ((argc != 3 &&
       !(argc == 4 && strcmp(argv[3], "drop-first-iforward") == 0) &&
       !(argc == 5 && strcmp(argv[3], "dtls") == 0) &&
       !(argc == 6 && strcmp(argv[3], "dtls") == 0 &&
           (strcmp(argv[5], "drop-first-iforward") == 0 ||
            strcmp(argv[5], "revoke-owner") == 0)))) {
    fprintf(stderr, "usage: pion_oracle_peer UDP_LISTEN_PORT UDP_PEER_PORT [drop-first-iforward] | [dtls CLIENT_SHA256_HEX [drop-first-iforward|revoke-owner]]\n");
    return 2;
  }
  use_real_dtls = argc >= 5;
  drop_first_iforward = (argc == 4 ||
      (argc == 6 && strcmp(argv[5], "drop-first-iforward") == 0));
  revoke_after_message = (argc == 6 && use_real_dtls &&
      strcmp(argv[5], "revoke-owner") == 0);
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
  cfg.peer_binding = use_real_dtls ?
      0u : UINT64_C(0x0123456789abcdef);
  cfg.max_retransmits = 5u;
  cfg.is_dtls_client = 0u; /* Pion DCEP client uses even SID=0. */
  cfg.is_owner = owner;
  cfg.entropy = entropy;
  cfg.mac = test_mac;
  cfg.verify_mac = test_verify;
  cfg.provider_context = &context;
  /* Plaintext SCTP-only oracle retains the old bounded behavior.
   * DTLS mode MUST NOT allocate/admit an SCTP Association before both
   * certificate pins and the exported generation have been verified. */
  if (!use_real_dtls &&
      cnet_sctp_session_create(&cfg, 0u, &session) !=
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
  if (use_real_dtls &&
      pion_test_dtls_init(&secure_transport, argv[4]) != 0) {
    fprintf(stderr, "C11 oracle real DTLS initialization failed\n");
    failure = 32; goto done;
  }
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
      const ssize_t raw_size = recvfrom(fd, encrypted, sizeof(encrypted),
          0, NULL, NULL);
      size_t plaintext_size = 0u, written = 0u;
      cnet_sctp_session_status status;
      if (raw_size <= 0) { failure = 9; break; }
      if (use_real_dtls) {
        int unwrapped = pion_test_dtls_input(&secure_transport, fd,
            &destination, encrypted, (size_t)raw_size,
            packet, sizeof(packet), &plaintext_size);
        if (unwrapped < 0) {
          fprintf(stderr,
              "C11 oracle DTLS packet authentication/record failure\n");
          if (!session) {
            puts("C11_DTLS_REJECTED_BEFORE_SESSION_CREATE");
            fflush(stdout);
          }
          failure = 33; break;
        }
        /* SCTP Owner NEVER sees handshake/ciphertext bytes. */
        if (unwrapped == 0) continue;
        if (!session) {
          if (!secure_transport.accepted ||
              secure_transport.generation == 0u ||
              context.admitted_generation != 0u) {
            failure = 34; break;
          }
          context.admitted_generation = secure_transport.generation;
          cfg.peer_binding = context.admitted_generation;
          if (cnet_sctp_session_create(&cfg, now, &session) !=
              CNET_SCTP_SESSION_OK) {
            failure = 35; break;
          }
          puts("C11_DTLS_SESSION_CREATED_AFTER_AUTH");
          fflush(stdout);
        }
      } else {
        if ((size_t)raw_size > sizeof(packet)) { failure = 9; break; }
        memcpy(packet, encrypted, (size_t)raw_size);
        plaintext_size = (size_t)raw_size;
      }
      if (plaintext_size < 16u) { failure = 9; break; }
      printf("RX_TYPE=%u\n", (unsigned)packet[12]);
      fflush(stdout);
      status = cnet_sctp_session_input(session, now, packet,
          plaintext_size, reply, sizeof(reply), &written);
      if (status != CNET_SCTP_SESSION_OK) {
        if (revoke_after_message && revoked && !revoked_input_refused &&
            status == CNET_SCTP_SESSION_OWNER && written == 0u) {
          /* OpenSSL authenticated/decrypted this real DTLS record, but
           * the stale C11 Owner generation refuses it BEFORE any ULP
           * event is admitted. Test-only single-threaded lease. */
          revoked_input_refused = 1;
          puts("C11_DTLS_OWNER_STALE_GENERATION_REJECTED");
          fflush(stdout);
          continue;
        }
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
          if (revoke_after_message && !revoked &&
              event.kind == CNET_SCTP_SESSION_EVENT_MESSAGE &&
              stage == 4u) {
            static const char confirmed[] = "pion-independent-idata";
            if (event.generation != original_generation ||
                event.stream_id != 0u || event.ppid != 53u ||
                event.message_size != sizeof(confirmed) - 1u ||
                memcmp(body, confirmed, sizeof(confirmed) - 1u) != 0) {
              failure = 36; break;
            }
            context.admitted_generation ^= UINT64_C(1);
            if (context.admitted_generation ==
                secure_transport.generation) {
              failure = 37; break;
            }
            revoked = 1;
            puts("C11_DTLS_OWNER_GENERATION_REVOKED");
            fflush(stdout);
            if (qualify_revoked_owner_facade(session, now,
                    original_generation) != 0) {
              fprintf(stderr,
                  "C11 revoked authenticated DTLS Owner crossed public facade\n");
              failure = 38; break;
            }
            puts("C11_DTLS_OWNER_REVOKED_PUBLIC_API_REFUSALS_PASS");
            fflush(stdout);
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
                event.message_size != strlen("c11-drop") ||
                !event.has_tsn || event.last_tsn != dropped_tsn) {
              failure = 19; break;
            }
            original_abandoned = 1;
            puts("C11_LOCAL_ABANDONMENT");
            fflush(stdout);
          }
          /* After revoking the test Owner's immutable DTLS generation,
           * even a redundant poll is correctly rejected with OWNER.
           * Stop scheduling/reading the old Session immediately; the next
           * decrypted Pion datagram is checked only by input() for a
           * failure-atomic OWNER refusal. */
          if (revoke_after_message && revoked) break;
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
          /* A dropped first I-FORWARD cannot receive an independent
           * cumulative acknowledgement before the replay is forwarded. */
          if (drop_first_iforward && !forward_retransmitted) {
            fprintf(stderr,
                "C11 oracle premature peer FORWARD confirmation\n");
            failure = 30; break;
          }
          if (progress.generation != original_generation ||
              progress.stream_id != 0u ||
              progress.abandon_reason != CNET_SCTP_SESSION_ABANDON_TIMED ||
              progress.message_size != strlen("c11-drop") ||
              progress.last_tsn != dropped_tsn ||
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
    if (!session || (revoke_after_message && revoked)) continue;
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
          int suppress = 0;
          /* The abandoned TSN is retired by I-FORWARD. A late retransmit
           * of the same I-DATA is a protocol regression, even if the peer
           * could otherwise ignore the already skipped MID. */
          if (written >= 20u && reply[12u] == 64u &&
              stage >= 3u && chunk_tsn(reply) == dropped_tsn) {
            fprintf(stderr,
                "C11 late abandoned I-DATA after I-FORWARD\n");
            failure = 31; break;
          }
          if (written != 0u && reply[12u] == 194u) {
            if (stage == 2u && dropped_tsn_live) {
              dropped_tsn_live = 0;
              stage = 3u;
              puts("C11_REAL_I_FORWARD_TSN_194");
              fflush(stdout);
              if (drop_first_iforward) {
                memcpy(saved_forward, reply, written);
                saved_forward_size = written;
                first_forward_dropped = 1;
                puts("C11_DROPPED_FIRST_I_FORWARD_194");
                fflush(stdout);
                suppress = 1;
              }
            } else if (stage == 3u && drop_first_iforward &&
                       first_forward_dropped && !forward_retransmitted) {
              /* The real Owner RTO must retransmit the same immutable
               * I-FORWARD wire record, not create another MID/TSN, before
               * recovering the peer's cumulative SACK. */
              if (written != saved_forward_size ||
                  memcmp(reply, saved_forward, written) != 0) {
                fprintf(stderr,
                    "C11 oracle I-FORWARD retry mutated retained bytes\n");
                failure = 29; break;
              }
              forward_retransmitted = 1;
              puts("C11_RETRANSMITTED_SAME_I_FORWARD_194");
              fflush(stdout);
            } else {
              fprintf(stderr,
                  "C11 oracle unexpected I-FORWARD while stage=%u\n",
                  stage);
              failure = 24; break;
            }
          } else if (written != 0u && reply[12u] == 192u) {
            fprintf(stderr,
                "C11 emitted legacy FORWARD instead of I-FORWARD\n");
            failure = 25; break;
          }
          if (!suppress &&
              publish(fd, &destination, reply, written) != 0) {
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
                event.message_size != strlen("c11-drop") ||
                !event.has_tsn || event.last_tsn != dropped_tsn) {
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
    /* Test-only synchronous Owner hand-back to explicitly stop and drain
     * after proving a stale authenticated datagram cannot be delivered. */
    if (revoke_after_message && revoked)
      context.admitted_generation = secure_transport.generation;
    if (cnet_sctp_session_stop(session) != CNET_SCTP_SESSION_OK ||
        !cnet_sctp_session_drained(session) ||
        cnet_sctp_session_destroy(session) != CNET_SCTP_SESSION_OK)
      failure = 17;
  }
  if (use_real_dtls) pion_test_dtls_stop(&secure_transport);
  if (fd >= 0) close(fd);
  if (!failure) { puts("C11_ORACLE_EXIT_OK"); fflush(stdout); }
  return failure;
}
