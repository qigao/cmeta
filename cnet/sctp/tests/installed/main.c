#include <cnet/sctp_wire.h>
#include <cnet/sctp_session.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct test_context {
  uint32_t tag;
  uint32_t tsn;
  uint32_t key;
} test_context;

static int owned(void *context) { return context != NULL; }

static int entropy(void *value, uint8_t *bytes, size_t size) {
  const test_context *context = (const test_context *)value;
  if (!context || !bytes || size != 8u) return -1;
  bytes[0] = (uint8_t)(context->tag >> 24);
  bytes[1] = (uint8_t)(context->tag >> 16);
  bytes[2] = (uint8_t)(context->tag >> 8);
  bytes[3] = (uint8_t)context->tag;
  bytes[4] = (uint8_t)(context->tsn >> 24);
  bytes[5] = (uint8_t)(context->tsn >> 16);
  bytes[6] = (uint8_t)(context->tsn >> 8);
  bytes[7] = (uint8_t)context->tsn;
  return 0;
}

/* Installed-consumer lifecycle fixture only. Production callers must provide
 * a cryptographic MAC whose key lifetime is bound to the DTLS generation. */
static int test_mac(void *value, const uint8_t *bytes, size_t size,
    uint8_t out[CNET_SCTP_SESSION_MAC_BYTES]) {
  const test_context *context = (const test_context *)value;
  uint32_t state;
  size_t i;
  if (!context || (!bytes && size != 0u) || !out) return -1;
  state = context->key;
  for (i = 0u; i < size; ++i) state = state * UINT32_C(65599) + bytes[i];
  for (i = 0u; i < CNET_SCTP_SESSION_MAC_BYTES; ++i) {
    state = state * UINT32_C(1664525) + UINT32_C(1013904223);
    out[i] = (uint8_t)(state >> 24);
  }
  return 0;
}

static int test_verify(void *value, const uint8_t *bytes, size_t size,
    const uint8_t claimed[CNET_SCTP_SESSION_MAC_BYTES]) {
  uint8_t actual[CNET_SCTP_SESSION_MAC_BYTES];
  uint8_t different = 0u;
  size_t i;
  if (!claimed || test_mac(value, bytes, size, actual) != 0) return -1;
  for (i = 0u; i < sizeof(actual); ++i)
    different |= (uint8_t)(actual[i] ^ claimed[i]);
  return different == 0u ? 0 : -1;
}

static cnet_sctp_session_create_config session_config(uint16_t port,
    int client, test_context *context) {
  cnet_sctp_session_create_config config = {0};
  config.abi_version = CNET_SCTP_SESSION_ABI_VERSION;
  config.struct_size = sizeof(config);
  config.local_port = port;
  config.remote_port = port == 5000u ? 5001u : 5000u;
  config.receive_window = 8192u;
  config.outbound_streams = 4u;
  config.inbound_streams = 4u;
  config.t1_ms = 100u;
  config.t1_max_ms = 400u;
  config.cookie_lifetime_ms = 1000u;
  config.peer_binding = UINT64_C(0x0123456789abcdef);
  config.max_retransmits = 3u;
  config.is_dtls_client = (uint8_t)client;
  config.is_owner = owned;
  config.entropy = entropy;
  config.mac = test_mac;
  config.verify_mac = test_verify;
  config.provider_context = context;
  return config;
}

static int rejected_abi_contract(void) {
  test_context context = {
      UINT32_C(0x11223344), UINT32_C(0x10203040), UINT32_C(0x13579bdf)};
  cnet_sctp_session_create_config config = session_config(5000u, 1, &context);
  cnet_sctp_session *session = (cnet_sctp_session *)(uintptr_t)1u;
  if (cnet_sctp_session_create(&config, 0u, NULL) !=
      CNET_SCTP_SESSION_INVALID)
    return 10;
  ++config.abi_version;
  if (cnet_sctp_session_create(&config, 0u, &session) !=
          CNET_SCTP_SESSION_INVALID ||
      session != NULL)
    return 11;
  config.abi_version = CNET_SCTP_SESSION_ABI_VERSION;
  --config.struct_size;
  session = (cnet_sctp_session *)(uintptr_t)1u;
  if (cnet_sctp_session_create(&config, 0u, &session) !=
          CNET_SCTP_SESSION_INVALID ||
      session != NULL)
    return 12;
  return 0;
}

