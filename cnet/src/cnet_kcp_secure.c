#include "cnet_kcp_secure_internal.h"

#include <salts/random.h>

#include "cnet_monocypher_namespace.h"
#include <monocypher.h>

#include <string.h>

enum {
  CNET_KCP_SECURE_WIRE_VERSION = 1,
  CNET_KCP_SECURE_CLIENT_HELLO = 1,
  CNET_KCP_SECURE_SERVER_HELLO = 2,
  CNET_KCP_SECURE_HANDSHAKE_AUTH_BYTES = 48,
  CNET_KCP_SECURE_RECORD_HEADER_BYTES = 32,
  CNET_KCP_SECURE_TAG_BYTES = 16,
  CNET_KCP_SECURE_NONCE_BYTES = 24
};

static const uint8_t CNET_KCP_SECURE_HANDSHAKE_MAGIC[4] = {'T', 'K', 'S', 'H'};
static const uint8_t CNET_KCP_SECURE_RECORD_MAGIC[4] = {'T', 'K', 'S', 'R'};

static void cnet_kcp_secure_write_u16(uint8_t *output, uint16_t value) {
  output[0] = (uint8_t)(value >> 8u);
  output[1] = (uint8_t)value;
}

static void cnet_kcp_secure_write_u64(uint8_t *output, uint64_t value) {
  size_t index;
  for (index = 0u; index < 8u; ++index)
    output[index] = (uint8_t)(value >> (56u - (8u * index)));
}

static uint16_t cnet_kcp_secure_read_u16(const uint8_t *input) {
  return (uint16_t)(((uint16_t)input[0] << 8u) | input[1]);
}

static uint64_t cnet_kcp_secure_read_u64(const uint8_t *input) {
  uint64_t value = 0u;
  size_t index;
  for (index = 0u; index < 8u; ++index) value = (value << 8u) | input[index];
  return value;
}

static bool cnet_kcp_secure_psk_valid(const uint8_t psk[CNET_KCP_PSK_BYTES]) {
  uint8_t combined = 0u;
  size_t index;
  if (psk == NULL) return false;
  for (index = 0u; index < CNET_KCP_PSK_BYTES; ++index) combined |= psk[index];
  return combined != 0u;
}

static void cnet_kcp_secure_mac(const uint8_t psk[CNET_KCP_PSK_BYTES], const void *data,
                                size_t size, uint8_t output[CNET_KCP_SECURE_TAG_BYTES]) {
  crypto_blake2b_keyed(output, CNET_KCP_SECURE_TAG_BYTES, psk, CNET_KCP_PSK_BYTES,
                       (const uint8_t *)data, size);
}

static bool cnet_kcp_secure_equal(const uint8_t *left, const uint8_t *right, size_t size) {
  uint8_t difference = 0u;
  size_t index;
  for (index = 0u; index < size; ++index) difference |= (uint8_t)(left[index] ^ right[index]);
  return difference == 0u;
}

static int cnet_kcp_secure_derive(cnet_kcp_secure_state *state) {
  static const uint8_t client_label[8] = {'c', '2', 's', '-', 'k', 'e', 'y', 0};
  static const uint8_t server_label[8] = {'s', '2', 'c', '-', 'k', 'e', 'y', 0};
  static const uint8_t fec_label[8] = {'f', 'e', 'c', '-', 'k', 'e', 'y', 0};
  uint8_t material[48];
  uint8_t client_key[CNET_KCP_SECURE_KEY_BYTES];
  uint8_t server_key[CNET_KCP_SECURE_KEY_BYTES];

  memcpy(material, state->client_nonce, sizeof(state->client_nonce));
  memcpy(material + 16u, state->server_nonce, sizeof(state->server_nonce));
  cnet_kcp_secure_write_u64(material + 32u, state->session_epoch);
  memcpy(material + 40u, client_label, sizeof(client_label));
  crypto_blake2b_keyed(client_key, sizeof(client_key), state->psk, sizeof(state->psk), material,
                       sizeof(material));
  memcpy(material + 40u, server_label, sizeof(server_label));
  crypto_blake2b_keyed(server_key, sizeof(server_key), state->psk, sizeof(state->psk), material,
                       sizeof(material));
  memcpy(material + 40u, fec_label, sizeof(fec_label));
  crypto_blake2b_keyed(state->fec_key, sizeof(state->fec_key), state->psk, sizeof(state->psk),
                       material, sizeof(material));
  if (state->role == CNET_SECURE_KCP_CLIENT) {
    memcpy(state->send_key, client_key, sizeof(client_key));
    memcpy(state->receive_key, server_key, sizeof(server_key));
  } else {
    memcpy(state->send_key, server_key, sizeof(server_key));
    memcpy(state->receive_key, client_key, sizeof(client_key));
  }
  state->send_packet_number = 0u;
  state->receive_highest = 0u;
  state->receive_bitmap = 0u;
  state->established = true;
  crypto_wipe(client_key, sizeof(client_key));
  crypto_wipe(server_key, sizeof(server_key));
  crypto_wipe(material, sizeof(material));
  return SALTS_OK;
}

static int cnet_kcp_secure_handshake_validate(const uint8_t psk[CNET_KCP_PSK_BYTES],
                                              const void *data, size_t size,
                                              uint8_t expected_type) {
  const uint8_t *input = (const uint8_t *)data;
  uint8_t expected[CNET_KCP_SECURE_TAG_BYTES];
  if (!cnet_kcp_secure_psk_valid(psk) || input == NULL ||
      size != CNET_KCP_SECURE_HANDSHAKE_BYTES ||
      memcmp(input, CNET_KCP_SECURE_HANDSHAKE_MAGIC, sizeof(CNET_KCP_SECURE_HANDSHAKE_MAGIC)) !=
          0 ||
      input[4] != CNET_KCP_SECURE_WIRE_VERSION || input[5] != expected_type ||
      cnet_kcp_secure_read_u16(input + 6u) != CNET_KCP_SECURE_HANDSHAKE_BYTES)
    return SALTS_EPROTO;
  cnet_kcp_secure_mac(psk, input, CNET_KCP_SECURE_HANDSHAKE_AUTH_BYTES, expected);
  if (!cnet_kcp_secure_equal(expected, input + CNET_KCP_SECURE_HANDSHAKE_AUTH_BYTES,
                             sizeof(expected))) {
    crypto_wipe(expected, sizeof(expected));
    return SALTS_EPERM;
  }
  crypto_wipe(expected, sizeof(expected));
  return SALTS_OK;
}

int cnet_kcp_secure_state_init(cnet_kcp_secure_state *state, cnet_secure_kcp_role role,
                               const uint8_t psk[CNET_KCP_PSK_BYTES]) {
  if (state == NULL || !cnet_kcp_secure_psk_valid(psk) ||
      (role != CNET_SECURE_KCP_CLIENT && role != CNET_SECURE_KCP_SERVER))
    return SALTS_EINVAL;
  memset(state, 0, sizeof(*state));
  state->role = role;
  memcpy(state->psk, psk, CNET_KCP_PSK_BYTES);
  return SALTS_OK;
}

void cnet_kcp_secure_state_wipe(cnet_kcp_secure_state *state) {
  if (state != NULL) crypto_wipe(state, sizeof(*state));
}

bool cnet_kcp_secure_is_handshake(const void *data, size_t size) {
  return data != NULL && size == CNET_KCP_SECURE_HANDSHAKE_BYTES &&
         memcmp(data, CNET_KCP_SECURE_HANDSHAKE_MAGIC,
                sizeof(CNET_KCP_SECURE_HANDSHAKE_MAGIC)) == 0;
}

int cnet_kcp_secure_client_hello_authenticate(const cnet_kcp_security_config *config,
                                              const void *data, size_t size) {
  if (config == NULL || config->size != sizeof(*config) ||
      config->mode != CNET_KCP_SECURITY_PSK_V1)
    return SALTS_EINVAL;
  return cnet_kcp_secure_handshake_validate(config->pre_shared_key, data, size,
                                            CNET_KCP_SECURE_CLIENT_HELLO);
}

int cnet_kcp_secure_build_client_hello(cnet_kcp_secure_state *state,
                                       uint8_t output[CNET_KCP_SECURE_HANDSHAKE_BYTES]) {
  if (state == NULL || output == NULL || state->role != CNET_SECURE_KCP_CLIENT)
    return SALTS_EINVAL;
  if (!state->hello_started) {
    const int status = cmeta_platform_secure_random(state->client_nonce,
                                                    sizeof(state->client_nonce));
    if (status != SALTS_OK) return status;
    state->hello_started = true;
  }
  memset(output, 0, CNET_KCP_SECURE_HANDSHAKE_BYTES);
  memcpy(output, CNET_KCP_SECURE_HANDSHAKE_MAGIC, sizeof(CNET_KCP_SECURE_HANDSHAKE_MAGIC));
  output[4] = CNET_KCP_SECURE_WIRE_VERSION;
  output[5] = CNET_KCP_SECURE_CLIENT_HELLO;
  cnet_kcp_secure_write_u16(output + 6u, CNET_KCP_SECURE_HANDSHAKE_BYTES);
  memcpy(output + 8u, state->client_nonce, sizeof(state->client_nonce));
  cnet_kcp_secure_mac(state->psk, output, CNET_KCP_SECURE_HANDSHAKE_AUTH_BYTES,
                      output + CNET_KCP_SECURE_HANDSHAKE_AUTH_BYTES);
  return SALTS_OK;
}