static int deliver(cnet_sctp_session *receiver, cnet_sctp_session *sender,
    uint64_t now, const uint8_t *packet, size_t packet_size) {
  uint8_t reply[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t final[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  size_t reply_size = 0u, final_size = 0u;
  if (cnet_sctp_session_input(receiver, now, packet, packet_size,
          reply, sizeof(reply), &reply_size) != CNET_SCTP_SESSION_OK)
    return -1;
  if (reply_size != 0u &&
      cnet_sctp_session_input(sender, now + 1u, reply, reply_size,
          final, sizeof(final), &final_size) != CNET_SCTP_SESSION_OK)
    return -1;
  return final_size == 0u ? 0 : -1;
}

static int session_round_trip(void) {
  test_context client_context = {
      UINT32_C(0x11223344), UINT32_C(0x10203040), UINT32_C(0x13579bdf)};
  test_context server_context = {
      UINT32_C(0x55667788), UINT32_C(0x50607080), UINT32_C(0x13579bdf)};
  cnet_sctp_session_create_config client_config =
      session_config(5000u, 1, &client_context);
  cnet_sctp_session_create_config server_config =
      session_config(5001u, 0, &server_context);
  cnet_sctp_session *client = NULL, *server = NULL;
  cnet_sctp_session_event event = {0};
  uint8_t first[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t second[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t third[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t fourth[CNET_SCTP_SESSION_MAX_PACKET_BYTES];
  uint8_t message[CNET_SCTP_SESSION_MAX_MESSAGE_BYTES];
  size_t first_size = 0u, second_size = 0u, third_size = 0u, fourth_size = 0u;
  int result = 20;

  if (cnet_sctp_session_create(&client_config, 0u, &client) !=
          CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_create(&server_config, 0u, &server) !=
          CNET_SCTP_SESSION_OK)
    goto cleanup;
  if (cnet_sctp_session_start(client, 1u, first, sizeof(first), &first_size) !=
          CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(server, 2u, first, first_size,
          second, sizeof(second), &second_size) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(client, 3u, second, second_size,
          third, sizeof(third), &third_size) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(server, 4u, third, third_size,
          fourth, sizeof(fourth), &fourth_size) != CNET_SCTP_SESSION_OK ||
      cnet_sctp_session_input(client, 5u, fourth, fourth_size,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      first_size != 0u)
    goto cleanup;

  if (cnet_sctp_session_open_channel(client, 6u, 0u,
          CNET_SCTP_SESSION_CHANNEL_RELIABLE_UNORDERED, 0u, 0u,
          "installed", 9u, NULL, 0u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      deliver(server, client, 7u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, 9u, &event, message, sizeof(message),
          second, sizeof(second), &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN || second_size == 0u ||
      deliver(client, server, 10u, second, second_size) != 0 ||
      cnet_sctp_session_poll(client, 12u, &event, message, sizeof(message),
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_CHANNEL_OPEN)
    goto cleanup;

  if (cnet_sctp_session_send(client, 13u, 0u, 1, "sdk", 3u,
          first, sizeof(first), &first_size) != CNET_SCTP_SESSION_OK ||
      deliver(server, client, 14u, first, first_size) != 0 ||
      cnet_sctp_session_poll(server, 16u, &event, message, sizeof(message),
          second, sizeof(second), &second_size) != CNET_SCTP_SESSION_OK ||
      event.kind != CNET_SCTP_SESSION_EVENT_MESSAGE || !event.binary ||
      event.message_size != 3u || memcmp(message, "sdk", 3u) != 0)
    goto cleanup;
  result = 0;

cleanup:
  if (client) {
    if (cnet_sctp_session_stop(client) != CNET_SCTP_SESSION_OK ||
        !cnet_sctp_session_drained(client) ||
        cnet_sctp_session_destroy(client) != CNET_SCTP_SESSION_OK)
      result = 21;
  }
  if (server) {
    if (cnet_sctp_session_stop(server) != CNET_SCTP_SESSION_OK ||
        !cnet_sctp_session_drained(server) ||
        cnet_sctp_session_destroy(server) != CNET_SCTP_SESSION_OK)
      result = 22;
  }
  return result;
}

int main(void) {
  const uint8_t payload[] = {1u, 2u, 3u};
  const cnet_sctp_wire_chunk chunk = {0u, 0u, payload, sizeof(payload)};
  cnet_sctp_wire_chunk received = {0};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  uint8_t packet[64] = {0};
  size_t packet_size = 0u;
  int abi_result;
  if (CNET_SCTP_WIRE_ABI_VERSION != 1u) return 1;
  abi_result = rejected_abi_contract();
  if (abi_result != 0) return abi_result;
  if (cnet_sctp_wire_encode(5000u, 5001u, 0x10203040u, &chunk, 1u,
                            packet, sizeof(packet), &packet_size) != CNET_SCTP_WIRE_OK)
    return 2;
  if (cnet_sctp_wire_decode(packet, packet_size, &header, &decoder) != CNET_SCTP_WIRE_OK)
    return 3;
  if (header.chunk_count != 1u || header.verification_tag != 0x10203040u)
    return 4;
  if (cnet_sctp_wire_next_chunk(&decoder, &received) != CNET_SCTP_WIRE_OK)
    return 5;
  if (received.value_size != sizeof(payload) || received.value[2] != 3u)
    return 6;
  if (cnet_sctp_wire_next_chunk(&decoder, &received) != CNET_SCTP_WIRE_END)
    return 7;
  return session_round_trip();
}