int cnet_kcp_secure_accept_client_hello(cnet_kcp_secure_state *state, const void *data,
                                        size_t size,
                                        uint8_t output[CNET_KCP_SECURE_HANDSHAKE_BYTES]) {
  const uint8_t *input = (const uint8_t *)data;
  int status;
  if (state == NULL || output == NULL || state->role != CNET_SECURE_KCP_SERVER)
    return SALTS_EINVAL;
  status = cnet_kcp_secure_handshake_validate(state->psk, data, size,
                                              CNET_KCP_SECURE_CLIENT_HELLO);
  if (status != SALTS_OK) return status;
  if (state->established &&
      !cnet_kcp_secure_equal(state->client_nonce, input + 8u, sizeof(state->client_nonce)))
    return SALTS_EBUSY;
  memcpy(state->client_nonce, input + 8u, sizeof(state->client_nonce));
  if (!state->established) {
    status = cmeta_platform_secure_random(state->server_nonce, sizeof(state->server_nonce));
    if (status == SALTS_OK)
      status = cmeta_platform_secure_random(&state->session_epoch, sizeof(state->session_epoch));
    if (status != SALTS_OK) return status;
    if (state->session_epoch == 0u) state->session_epoch = 1u;
    status = cnet_kcp_secure_derive(state);
    if (status != SALTS_OK) return status;
  }
  memset(output, 0, CNET_KCP_SECURE_HANDSHAKE_BYTES);
  memcpy(output, CNET_KCP_SECURE_HANDSHAKE_MAGIC, sizeof(CNET_KCP_SECURE_HANDSHAKE_MAGIC));
  output[4] = CNET_KCP_SECURE_WIRE_VERSION;
  output[5] = CNET_KCP_SECURE_SERVER_HELLO;
  cnet_kcp_secure_write_u16(output + 6u, CNET_KCP_SECURE_HANDSHAKE_BYTES);
  memcpy(output + 8u, state->client_nonce, sizeof(state->client_nonce));
  memcpy(output + 24u, state->server_nonce, sizeof(state->server_nonce));
  cnet_kcp_secure_write_u64(output + 40u, state->session_epoch);
  cnet_kcp_secure_mac(state->psk, output, CNET_KCP_SECURE_HANDSHAKE_AUTH_BYTES,
                      output + CNET_KCP_SECURE_HANDSHAKE_AUTH_BYTES);
  return SALTS_OK;
}

int cnet_kcp_secure_accept_server_hello(cnet_kcp_secure_state *state, const void *data,
                                        size_t size) {
  const uint8_t *input = (const uint8_t *)data;
  int status;
  if (state == NULL || state->role != CNET_SECURE_KCP_CLIENT) return SALTS_EINVAL;
  if (state->established) return SALTS_EALREADY;
  status = cnet_kcp_secure_handshake_validate(state->psk, data, size,
                                              CNET_KCP_SECURE_SERVER_HELLO);
  if (status != SALTS_OK) return status;
  if (!cnet_kcp_secure_equal(state->client_nonce, input + 8u, sizeof(state->client_nonce)))
    return SALTS_EPERM;
  memcpy(state->server_nonce, input + 24u, sizeof(state->server_nonce));
  state->session_epoch = cnet_kcp_secure_read_u64(input + 40u);
  if (state->session_epoch == 0u) return SALTS_EPROTO;
  return cnet_kcp_secure_derive(state);
}

static void cnet_kcp_secure_nonce(uint8_t output[CNET_KCP_SECURE_NONCE_BYTES], uint64_t epoch,
                                  uint64_t packet_number, uint8_t direction) {
  memset(output, 0, CNET_KCP_SECURE_NONCE_BYTES);
  cnet_kcp_secure_write_u64(output, epoch);
  cnet_kcp_secure_write_u64(output + 8u, packet_number);
  output[16] = direction;
}

int cnet_kcp_secure_seal(cnet_kcp_secure_state *state, const void *plain, size_t plain_size,
                         void *output, size_t output_capacity, size_t *output_size) {
  uint8_t nonce[CNET_KCP_SECURE_NONCE_BYTES];
  uint8_t *record = (uint8_t *)output;
  uint8_t direction;
  uint64_t packet_number;
  if (output_size != NULL) *output_size = 0u;
  if (state == NULL || !state->established || plain == NULL || plain_size == 0u ||
      plain_size > UINT16_MAX || output == NULL || output_size == NULL ||
      output_capacity < CNET_KCP_SECURE_RECORD_HEADER_BYTES + plain_size +
                            CNET_KCP_SECURE_TAG_BYTES)
    return SALTS_EINVAL;
  if (state->send_packet_number == UINT64_MAX) return SALTS_ERANGE;
  packet_number = ++state->send_packet_number;
  direction = (uint8_t)state->role;
  memset(record, 0, CNET_KCP_SECURE_RECORD_HEADER_BYTES);
  memcpy(record, CNET_KCP_SECURE_RECORD_MAGIC, sizeof(CNET_KCP_SECURE_RECORD_MAGIC));
  record[4] = CNET_KCP_SECURE_WIRE_VERSION;
  record[5] = direction;
  cnet_kcp_secure_write_u16(record + 6u, CNET_KCP_SECURE_RECORD_HEADER_BYTES);
  cnet_kcp_secure_write_u64(record + 8u, state->session_epoch);
  cnet_kcp_secure_write_u64(record + 16u, packet_number);
  cnet_kcp_secure_write_u16(record + 24u, (uint16_t)plain_size);
  cnet_kcp_secure_nonce(nonce, state->session_epoch, packet_number, direction);
  crypto_aead_lock(record + CNET_KCP_SECURE_RECORD_HEADER_BYTES,
                   record + CNET_KCP_SECURE_RECORD_HEADER_BYTES + plain_size, state->send_key,
                   nonce, record, CNET_KCP_SECURE_RECORD_HEADER_BYTES, (const uint8_t *)plain,
                   plain_size);
  crypto_wipe(nonce, sizeof(nonce));
  *output_size = CNET_KCP_SECURE_RECORD_HEADER_BYTES + plain_size + CNET_KCP_SECURE_TAG_BYTES;
  return SALTS_OK;
}

int cnet_kcp_secure_open(cnet_kcp_secure_state *state, const void *record_value,
                         size_t record_size, void *output, size_t output_capacity,
                         size_t *output_size) {
  const uint8_t *record = (const uint8_t *)record_value;
  uint8_t nonce[CNET_KCP_SECURE_NONCE_BYTES];
  uint64_t packet_number;
  uint64_t distance;
  uint16_t plain_size;
  uint8_t expected_direction;
  if (output_size != NULL) *output_size = 0u;
  if (state == NULL || !state->established || record == NULL || output == NULL ||
      output_size == NULL || record_size < CNET_KCP_SECURE_RECORD_OVERHEAD ||
      memcmp(record, CNET_KCP_SECURE_RECORD_MAGIC, sizeof(CNET_KCP_SECURE_RECORD_MAGIC)) != 0 ||
      record[4] != CNET_KCP_SECURE_WIRE_VERSION ||
      cnet_kcp_secure_read_u16(record + 6u) != CNET_KCP_SECURE_RECORD_HEADER_BYTES ||
      cnet_kcp_secure_read_u64(record + 8u) != state->session_epoch)
    return SALTS_EPROTO;
  expected_direction = state->role == CNET_SECURE_KCP_CLIENT ? CNET_SECURE_KCP_SERVER
                                                             : CNET_SECURE_KCP_CLIENT;
  if (record[5] != expected_direction) return SALTS_EPERM;
  packet_number = cnet_kcp_secure_read_u64(record + 16u);
  plain_size = cnet_kcp_secure_read_u16(record + 24u);
  if (packet_number == 0u || plain_size == 0u || plain_size > output_capacity ||
      record_size != CNET_KCP_SECURE_RECORD_HEADER_BYTES + (size_t)plain_size +
                         CNET_KCP_SECURE_TAG_BYTES)
    return SALTS_EPROTO;
  if (packet_number <= state->receive_highest) {
    distance = state->receive_highest - packet_number;
    if (distance >= 64u || (state->receive_bitmap & (UINT64_C(1) << distance)) != 0u)
      return SALTS_EALREADY;
  }
  cnet_kcp_secure_nonce(nonce, state->session_epoch, packet_number, record[5]);
  if (crypto_aead_unlock((uint8_t *)output,
                         record + CNET_KCP_SECURE_RECORD_HEADER_BYTES + plain_size,
                         state->receive_key, nonce, record, CNET_KCP_SECURE_RECORD_HEADER_BYTES,
                         record + CNET_KCP_SECURE_RECORD_HEADER_BYTES, plain_size) != 0) {
    crypto_wipe(nonce, sizeof(nonce));
    return SALTS_EPERM;
  }
  crypto_wipe(nonce, sizeof(nonce));
  if (packet_number > state->receive_highest) {
    distance = packet_number - state->receive_highest;
    state->receive_bitmap = distance >= 64u ? UINT64_C(1)
                                            : (state->receive_bitmap << distance) | UINT64_C(1);
    state->receive_highest = packet_number;
  } else {
    distance = state->receive_highest - packet_number;
    state->receive_bitmap |= UINT64_C(1) << distance;
  }
  *output_size = plain_size;
  return SALTS_OK;
}
